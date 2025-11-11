#pragma once

#include "../BaseSerialInterface.h"
#include <bluefruit.h>

#ifndef BLE_TX_POWER
#define BLE_TX_POWER 4
#endif

class SerialBLEInterface : public BaseSerialInterface {
  BLEUart bleuart;
  bool _isEnabled;
  bool _isDeviceConnected;
  unsigned long _last_write;
  uint16_t _connHandle;

  struct Frame {
    uint16_t len;
    uint16_t pos;
    uint8_t buf[MAX_FRAME_SIZE];
  };

  #define FRAME_QUEUE_SIZE  2
  int send_queue_len;
  Frame send_queue[FRAME_QUEUE_SIZE];

  void clearBuffers() {
    send_queue_len = 0;
    for (int i = 0; i < FRAME_QUEUE_SIZE; ++i) {
      send_queue[i].len = 0;
      send_queue[i].pos = 0;
    }
  }
  static void onConnect(uint16_t connection_handle);
  static void onDisconnect(uint16_t connection_handle, uint8_t reason);
  static void onSecured(uint16_t connection_handle);
  uint32_t computeWriteInterval() const;

public:
  SerialBLEInterface() {
    _isEnabled = false;
    _isDeviceConnected = false;
    _last_write = 0;
    _connHandle = BLE_CONN_HANDLE_INVALID;
    send_queue_len = 0;
  }

  void startAdv();
  void stopAdv();
  void begin(const char* device_name, uint32_t pin_code);

  // BaseSerialInterface methods
  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _isEnabled; }

  bool isConnected() const override;

  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;
};

#if BLE_DEBUG_LOGGING && ARDUINO
  #include <Arduino.h>
  #define BLE_DEBUG_PRINT(F, ...) Serial.printf("BLE: " F, ##__VA_ARGS__)
  #define BLE_DEBUG_PRINTLN(F, ...) Serial.printf("BLE: " F "\n", ##__VA_ARGS__)
#else
  #define BLE_DEBUG_PRINT(...) {}
  #define BLE_DEBUG_PRINTLN(...) {}
#endif
