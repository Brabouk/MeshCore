#include "SerialBLEInterface.h"
#include <string.h>
#include "ble_gap.h"
#include "ble_hci.h"
#include "nrf_nvic.h"

static SerialBLEInterface* instance = nullptr;

void SerialBLEInterface::onConnect(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: connected handle=0x%04X", connection_handle);
  if (instance) {
    // Connection established but not yet secure - wait for onSecured() before allowing data
    instance->_isDeviceConnected = false;
    // Buffers already cleared by enable() or previous onDisconnect()
  }
}

void SerialBLEInterface::onDisconnect(uint16_t connection_handle, uint8_t reason) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disconnected handle=0x%04X reason=%u", connection_handle, (unsigned)reason);
  if (instance) {
    instance->_isDeviceConnected = false;
    instance->clearBuffers();
  }
}

void SerialBLEInterface::onSecured(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: onSecured handle=0x%04X", connection_handle);
  if (instance) {
    instance->_isDeviceConnected = true;
    
    ble_gap_conn_params_t conn_params;
    conn_params.min_conn_interval = 12;   // 15ms (iOS-compliant)
    conn_params.max_conn_interval = 24;   // 30ms
    conn_params.slave_latency = 0;
    conn_params.conn_sup_timeout = 200;   // 2s (Apple minimum)
    
    uint32_t err_code = sd_ble_gap_conn_param_update(connection_handle, &conn_params);
    if (err_code == NRF_SUCCESS) {
      BLE_DEBUG_PRINTLN("Connection parameter update requested: 15-30ms interval, 2s timeout");
    } else {
      BLE_DEBUG_PRINTLN("Failed to request connection parameter update: %lu", err_code);
    }
  }
}

bool SerialBLEInterface::onPairingPasskey(uint16_t connection_handle, uint8_t const passkey[6], bool match_request) {
  (void)connection_handle;
  (void)passkey;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing passkey request match=%d", match_request);
  return true;
}

void SerialBLEInterface::onPairingComplete(uint16_t connection_handle, uint8_t auth_status) {
  (void)connection_handle;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing complete status=%u", (unsigned)auth_status);
  if (auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
    BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing successful");
  } else {
    BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing failed, disconnecting");
    if (instance) {
      instance->disconnect();
    }
  }
}

void SerialBLEInterface::onBLEEvent(ble_evt_t* evt) {
  if (!instance) return;
  
  if (evt->header.evt_id == BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST) {
    uint16_t conn_handle = evt->evt.gap_evt.conn_handle;
    BLE_DEBUG_PRINTLN("CONN_PARAM_UPDATE_REQUEST: handle=0x%04X, min_interval=%u, max_interval=%u, latency=%u, timeout=%u",
                     conn_handle,
                     (unsigned)evt->evt.gap_evt.params.conn_param_update_request.conn_params.min_conn_interval,
                     (unsigned)evt->evt.gap_evt.params.conn_param_update_request.conn_params.max_conn_interval,
                     (unsigned)evt->evt.gap_evt.params.conn_param_update_request.conn_params.slave_latency,
                     (unsigned)evt->evt.gap_evt.params.conn_param_update_request.conn_params.conn_sup_timeout);
    
    uint32_t err_code = sd_ble_gap_conn_param_update(conn_handle, NULL);  // NULL = use PPCP (iOS requirement)
    if (err_code == NRF_SUCCESS) {
      BLE_DEBUG_PRINTLN("Accepted CONN_PARAM_UPDATE_REQUEST (using PPCP)");
    } else {
      BLE_DEBUG_PRINTLN("ERROR: Failed to accept CONN_PARAM_UPDATE_REQUEST: 0x%08X", err_code);
    }
  }
}

