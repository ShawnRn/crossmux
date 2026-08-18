#include "InkLinkActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <GfxRenderer.h>
#include <WiFi.h>
#include <string.h>

// ---- CRC32 (table-based, self-contained) -----------------------------------

static uint32_t crc32Table[256];
static bool crc32TableReady = false;

static void mkCrc32Table() {
  if (crc32TableReady) return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc32Table[i] = c;
  }
  crc32TableReady = true;
}

static uint32_t crc32File(const char* path) {
  mkCrc32Table();
  File f = HalStorage::open(path, FILE_READ);
  if (!f) return 0;
  uint32_t crc = 0xFFFFFFFFu;
  uint8_t buf[512];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    for (int i = 0; i < n; i++) crc = crc32Table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  }
  f.close();
  return crc ^ 0xFFFFFFFFu;
}

// ---- Static member ---------------------------------------------------------

InkLinkActivity* InkLinkActivity::sInstance = nullptr;

// ---- Construction / destruction --------------------------------------------

InkLinkActivity::InkLinkActivity() { sInstance = this; }

InkLinkActivity::~InkLinkActivity() {
  if (wsServer_) {
    wsServer_->close();
    delete wsServer_;
    wsServer_ = nullptr;
  }
  sInstance = nullptr;
}

// ---- Lifecycle -------------------------------------------------------------

void InkLinkActivity::onEnter(GfxRenderer& renderer, const ActivityContext& /*ctx*/) {
  HalStorage::mkdirp(inklink::SLOT_DIR);
  snprintf(tmpPath_, sizeof(tmpPath_), "%s/_incoming.tmp", inklink::SLOT_DIR);

  if (!wsServer_) {
    wsServer_ = new WebSocketsServer(inklink::WS_PORT);
    wsServer_->begin();
    wsServer_->onEvent(InkLinkActivity::onWsEvent);
    serverStarted_ = true;
    Serial.printf("[InkLink] WS server started on :%u\n", inklink::WS_PORT);
  }

  state_ = State::Idle;
  needsRedraw_ = true;
}

void InkLinkActivity::onExit(GfxRenderer& /*renderer*/) {
  if (wsServer_) {
    wsServer_->close();
    delete wsServer_;
    wsServer_ = nullptr;
    serverStarted_ = false;
  }
}

void InkLinkActivity::loop(GfxRenderer& renderer, const InputEvent& input) {
  if (wsServer_) wsServer_->loop();

  // Physical button: cycle named slots (Left = previous, Right = next)
  if (input.type == InputEvent::Type::ButtonPress) {
    if (input.button == InputEvent::Button::Left) {
      loadSlotByIndex(renderer, (currentSlot_ - 1 + inklink::SLOT_COUNT) % inklink::SLOT_COUNT);
    } else if (input.button == InputEvent::Button::Right) {
      loadSlotByIndex(renderer, (currentSlot_ + 1) % inklink::SLOT_COUNT);
    }
  }

  // Transition: binary reception complete → verify
  if (state_ == State::Verifying) {
    finishReceiving(renderer);
  }
}

void InkLinkActivity::render(GfxRenderer& renderer) {
  if (!needsRedraw_) return;
  needsRedraw_ = false;

  char defaultSlotPath[64];
  snprintf(defaultSlotPath, sizeof(defaultSlotPath), "%s/slot_%s.bin",
           inklink::SLOT_DIR, inklink::SLOT_NAMES[0]);
  if (HalStorage::exists(defaultSlotPath)) {
    currentSlot_ = 0;
    displayFrameFromFile(renderer, defaultSlotPath);
    return;
  }

  drawStatusScreen(renderer);
}

// ---- WS trampoline ---------------------------------------------------------

void InkLinkActivity::onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (!sInstance) return;
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[InkLink] Client %u connected\n", num);
      break;
    case WStype_DISCONNECTED:
      sInstance->handleWsDisconnect(num);
      break;
    case WStype_TEXT:
      sInstance->handleWsText(num, payload, length);
      break;
    case WStype_BIN:
      sInstance->handleWsBinary(num, payload, length);
      break;
    default:
      break;
  }
}

