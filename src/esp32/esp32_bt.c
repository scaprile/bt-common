/*
 * Copyright (c) 2014-2017 Cesanta Software Limited
 * All rights reserved
 */

#include "esp32_bt.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "bt.h"
#include "bta_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatts_api.h"

#include "common/cs_dbg.h"
#include "common/queue.h"

#include "fw/src/mgos_sys_config.h"
#include "fw/src/mgos_timers.h"
#include "fw/src/mgos_utils.h"
#include "fw/src/mgos_wifi.h"

struct esp32_bt_service_entry {
  const esp_gatts_attr_db_t *svc_descr;
  size_t num_attrs;
  bool registered;
  mgos_bt_gatts_handler_t cb;
  void *cb_arg;
  uint16_t *attr_handles;
  SLIST_ENTRY(esp32_bt_service_entry) next;
};

struct esp32_bt_session_entry {
  struct esp32_bt_session bs;
  struct esp32_bt_service_entry *se;
  SLIST_ENTRY(esp32_bt_session_entry) next;
};

struct esp32_bt_connection_entry {
  struct esp32_bt_connection bc;
  SLIST_HEAD(sessions, esp32_bt_session_entry) sessions;
  SLIST_ENTRY(esp32_bt_connection_entry) next;
};

static SLIST_HEAD(s_svcs, esp32_bt_service_entry) s_svcs =
    SLIST_HEAD_INITIALIZER(s_svcs);
static SLIST_HEAD(s_conns, esp32_bt_connection_entry) s_conns =
    SLIST_HEAD_INITIALIZER(s_conns);

static const char *s_dev_name = NULL;
static bool s_gatts_registered = false;
static esp_gatt_if_t s_gatts_if;

const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
const uint16_t char_decl_uuid = ESP_GATT_UUID_CHAR_DECLARE;
const uint8_t char_prop_read_write =
    (ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE);
const uint8_t char_prop_read_notify =
    (ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY);
const uint8_t char_prop_write = (ESP_GATT_CHAR_PROP_BIT_WRITE);

static esp_ble_adv_data_t mos_rpc_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x20,
    .max_interval = 0x40,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t mos_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

const char *mgos_bt_addr_to_str(const esp_bd_addr_t bda, char *out) {
  sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", bda[0], bda[1], bda[2], bda[3],
          bda[4], bda[5]);
  return out;
}

const char *bt_uuid128_to_str(const uint8_t *u, char *out) {
  sprintf(out,
          "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
          "%02x%02x%02x%02x%02x%02x",
          u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8], u[7], u[6],
          u[5], u[4], u[3], u[2], u[1], u[0]);
  return out;
}

const char *mgos_bt_uuid_to_str(const esp_bt_uuid_t *uuid, char *out) {
  switch (uuid->len) {
    case ESP_UUID_LEN_16: {
      sprintf(out, "%04x", uuid->uuid.uuid16);
      break;
    }
    case ESP_UUID_LEN_32: {
      sprintf(out, "%08x", uuid->uuid.uuid32);
      break;
    }
    case ESP_UUID_LEN_128: {
      bt_uuid128_to_str(uuid->uuid.uuid128, out);
      break;
    }
    default: { sprintf(out, "?(%u)", uuid->len); }
  }
  return out;
}

static void esp32_bt_register_services(void) {
  struct esp32_bt_service_entry *se;
  if (!s_gatts_registered) return;
  SLIST_FOREACH(se, &s_svcs, next) {
    if (se->registered) continue;
    esp_err_t r = esp_ble_gatts_create_attr_tab(se->svc_descr, s_gatts_if,
                                                se->num_attrs, 0);
    LOG(LL_DEBUG, ("esp_ble_gatts_create_attr_tab %d", r));
    se->registered = true;
  }
}

static struct esp32_bt_service_entry *find_service_by_uuid(
    const esp_bt_uuid_t *uuid) {
  struct esp32_bt_service_entry *se;
  SLIST_FOREACH(se, &s_svcs, next) {
    if (se->svc_descr[0].att_desc.length == uuid->len &&
        memcmp(se->svc_descr[0].att_desc.value, uuid->uuid.uuid128,
               uuid->len) == 0) {
      return se;
    }
  }
  return NULL;
}