void SerialBLEInterface::begin(const char* device_name, uint32_t pin_code) {
  if (instance != nullptr && instance != this) {
    BLE_DEBUG_PRINTLN("WARNING: SerialBLEInterface instance already exists, overwriting");
  }
  instance = this;

  // Validate device_name parameter
  if (device_name == nullptr) {
    BLE_DEBUG_PRINTLN("ERROR: device_name is NULL");
    return;
  }
  
  // BLE device name max length is 31 bytes (BLE_GAP_DEVNAME_NAME_MAX_LEN)
  size_t name_len = strlen(device_name);
  if (name_len == 0) {
    BLE_DEBUG_PRINTLN("ERROR: device_name is empty");
    return;
  }
  if (name_len > 31) {
    BLE_DEBUG_PRINTLN("ERROR: device_name too long (%zu bytes, max 31)", name_len);
    return;
  }

  char charpin[20];
  snprintf(charpin, sizeof(charpin), "%lu", (unsigned long)pin_code);

  // If we want to control BLE LED ourselves, uncomment this:
  // Bluefruit.autoConnLed(false);
  
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  
  ble_gap_conn_params_t ppcp_params;
  ppcp_params.min_conn_interval = 12;   // 15ms (iOS-compliant)
  ppcp_params.max_conn_interval = 24;   // 30ms
  ppcp_params.slave_latency = 0;
  ppcp_params.conn_sup_timeout = 200;   // 2s (Apple minimum)
  
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
  bleuart.setRxCallback(onBleUartRX);

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();

  Bluefruit.Advertising.addService(bleuart);

  Bluefruit.ScanResponse.addName();

  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);

  Bluefruit.Advertising.restartOnDisconnect(true);

}

void SerialBLEInterface::clearBuffers() {
  uint8_t nrf_nvic_state;
  sd_nvic_critical_region_enter(&nrf_nvic_state);
  send_queue_len = 0;
  recv_queue_len = 0;
  sd_nvic_critical_region_exit(nrf_nvic_state);
}

// Enable interface and start advertising
void SerialBLEInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  Bluefruit.Advertising.start(0);
}

