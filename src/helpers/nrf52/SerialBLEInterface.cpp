#include "SerialBLEInterface.h"
#include <string.h>
#include "ble_gap.h"

static SerialBLEInterface* instance;

void SerialBLEInterface::onConnect(uint16_t connection_handle) {
  (void)connection_handle;  // Unused - we only support one connection
  BLE_DEBUG_PRINTLN("SerialBLEInterface: connected handle=0x%04X", connection_handle);
  if (instance) {
    instance->_isDeviceConnected = false;
    instance->clearBuffers();
  }
}

// Callback invoked when BLE connection is terminated
// Clears connection state
void SerialBLEInterface::onDisconnect(uint16_t connection_handle, uint8_t reason) {
  (void)connection_handle;  // Unused - we only support one connection
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disconnected handle=0x%04X reason=%d", connection_handle, reason);
  if(instance){
    instance->_isDeviceConnected = false;
    instance->clearBuffers();
  }
}

// Callback invoked when BLE connection security is established
// Marks device as fully connected and requests optimal connection parameters
void SerialBLEInterface::onSecured(uint16_t connection_handle) {
  (void)connection_handle;  // Unused - we only support one connection
  BLE_DEBUG_PRINTLN("SerialBLEInterface: onSecured");
  if(instance){
    instance->_isDeviceConnected = true;
    
    // Request connection parameter update with Apple-compliant values
    // Min: 15ms (12 × 1.25ms), Max: 30ms (24 × 1.25ms), Latency: 0, Timeout: 2s (200 × 10ms)
    ble_gap_conn_params_t conn_params;
    conn_params.min_conn_interval = 12;   // 15ms
    conn_params.max_conn_interval = 24;   // 30ms
    conn_params.slave_latency = 0;
    conn_params.conn_sup_timeout = 200;   // 2 seconds (Apple minimum recommendation)
    
    uint32_t err_code = sd_ble_gap_conn_param_update(0x0000, &conn_params);
    if (err_code == NRF_SUCCESS) {
      BLE_DEBUG_PRINTLN("Connection parameter update requested: 15-30ms interval, 2s timeout");
    } else {
      BLE_DEBUG_PRINTLN("Failed to request connection parameter update: %lu", err_code);
    }
  }
}

// Callback for BLE pairing passkey display/verification
// Returns true to accept pairing request
bool SerialBLEInterface::onPairingPasskey(uint16_t connection_handle, uint8_t const passkey[6], bool match_request) {
  (void)connection_handle;  // Unused - we only support one connection
  (void)passkey;  // Unused
  BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing passkey request match=%d", match_request);
  return true;
}

// Callback invoked when BLE pairing process completes
// Disconnects if pairing failed, otherwise connection proceeds to secured state
void SerialBLEInterface::onPairingComplete(uint16_t connection_handle, uint8_t auth_status) {
  (void)connection_handle;  // Unused - we only support one connection
  BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing complete status=%d", auth_status);
  if (auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
    BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing successful");
  } else {
    BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing failed, disconnecting");
    if (instance) {
      instance->disconnect();
    }
  }
}

// BLE event handler - handles iOS connection parameter update requests
void SerialBLEInterface::onBLEEvent(ble_evt_t* evt) {
  if (!instance) return;
  
  if (evt->header.evt_id == BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST) {
    // iOS 13+ sends this during reconnection - we MUST respond or SoftDevice will assert/crash
    uint16_t conn_handle = evt->evt.gap_evt.conn_handle;
    BLE_DEBUG_PRINTLN("CONN_PARAM_UPDATE_REQUEST: handle=0x%04X, min_interval=%d, max_interval=%d, latency=%d, timeout=%d",
                     conn_handle,
                     evt->evt.gap_evt.params.conn_param_update_request.conn_params.min_conn_interval,
                     evt->evt.gap_evt.params.conn_param_update_request.conn_params.max_conn_interval,
                     evt->evt.gap_evt.params.conn_param_update_request.conn_params.slave_latency,
                     evt->evt.gap_evt.params.conn_param_update_request.conn_params.conn_sup_timeout);
    
    // Accept iOS's requested parameters by calling with NULL (uses PPCP from GAP service)
    uint32_t err_code = sd_ble_gap_conn_param_update(conn_handle, NULL);
    if (err_code == NRF_SUCCESS) {
      BLE_DEBUG_PRINTLN("Accepted CONN_PARAM_UPDATE_REQUEST (using PPCP)");
    } else {
      BLE_DEBUG_PRINTLN("ERROR: Failed to accept CONN_PARAM_UPDATE_REQUEST: 0x%08X", err_code);
    }
  }
}

