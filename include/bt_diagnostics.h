#pragma once

#include <Arduino.h>
#include <atomic>
#include <esp_gap_bt_api.h>

namespace {
bool isDenonAddress(const uint8_t address[6]);
}

// Included by main.cpp only. BluetoothSerial owns the GAP callback; observe its
// events through the linker wrapper and forward every event unchanged.
namespace btDiagnostics {
std::atomic<esp_bt_gap_cb_t> originalGap{nullptr};
std::atomic<uint32_t> connectCount{0};
std::atomic<int> lastConnectStatus{-1};
std::atomic<uint32_t> lastConnectAtMs{0};
std::atomic<int> lastDisconnectReason{-1};
std::atomic<uint32_t> lastDisconnectAtMs{0};
int establishedHandle = -1;  // Only accessed by the serialized GAP callback.

void gap(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  const uint32_t now = millis();
  if (event == ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT &&
      isDenonAddress(param->acl_conn_cmpl_stat.bda)) {
    const auto &connection = param->acl_conn_cmpl_stat;
    lastConnectAtMs.store(now, std::memory_order_relaxed);
    lastConnectStatus.store(connection.stat, std::memory_order_relaxed);
    connectCount.fetch_add(1, std::memory_order_relaxed);
    if (connection.stat == ESP_BT_STATUS_HCI_SUCCESS) {
      establishedHandle = connection.handle;
    }
    Serial.printf("BT_DIAG ACL_CONNECT t=%u status=%d handle=%u\n",
                  now, static_cast<int>(connection.stat), connection.handle);
  } else if (event == ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT &&
             isDenonAddress(param->acl_disconn_cmpl_stat.bda)) {
    const auto &disconnection = param->acl_disconn_cmpl_stat;
    if (establishedHandle == disconnection.handle) {
      lastDisconnectAtMs.store(now, std::memory_order_relaxed);
      lastDisconnectReason.store(disconnection.reason, std::memory_order_relaxed);
      establishedHandle = -1;
    }
    Serial.printf("BT_DIAG ACL_DISCONNECT t=%u reason=%d handle=%u\n",
                  now, static_cast<int>(disconnection.reason), disconnection.handle);
  }
  const auto callback = originalGap.load(std::memory_order_relaxed);
  if (callback) callback(event, param);
}
}  // namespace btDiagnostics

extern "C" esp_err_t __real_esp_bt_gap_register_callback(esp_bt_gap_cb_t);
extern "C" esp_err_t __wrap_esp_bt_gap_register_callback(esp_bt_gap_cb_t callback) {
  // Preserve the SDK's null-callback validation and failed-registration result.
  if (!callback) return __real_esp_bt_gap_register_callback(callback);
  const auto previous = btDiagnostics::originalGap.exchange(callback);
  const esp_err_t result = __real_esp_bt_gap_register_callback(btDiagnostics::gap);
  if (result != ESP_OK) btDiagnostics::originalGap.store(previous);
  return result;
}
