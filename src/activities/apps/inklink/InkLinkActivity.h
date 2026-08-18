#pragma once

#include "../../Activity.h"
#include <HalDisplay.h>
#include <WebSocketsServer.h>

// InkLink: silent LAN push for ink-Macintosh
// Mac sends JSON header + 104544-byte raw 2-plane frame over WebSocket.
// X3 streams to SD, verifies CRC32, and calls EPD 4-gray refresh with no UI.

namespace inklink {

static constexpr uint16_t WS_PORT = 8124;
// Frame = MSB plane + LSB plane, each = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT
static constexpr uint32_t FRAME_BYTES = 2UL * HalDisplay::BUFFER_SIZE;

static constexpr const char* SLOT_DIR = "/inklink";
static constexpr int SLOT_COUNT = 4;
static constexpr const char* SLOT_NAMES[SLOT_COUNT] = {"desk", "home", "brief", "focus"};

}  // namespace inklink

class InkLinkActivity final : public Activity {
 public:
  explicit InkLinkActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~InkLinkActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class State {
    Idle,
    Receiving,
    Verifying,
    Displaying,
    SlotLoading,
  };

  static void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static InkLinkActivity* sInstance;

  void handleWsText(uint8_t num, const uint8_t* payload, size_t length);
  void handleWsBinary(uint8_t num, const uint8_t* payload, size_t length);
  void handleWsDisconnect(uint8_t num);

  void startReceiving(uint8_t num, const char* slot, uint32_t expectedSize, uint32_t expectedCrc);
  void finishReceiving();
  void abortReceiving();
  void displayFrameFromFile(const char* path);
  void loadSlotByIndex(int index);
  void drawStatusScreen();

  WebSocketsServer* wsServer_ = nullptr;
  State state_ = State::Idle;

  uint8_t  activeClient_  = 0xFF;
  char     pendingSlot_[32] = {};
  uint32_t pendingSize_   = 0;
  uint32_t pendingCrc_    = 0;
  uint32_t receivedBytes_ = 0;

  char tmpPath_[64]  = {};
  char slotPath_[64] = {};

  int  currentSlot_ = -1;
  bool needsRedraw_  = true;
  bool serverStarted_ = false;
};
