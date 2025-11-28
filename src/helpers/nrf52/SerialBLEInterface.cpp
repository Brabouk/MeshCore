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
  // Lock-free: just reset pointers, ISR will see empty queue
  send_queue_head = 0;
  send_queue_tail = 0;
  __sync_synchronize();  // Memory barrier
  recv_queue_head = 0;
  recv_queue_tail = 0;
  __sync_synchronize();  // Memory barrier
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

  bool connected = isConnected();
  if (connected && len > 0) {
    // Lock-free: check if queue has space
    uint8_t head = send_queue_head;
    uint8_t tail = send_queue_tail;
    __sync_synchronize();  // Memory barrier - ensure we see latest tail
    
    uint8_t next_head = (head + 1) % FRAME_QUEUE_SIZE;
    if (next_head == tail) {
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    // Write frame data
    send_queue[head].len = len;
    send_queue[head].retry_count = 0;
    memcpy(send_queue[head].buf, src, len);
    
    // Atomically update head pointer
    __sync_synchronize();  // Memory barrier - ensure data is written before head update
    send_queue_head = next_head;
    
    return len;
  }
  return 0;
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  bool connected = isConnected();
  if (connected) {
    // Process send queue - lock-free ring buffer
    uint8_t tail = send_queue_tail;
    uint8_t head = send_queue_head;
    __sync_synchronize();  // Memory barrier
    
    if (tail != head) {
      // Copy frame data (ISR-safe: we're the only reader)
      Frame frame_to_send = send_queue[tail];
      __sync_synchronize();  // Memory barrier - ensure we have consistent copy
      
      size_t written = bleuart.write(frame_to_send.buf, frame_to_send.len);
      if (written > 0) {
        if (written == frame_to_send.len) {
          // Complete write - advance tail pointer
          BLE_DEBUG_PRINTLN("writeBytes: sz=%u, hdr=%u", (unsigned)frame_to_send.len, (unsigned)frame_to_send.buf[0]);
          __sync_synchronize();  // Memory barrier
          send_queue_tail = (tail + 1) % FRAME_QUEUE_SIZE;
        } else {
          // Partial write - retry whole frame next time (lock-free: can't modify in place)
          BLE_DEBUG_PRINTLN("writeBytes: partial write, sent=%zu of %u, will retry whole frame", written, (unsigned)frame_to_send.len);
          // Frame stays in queue, will be retried on next call
          __sync_synchronize();  // Memory barrier
        }
      } else {
        // Write failed - check connection and handle retries
        bool still_connected = isConnected();
        if (still_connected) {
          BLE_DEBUG_PRINTLN("writeBytes failed (buffer full?), will retry");
        } else {
          send_queue[tail].retry_count++;
          if (send_queue[tail].retry_count >= MAX_WRITE_RETRIES) {
            BLE_DEBUG_PRINTLN("writeBytes failed after %u retries, dropping frame", (unsigned)MAX_WRITE_RETRIES);
            __sync_synchronize();  // Memory barrier
            send_queue_tail = (tail + 1) % FRAME_QUEUE_SIZE;
          } else {
            BLE_DEBUG_PRINTLN("writeBytes failed, retry %u/%u", (unsigned)send_queue[tail].retry_count, (unsigned)MAX_WRITE_RETRIES);
            __sync_synchronize();  // Memory barrier
          }
        }
      }
    }
  }
  
  // Check receive queue - lock-free ring buffer (ISR writes, we read)
  uint8_t recv_tail = recv_queue_tail;
  uint8_t recv_head = recv_queue_head;
  __sync_synchronize();  // Memory barrier - ensure we see latest head from ISR
  
  if (recv_tail != recv_head) {
    // Copy frame data
    size_t len = recv_queue[recv_tail].len;
    memcpy(dest, recv_queue[recv_tail].buf, len);
    
    // Advance tail pointer
    __sync_synchronize();  // Memory barrier - ensure copy completes before tail update
    recv_queue_tail = (recv_tail + 1) % FRAME_QUEUE_SIZE;
    
    BLE_DEBUG_PRINTLN("readBytes: sz=%zu, hdr=%u", len, (unsigned)dest[0]);
    return len;
  }
  
  return 0;
}

void SerialBLEInterface::onBleUartRX(uint16_t conn_handle) {
  (void)conn_handle;
  if (!instance) {
    return;
  }
  
  // Lock-free: ISR writes to recv queue
  while (instance->bleuart.available() > 0) {
    uint8_t head = instance->recv_queue_head;
    uint8_t tail = instance->recv_queue_tail;
    __sync_synchronize();  // Memory barrier
    
    uint8_t next_head = (head + 1) % FRAME_QUEUE_SIZE;
    if (next_head == tail) {
      // Queue full - drain remaining data to prevent overflow
      while (instance->bleuart.available() > 0) {
        instance->bleuart.read();
      }
      BLE_DEBUG_PRINTLN("onBleUartRX: recv queue full, dropping data");
      break;
    }
    
    int avail = instance->bleuart.available();
    int read_len = avail > MAX_FRAME_SIZE ? MAX_FRAME_SIZE : avail;
    
    instance->recv_queue[head].len = read_len;
    instance->bleuart.readBytes(instance->recv_queue[head].buf, read_len);
    
    // Atomically update head pointer
    __sync_synchronize();  // Memory barrier - ensure data is written before head update
    instance->recv_queue_head = next_head;
  }
}

bool SerialBLEInterface::isConnected() const {
  return _isDeviceConnected && Bluefruit.connected() > 0;
}

bool SerialBLEInterface::isWriteBusy() const {
  // Lock-free: check queue size
  uint8_t head = send_queue_head;
  uint8_t tail = send_queue_tail;
  __sync_synchronize();  // Memory barrier
  uint8_t size = getQueueSize(head, tail, FRAME_QUEUE_SIZE);
  return size >= FRAME_QUEUE_SIZE;
}