void SerialBLEInterface::disconnect() {
  // Disconnect all possible connection handles (S140 supports up to 20 connections)
  // Invalid handles are ignored, so we can safely try all of them
  for (uint16_t conn_handle = 0; conn_handle < 20; conn_handle++) {
    sd_ble_gap_disconnect(conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
  }
}

void SerialBLEInterface::disable() {
  _isEnabled = false;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disable");

  disconnect();  // Will trigger onDisconnect() which clears buffers
  Bluefruit.Advertising.stop();
}

size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%zu", len);
    return 0;
  }

  bool connected = isConnected();  // Cache result to avoid multiple calls
  if (connected && len > 0) {
    uint8_t nrf_nvic_state;
    sd_nvic_critical_region_enter(&nrf_nvic_state);
    
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      sd_nvic_critical_region_exit(nrf_nvic_state);
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;
    send_queue[send_queue_len].retry_count = 0;
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;
    
    sd_nvic_critical_region_exit(nrf_nvic_state);
    return len;
  }
  return 0;
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  uint8_t nrf_nvic_state;
  bool connected = isConnected();  // Cache result to avoid multiple calls
  if (connected) {
    // Check send queue and process if available
    sd_nvic_critical_region_enter(&nrf_nvic_state);
    bool has_queue = send_queue_len > 0;
    Frame frame_to_send;
    if (has_queue) {
      frame_to_send = send_queue[0];
    }
    sd_nvic_critical_region_exit(nrf_nvic_state);
    
    if (has_queue) {
      size_t written = bleuart.write(frame_to_send.buf, frame_to_send.len);
      if (written > 0) {
        if (written == frame_to_send.len) {
          // Complete write - remove frame from queue
          BLE_DEBUG_PRINTLN("writeBytes: sz=%u, hdr=%u", (unsigned)frame_to_send.len, (unsigned)frame_to_send.buf[0]);
          
          sd_nvic_critical_region_enter(&nrf_nvic_state);
          send_queue_len--;
          if (send_queue_len > 0) {
            memmove(&send_queue[0], &send_queue[1], send_queue_len * sizeof(Frame));  // Handles overlapping memory
          }
          sd_nvic_critical_region_exit(nrf_nvic_state);
        } else {
          // Partial write - update frame to contain remaining bytes
          BLE_DEBUG_PRINTLN("writeBytes: partial write, sent=%zu of %u, hdr=%u", written, (unsigned)frame_to_send.len, (unsigned)frame_to_send.buf[0]);
          
          sd_nvic_critical_region_enter(&nrf_nvic_state);
          size_t remaining = frame_to_send.len - written;
          // Shift remaining bytes to start of buffer
          memmove(send_queue[0].buf, send_queue[0].buf + written, remaining);
          send_queue[0].len = remaining;
          send_queue[0].retry_count = 0;  // Reset retry on partial success
          sd_nvic_critical_region_exit(nrf_nvic_state);
          // Frame stays in queue for retry on next call
        }
      } else {
        // Write failed (written == 0) - check if connection is still valid
        // Re-check connection state as it may have changed during write
        bool still_connected = isConnected();
        
        sd_nvic_critical_region_enter(&nrf_nvic_state);
        if (still_connected) {
          // Connection still valid - likely temporary buffer full, don't count as retry
          // Frame stays in queue for next attempt without incrementing retry counter
          BLE_DEBUG_PRINTLN("writeBytes failed (buffer full?), will retry");
        } else {
          // Connection lost - increment retry counter
          send_queue[0].retry_count++;
          if (send_queue[0].retry_count >= MAX_WRITE_RETRIES) {
            // Drop frame after max retries
            BLE_DEBUG_PRINTLN("writeBytes failed after %u retries, dropping frame", (unsigned)MAX_WRITE_RETRIES);
            send_queue_len--;
            if (send_queue_len > 0) {
              memmove(&send_queue[0], &send_queue[1], send_queue_len * sizeof(Frame));
            }
          } else {
            BLE_DEBUG_PRINTLN("writeBytes failed, retry %u/%u", (unsigned)send_queue[0].retry_count, (unsigned)MAX_WRITE_RETRIES);
          }
        }
        sd_nvic_critical_region_exit(nrf_nvic_state);
      }
    }
  }
  
  // Check receive queue
  sd_nvic_critical_region_enter(&nrf_nvic_state);
  if (recv_queue_len > 0) {
    size_t len = recv_queue[0].len;
    memcpy(dest, recv_queue[0].buf, len);
    
    recv_queue_len--;
    if (recv_queue_len > 0) {
      memmove(&recv_queue[0], &recv_queue[1], recv_queue_len * sizeof(Frame));
    }
    sd_nvic_critical_region_exit(nrf_nvic_state);
    
    BLE_DEBUG_PRINTLN("readBytes: sz=%zu, hdr=%u", len, (unsigned)dest[0]);
    return len;
  }
  sd_nvic_critical_region_exit(nrf_nvic_state);
  
  return 0;
}

void SerialBLEInterface::onBleUartRX(uint16_t conn_handle) {
  (void)conn_handle;
  if (!instance) {
    return;
  }
  
  uint8_t nrf_nvic_state;
  sd_nvic_critical_region_enter(&nrf_nvic_state);
  
  // Read all available data into queue
  while (instance->bleuart.available() > 0) {
    if (instance->recv_queue_len >= FRAME_QUEUE_SIZE) {
      // Queue full - drain remaining data to prevent overflow
      while (instance->bleuart.available() > 0) {
        instance->bleuart.read();
      }
      BLE_DEBUG_PRINTLN("onBleUartRX: recv queue full, dropping data");
      break;
    }
    
    int avail = instance->bleuart.available();
    int read_len = avail > MAX_FRAME_SIZE ? MAX_FRAME_SIZE : avail;
    
    instance->recv_queue[instance->recv_queue_len].len = read_len;
    instance->bleuart.readBytes(instance->recv_queue[instance->recv_queue_len].buf, read_len);
    instance->recv_queue_len++;
  }
  
  sd_nvic_critical_region_exit(nrf_nvic_state);
}

bool SerialBLEInterface::isConnected() const {
  return _isDeviceConnected && Bluefruit.connected() > 0;
}

bool SerialBLEInterface::isWriteBusy() const {
  uint8_t nrf_nvic_state;
  sd_nvic_critical_region_enter(&nrf_nvic_state);
  bool busy = send_queue_len >= FRAME_QUEUE_SIZE;
  sd_nvic_critical_region_exit(nrf_nvic_state);
  return busy;
}
