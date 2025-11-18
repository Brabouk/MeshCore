#include "SerialBLEInterface.h"
#include "ble_gap.h"
#include "ble_err.h"
#include "ble_hci.h"
#include "nrf_error.h"

static SerialBLEInterface* instance;

// Helper function to validate connection handle using SoftDevice API
static bool isValidConnectionHandle(uint16_t conn_handle) {
  if (conn_handle == BLE_CONN_HANDLE_INVALID) {
    return false;
  }
  
  // Query connection security state - returns BLE_ERROR_INVALID_CONN_HANDLE if handle is invalid
  ble_gap_conn_sec_t conn_sec;
  uint32_t err = sd_ble_gap_conn_sec_get(conn_handle, &conn_sec);
  return (err == NRF_SUCCESS);
}

void SerialBLEInterface::onConnect(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: connected");
  // we now set _isDeviceConnected=true in onSecured callback instead
}

void SerialBLEInterface::onDisconnect(uint16_t connection_handle, uint8_t reason) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disconnected reason=%d", reason);
  if(instance){
    // Clear all connection state to prevent accumulation between cycles
    instance->_isDeviceConnected = false;
    instance->_write_retry_count = 0;
    instance->_first_write_failure_time = 0;  // Reset failure timer
    instance->send_queue_len = 0;  // Clear queue on disconnect
    instance->_last_write = 0;  // Reset write timing
    instance->_last_disconnect_time = millis();  // Update last disconnect time for rate limiting
    instance->_disconnect_pending = false;  // Disconnect event received
    instance->_disconnect_initiated_time = 0;  // Clear disconnect timeout
    instance->startAdv();
  }
}

void SerialBLEInterface::onSecured(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: onSecured");
  if(instance){
    // Validate connection handle before setting state
    if (isValidConnectionHandle(connection_handle)) {
      instance->_isDeviceConnected = true;
      instance->_write_retry_count = 0;  // Reset retry counter on new secure connection
      instance->_first_write_failure_time = 0;  // Reset failure timer on new connection
      instance->_last_write = 0;  // Reset write timing
      // no need to stop advertising on connect, as the ble stack does this automatically
    } else {
      BLE_DEBUG_PRINTLN("SerialBLEInterface: onSecured with invalid handle=%d", connection_handle);
      instance->_isDeviceConnected = false;
    }
  }
}