// ---- WS message handlers ---------------------------------------------------

void InkLinkActivity::handleWsText(uint8_t num, const uint8_t* payload, size_t length) {
  // Expected: {"slot":"desk","size":104544,"crc32":3291864591}
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, length)) {
    wsServer_->sendTXT(num, "{\"error\":\"bad_json\"}");
    return;
  }

  const char* slot = doc["slot"] | "desk";
  uint32_t size = doc["size"] | 0u;
  uint32_t crcExpected = doc["crc32"] | 0u;

  if (size != static_cast<uint32_t>(inklink::FRAME_BYTES)) {
    wsServer_->sendTXT(num, "{\"error\":\"bad_size\"}");
    return;
  }

  startReceiving(num, slot, size, crcExpected);
  wsServer_->sendTXT(num, "{\"ok\":\"ready\"}");
}

void InkLinkActivity::handleWsBinary(uint8_t num, const uint8_t* payload, size_t length) {
  if (state_ != State::Receiving || num != activeClient_) return;

  File f = HalStorage::open(tmpPath_, FILE_APPEND);
  if (!f) {
    abortReceiving();
    wsServer_->sendTXT(num, "{\"error\":\"sd_write\"}");
    return;
  }
  f.write(payload, length);
  f.close();
  receivedBytes_ += length;

  if (receivedBytes_ >= pendingSize_) {
    state_ = State::Verifying;  // loop() picks this up
  }
}

void InkLinkActivity::handleWsDisconnect(uint8_t num) {
  if (num == activeClient_) abortReceiving();
}

// ---- Receive lifecycle -----------------------------------------------------

void InkLinkActivity::startReceiving(uint8_t num, const char* slot,
                                     uint32_t size, uint32_t crc) {
  abortReceiving();
  activeClient_  = num;
  pendingSize_   = size;
  pendingCrc_    = crc;
  receivedBytes_ = 0;
  strlcpy(pendingSlot_, slot, sizeof(pendingSlot_));

  snprintf(slotPath_, sizeof(slotPath_), "%s/slot_%s.bin",
           inklink::SLOT_DIR, pendingSlot_);

  // Truncate tmp file
  HalStorage::remove(tmpPath_);
  state_ = State::Receiving;
  Serial.printf("[InkLink] Recv start: slot=%s size=%u\n", pendingSlot_, size);
}

void InkLinkActivity::finishReceiving(GfxRenderer& renderer) {
  state_ = State::Idle;  // set early to prevent re-entry

  // Verify CRC32
  uint32_t actual = crc32File(tmpPath_);
  if (pendingCrc_ != 0 && actual != pendingCrc_) {
    Serial.printf("[InkLink] CRC mismatch: got=%08X want=%08X\n", actual, pendingCrc_);
    wsServer_->sendTXT(activeClient_, "{\"error\":\"crc_mismatch\"}");
    HalStorage::remove(tmpPath_);
    return;
  }

  // Promote tmp → slot file
  HalStorage::remove(slotPath_);
  HalStorage::rename(tmpPath_, slotPath_);

  // Display
  state_ = State::Displaying;
  displayFrameFromFile(renderer, slotPath_);
  state_ = State::Idle;

  wsServer_->sendTXT(activeClient_, "{\"ok\":\"displayed\"}");
  activeClient_ = 0xFF;

  // Track current slot
  for (int i = 0; i < inklink::SLOT_COUNT; i++) {
    if (strcmp(pendingSlot_, inklink::SLOT_NAMES[i]) == 0) { currentSlot_ = i; break; }
  }
  needsRedraw_ = false;
}

void InkLinkActivity::abortReceiving() {
  if (state_ == State::Receiving || state_ == State::Verifying) {
    HalStorage::remove(tmpPath_);
    state_         = State::Idle;
    activeClient_  = 0xFF;
    receivedBytes_ = 0;
  }
}