static struct esp32_bt_service_entry *find_service_by_attr_handle(
    uint16_t attr_handle) {
  struct esp32_bt_service_entry *se;
  SLIST_FOREACH(se, &s_svcs, next) {
    for (size_t i = 0; i < se->num_attrs; i++) {
      if (se->attr_handles[i] == attr_handle) return se;
    }
  }
  return NULL;
}

static struct esp32_bt_connection_entry *find_connection(esp_gatt_if_t gatt_if,
                                                         uint16_t conn_id) {
  struct esp32_bt_connection_entry *ce = NULL;
  SLIST_FOREACH(ce, &s_conns, next) {
    if (ce->bc.gatt_if == gatt_if && ce->bc.conn_id == conn_id) return ce;
  }
  return NULL;
}

static void esp32_bt_gatts_ev(esp_gatts_cb_event_t ev, esp_gatt_if_t gatt_if,
                              esp_ble_gatts_cb_param_t *ep) {
  esp_err_t r;
  char buf[BT_UUID_STR_LEN];
  switch (ev) {
    case ESP_GATTS_REG_EVT: {
      const struct gatts_reg_evt_param *p = &ep->reg;
      LOG(LL_DEBUG, ("REG if %d st %d app %d", gatt_if, p->status, p->app_id));
      esp_ble_gap_set_device_name(s_dev_name);
      r = esp_ble_gap_config_adv_data(&mos_rpc_adv_data);
      LOG(LL_DEBUG, ("esp_ble_gap_config_adv_data %d", r));
      s_gatts_if = gatt_if;
      s_gatts_registered = true;
      esp32_bt_register_services();
      break;
    }
    case ESP_GATTS_READ_EVT: {
      const struct gatts_read_evt_param *p = &ep->read;
      LOG(LL_DEBUG, ("READ %s cid %d tid 0x%08x h %u off %d%s%s",
                     mgos_bt_addr_to_str(p->bda, buf), p->conn_id, p->trans_id,
                     p->handle, p->offset, (p->is_long ? " long" : ""),
                     (p->need_rsp ? " need_rsp" : "")));
      if (!p->need_rsp) break;
      bool ret = false;
      struct esp32_bt_connection_entry *ce =
          find_connection(gatt_if, p->conn_id);
      if (ce != NULL) {
        struct esp32_bt_service_entry *se =
            find_service_by_attr_handle(p->handle);
        struct esp32_bt_session_entry *sse;
        SLIST_FOREACH(sse, &ce->sessions, next) {
          if (sse->se == se) {
            ret = sse->se->cb(&sse->bs, ev, ep);
          }
        }
      }
      if (!ret) {
        esp_ble_gatts_send_response(gatt_if, p->conn_id, p->trans_id,
                                    ESP_GATT_READ_NOT_PERMIT, NULL);
      } else {
        /* Response was sent by the callback. */
      }
      break;
    }
    case ESP_GATTS_WRITE_EVT: {
      const struct gatts_write_evt_param *p = &ep->write;
      LOG(LL_DEBUG, ("WRITE %s cid %d tid 0x%08x h %u off %d len %d%s%s",
                     mgos_bt_addr_to_str(p->bda, buf), p->conn_id, p->trans_id,
                     p->handle, p->offset, p->len, (p->is_prep ? " prep" : ""),
                     (p->need_rsp ? " need_rsp" : "")));
      if (!p->need_rsp) break;
      bool ret = false;
      struct esp32_bt_connection_entry *ce =
          find_connection(gatt_if, p->conn_id);
      if (ce != NULL) {
        struct esp32_bt_service_entry *se =
            find_service_by_attr_handle(p->handle);
        struct esp32_bt_session_entry *sse;
        SLIST_FOREACH(sse, &ce->sessions, next) {
          if (sse->se == se) {
            ret = sse->se->cb(&sse->bs, ev, ep);
          }
        }
      }
      esp_ble_gatts_send_response(
          gatt_if, p->conn_id, p->trans_id,
          (ret ? ESP_GATT_OK : ESP_GATT_WRITE_NOT_PERMIT), NULL);
      break;
    }
    case ESP_GATTS_EXEC_WRITE_EVT: {
      const struct gatts_exec_write_evt_param *p = &ep->exec_write;
      LOG(LL_DEBUG, ("EXEC_WRITE %s cid %d tid 0x%08x flag %d",
                     mgos_bt_addr_to_str(p->bda, buf), p->conn_id, p->trans_id,
                     p->exec_write_flag));
      break;
    }
    case ESP_GATTS_MTU_EVT: {
      const struct gatts_mtu_evt_param *p = &ep->mtu;
      LOG(LL_DEBUG, ("MTU cid %d mtu %d", p->conn_id, p->mtu));
      struct esp32_bt_connection_entry *ce =
          find_connection(gatt_if, p->conn_id);
      if (ce != NULL) ce->bc.mtu = p->mtu;
      break;
    }
    case ESP_GATTS_CONF_EVT: {
      const struct gatts_conf_evt_param *p = &ep->conf;
      LOG(LL_DEBUG, ("CONF cid %d st %d", p->conn_id, p->status));
      break;
    }
    case ESP_GATTS_UNREG_EVT: {
      LOG(LL_DEBUG, ("UNREG"));
      break;
    }
    case ESP_GATTS_CREATE_EVT: {
      const struct gatts_create_evt_param *p = &ep->create;
      LOG(LL_DEBUG,
          ("CREATE st %d svch %d svcid %s %d%s", p->status, p->service_handle,
           mgos_bt_uuid_to_str(&p->service_id.id.uuid, buf),
           p->service_id.id.inst_id,
           (p->service_id.is_primary ? " primary" : "")));
      break;
    }
    case ESP_GATTS_ADD_INCL_SRVC_EVT: {
      const struct gatts_add_incl_srvc_evt_param *p = &ep->add_incl_srvc;
      LOG(LL_DEBUG, ("ADD_INCL_SRVC st %d ah %u svch %u", p->status,
                     p->attr_handle, p->service_handle));
      break;
    }
    case ESP_GATTS_ADD_CHAR_EVT: {
      const struct gatts_add_char_evt_param *p = &ep->add_char;
      LOG(LL_DEBUG,
          ("ADD_CHAR st %d ah %u svch %u uuid %s", p->status, p->attr_handle,
           p->service_handle, mgos_bt_uuid_to_str(&p->char_uuid, buf)));
      break;
    }
    case ESP_GATTS_ADD_CHAR_DESCR_EVT: {
      const struct gatts_add_char_descr_evt_param *p = &ep->add_char_descr;
      LOG(LL_DEBUG, ("ADD_CHAR_DESCR st %d ah %u svch %u uuid %s", p->status,
                     p->attr_handle, p->service_handle,
                     mgos_bt_uuid_to_str(&p->char_uuid, buf)));
      break;
    }
    case ESP_GATTS_DELETE_EVT: {
      const struct gatts_delete_evt_param *p = &ep->del;
      LOG(LL_DEBUG, ("DELETE st %d svch %u", p->status, p->service_handle));
      break;
    }
    case ESP_GATTS_START_EVT: {
      const struct gatts_start_evt_param *p = &ep->start;
      LOG(LL_DEBUG, ("START st %d svch %u", p->status, p->service_handle));
      break;
    }
    case ESP_GATTS_STOP_EVT: {
      const struct gatts_stop_evt_param *p = &ep->stop;
      LOG(LL_DEBUG, ("STOP st %d svch %u", p->status, p->service_handle));
      break;
    }
    case ESP_GATTS_CONNECT_EVT: {
      const struct gatts_connect_evt_param *p = &ep->connect;
      LOG(LL_INFO, ("CONNECT cid %d addr %s%s", p->conn_id,
                    mgos_bt_addr_to_str(p->remote_bda, buf),
                    (p->is_connected ? " connected" : "")));
      if (!p->is_connected) break;
      esp_ble_conn_update_params_t conn_params = {0};
      memcpy(conn_params.bda, p->remote_bda, ESP_BD_ADDR_LEN);
      conn_params.latency = 0;
      conn_params.max_int = 0x50; /* max_int = 0x50*1.25ms = 100ms */
      conn_params.min_int = 0x30; /* min_int = 0x30*1.25ms = 60ms */
      conn_params.timeout = 400;  /* timeout = 400*10ms = 4000ms */
      esp_ble_gap_update_conn_params(&conn_params);
      /* Resume advertising */
      if (get_cfg()->bt.adv_enable) {
        esp_ble_gap_start_advertising(&mos_adv_params);
      }
      struct esp32_bt_connection_entry *ce =
          (struct esp32_bt_connection_entry *) calloc(1, sizeof(*ce));
      ce->bc.gatt_if = gatt_if;
      ce->bc.conn_id = p->conn_id;
      ce->bc.mtu = ESP_GATT_DEF_BLE_MTU_SIZE;
      memcpy(ce->bc.peer_addr, p->remote_bda, ESP_BD_ADDR_LEN);
      /* Create a session for each of the currently registered services. */
      struct esp32_bt_service_entry *se;
      SLIST_FOREACH(se, &s_svcs, next) {
        struct esp32_bt_session_entry *sse =
            (struct esp32_bt_session_entry *) calloc(1, sizeof(*sse));
        sse->se = se;
        sse->bs.bc = &ce->bc;
        SLIST_INSERT_HEAD(&ce->sessions, sse, next);
        se->cb(&sse->bs, ev, ep);
      }
      SLIST_INSERT_HEAD(&s_conns, ce, next);
      break;
    }
    case ESP_GATTS_DISCONNECT_EVT: {
      const struct gatts_disconnect_evt_param *p = &ep->disconnect;
      LOG(LL_INFO, ("DISCONNECT cid %d addr %s%s", p->conn_id,
                    mgos_bt_addr_to_str(p->remote_bda, buf),
                    (p->is_connected ? " connected" : "")));

      struct esp32_bt_connection_entry *ce =
          find_connection(gatt_if, p->conn_id);
      if (ce != NULL) {
        struct esp32_bt_session_entry *sse, *sset;
        SLIST_FOREACH_SAFE(sse, &ce->sessions, next, sset) {
          sse->se->cb(&sse->bs, ev, ep);
          free(sse);
        }
        SLIST_REMOVE(&s_conns, ce, esp32_bt_connection_entry, next);
        free(ce);
      }
      if (get_cfg()->bt.adv_enable) {
        esp_ble_gap_start_advertising(&mos_adv_params);
      }
      break;
    }
    case ESP_GATTS_OPEN_EVT: {
      const struct gatts_open_evt_param *p = &ep->open;
      LOG(LL_DEBUG, ("OPEN st %d", p->status));
      break;
    }
    case ESP_GATTS_CANCEL_OPEN_EVT: {
      const struct gatts_cancel_open_evt_param *p = &ep->cancel_open;
      LOG(LL_DEBUG, ("CANCEL_OPEN st %d", p->status));
      break;
    }
    case ESP_GATTS_CLOSE_EVT: {
      const struct gatts_close_evt_param *p = &ep->close;
      LOG(LL_DEBUG, ("CLOSE st %d cid %d", p->status, p->conn_id));
      break;
    }
    case ESP_GATTS_LISTEN_EVT: {
      LOG(LL_DEBUG, ("LISTEN"));
      break;
    }
    case ESP_GATTS_CONGEST_EVT: {
      const struct gatts_congest_evt_param *p = &ep->congest;
      LOG(LL_DEBUG,
          ("CONGEST cid %d%s", p->conn_id, (p->congested ? " congested" : "")));
      break;
    }
    case ESP_GATTS_RESPONSE_EVT: {
      const struct gatts_rsp_evt_param *p = &ep->rsp;
      LOG(LL_DEBUG, ("RESPONSE st %d ah %d", p->status, p->handle));
      break;
    }
    case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
      const struct gatts_add_attr_tab_evt_param *p = &ep->add_attr_tab;
      LOG(LL_DEBUG,
          ("CREAT_ATTR_TAB st %d svc_uuid %s nh %d hh %p", p->status,
           mgos_bt_uuid_to_str(&p->svc_uuid, buf), p->num_handle, p->handles));
      if (p->status != 0) {
        LOG(LL_ERROR,
            ("Failed to register service attribute table: %d", p->status));
        break;
      }
      struct esp32_bt_service_entry *se = find_service_by_uuid(&p->svc_uuid);
      if (se == NULL || se->num_attrs != p->num_handle) break;
      se->attr_handles =
          (uint16_t *) calloc(p->num_handle, sizeof(*se->attr_handles));
      memcpy(se->attr_handles, p->handles,
             p->num_handle * sizeof(*se->attr_handles));
      se->cb(NULL, ev, ep);
      uint16_t svch = se->attr_handles[0];
      LOG(LL_INFO,
          ("Starting BT service %s", mgos_bt_uuid_to_str(&p->svc_uuid, buf)));
      esp_ble_gatts_start_service(svch);
      break;
    }
    case ESP_GATTS_SET_ATTR_VAL_EVT: {
      const struct gatts_set_attr_val_evt_param *p = &ep->set_attr_val;
      LOG(LL_DEBUG, ("SET_ATTR_VAL sh %d ah %d st %d", p->srvc_handle,
                     p->attr_handle, p->status));
      break;
    }
  }
}