void SerialBLEInterface::begin(const char* device_name, uint32_t pin_code) {

  instance = this;

  char charpin[20];
  sprintf(charpin, "%d", pin_code);

  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.configPrphConn(250, BLE_GAP_EVENT_LENGTH_MIN, 16, 16);  // increase MTU
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
  if(Bluefruit.Advertising.isRunning()){
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
  Bluefruit.Advertising.restartOnDisconnect(false); // don't restart automatically as we handle it in onDisconnect
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

    send_queue[send_queue_len].len = len;  // add to send queue
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}

#define  BLE_WRITE_MIN_INTERVAL   60

bool SerialBLEInterface::isWriteBusy() const {
  return millis() < _last_write + BLE_WRITE_MIN_INTERVAL;   // still too soon to start another write?
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  // Check for disconnect event timeout - if we initiated a disconnect but event didn't arrive
  if (_disconnect_pending && _disconnect_initiated_time > 0) {
    unsigned long time_since_disconnect = millis() - _disconnect_initiated_time;
    if (time_since_disconnect >= DISCONNECT_EVENT_TIMEOUT) {
      BLE_DEBUG_PRINTLN("Disconnect event timeout - assuming disconnected after %d ms", 
                       (uint32_t)time_since_disconnect);
      // Disconnect event didn't arrive - assume we're disconnected
      _isDeviceConnected = false;
      _disconnect_pending = false;
      _disconnect_initiated_time = 0;
      _last_disconnect_time = millis();
      send_queue_len = 0;  // Clear queue
      _write_retry_count = 0;
      _first_write_failure_time = 0;
      _last_write = 0;
    }
  }
  
  if (send_queue_len > 0   // first, check send queue
    && millis() >= _last_write + BLE_WRITE_MIN_INTERVAL    // space the writes apart
  ) {
    // Verify connection is still valid before attempting write
    // Check multiple conditions to catch stale connection states
    bool connection_valid = _isDeviceConnected && 
                           Bluefruit.connected() &&
                           bleuart.notifyEnabled();
    
    // Additional validation: check connection handle validity using SoftDevice API
    if (connection_valid) {
      uint16_t conn_handle;
      if (Bluefruit.getConnectedHandles(&conn_handle, 1) > 0) {
        if (!isValidConnectionHandle(conn_handle)) {
          BLE_DEBUG_PRINTLN("Connection handle invalid, marking connection as invalid");
          connection_valid = false;
        }
      } else {
        // No connected handles found
        connection_valid = false;
      }
    }
    
    if (connection_valid) {
      _last_write = millis();
      size_t written = bleuart.write(send_queue[0].buf, send_queue[0].len);
      
      if (written > 0) {
        _write_retry_count = 0;  // Reset retry counter on success
        _first_write_failure_time = 0;  // Reset failure timer on success
        BLE_DEBUG_PRINTLN("writeBytes: sz=%d, hdr=%d", (uint32_t)send_queue[0].len, (uint32_t) send_queue[0].buf[0]);

        send_queue_len--;
        for (int i = 0; i < send_queue_len; i++) {   // delete top item from queue
          send_queue[i] = send_queue[i + 1];
        }
      } else {
        // Track when write failures started
        if (_first_write_failure_time == 0) {
          _first_write_failure_time = millis();
        }
        
        _write_retry_count++;
        BLE_DEBUG_PRINTLN("writeBytes failed, retry=%d", _write_retry_count);
        
        // Re-check connection validity after failed write
        bool still_valid = _isDeviceConnected && 
                          Bluefruit.connected() &&
                          bleuart.notifyEnabled();
        
        // Additional validation: check connection handle validity
        if (still_valid) {
          uint16_t conn_handle;
          if (Bluefruit.getConnectedHandles(&conn_handle, 1) > 0) {
            if (!isValidConnectionHandle(conn_handle)) {
              BLE_DEBUG_PRINTLN("Connection handle invalid after write failure");
              still_valid = false;
            }
          } else {
            still_valid = false;
          }
        }
        
        // Check if we've been failing for too long - force disconnect to recover
        bool failure_timeout = (_first_write_failure_time > 0) && 
                              (millis() - _first_write_failure_time >= MAX_WRITE_FAILURE_DURATION);
        
        // Drop frame after max retries, if connection is invalid, or if failures persist too long
        if (_write_retry_count >= MAX_WRITE_RETRIES || !still_valid || failure_timeout) {
          if (failure_timeout) {
            BLE_DEBUG_PRINTLN("Write failures persisted for %d ms, forcing disconnect", 
                             (uint32_t)(millis() - _first_write_failure_time));
            // Force disconnect to recover from bad connection state
            // Check rate limiting - prevent rapid disconnect/reconnect cycles
            unsigned long time_since_last_disconnect = millis() - _last_disconnect_time;
            if (time_since_last_disconnect < MIN_DISCONNECT_INTERVAL) {
              BLE_DEBUG_PRINTLN("Disconnect rate limited - waiting %d ms", 
                               (uint32_t)(MIN_DISCONNECT_INTERVAL - time_since_last_disconnect));
              // Don't disconnect yet, but clear state to prevent further writes
              _isDeviceConnected = false;
            } else {
              // Validate handle before disconnecting
              if (Bluefruit.connected()) {
                uint16_t conn_handle;
                if (Bluefruit.getConnectedHandles(&conn_handle, 1) > 0) {
                  if (isValidConnectionHandle(conn_handle)) {
                    // Use SoftDevice API directly to get error codes
                    uint32_t err = sd_ble_gap_disconnect(conn_handle, BLE_HCI_LOCAL_HOST_TERMINATED_CONNECTION);
                    if (err == NRF_SUCCESS) {
                      _disconnect_initiated_time = millis();
                      _disconnect_pending = true;
                      BLE_DEBUG_PRINTLN("Disconnect initiated, waiting for event");
                    } else if (err == NRF_ERROR_NO_MEM) {
                      BLE_DEBUG_PRINTLN("NRF_ERROR_NO_MEM - SoftDevice out of memory, clearing state");
                      // SoftDevice is out of memory - force complete state reset
                      _isDeviceConnected = false;
                      _disconnect_pending = false;
                      _disconnect_initiated_time = 0;
                      _last_disconnect_time = millis();
                    } else {
                      BLE_DEBUG_PRINTLN("Disconnect failed with error: 0x%08X", (uint32_t)err);
                      // Disconnect failed, but clear state anyway
                      _isDeviceConnected = false;
                    }
                  } else {
                    BLE_DEBUG_PRINTLN("Cannot disconnect - handle is invalid");
                    _isDeviceConnected = false;
                  }
                } else {
                  // No connected handles - already disconnected
                  _isDeviceConnected = false;
                }
              } else {
                // Not connected - just clear state
                _isDeviceConnected = false;
              }
            }
          }
          
          BLE_DEBUG_PRINTLN("Dropping frame after %d failed write attempts (connection_valid=%d, timeout=%d)", 
                           _write_retry_count, still_valid, failure_timeout);
          _write_retry_count = 0;
          _first_write_failure_time = 0;
          send_queue_len--;
          for (int i = 0; i < send_queue_len; i++) {
            send_queue[i] = send_queue[i + 1];
          }
          
          // If connection is invalid, clear connection state
          if (!still_valid || failure_timeout) {
            BLE_DEBUG_PRINTLN("Connection invalid, clearing state");
            _isDeviceConnected = false;
          }
        }
      }
    } else {
      // Connection invalid, drop all queued frames
      if (send_queue_len > 0) {
        BLE_DEBUG_PRINTLN("Connection invalid, dropping %d queued frames", send_queue_len);
        send_queue_len = 0;
        _write_retry_count = 0;
        _first_write_failure_time = 0;
        _isDeviceConnected = false;
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