// Initialize BLE stack with device name and PIN code
// Configures security, advertising, and registers all callbacks
void SerialBLEInterface::begin(const char* device_name, uint32_t pin_code) {

  instance = this;

  char charpin[20];
  sprintf(charpin, "%d", pin_code);

  // If we want to control BLE LED ourselves, uncomment this:
  // Bluefruit.autoConnLed(false);
  
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  
  // Set Peripheral Preferred Connection Parameters (PPCP) for iOS compatibility
  // iOS reads these during connection and may use them
  // Min: 15ms (12 × 1.25ms), Max: 30ms (24 × 1.25ms), Latency: 0, Timeout: 2s (200 × 10ms)
  ble_gap_conn_params_t ppcp_params;
  ppcp_params.min_conn_interval = 12;   // 15ms
  ppcp_params.max_conn_interval = 24;   // 30ms
  ppcp_params.slave_latency = 0;
  ppcp_params.conn_sup_timeout = 200;   // 2 seconds (Apple minimum recommendation)
  
  uint32_t err_code = sd_ble_gap_ppcp_set(&ppcp_params);
  if (err_code == NRF_SUCCESS) {
    BLE_DEBUG_PRINTLN("PPCP set: 15-30ms interval, 2s timeout");
  } else {
    BLE_DEBUG_PRINTLN("Failed to set PPCP: %lu", err_code);
  }
  
  Bluefruit.setTxPower(BLE_TX_POWER);
  Bluefruit.setName(device_name);

  Bluefruit.Security.setMITM(true);
  Bluefruit.Security.setPIN(charpin);
  Bluefruit.Security.setIOCaps(true, false, false);
  Bluefruit.Security.setPairPasskeyCallback(onPairingPasskey);
  Bluefruit.Security.setPairCompleteCallback(onPairingComplete);

  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);
  Bluefruit.Security.setSecuredCallback(onSecured);

  Bluefruit.setEventCallback(onBLEEvent);

  bleuart.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
  bleuart.begin();

  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();

  Bluefruit.Advertising.addService(bleuart);

  Bluefruit.ScanResponse.addName();

  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);

  Bluefruit.Advertising.restartOnDisconnect(true);

}

// Enable interface and start advertising
void SerialBLEInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  Bluefruit.Advertising.start(0);
}

// Disconnect active BLE connection (asynchronous - onDisconnect callback handles cleanup)
void SerialBLEInterface::disconnect() {
  if (Bluefruit.connected() > 0) {
    Bluefruit.disconnect(0);
  }
}

// Disable interface, disconnect connections, and stop advertising
void SerialBLEInterface::disable() {
  _isEnabled = false;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disable");

  disconnect();
  Bluefruit.Advertising.stop();
}

// Queue frame for transmission over BLE
// Returns frame length if queued successfully, 0 if queue is full or not connected
size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%d", len);
    return 0;
  }

  if (isConnected() && len > 0) {
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}

// Process received frames, handle outgoing queue, and manage connection state
// Returns length of received frame, or 0 if no frame available
size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  if (send_queue_len > 0) {
    if (_isDeviceConnected && Bluefruit.connected() > 0) {
      size_t written = bleuart.write(send_queue[0].buf, send_queue[0].len);
      if (written > 0) {
        BLE_DEBUG_PRINTLN("writeBytes: sz=%d, hdr=%d",
                         (uint32_t)send_queue[0].len, (uint32_t)send_queue[0].buf[0]);

        send_queue_len--;
        // Shift remaining frames down using memmove (handles overlapping memory correctly)
        if (send_queue_len > 0) {
          memmove(&send_queue[0], &send_queue[1], send_queue_len * sizeof(Frame));
        }
      } else {
        // Write failed - keep frame in queue and try again next time
        BLE_DEBUG_PRINTLN("writeBytes failed, keeping frame in queue");
      }
    }
  } else {
    if (_isDeviceConnected) {
      int avail = bleuart.available();
      if (avail > 0) {
        int got = bleuart.readBytes(dest, avail > MAX_FRAME_SIZE ? MAX_FRAME_SIZE : avail);
        BLE_DEBUG_PRINTLN("readBytes: sz=%d, hdr=%d", got, (uint32_t) dest[0]);
        return got;
      }
    }
  }
  return 0;
}

// Check if device is connected by verifying connection state
bool SerialBLEInterface::isConnected() const {
  if (!_isDeviceConnected) return false;
  return Bluefruit.connected() > 0;
}

// Check if write queue is at capacity
// Returns true when queue is full, providing backpressure to callers
bool SerialBLEInterface::isWriteBusy() const {
  return send_queue_len >= FRAME_QUEUE_SIZE;
}
