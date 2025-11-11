#include "SerialBLEInterface.h"
#include <ble_hci.h>

static SerialBLEInterface* instance;

constexpr size_t kMaxBleChunkSize = 96;
constexpr uint32_t kWriteIntervalNormalMs = 80;
constexpr uint32_t kWriteIntervalWeakMs = 140;
constexpr uint32_t kWriteIntervalCriticalMs = 220;

void SerialBLEInterface::onConnect(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: connected");
  if (instance) {
    instance->_connHandle = connection_handle;
  }
}

void SerialBLEInterface::onDisconnect(uint16_t connection_handle, uint8_t reason) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disconnected reason=0x%02X", reason);
  (void)connection_handle;
  if (instance){
    instance->_isDeviceConnected = false;
    instance->_connHandle = BLE_CONN_HANDLE_INVALID;
    instance->_last_write = 0;
    instance->clearBuffers();
    // Advertising auto-restarts via restartOnDisconnect(true). If that fails,
    // the main loop will kick it off again on the next enable().
  }
}

void SerialBLEInterface::onSecured(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: onSecured");
  if(instance){
    instance->_isDeviceConnected = true;
    instance->_connHandle = connection_handle;
    // no need to stop advertising on connect, as the ble stack does this automatically
  }
}

void SerialBLEInterface::begin(const char* device_name, uint32_t pin_code) {

  instance = this;

  char charpin[20];
  sprintf(charpin, "%d", pin_code);

  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.configPrphConn(128, BLE_GAP_EVENT_LENGTH_MIN, 8, 8);
  Bluefruit.setTxPower(BLE_TX_POWER);
  Bluefruit.begin();
  Bluefruit.setName(device_name);

  Bluefruit.Security.setMITM(true);
  Bluefruit.Security.setPIN(charpin);

  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);
  Bluefruit.Security.setSecuredCallback(onSecured);

  // To be consistent OTA DFU should be added first if it exists
  //bledfu.begin();

  // Configure and start the BLE Uart service
  bleuart.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
  bleuart.begin();
  
}

void SerialBLEInterface::startAdv() {

  BLE_DEBUG_PRINTLN("SerialBLEInterface: starting advertising");
  
  // clean restart if already advertising
  if (Bluefruit.Advertising.isRunning()) {
    BLE_DEBUG_PRINTLN("SerialBLEInterface: already advertising, stopping to allow clean restart");
    Bluefruit.Advertising.stop();
  }

  Bluefruit.Advertising.clearData(); // clear advertising data
  Bluefruit.ScanResponse.clearData(); // clear scan response data

  // Advertising packet
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();

  // Include the BLE UART (AKA 'NUS') 128-bit UUID
  Bluefruit.Advertising.addService(bleuart);

  // Secondary Scan Response packet (optional)
  // Since there is no room for 'Name' in Advertising packet
  Bluefruit.ScanResponse.addName();

  /* Start Advertising
   * - Enable auto advertising if disconnected
   * - Interval:  fast mode = 20 ms, slow mode = 152.5 ms
   * - Timeout for fast mode is 30 seconds
   * - Start(timeout) with timeout = 0 will advertise forever (until connected)
   *
   * For recommended advertising interval
   * https://developer.apple.com/library/content/qa/qa1931/_index.html
   */
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);      // number of seconds in fast mode
  Bluefruit.Advertising.start(0);                // 0 = Don't stop advertising after n seconds
}

void SerialBLEInterface::stopAdv() {

  BLE_DEBUG_PRINTLN("SerialBLEInterface: stopping advertising");
  
  // we only want to stop advertising if it's running, otherwise an invalid state error is logged by ble stack
  if(!Bluefruit.Advertising.isRunning()){
    return;
  }

  // stop advertising
  Bluefruit.Advertising.stop();

}

// ---------- public methods

void SerialBLEInterface::enable() { 
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();
  _connHandle = BLE_CONN_HANDLE_INVALID;
  _last_write = 0;

  // Start advertising
  startAdv();
}

void SerialBLEInterface::disable() {
  _isEnabled = false;
  BLE_DEBUG_PRINTLN("SerialBLEInterface::disable");

#ifdef RAK_BOARD
  Bluefruit.disconnect(Bluefruit.connHandle());
#else
  uint16_t conn_id;
  if (Bluefruit.getConnectedHandles(&conn_id, 1) > 0) {
    Bluefruit.disconnect(conn_id);
  }
#endif

  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();

  _connHandle = BLE_CONN_HANDLE_INVALID;
  _isDeviceConnected = false;
  _last_write = 0;
  clearBuffers();

  stopAdv();
}

size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%d", len);
    return 0;
  }

  if (_isDeviceConnected && len > 0) {
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    Frame& slot = send_queue[send_queue_len];
    slot.len = len;          // add to send queue
    slot.pos = 0;
    memcpy(slot.buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}

bool SerialBLEInterface::isWriteBusy() const {
  return millis() < _last_write + computeWriteInterval();   // still too soon to start another write?
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  unsigned long now = millis();
  if (send_queue_len > 0) {   // first, check send queue
    uint32_t interval = computeWriteInterval();
    if (now >= _last_write + interval) {    // space the writes apart
      Frame& frame = send_queue[0];
      size_t remaining = frame.len - frame.pos;
      size_t chunk = remaining > kMaxBleChunkSize ? kMaxBleChunkSize : remaining;
      if (chunk > 0) {
        _last_write = now;
        bleuart.write(frame.buf + frame.pos, chunk);
        BLE_DEBUG_PRINTLN("writeBytes: sz=%d/%d hdr=%d", (uint32_t)chunk, (uint32_t)frame.len, (uint32_t) frame.buf[0]);
        frame.pos += chunk;
        if (frame.pos >= frame.len) {
          send_queue_len--;
          for (int i = 0; i < send_queue_len; i++) {   // delete top item from queue
            send_queue[i] = send_queue[i + 1];
          }
        }
      }
    }
  } else {
    int len = bleuart.available();
    if (len > 0) {
      bleuart.readBytes(dest, len);
      BLE_DEBUG_PRINTLN("readBytes: sz=%d, hdr=%d", len, (uint32_t) dest[0]);
      return len;
    }
  }
  return 0;
}

bool SerialBLEInterface::isConnected() const {
  return _isDeviceConnected;
}

uint32_t SerialBLEInterface::computeWriteInterval() const {
  if (!_isDeviceConnected) {
    return kWriteIntervalNormalMs;
  }

  if (send_queue_len == 0) {
    return kWriteIntervalNormalMs;
  }

  const Frame& frame = send_queue[0];
  if (send_queue_len > 1) {
    return kWriteIntervalCriticalMs;
  }

  if (frame.pos > 0) {
    return kWriteIntervalWeakMs;
  }

  if (frame.len > kMaxBleChunkSize) {
    return kWriteIntervalWeakMs;
  }

  return kWriteIntervalNormalMs;
}