// ---- Frame display ---------------------------------------------------------
// File layout (written by NSImage+RawEink.swift on Mac):
//   [0 .. BUFFER_SIZE-1]          = MSB plane (bit 1 of each pixel's 2-bit gray)
//   [BUFFER_SIZE .. 2*BUFFER_SIZE-1] = LSB plane (bit 0)
// BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT = 66 * 792 = 52272 bytes
//
// GfxRenderer grayscale pipeline:
//   setRenderMode(GRAYSCALE_LSB)  → paint LSB data into frameBuffer
//   copyGrayscaleLsbBuffers()     → frameBuffer → LSB plane in HalDisplay
//   setRenderMode(GRAYSCALE_MSB)  → paint MSB data
//   copyGrayscaleMsbBuffers()
//   displayGrayBuffer()           → triggers EPD 4-gray waveform

void InkLinkActivity::displayFrameFromFile(GfxRenderer& renderer, const char* path) {
  const uint32_t planeSize = HalDisplay::BUFFER_SIZE;

  if (!renderer.hasFrameBuffer()) {
    Serial.println("[InkLink] No frame buffer available");
    return;
  }

  File f = HalStorage::open(path, FILE_READ);
  if (!f) {
    Serial.printf("[InkLink] Cannot open %s\n", path);
    return;
  }

  // --- Display BW base frame first (required by X3 hardware protocol) ---
  renderer.preconditionGrayscale();

  // --- LSB pass ---
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  f.seek(planeSize);
  // Read LSB plane into frameBuffer
  size_t lsbRead = f.read(renderer.frameBuffer, planeSize);
  if (lsbRead != planeSize) {
    Serial.printf("[InkLink] Short read LSB: %zu/%u\n", lsbRead, planeSize);
    f.close();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }
  renderer.copyGrayscaleLsbBuffers();

  // --- MSB pass: rewind and read MSB plane ---
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  f.seek(0);
  size_t msbRead = f.read(renderer.frameBuffer, planeSize);
  f.close();
  if (msbRead != planeSize) {
    Serial.printf("[InkLink] Short read MSB: %zu/%u\n", msbRead, planeSize);
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }
  renderer.copyGrayscaleMsbBuffers();

  // --- Trigger EPD refresh ---
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();

  Serial.printf("[InkLink] Frame displayed: %s\n", path);
}

void InkLinkActivity::loadSlotByIndex(GfxRenderer& renderer, int index) {
  if (index < 0 || index >= inklink::SLOT_COUNT) return;
  char path[64];
  snprintf(path, sizeof(path), "%s/slot_%s.bin",
           inklink::SLOT_DIR, inklink::SLOT_NAMES[index]);

  if (!HalStorage::exists(path)) {
    Serial.printf("[InkLink] Slot '%s' empty\n", inklink::SLOT_NAMES[index]);
    return;
  }

  state_ = State::SlotLoading;
  currentSlot_ = index;
  displayFrameFromFile(renderer, path);
  state_ = State::Idle;
  needsRedraw_ = false;
}

// ---- Status screen ---------------------------------------------------------

void InkLinkActivity::drawStatusScreen(GfxRenderer& renderer) {
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();

  const int sw = renderer.getDisplayWidth();
  const int sh = renderer.getDisplayHeight();
  const int cy = sh / 2;

  // Title
  renderer.drawCenteredText(UI_14_FONT_ID, cy - 40, "InkLink");

  // WebSocket URL
  char wsUrl[64];
  snprintf(wsUrl, sizeof(wsUrl), "ws://%s:%u",
           WiFi.localIP().toString().c_str(), inklink::WS_PORT);
  renderer.drawCenteredText(UI_12_FONT_ID, cy, wsUrl);

  // Hint
  renderer.drawCenteredText(UI_12_FONT_ID, cy + 30,
                             "Waiting for ink-Macintosh…", false);

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
