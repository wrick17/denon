// Exercise the production hook with only Arduino/SDK boundaries stubbed.
#include <cassert>
#include <cstring>
#include <initializer_list>
#include "bt_diagnostics.h"

uint32_t testMillis = 0;
TestSerial Serial;
namespace {
const uint8_t target[6] = {1, 2, 3, 4, 5, 6};
bool isDenonAddress(const uint8_t address[6]) {
  return std::memcmp(address, target, sizeof(target)) == 0;
}
esp_bt_gap_cb_t registered = nullptr;
esp_bt_gap_cb_t registrationArgument = nullptr;
esp_err_t registrationResult = ESP_OK;
unsigned forwarded = 0;
esp_bt_gap_cb_event_t forwardedEvent;
esp_bt_gap_cb_param_t *forwardedParam = nullptr;
void original(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  ++forwarded;
  forwardedEvent = event;
  forwardedParam = param;
}
void replacement(esp_bt_gap_cb_event_t, esp_bt_gap_cb_param_t *) {
  assert(false && "Failed registration must preserve the original delegate");
}
void emit(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t &param) {
  const unsigned before = forwarded;
  registered(event, &param);
  assert(forwarded == before + 1);
  assert(forwardedEvent == event && forwardedParam == &param);
}
void connect(int status, uint16_t handle, bool matches = true) {
  esp_bt_gap_cb_param_t param{};
  if (matches) std::memcpy(param.acl_conn_cmpl_stat.bda, target, sizeof(target));
  param.acl_conn_cmpl_stat.stat = status;
  param.acl_conn_cmpl_stat.handle = handle;
  emit(ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT, param);
}
void disconnect(int reason, uint16_t handle, bool matches = true) {
  esp_bt_gap_cb_param_t param{};
  if (matches) std::memcpy(param.acl_disconn_cmpl_stat.bda, target, sizeof(target));
  param.acl_disconn_cmpl_stat.reason = reason;
  param.acl_disconn_cmpl_stat.handle = handle;
  emit(ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT, param);
}
}

extern "C" esp_err_t __real_esp_bt_gap_register_callback(esp_bt_gap_cb_t callback) {
  registrationArgument = callback;
  if (!callback) return ESP_ERR_INVALID_ARG;
  if (registrationResult != ESP_OK) return registrationResult;
  registered = callback;
  // Even an event delivered during registration must reach the delegate.
  esp_bt_gap_cb_param_t param{};
  emit(ESP_BT_GAP_DISC_STATE_CHANGED_EVT, param);
  return ESP_OK;
}

int main() {
  assert(__wrap_esp_bt_gap_register_callback(original) == ESP_OK);
  assert(registered == btDiagnostics::gap);
  assert(btDiagnostics::lastConnectStatus.load() == -1);
  assert(btDiagnostics::lastDisconnectReason.load() == -1);
  esp_bt_gap_cb_param_t param{};
  for (auto event : {ESP_BT_GAP_DISC_RES_EVT, ESP_BT_GAP_DISC_STATE_CHANGED_EVT,
                     ESP_BT_GAP_AUTH_CMPL_EVT, ESP_BT_GAP_CFM_REQ_EVT}) {
    emit(event, param);
  }

  connect(260, 0xffff, false);
  assert(btDiagnostics::connectCount.load() == 0);
  testMillis = 100;
  connect(260, 0xffff);
  disconnect(260, 0xffff);
  assert(btDiagnostics::connectCount.load() == 1);
  assert(btDiagnostics::lastConnectStatus.load() == 260);
  assert(btDiagnostics::lastConnectAtMs.load() == 100);
  assert(btDiagnostics::lastDisconnectReason.load() == -1);

  testMillis = 200;
  connect(ESP_BT_STATUS_HCI_SUCCESS, 7);
  disconnect(0x108, 7, false);
  disconnect(0x108, 8);
  assert(btDiagnostics::lastDisconnectReason.load() == -1);
  testMillis = 300;
  disconnect(0x108, 7);
  assert(btDiagnostics::lastDisconnectReason.load() == 0x108);
  assert(btDiagnostics::lastDisconnectAtMs.load() == 300);

  testMillis = 400;
  connect(260, 0xffff);
  disconnect(260, 7);
  assert(btDiagnostics::connectCount.load() == 3);
  assert(btDiagnostics::lastConnectAtMs.load() == 400);
  assert(btDiagnostics::lastDisconnectReason.load() == 0x108);
  assert(btDiagnostics::lastDisconnectAtMs.load() == 300);
  testMillis = 500;
  connect(ESP_BT_STATUS_HCI_SUCCESS, 8);
  assert(btDiagnostics::lastDisconnectReason.load() == 0x108);
  testMillis = 600;
  disconnect(0x113, 8);
  assert(btDiagnostics::lastDisconnectReason.load() == 0x113);
  assert(btDiagnostics::lastDisconnectAtMs.load() == 600);

  registrationResult = 1;
  assert(__wrap_esp_bt_gap_register_callback(replacement) == 1);
  emit(ESP_BT_GAP_CFM_REQ_EVT, param);
  assert(__wrap_esp_bt_gap_register_callback(nullptr) == ESP_ERR_INVALID_ARG);
  assert(registrationArgument == nullptr);
  emit(ESP_BT_GAP_AUTH_CMPL_EVT, param);
}