static void esp32_bt_gap_ev(esp_gap_ble_cb_event_t ev,
                            esp_ble_gap_cb_param_t *ep) {
  switch (ev) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT: {
      const struct ble_adv_data_cmpl_evt_param *p = &ep->adv_data_cmpl;
      LOG(LL_DEBUG, ("ADV_DATA_SET_COMPLETE st %d", p->status));
      esp_ble_gap_start_advertising(&mos_adv_params);
      break;
    }
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT: {
      const struct ble_scan_rsp_data_cmpl_evt_param *p =
          &ep->scan_rsp_data_cmpl;
      LOG(LL_DEBUG, ("SCAN_RSP_DATA_SET_COMPLETE st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
      const struct ble_scan_param_cmpl_evt_param *p = &ep->scan_param_cmpl;
      LOG(LL_DEBUG, ("SCAN_PARAM_SET_COMPLETE st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      LOG(LL_DEBUG, ("SCAN_RESULT"));
      break;
    }
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: {
      const struct ble_adv_data_raw_cmpl_evt_param *p = &ep->adv_data_raw_cmpl;
      LOG(LL_DEBUG, ("ADV_DATA_RAW_SET_COMPLETE st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT: {
      const struct ble_scan_rsp_data_raw_cmpl_evt_param *p =
          &ep->scan_rsp_data_raw_cmpl;
      LOG(LL_DEBUG, ("SCAN_RSP_DATA_RAW_SET_COMPLETE st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT: {
      const struct ble_adv_start_cmpl_evt_param *p = &ep->adv_start_cmpl;
      LOG(LL_DEBUG, ("ADV_START_COMPLETE st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: {
      const struct ble_scan_start_cmpl_evt_param *p = &ep->scan_start_cmpl;
      LOG(LL_DEBUG, ("SCAN_START_COMPLETE st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      LOG(LL_DEBUG, ("AUTH_CMPL"));
      break;
    }
    case ESP_GAP_BLE_KEY_EVT: {
      LOG(LL_DEBUG, ("KEY"));
      break;
    }
    case ESP_GAP_BLE_SEC_REQ_EVT: {
      LOG(LL_DEBUG, ("SEC_REQ"));
      break;
    }
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: {
      LOG(LL_DEBUG, ("PASSKEY_NOTIF"));
      break;
    }
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: {
      LOG(LL_DEBUG, ("PASSKEY_REQ"));
      break;
    }
    case ESP_GAP_BLE_OOB_REQ_EVT: {
      LOG(LL_DEBUG, ("OOB_REQ"));
      break;
    }
    case ESP_GAP_BLE_LOCAL_IR_EVT: {
      LOG(LL_DEBUG, ("LOCAL_IR"));
      break;
    }
    case ESP_GAP_BLE_LOCAL_ER_EVT: {
      LOG(LL_DEBUG, ("LOCAL_ER"));
      break;
    }
    case ESP_GAP_BLE_NC_REQ_EVT: {
      LOG(LL_DEBUG, ("NC_REQ"));
      break;
    }
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT: {
      const struct ble_adv_stop_cmpl_evt_param *p = &ep->adv_stop_cmpl;
      LOG(LL_DEBUG, ("ADV_STOP_COMPLETE st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: {
      const struct ble_scan_stop_cmpl_evt_param *p = &ep->scan_stop_cmpl;
      LOG(LL_DEBUG, ("SCAN_STOP_COMPLETE st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT: {
      const struct ble_set_rand_cmpl_evt_param *p = &ep->set_rand_addr_cmpl;
      LOG(LL_DEBUG, ("SET_STATIC_RAND_ADDR st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: {
      const struct ble_update_conn_params_evt_param *p =
          &ep->update_conn_params;
      LOG(LL_DEBUG, ("UPDATE_CONN_PARAMS st %d", p->status));
      break;
    }
    case ESP_GAP_BLE_SET_PKT_LENGTH_COMPLETE_EVT: {
      const struct ble_pkt_data_length_cmpl_evt_param *p =
          &ep->pkt_data_lenth_cmpl;
      LOG(LL_DEBUG, ("SET_PKT_LENGTH_COMPLETE st %d rx_len %u tx_len %u",
                     p->status, p->params.rx_len, p->params.tx_len));
      break;
    }
  }
}

bool mgos_bt_gatts_register_service(const esp_gatts_attr_db_t *svc_descr,
                                    size_t num_attrs,
                                    mgos_bt_gatts_handler_t cb) {
  if (cb == NULL) return false;
  struct esp32_bt_service_entry *se =
      (struct esp32_bt_service_entry *) calloc(1, sizeof(*se));
  if (se == NULL) return false;
  se->svc_descr = svc_descr;
  se->num_attrs = num_attrs;
  se->cb = cb;
  SLIST_INSERT_HEAD(&s_svcs, se, next);
  esp32_bt_register_services();
  return true;
}

static void mgos_bt_wifi_changed_cb(enum mgos_wifi_status ev, void *arg) {
  if (ev != MGOS_WIFI_IP_ACQUIRED) return;
  LOG(LL_INFO, ("WiFi connected, disabling Bluetooth"));
  get_cfg()->bt.enable = false;
  char *msg = NULL;
  if (save_cfg(get_cfg(), &msg)) {
    esp_bt_controller_disable(ESP_BT_MODE_BTDM);
  }
  (void) arg;
}

bool mgos_bt_common_init(void) {
  bool ret = false;
  const struct sys_config *cfg = get_cfg();
  const struct sys_config_bt *btcfg = &cfg->bt;
  if (!btcfg->enable) {
    LOG(LL_INFO, ("Bluetooth is disabled"));
    return true;
  }

  if (!btcfg->keep_enabled) {
    mgos_wifi_add_on_change_cb(mgos_bt_wifi_changed_cb, NULL);
  }

  const char *dev_name = btcfg->dev_name;
  if (dev_name == NULL) dev_name = cfg->device.id;
  if (dev_name == NULL) {
    LOG(LL_ERROR, ("bt.dev_name or device.id must be set"));
    return false;
  }

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_bt_controller_init(&bt_cfg);
  if (err) {
    LOG(LL_ERROR, ("BT init failed: %d", err));
    goto out;
  }
  err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
  if (err) {
    LOG(LL_ERROR, ("BT enable failed: %d", err));
    goto out;
  }
  err = esp_bluedroid_init();
  if (err != ESP_OK) {
    LOG(LL_ERROR, ("bluedroid init failed: %d", err));
    goto out;
  }
  err = esp_bluedroid_enable();
  if (err != ESP_OK) {
    LOG(LL_ERROR, ("bluedroid enable failed: %d", err));
    goto out;
  }

  s_dev_name = dev_name;
  esp_ble_gatts_register_callback(esp32_bt_gatts_ev);
  esp_ble_gap_register_callback(esp32_bt_gap_ev);
  esp_ble_gatts_app_register(0);

  LOG(LL_INFO, ("Bluetooth init ok, advertising %s",
                (btcfg->adv_enable ? "enabled" : "disabled")));
  ret = true;

out:
  return ret;
}
