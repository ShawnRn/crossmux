#include "InkLinkActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"

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
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return 0;
  uint32_t crc = 0xFFFFFFFFu;
  uint8_t buf[512];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; i++) crc = crc32Table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  }
  f.close();
  return crc ^ 0xFFFFFFFFu;
}

// ---- Static member ---------------------------------------------------------

InkLinkActivity* InkLinkActivity::sInstance = nullptr;

// ---- Construction / destruction --------------------------------------------

InkLinkActivity::InkLinkActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("InkLink", renderer, mappedInput) {
  sInstance = this;
}

InkLinkActivity::~InkLinkActivity() {
  teardownWifi();
  sInstance = nullptr;
}

// ---- Lifecycle -------------------------------------------------------------

void InkLinkActivity::onEnter() {
  Activity::onEnter();
  Storage.mkdir(inklink::SLOT_DIR, true);
  snprintf(tmpPath_, sizeof(tmpPath_), "%s/_incoming.tmp", inklink::SLOT_DIR);

  state_ = State::Idle;
  needsRedraw_ = true;

  if (WiFi.status() == WL_CONNECTED) {
    wifiState_ = WifiState::Connected;
    startWsServer();
  } else {
    if (startSavedWifiAssociation()) {
      wifiState_ = WifiState::Connecting;
      wifiConnectStartMs_ = millis();
    } else {
      wifiState_ = WifiState::Disconnected;
      openWifiSelection();
    }
  }

  requestUpdate();
}

void InkLinkActivity::onExit() {
  teardownWifi();
  Activity::onExit();
}

void InkLinkActivity::loop() {
  if (wifiState_ == WifiState::Connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiState_ = WifiState::Connected;
      startWsServer();
      needsRedraw_ = true;
      requestUpdate();
    } else if (millis() - wifiConnectStartMs_ > 15000) {
      wifiState_ = WifiState::Failed;
      needsRedraw_ = true;
      requestUpdate();
    }
  }

  if (wsServer_) wsServer_->loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (wifiState_ != WifiState::Connected) {
      openWifiSelection();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::ScreenLeft)) {
    loadSlotByIndex((currentSlot_ - 1 + inklink::SLOT_COUNT) % inklink::SLOT_COUNT);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
             mappedInput.wasReleased(MappedInputManager::Button::ScreenRight)) {
    loadSlotByIndex((currentSlot_ + 1) % inklink::SLOT_COUNT);
  }

  // Transition: binary reception complete -> verify
  if (state_ == State::Verifying) {
    finishReceiving();
  }
}

void InkLinkActivity::render(RenderLock&&) {
  if (!needsRedraw_) return;
  needsRedraw_ = false;

  char defaultSlotPath[64];
  snprintf(defaultSlotPath, sizeof(defaultSlotPath), "%s/slot_%s.bin",
           inklink::SLOT_DIR, inklink::SLOT_NAMES[0]);
  if (currentSlot_ < 0 && Storage.exists(defaultSlotPath)) {
    currentSlot_ = 0;
    displayFrameFromFile(defaultSlotPath);
    return;
  }

  drawStatusScreen();
}

// ---- WiFi & Server management ----------------------------------------------

bool InkLinkActivity::startSavedWifiAssociation() {
  if (WiFi.status() == WL_CONNECTED) return true;

  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) return false;

  const auto credential = WIFI_STORE.findCredential(lastSsid);
  if (!credential) return false;

  WiFi.persistent(false);
  NetworkStartup::setMode(renderer, WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);
  if (credential->password.empty()) {
    WiFi.begin(credential->ssid.c_str());
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }
  broughtWifiUp_ = true;
  return true;
}

void InkLinkActivity::openWifiSelection() {
  if (!startActivityForResultWith<WifiSelectionActivity>([this](const ActivityResult& result) {
        handleWifiResult(result);
      })) {
    wifiState_ = WifiState::Failed;
    needsRedraw_ = true;
    requestUpdate();
  }
}

