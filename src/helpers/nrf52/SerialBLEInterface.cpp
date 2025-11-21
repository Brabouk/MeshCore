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
    instance->stopAdv();
    
    // Cancel any pending advertising restart since we're now connected
    instance->_advRestartPending = false;
    instance->_advRestartTime = 0;
  }
}

// Callback invoked when BLE connection is terminated
// Clears connection state and drains remaining RX buffer data
void SerialBLEInterface::onDisconnect(uint16_t connection_handle, uint8_t reason) {
  (void)connection_handle;  // Unused - we only support one connection
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disconnected handle=0x%04X reason=%d", connection_handle, reason);
  if(instance){
    instance->_isDeviceConnected = false;
    
    // Clear any stuck pending writes (TX completions won't come after disconnect)
    if (instance->_pending_writes > 0) {
      BLE_DEBUG_PRINTLN("Clearing stuck _pending_writes=%d on disconnect", instance->_pending_writes);
      instance->_pending_writes = 0;
    }
    
    instance->clearBuffers();
    
    // Delay advertising restart to respect grace period
    if (instance->_isEnabled) {
      instance->_advRestartPending = true;
      instance->_advRestartTime = millis();
    }
    
    // Drain any remaining data in BLE UART RX buffer to start with clean slate
    uint8_t discard[32];
    int drained_total = 0;
    while (instance->bleuart.available() > 0) {
      int chunk = instance->bleuart.available() < (int)sizeof(discard) ? instance->bleuart.available() : (int)sizeof(discard);
      int drained = instance->bleuart.readBytes(discard, chunk);
      if (drained <= 0) break;
      drained_total += drained;
    }
    if (drained_total > 0) {
      BLE_DEBUG_PRINTLN("Drained %d bytes from BLE RX buffer on disconnect", drained_total);
    }
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

// BLE event handler - only handles TX completion events to track pending writes
// This runs in BLE SoftDevice event context, which on nRF52 is typically cooperative
// (doesn't preempt main loop). The _pending_writes decrement is not atomic, but safe
// because main loop checks this value via isWriteBusy() which only reads (no RMW).
// If porting to preemptive RTOS, protect _pending_writes with mutex or use atomic ops.
void SerialBLEInterface::onBLEEvent(ble_evt_t* evt) {
  if (!instance) return;
  
  // Extract connection handle based on event type
  uint16_t conn_handle = 0xFFFF;
  if (evt->header.evt_id == BLE_GATTS_EVT_HVN_TX_COMPLETE) {
    conn_handle = evt->evt.gatts_evt.conn_handle;
  } else if (evt->header.evt_id >= BLE_GAP_EVT_BASE) {
    conn_handle = evt->evt.gap_evt.conn_handle;
  }
  
  switch (evt->header.evt_id) {
    case BLE_GATTS_EVT_HVN_TX_COMPLETE: {
      if (instance->_pending_writes > 0) {
        uint8_t completed = evt->evt.gatts_evt.params.hvn_tx_complete.count;
        // Read-modify-write: not atomic, but safe in cooperative BLE event context
        if (instance->_pending_writes >= completed) {
          instance->_pending_writes -= completed;
        } else {
          instance->_pending_writes = 0;
        }
        BLE_DEBUG_PRINTLN("TX complete: %d, pending now: %d", completed, instance->_pending_writes);
      }
      break;
    }
    
    case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST: {
      // iOS 13+ sends this during reconnection - we MUST respond or SoftDevice will assert/crash
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
      break;
    }
    
    case BLE_GAP_EVT_CONN_PARAM_UPDATE: {
      BLE_DEBUG_PRINTLN("CONN_PARAM_UPDATE: handle=0x%04X, interval=%d, latency=%d, timeout=%d",
                       conn_handle,
                       evt->evt.gap_evt.params.conn_param_update.conn_params.min_conn_interval,
                       evt->evt.gap_evt.params.conn_param_update.conn_params.slave_latency,
                       evt->evt.gap_evt.params.conn_param_update.conn_params.conn_sup_timeout);
      break;
    }
    
    case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
      // iOS may send this during reconnection
      BLE_DEBUG_PRINTLN("PHY_UPDATE_REQUEST: handle=0x%04X", conn_handle);
      
      ble_gap_phys_t phy_params;
      phy_params.tx_phys = BLE_GAP_PHY_AUTO;
      phy_params.rx_phys = BLE_GAP_PHY_AUTO;
      
      uint32_t err_code = sd_ble_gap_phy_update(conn_handle, &phy_params);
      if (err_code == NRF_SUCCESS) {
        BLE_DEBUG_PRINTLN("Accepted PHY_UPDATE_REQUEST");
      } else {
        BLE_DEBUG_PRINTLN("ERROR: Failed to accept PHY_UPDATE_REQUEST: 0x%08X", err_code);
      }
      break;
    }
    
    case BLE_GAP_EVT_PHY_UPDATE: {
      BLE_DEBUG_PRINTLN("PHY_UPDATE: handle=0x%04X, tx_phy=%d, rx_phy=%d, status=%d",
                       conn_handle,
                       evt->evt.gap_evt.params.phy_update.tx_phy,
                       evt->evt.gap_evt.params.phy_update.rx_phy,
                       evt->evt.gap_evt.params.phy_update.status);
      break;
    }
    
    case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST: {
      // iOS may send this during reconnection
      BLE_DEBUG_PRINTLN("DATA_LENGTH_UPDATE_REQUEST: handle=0x%04X", conn_handle);
      
      // Accept with AUTO (let SoftDevice choose optimal values)
      uint32_t err_code = sd_ble_gap_data_length_update(conn_handle, NULL, NULL);
      if (err_code == NRF_SUCCESS) {
        BLE_DEBUG_PRINTLN("Accepted DATA_LENGTH_UPDATE_REQUEST (AUTO)");
      } else {
        BLE_DEBUG_PRINTLN("ERROR: Failed to accept DATA_LENGTH_UPDATE_REQUEST: 0x%08X", err_code);
      }
      break;
    }
    
    case BLE_GAP_EVT_DATA_LENGTH_UPDATE: {
      BLE_DEBUG_PRINTLN("DATA_LENGTH_UPDATE: handle=0x%04X, max_tx_octets=%d, max_rx_octets=%d",
                       conn_handle,
                       evt->evt.gap_evt.params.data_length_update.effective_params.max_tx_octets,
                       evt->evt.gap_evt.params.data_length_update.effective_params.max_rx_octets);
      break;
    }
    
    case BLE_GAP_EVT_TIMEOUT:
    case BLE_GATTS_EVT_TIMEOUT: {
      BLE_DEBUG_PRINTLN("TIMEOUT: handle=0x%04X, src=%d", conn_handle, evt->header.evt_id);
      break;
    }
    
    default:
      // Log other GAP events for debugging
      if (evt->header.evt_id >= BLE_GAP_EVT_BASE && evt->header.evt_id < BLE_GAP_EVT_LAST) {
        BLE_DEBUG_PRINTLN("Unhandled GAP event: 0x%02X (handle=0x%04X)", evt->header.evt_id, conn_handle);
      }
      break;
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

  Bluefruit.Advertising.restartOnDisconnect(false);  // We'll manually restart after grace period
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);

}

// Start BLE advertising if not already running
void SerialBLEInterface::startAdv() {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: starting advertising");

  if(Bluefruit.Advertising.isRunning()){
    BLE_DEBUG_PRINTLN("SerialBLEInterface: already advertising");
    return;
  }

  Bluefruit.Advertising.start(0);
}

// Stop BLE advertising if currently running
void SerialBLEInterface::stopAdv() {

  BLE_DEBUG_PRINTLN("SerialBLEInterface: stopping advertising");
  
  if(!Bluefruit.Advertising.isRunning()){
    return;
  }

  Bluefruit.Advertising.stop();

}

// Enable interface, clear buffers, and start advertising
void SerialBLEInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  Bluefruit.Advertising.restartOnDisconnect(false);  // We'll manually restart after grace period
  startAdv();
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

  Bluefruit.Advertising.restartOnDisconnect(false);
  stopAdv();
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

// Check if write queue has reached maximum pending writes
bool SerialBLEInterface::isWriteBusy() const {
  return _pending_writes >= MAX_PENDING_WRITES;
}

// Process received frames, handle outgoing queue, and manage connection state
// Returns length of received frame, or 0 if no frame available
size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  // Check if we need to restart advertising after grace period
  // Only restart if not connected and grace period has expired
  if (_advRestartPending && _advRestartTime > 0 && _isEnabled && !_isDeviceConnected && Bluefruit.connected() == 0) {
    unsigned long time_since_disconnect = millis() - _advRestartTime;
    if (time_since_disconnect >= CONNECT_EVENT_GRACE_PERIOD) {
      BLE_DEBUG_PRINTLN("Grace period expired, restarting advertising");
      _advRestartPending = false;
      _advRestartTime = 0;
      startAdv();
    }
  }
  
  if (send_queue_len > 0 && _pending_writes < MAX_PENDING_WRITES) {
    if (_isDeviceConnected && Bluefruit.connected() > 0) {
      size_t written = bleuart.write(send_queue[0].buf, send_queue[0].len);
      if (written > 0) {
        _pending_writes++;
        BLE_DEBUG_PRINTLN("writeBytes: sz=%d, hdr=%d, pending=%d",
                         (uint32_t)send_queue[0].len, (uint32_t)send_queue[0].buf[0],
                         _pending_writes);

        send_queue_len--;
        // Shift remaining frames down using memmove (handles overlapping memory correctly)
        if (send_queue_len > 0) {
          memmove(&send_queue[0], &send_queue[1], send_queue_len * sizeof(Frame));
        }
      } else {
        // Write failed - keep frame in queue and try again next time
        BLE_DEBUG_PRINTLN("writeBytes failed, keeping frame in queue, pending=%d", _pending_writes);
        
        // If connection appears invalid, reset _pending_writes since TX completions won't come
        bool still_valid = _isDeviceConnected && Bluefruit.connected() > 0;
        
        if (!still_valid && _pending_writes > 0) {
          BLE_DEBUG_PRINTLN("Resetting stuck _pending_writes=%d due to invalid connection", _pending_writes);
          _pending_writes = 0;
        }
      }
    }
  } else {
    if (_isDeviceConnected) {
      int avail = bleuart.available();
      if (avail > 0) {
        int got = bleuart.readBytes(dest, avail > MAX_FRAME_SIZE ? MAX_FRAME_SIZE : avail);

        if (avail > MAX_FRAME_SIZE) {
          uint8_t discard[32];
          int remaining = avail - got;
          while (remaining > 0) {
            int chunk = remaining < (int)sizeof(discard) ? remaining : (int)sizeof(discard);
            int drained = bleuart.readBytes(discard, chunk);
            if (drained <= 0) break;
            remaining -= drained;
          }
          BLE_DEBUG_PRINTLN("WARN: BLE RX overflow: avail=%d, read=%d, drained=%d", avail, got, avail - got - remaining);
        }

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
