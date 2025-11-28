#pragma once

#include "../BaseSerialInterface.h"
#include <bluefruit.h>

#ifndef BLE_TX_POWER
#define BLE_TX_POWER 4
#endif

class SerialBLEInterface : public BaseSerialInterface {
  BLEUart bleuart;
  bool _isEnabled;
  bool _isDeviceConnected;  // Only true after security established (onSecured)

  struct Frame {
    uint8_t len;
    uint8_t retry_count;
    uint8_t buf[MAX_FRAME_SIZE];
  };

  #define FRAME_QUEUE_SIZE  8
  #define MAX_WRITE_RETRIES 3
  
  // Lock-free ring buffers - volatile for ISR safety
  volatile uint8_t send_queue_head;  // Write index (main thread)
  volatile uint8_t send_queue_tail;  // Read index (main thread, but ISR-safe reads)
  Frame send_queue[FRAME_QUEUE_SIZE];
  
  volatile uint8_t recv_queue_head;  // Write index (ISR)
  volatile uint8_t recv_queue_tail;  // Read index (main thread)
  Frame recv_queue[FRAME_QUEUE_SIZE];
  
  // Helper to get queue size without critical section
  static inline uint8_t getQueueSize(uint8_t head, uint8_t tail, uint8_t size) {
    if (head >= tail) {
      return head - tail;
    } else {
      return size - tail + head;
    }
  }

  void clearBuffers();
  static void onConnect(uint16_t connection_handle);
  static void onDisconnect(uint16_t connection_handle, uint8_t reason);
  static void onSecured(uint16_t connection_handle);
  static bool onPairingPasskey(uint16_t connection_handle, uint8_t const passkey[6], bool match_request);
  static void onPairingComplete(uint16_t connection_handle, uint8_t auth_status);
  static void onBLEEvent(ble_evt_t* evt);
  static void onBleUartRX(uint16_t conn_handle);

public:
  SerialBLEInterface() {
    _isEnabled = false;
    _isDeviceConnected = false;
    send_queue_head = 0;
    send_queue_tail = 0;
    recv_queue_head = 0;
    recv_queue_tail = 0;
  }

  void begin(const char* device_name, uint32_t pin_code);
  void disconnect();
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