void InkLinkActivity::handleWifiResult(const ActivityResult& result) {
  if (!result.isCancelled && WiFi.status() == WL_CONNECTED) {
    wifiState_ = WifiState::Connected;
    broughtWifiUp_ = true;
    startWsServer();
  } else {
    wifiState_ = WifiState::Failed;
  }
  needsRedraw_ = true;
  requestUpdate();
}

void InkLinkActivity::startWsServer() {
  if (!wsServer_) {
    wsServer_ = new WebSocketsServer(inklink::WS_PORT);
    wsServer_->begin();
    wsServer_->onEvent(InkLinkActivity::onWsEvent);
    serverStarted_ = true;
    LOG_INF("InkLink", "WS server started on :%u, IP: %s",
            inklink::WS_PORT, WiFi.localIP().toString().c_str());
  }
}

void InkLinkActivity::stopWsServer() {
  if (wsServer_) {
    wsServer_->close();
    delete wsServer_;
    wsServer_ = nullptr;
    serverStarted_ = false;
  }
}

void InkLinkActivity::teardownWifi() {
  stopWsServer();
  if (broughtWifiUp_) {
    WiFi.disconnect(false);
    delay(50);
    WiFi.mode(WIFI_OFF);
    esp_wifi_deinit();
    broughtWifiUp_ = false;
  }
}

// ---- WS trampoline ---------------------------------------------------------

void InkLinkActivity::onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (!sInstance) return;
  switch (type) {
    case WStype_CONNECTED:
      LOG_INF("InkLink", "Client %u connected", num);
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
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    if (wsServer_) wsServer_->sendTXT(num, "{\"error\":\"bad_json\"}");
    return;
  }

  const char* slot = doc["slot"] | "desk";
  uint32_t size = doc["size"] | 0u;
  uint32_t crcExpected = doc["crc32"] | 0u;

  if (size != static_cast<uint32_t>(inklink::FRAME_BYTES)) {
    if (wsServer_) wsServer_->sendTXT(num, "{\"error\":\"bad_size\"}");
    return;
  }

  startReceiving(num, slot, size, crcExpected);
  if (wsServer_) wsServer_->sendTXT(num, "{\"ok\":\"ready\"}");
}

void InkLinkActivity::handleWsBinary(uint8_t num, const uint8_t* payload, size_t length) {
  if (state_ != State::Receiving || num != activeClient_) return;

  HalFile f = Storage.open(tmpPath_, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    abortReceiving();
    if (wsServer_) wsServer_->sendTXT(num, "{\"error\":\"sd_write\"}");
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
  Storage.remove(tmpPath_);
  state_ = State::Receiving;
  LOG_INF("InkLink", "Recv start: slot=%s size=%u", pendingSlot_, size);
}

void InkLinkActivity::finishReceiving() {
  state_ = State::Idle;  // set early to prevent re-entry

  // Verify CRC32
  uint32_t actual = crc32File(tmpPath_);
  if (pendingCrc_ != 0 && actual != pendingCrc_) {
    LOG_ERR("InkLink", "CRC mismatch: got=%08X want=%08X", actual, pendingCrc_);
    if (wsServer_) wsServer_->sendTXT(activeClient_, "{\"error\":\"crc_mismatch\"}");
    Storage.remove(tmpPath_);
    return;
  }

  // Promote tmp -> slot file
  Storage.remove(slotPath_);
  Storage.rename(tmpPath_, slotPath_);

  // Display
  state_ = State::Displaying;
  displayFrameFromFile(slotPath_);
  state_ = State::Idle;

  if (wsServer_) wsServer_->sendTXT(activeClient_, "{\"ok\":\"displayed\"}");
  activeClient_ = 0xFF;

  // Track current slot
  for (int i = 0; i < inklink::SLOT_COUNT; i++) {
    if (strcmp(pendingSlot_, inklink::SLOT_NAMES[i]) == 0) { currentSlot_ = i; break; }
  }
  needsRedraw_ = false;
}

void InkLinkActivity::abortReceiving() {
  if (state_ == State::Receiving || state_ == State::Verifying) {
    Storage.remove(tmpPath_);
    state_         = State::Idle;
    activeClient_  = 0xFF;
    receivedBytes_ = 0;
  }
}

// ---- Frame display ---------------------------------------------------------

void InkLinkActivity::displayFrameFromFile(const char* path) {
  const uint32_t planeSize = HalDisplay::BUFFER_SIZE;

  if (!renderer.hasFrameBuffer()) {
    LOG_ERR("InkLink", "No frame buffer available");
    return;
  }

  uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) {
    LOG_ERR("InkLink", "Frame buffer pointer is null");
    return;
  }

  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) {
    LOG_ERR("InkLink", "Cannot open %s", path);
    return;
  }

  // --- Display BW base frame first (required by X3 hardware protocol) ---
  renderer.preconditionGrayscale();

  // --- LSB pass ---
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  f.seek(planeSize);
  int lsbRead = f.read(fb, planeSize);
  if (lsbRead != static_cast<int>(planeSize)) {
    LOG_ERR("InkLink", "Short read LSB: %d/%u", lsbRead, planeSize);
    f.close();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }
  renderer.copyGrayscaleLsbBuffers();

  // --- MSB pass: rewind and read MSB plane ---
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  f.seek(0);
  int msbRead = f.read(fb, planeSize);
  f.close();
  if (msbRead != static_cast<int>(planeSize)) {
    LOG_ERR("InkLink", "Short read MSB: %d/%u", msbRead, planeSize);
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }
  renderer.copyGrayscaleMsbBuffers();

  // --- Trigger EPD refresh ---
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  renderer.cleanupGrayscaleWithFrameBuffer();

  LOG_INF("InkLink", "Frame displayed: %s", path);
}

