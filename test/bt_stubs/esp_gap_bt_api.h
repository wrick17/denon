#pragma once
#include <cstdint>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr int ESP_BT_STATUS_HCI_SUCCESS = 0x100;
enum esp_bt_gap_cb_event_t {
  ESP_BT_GAP_DISC_RES_EVT,
  ESP_BT_GAP_DISC_STATE_CHANGED_EVT,
  ESP_BT_GAP_AUTH_CMPL_EVT,
  ESP_BT_GAP_CFM_REQ_EVT,
  ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT,
  ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT,
};
union esp_bt_gap_cb_param_t {
  struct { int stat; uint16_t handle; uint8_t bda[6]; } acl_conn_cmpl_stat;
  struct { int reason; uint16_t handle; uint8_t bda[6]; } acl_disconn_cmpl_stat;
};
using esp_bt_gap_cb_t = void (*)(esp_bt_gap_cb_event_t, esp_bt_gap_cb_param_t *);
