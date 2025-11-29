#include "SerialBLEInterface.h"
#include <string.h>
#include "ble_gap.h"
#include "ble_hci.h"

static SerialBLEInterface* instance = nullptr;

void SerialBLEInterface::onConnect(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: connected handle=0x%04X", connection_handle);
  if (instance) {
    instance->_conn_handle = connection_handle;
    instance->_isDeviceConnected = false;  // Wait for onSecured() before allowing data
    instance->clearBuffers();  // Clear queues and BLEUart FIFO to prevent stale data
  }
}

void SerialBLEInterface::onDisconnect(uint16_t connection_handle, uint8_t reason) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disconnected handle=0x%04X reason=%u", connection_handle, (unsigned)reason);
  if (instance) {
    // Only process if this is our tracked connection (handle matches)
    // Note: Handle numbers can be reused, but Bluefruit deletes the connection object on disconnect,
    // so stale callbacks won't have a valid connection object
    if (instance->_conn_handle == connection_handle) {
      instance->_conn_handle = BLE_CONN_HANDLE_INVALID;
      instance->_isDeviceConnected = false;
      instance->clearBuffers();  // This also flushes BLEUart FIFO
    }
  }
}

void SerialBLEInterface::onSecured(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: onSecured handle=0x%04X", connection_handle);
  if (instance) {
    // Validate: handle must match AND we must be waiting for security (_isDeviceConnected == false)
    // AND connection object must exist and be connected
    // This prevents stale callbacks from old connections with the same handle number
    BLEConnection* conn = Bluefruit.Connection(connection_handle);
    if (instance->_conn_handle == connection_handle && 
        !instance->_isDeviceConnected &&  // Must be waiting for security
        conn != nullptr && 
        conn->connected()) {
      instance->_isDeviceConnected = true;
      
      // Flush any stale TX data from previous connection
      instance->bleuart.flushTXD();
      
      ble_gap_conn_params_t conn_params;
      conn_params.min_conn_interval = 12;   // 15ms
      conn_params.max_conn_interval = 24;   // 30ms
      conn_params.slave_latency = 0;
      conn_params.conn_sup_timeout = 200;   // 2s
      
      uint32_t err_code = sd_ble_gap_conn_param_update(connection_handle, &conn_params);
      if (err_code == NRF_SUCCESS) {
        BLE_DEBUG_PRINTLN("Connection parameter update requested: 15-30ms interval, 2s timeout");
      } else {
        BLE_DEBUG_PRINTLN("Failed to request connection parameter update: %lu", err_code);
      }
    } else {
      BLE_DEBUG_PRINTLN("onSecured: ignoring stale/duplicate callback");
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
  BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing complete handle=0x%04X status=%u", connection_handle, (unsigned)auth_status);
  if (instance) {
    // Validate: handle must match AND connection object must exist and be connected
    BLEConnection* conn = Bluefruit.Connection(connection_handle);
    if (instance->_conn_handle == connection_handle && 
        conn != nullptr && 
        conn->connected()) {
      if (auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
        BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing successful");
      } else {
        BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing failed, disconnecting");
        instance->disconnect();
      }
    } else {
      BLE_DEBUG_PRINTLN("onPairingComplete: ignoring stale callback");
    }
  }
}

void SerialBLEInterface::onBLEEvent(ble_evt_t* evt) {
  if (!instance) return;
  
  if (evt->header.evt_id == BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST) {
    uint16_t conn_handle = evt->evt.gap_evt.conn_handle;
    // Validate: handle must match AND connection object must exist and be connected
    BLEConnection* conn = Bluefruit.Connection(conn_handle);
    if (instance->_conn_handle == conn_handle && 
        conn != nullptr && 
        conn->connected()) {
      BLE_DEBUG_PRINTLN("CONN_PARAM_UPDATE_REQUEST: handle=0x%04X, min_interval=%u, max_interval=%u, latency=%u, timeout=%u",
                       conn_handle,
                       (unsigned)evt->evt.gap_evt.params.conn_param_update_request.conn_params.min_conn_interval,
                       (unsigned)evt->evt.gap_evt.params.conn_param_update_request.conn_params.max_conn_interval,
                       (unsigned)evt->evt.gap_evt.params.conn_param_update_request.conn_params.slave_latency,
                       (unsigned)evt->evt.gap_evt.params.conn_param_update_request.conn_params.conn_sup_timeout);
      
      uint32_t err_code = sd_ble_gap_conn_param_update(conn_handle, NULL);  // NULL = use PPCP
      if (err_code == NRF_SUCCESS) {
        BLE_DEBUG_PRINTLN("Accepted CONN_PARAM_UPDATE_REQUEST (using PPCP)");
      } else {
        BLE_DEBUG_PRINTLN("ERROR: Failed to accept CONN_PARAM_UPDATE_REQUEST: 0x%08X", err_code);
      }
    } else {
      BLE_DEBUG_PRINTLN("CONN_PARAM_UPDATE_REQUEST: ignoring stale callback for handle=0x%04X", conn_handle);
    }
  }
}

void SerialBLEInterface::begin(const char* device_name, uint32_t pin_code) {
  if (instance != nullptr && instance != this) {
    BLE_DEBUG_PRINTLN("WARNING: SerialBLEInterface instance already exists, overwriting");
  }
  instance = this;

  if (device_name == nullptr) {
    BLE_DEBUG_PRINTLN("ERROR: device_name is NULL");
    return;
  }
  
  size_t name_len = strlen(device_name);  // Max 31 bytes (BLE_GAP_DEVNAME_NAME_MAX_LEN)
  if (name_len == 0) {
    BLE_DEBUG_PRINTLN("ERROR: device_name is empty");
    return;
  }
  if (name_len > 31) {
    BLE_DEBUG_PRINTLN("ERROR: device_name too long (%u bytes, max 31)", (unsigned)name_len);
    return;
  }

  char charpin[20];
  snprintf(charpin, sizeof(charpin), "%lu", (unsigned long)pin_code);

  // If we want to control BLE LED ourselves, uncomment this:
  // Bluefruit.autoConnLed(false);
  
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  
  ble_gap_conn_params_t ppcp_params;
  ppcp_params.min_conn_interval = 12;   // 15ms
  ppcp_params.max_conn_interval = 24;   // 30ms
  ppcp_params.slave_latency = 0;
  ppcp_params.conn_sup_timeout = 200;   // 2s
  
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
  send_queue_len = 0;
  recv_queue_len = 0;
  bleuart.flush();  // Clear BLEUart FIFO (safe: begin() must be called first)
}

void SerialBLEInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  Bluefruit.Advertising.start(0);
}

void SerialBLEInterface::disconnect() {
  if (_conn_handle != BLE_CONN_HANDLE_INVALID) {
    sd_ble_gap_disconnect(_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
  }
}

void SerialBLEInterface::disable() {
  _isEnabled = false;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disable");

  disconnect();
  Bluefruit.Advertising.stop();
}

size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%u", (unsigned)len);
    return 0;
  }

  bool connected = isConnected();
  if (connected && len > 0) {
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

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  // Check connection is valid before attempting write
  if (send_queue_len > 0) {
    if (!isConnected()) {
      // Connection is invalid, clear send queue to prevent further attempts
      BLE_DEBUG_PRINTLN("writeBytes: connection invalid, clearing send queue");
      send_queue_len = 0;
    } else {
      Frame frame_to_send = send_queue[0];
      
      size_t written = bleuart.write(frame_to_send.buf, frame_to_send.len);
      if (written > 0) {
        if (written == frame_to_send.len) {
          BLE_DEBUG_PRINTLN("writeBytes: sz=%u, hdr=%u", (unsigned)frame_to_send.len, (unsigned)frame_to_send.buf[0]);
        } else {
          BLE_DEBUG_PRINTLN("writeBytes: partial write, sent=%u of %u, dropping frame", (unsigned)written, (unsigned)frame_to_send.len);
        }
        // Only dequeue on successful write to prevent rapid buffer fillup
        send_queue_len--;
        for (uint8_t i = 0; i < send_queue_len; i++) {
          send_queue[i] = send_queue[i + 1];
        }
      } else {
        // bleuart.write() returns 0 if connection is invalid or buffer full
        // Re-check connection state - if disconnected, drop frame; if buffer full, keep for retry
        if (!isConnected()) {
          // Connection lost - drop frame (no point keeping it)
          BLE_DEBUG_PRINTLN("writeBytes failed: connection lost, dropping frame");
          send_queue_len--;
          for (uint8_t i = 0; i < send_queue_len; i++) {
            send_queue[i] = send_queue[i + 1];
          }
        } else {
          // Buffer full - keep frame for retry (checkRecvFrame() will be called again)
          BLE_DEBUG_PRINTLN("writeBytes failed (buffer full), keeping frame for retry");
        }
      }
    }
  }
  
  if (recv_queue_len > 0) {
    size_t len = recv_queue[0].len;
    memcpy(dest, recv_queue[0].buf, len);
    
    BLE_DEBUG_PRINTLN("readBytes: sz=%u, hdr=%u", (unsigned)len, (unsigned)dest[0]);
    
    recv_queue_len--;
    for (uint8_t i = 0; i < recv_queue_len; i++) {
      recv_queue[i] = recv_queue[i + 1];
    }
    return len;
  }
  
  return 0;
}

void SerialBLEInterface::onBleUartRX(uint16_t conn_handle) {
  if (!instance) {
    return;
  }
  
  // Validate: handle must match AND connection must be secured
  // This prevents processing data from stale connections with the same handle number
  if (instance->_conn_handle != conn_handle || !instance->isConnected()) {
    // Discard data from wrong connection, unsecured connection, or stale connection
    while (instance->bleuart.available() > 0) {
      instance->bleuart.read();
    }
    return;
  }
  
  while (instance->bleuart.available() > 0) {
    if (instance->recv_queue_len >= FRAME_QUEUE_SIZE) {
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
    
    // If overflow detected, drain surplus bytes to prevent frame corruption
    if (avail > MAX_FRAME_SIZE) {
      BLE_DEBUG_PRINTLN("onBleUartRX: WARN: BLE RX overflow, avail=%d, draining surplus", avail);
      int surplus = avail - MAX_FRAME_SIZE;
      uint8_t drain_buf[32];  // Drain in small chunks
      while (surplus > 0) {
        int chunk = surplus > 32 ? 32 : surplus;
        instance->bleuart.readBytes(drain_buf, chunk);
        surplus -= chunk;
      }
    }
  }
}

bool SerialBLEInterface::isConnected() const {
  // Only check connection state, not queue state
  return _isDeviceConnected && Bluefruit.connected() > 0;
}

bool SerialBLEInterface::isWriteBusy() const {
  return send_queue_len >= FRAME_QUEUE_SIZE;
}