void InkLinkActivity::loadSlotByIndex(int index) {
  if (index < 0 || index >= inklink::SLOT_COUNT) return;
  char path[64];
  snprintf(path, sizeof(path), "%s/slot_%s.bin",
           inklink::SLOT_DIR, inklink::SLOT_NAMES[index]);

  if (!Storage.exists(path)) {
    LOG_INF("InkLink", "Slot '%s' empty", inklink::SLOT_NAMES[index]);
    return;
  }

  state_ = State::SlotLoading;
  currentSlot_ = index;
  displayFrameFromFile(path);
  state_ = State::Idle;
  needsRedraw_ = false;
}

// ---- Status screen ---------------------------------------------------------

void InkLinkActivity::drawStatusScreen() {
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();

  const int sh = renderer.getDisplayHeight();
  const int cy = sh / 2;

  // Title
  renderer.drawCenteredText(UI_12_FONT_ID, cy - 50, "InkLink", true, EpdFontFamily::BOLD);

  if (wifiState_ == WifiState::Connected) {
    // WebSocket URL
    char wsUrl[64];
    snprintf(wsUrl, sizeof(wsUrl), "ws://%s:%u",
             WiFi.localIP().toString().c_str(), inklink::WS_PORT);
    renderer.drawCenteredText(UI_12_FONT_ID, cy - 10, wsUrl, true);

    // Current slot & hint
    char slotHint[64];
    const char* slotName = (currentSlot_ >= 0 && currentSlot_ < inklink::SLOT_COUNT)
                               ? inklink::SLOT_NAMES[currentSlot_]
                               : inklink::SLOT_NAMES[0];
    snprintf(slotHint, sizeof(slotHint), "Slot: %s  (< / > to switch)", slotName);
    renderer.drawCenteredText(UI_10_FONT_ID, cy + 20, slotHint, true);

    renderer.drawCenteredText(UI_10_FONT_ID, cy + 50,
                              "Waiting for ink-Macintosh...", true);
  } else if (wifiState_ == WifiState::Connecting) {
    renderer.drawCenteredText(UI_12_FONT_ID, cy, "Connecting to WiFi...", true);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, cy - 10, "WiFi Disconnected", true);
    renderer.drawCenteredText(UI_10_FONT_ID, cy + 20, "Press Confirm to Select WiFi", true);
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
