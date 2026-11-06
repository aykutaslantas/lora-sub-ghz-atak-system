/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "esp_log.h"
#include "nvs_flash.h"
/* BLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_spp_server.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <stdlib.h>
#include <string.h>

/* ---- LoRa node -> ESP32 -> telefon kopru ayarlari ----
 * STM32 node, aldigi "kim nerede" tablosunu USART1 (NUCLEO D1/PB6) ile
 * [AA][55][LEN][payload][CRC8] cercevesi olarak yollar. Burada UART1 ile alinir,
 * CRC kontrol edilir ve payload (D1 N kayitlar) BLE notify ile telefona iletilir.
 * Kablo: NUCLEO D1 -> ESP32-C3 GPIO4, GND <-> GND (ikisi de 3.3 V). */
#define NODE_UART_NUM        UART_NUM_1
#define NODE_UART_RX_PIN     4          /* NUCLEO D1 (PB6) buraya */
#define NODE_UART_TX_PIN     5          /* -> NUCLEO D0 (PB7): telefondan gelen komutlar (Faz 2) */
#define NODE_UART_BAUD       115200
#define DL_TYPE_TABLE        0xD1       /* STM32 tarafiyla ayni */
#define POS_RECORD_SIZE      12         /* pos_packet_t */
#define CMD_TYPE             0xC1       /* Faz 2: isaret / komut paketi (cmd_packet_t) */
#define CMD_TYPE_DEL_REQ     0xC2       /* silme istegi (telefon -> admin) */
#define CMD_TYPE_DEL_ACK     0xC3       /* admin karari (admin -> herkes) */
#define CMD_TYPE_SET_POS     0xC4       /* telefonun konumu -> SADECE bagli node (LoRa'ya cikmaz) */
#define CMD_TYPE_ACK         0xC5       /* telefon: admin komutu OKUNDU -> node -> admin */
#define CMD_TYPE_NODE_STATUS 0xC6       /* node -> telefon: tabloda var miyim (dolayli onay) */
#define CMD_TYPE_JOIN_PARAMS 0xC9       /* telefon -> node: takim / numara / PIN (katilma) */
#define CMD_TYPE_LEAVE       0xCC       /* telefon -> node: takimdan ayril (node admin'e 0xA4 yollar) */
#define CMD_TYPE_JOIN_STATUS 0xCB       /* node -> telefon: katilma durumu (8 B) */
#define CMD_PACKET_LEN       15
static const char *BRIDGE_TAG = "BRIDGE";
static void phone_to_node(const uint8_t *p, uint16_t len);

static int ble_spp_server_gap_event(struct ble_gap_event *event, void *arg);
static uint8_t own_addr_type;
int gatt_svr_register(void);
QueueHandle_t spp_common_uart_queue = NULL;
static bool conn_handle_subs[CONFIG_BT_NIMBLE_MAX_CONNECTIONS + 1];
static uint16_t ble_spp_svc_gatt_read_val_handle;

void ble_store_config_init(void);

/**
 * Logs information about a connection to the console.
 */
static void
ble_spp_server_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    MODLOG_DFLT(INFO, "handle=%d our_ota_addr_type=%d our_ota_addr=",
                desc->conn_handle, desc->our_ota_addr.type);
    print_addr(desc->our_ota_addr.val);
    MODLOG_DFLT(INFO, " our_id_addr_type=%d our_id_addr=",
                desc->our_id_addr.type);
    print_addr(desc->our_id_addr.val);
    MODLOG_DFLT(INFO, " peer_ota_addr_type=%d peer_ota_addr=",
                desc->peer_ota_addr.type);
    print_addr(desc->peer_ota_addr.val);
    MODLOG_DFLT(INFO, " peer_id_addr_type=%d peer_id_addr=",
                desc->peer_id_addr.type);
    print_addr(desc->peer_id_addr.val);
    MODLOG_DFLT(INFO, " conn_itvl=%d conn_latency=%d supervision_timeout=%d "
                "encrypted=%d authenticated=%d bonded=%d\n",
                desc->conn_itvl, desc->conn_latency,
                desc->supervision_timeout,
                desc->sec_state.encrypted,
                desc->sec_state.authenticated,
                desc->sec_state.bonded);
}

/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static void
ble_spp_server_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    /**
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info).
     *     o Advertising tx power.
     *     o Device name.
     *     o 16-bit service UUIDs (alert notifications).
     */

    memset(&fields, 0, sizeof fields);

    /* Advertise two flags:
     *     o Discoverability in forthcoming advertisement (general)
     *     o BLE-only (BR/EDR unsupported).
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;

    /* Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name;
    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.uuids16 = (ble_uuid16_t[]) {
        BLE_UUID16_INIT(BLE_SVC_SPP_UUID16)
    };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
        return;
    }

    /* Begin advertising. */
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_spp_server_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
        return;
    }
}

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that forms.
 * ble_spp_server uses the same callback for all connections.
 *
 * @param event                 The type of event being signalled.
 * @param ctxt                  Various information pertaining to the event.
 * @param arg                   Application-specified argument; unused by
 *                                  ble_spp_server.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int
ble_spp_server_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        MODLOG_DFLT(INFO, "connection %s; status=%d ",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            ble_spp_server_print_conn_desc(&desc);
        }
        MODLOG_DFLT(INFO, "\n");
        if (event->connect.status != 0 || CONFIG_BT_NIMBLE_MAX_CONNECTIONS > 1) {
            /* Connection failed or if multiple connection allowed; resume advertising. */
            ble_spp_server_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
        ble_spp_server_print_conn_desc(&event->disconnect.conn);
        MODLOG_DFLT(INFO, "\n");

        if (event->disconnect.conn.conn_handle <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            conn_handle_subs[event->disconnect.conn.conn_handle] = false;
        }

        /* Connection terminated; resume advertising. */
        ble_spp_server_advertise();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters. */
        MODLOG_DFLT(INFO, "connection updated; status=%d ",
                    event->conn_update.status);
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        assert(rc == 0);
        ble_spp_server_print_conn_desc(&desc);
        MODLOG_DFLT(INFO, "\n");
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        MODLOG_DFLT(INFO, "advertise complete; reason=%d",
                    event->adv_complete.reason);
        ble_spp_server_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        MODLOG_DFLT(INFO, "subscribe event; conn_handle=%d attr_handle=%d "
                    "reason=%d prevn=%d curn=%d previ=%d curi=%d\n",
                    event->subscribe.conn_handle,
                    event->subscribe.attr_handle,
                    event->subscribe.reason,
                    event->subscribe.prev_notify,
                    event->subscribe.cur_notify,
                    event->subscribe.prev_indicate,
                    event->subscribe.cur_indicate);
        if (event->subscribe.conn_handle <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            conn_handle_subs[event->subscribe.conn_handle] = true;
        }
        return 0;

    default:
        return 0;
    }
}

static void
ble_spp_server_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

static void
ble_spp_server_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Printing ADDR */
    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);

    MODLOG_DFLT(INFO, "Device Address: ");
    print_addr(addr_val);
    MODLOG_DFLT(INFO, "\n");
    /* Begin advertising. */
    ble_spp_server_advertise();
}

void ble_spp_server_host_task(void *param)
{
    MODLOG_DFLT(INFO, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

/* Callback function for custom service */
static int  ble_svc_gatt_handler(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        MODLOG_DFLT(INFO, "Callback for read");
        break;

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        /* Telefon 0xABF1'e yazdi (Faz 2: isaret / komut) -> UART ile node'a */
        uint8_t buf[32];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
            ESP_LOGW(BRIDGE_TAG, "telefondan cok uzun yazma, atildi");
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        phone_to_node(buf, len);
        break;
    }

    default:
        MODLOG_DFLT(INFO, "\nDefault Callback");
        break;
    }
    return 0;
}

/* Define new custom service */
static const struct ble_gatt_svc_def new_ble_svc_gatt_defs[] = {
    {
        /*** Service: SPP */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_SPP_UUID16),
        .characteristics = (struct ble_gatt_chr_def[])
        { {
                /* Support SPP service */
                .uuid = BLE_UUID16_DECLARE(BLE_SVC_SPP_CHR_UUID16),
                .access_cb = ble_svc_gatt_handler,
                .val_handle = &ble_spp_svc_gatt_read_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            }, {
                0, /* No more characteristics */
            }
        },
    },
    {
        0, /* No more services. */
    },
};

static void
gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        MODLOG_DFLT(DEBUG, "registered service %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        MODLOG_DFLT(DEBUG, "registering characteristic %s with "
                    "def_handle=%d val_handle=%d\n",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle,
                    ctxt->chr.val_handle);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        MODLOG_DFLT(DEBUG, "registering descriptor %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                    ctxt->dsc.handle);
        break;

    default:
        assert(0);
        break;
    }
}

int gatt_svr_init(void)
{
    int rc = 0;
#if CONFIG_BT_NIMBLE_GAP_SERVICE
    ble_svc_gap_init();
#endif
#if MYNEWT_VAL(BLE_GATTS)
    ble_svc_gatt_init();
#endif
    rc = ble_gatts_count_cfg(new_ble_svc_gatt_defs);

    if (rc != 0) {
        return rc;
    }

    rc = ble_gatts_add_svcs(new_ble_svc_gatt_defs);
    if (rc != 0) {
        return rc;
    }

    return 0;
}


void ble_server_uart_task(void *pvParameters)
{
    MODLOG_DFLT(INFO, "BLE server UART_task started\n");
    uart_event_t event;
    int rc = 0;
    for (;;) {
        //Waiting for UART event.
        if (xQueueReceive(spp_common_uart_queue, (void * )&event, (TickType_t)portMAX_DELAY))            {
            switch (event.type) {
            //Event of UART receiving data
            case UART_DATA:
                if (event.size) {
                    uint8_t *ntf;
                    ntf = (uint8_t *)malloc(sizeof(uint8_t) * event.size);
                    if (ntf == NULL) {
                        MODLOG_DFLT(ERROR, "malloc failed for UART data, size=%u", event.size);
                        continue;
                    }
                    memset(ntf, 0x00, event.size);
                    uart_read_bytes(UART_NUM_0, ntf, event.size, portMAX_DELAY);

                    for (int i = 0; i <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {
                        /* Check if client has subscribed to notifications */
                        if (conn_handle_subs[i]) {
                            struct os_mbuf *txom;
                            txom = ble_hs_mbuf_from_flat(ntf, event.size);
                            rc = ble_gatts_notify_custom(i, ble_spp_svc_gatt_read_val_handle,
                                                         txom);
                            if (rc == 0) {
                                MODLOG_DFLT(INFO, "Notification sent successfully");
                            } else {
                                MODLOG_DFLT(INFO, "Error in sending notification rc = %d", rc);
                            }
                        }
                    }

		    free(ntf);
                }
                break;
            default:
                break;
            }
        }
    }
    vTaskDelete(NULL);
}
/* CRC-8: polinom 0x07, init 0x00 (STM32 ve Zybo tarafiyla ayni) */
static uint8_t crc8_update(uint8_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* Faz 2: telefondan gelen komutu [AA][55][LEN][15 byte][CRC8] cercevesiyle
   UART1 TX (GPIO5) -> NUCLEO D0 (PB7) uzerinden node'a yolla */
static void phone_to_node(const uint8_t *p, uint16_t len)
{
    if (len != CMD_PACKET_LEN ||
        (p[0] != CMD_TYPE && p[0] != CMD_TYPE_DEL_REQ && p[0] != CMD_TYPE_SET_POS &&
         p[0] != CMD_TYPE_ACK && p[0] != CMD_TYPE_JOIN_PARAMS && p[0] != CMD_TYPE_LEAVE)) {
        ESP_LOGW(BRIDGE_TAG, "telefondan tanimsiz yazma: %u byte, tip=0x%02X",
                 len, len ? p[0] : 0);
        return;
    }
    uint8_t frame[3 + CMD_PACKET_LEN + 1];
    uint8_t crc = 0;
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = (uint8_t)len;
    memcpy(&frame[3], p, len);
    for (int i = 2; i < 3 + len; i++) {
        crc = crc8_update(crc, frame[i]);
    }
    frame[3 + len] = crc;
    uart_write_bytes(NODE_UART_NUM, frame, sizeof(frame));
    if (p[0] == CMD_TYPE) {
        ESP_LOGI(BRIDGE_TAG, "KOMUT telefon -> node: dst=0x%04X id=%u tur=%u",
                 (unsigned)(p[3] | (p[4] << 8)), p[5], p[6]);
    } else if (p[0] == CMD_TYPE_SET_POS) {
        ESP_LOGI(BRIDGE_TAG, "KONUM telefon -> node: %s", p[9] == 1 ? "ayarla" : "kapat");
    } else if (p[0] == CMD_TYPE_JOIN_PARAMS) {
        ESP_LOGI(BRIDGE_TAG, "KATILMA telefon -> node: takim=%u numara=%u", p[1], p[2]);
    } else if (p[0] == CMD_TYPE_LEAVE) {
        ESP_LOGI(BRIDGE_TAG, "TAKIMDAN AYRIL telefon -> node");
    } else if (p[0] == CMD_TYPE_ACK) {
        ESP_LOGI(BRIDGE_TAG, "OKUNDU telefon -> node: komut 0x%04X/%u",
                 (unsigned)(p[7] | (p[8] << 8)), p[9]);
    } else {
        ESP_LOGI(BRIDGE_TAG, "SILME ISTEGI telefon -> node: id=%u hedef=0x%04X/%u",
                 p[5], (unsigned)(p[7] | (p[8] << 8)), p[9]);
    }
}

/* Abone olan (notify acmis) butun telefonlara veriyi yolla */
static void ble_notify_all(const uint8_t *data, uint16_t len)
{
    int sent = 0;
    for (int i = 0; i <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {
        if (!conn_handle_subs[i]) {
            continue;
        }
        uint16_t mtu = ble_att_mtu(i);
        if (mtu != 0 && len > (uint16_t)(mtu - 3)) {
            ESP_LOGW(BRIDGE_TAG, "conn %d: MTU=%u kucuk, %u byte'in sadece %u'u gider. "
                     "Telefonda MTU'yu artir (nRF Connect: Request MTU)", i, mtu, len, mtu - 3);
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        int rc = ble_gatts_notify_custom(i, ble_spp_svc_gatt_read_val_handle, om);
        if (rc == 0) {
            sent++;
        } else {
            ESP_LOGW(BRIDGE_TAG, "conn %d: notify hatasi rc=%d", i, rc);
        }
    }
    if (sent == 0) {
        ESP_LOGI(BRIDGE_TAG, "bagli/abone telefon yok, veri atildi");
    }
}

/* Tabloyu okunur sekilde logla (hata ayiklama icin) */
static void log_table(const uint8_t *p, uint16_t len)
{
    uint8_t n = p[1];
    ESP_LOGI(BRIDGE_TAG, "TABLO %u node:", n);
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *r = &p[2 + i * POS_RECORD_SIZE];
        uint16_t addr = (uint16_t)(r[0] | (r[1] << 8));
        uint8_t  seq  = r[2];
        int32_t  lat  = (int32_t)((uint32_t)r[3] | ((uint32_t)r[4] << 8) | ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 24));
        int32_t  lon  = (int32_t)((uint32_t)r[7] | ((uint32_t)r[8] << 8) | ((uint32_t)r[9] << 16) | ((uint32_t)r[10] << 24));
        uint8_t  flg  = r[11];
        ESP_LOGI(BRIDGE_TAG, "  0x%04X seq=%3u lat=%s%ld.%07ld lon=%s%ld.%07ld fix=%d",
                 addr, seq,
                 lat < 0 ? "-" : "", (long)(labs(lat) / 10000000), (long)(labs(lat) % 10000000),
                 lon < 0 ? "-" : "", (long)(labs(lon) / 10000000), (long)(labs(lon) % 10000000),
                 flg & 0x01);
    }
}

/* Gecerli bir cerceve geldi: tablo ise logla, her durumda telefona ilet */
static void on_node_frame(const uint8_t *payload, uint16_t len)
{
    if (len >= 2 && payload[0] == DL_TYPE_TABLE && payload[1] >= 1 &&
        len == (uint16_t)(2 + POS_RECORD_SIZE * payload[1])) {
        log_table(payload, len);
    } else if (len == CMD_PACKET_LEN && payload[0] == CMD_TYPE_DEL_ACK) {
        ESP_LOGI(BRIDGE_TAG, "ADMIN KARARI -> telefon: %s hedef=0x%04X/%u istek_id=%u",
                 payload[6] == 1 ? "ONAY" : "RED",
                 (unsigned)(payload[7] | (payload[8] << 8)), payload[9], payload[10]);
    } else if (len == CMD_PACKET_LEN && payload[0] == CMD_TYPE) {
        ESP_LOGI(BRIDGE_TAG, "KOMUT agdan -> telefon: src=0x%04X dst=0x%04X id=%u tur=%u",
                 (unsigned)(payload[1] | (payload[2] << 8)),
                 (unsigned)(payload[3] | (payload[4] << 8)), payload[5], payload[6]);
    } else if (len == 8 && payload[0] == CMD_TYPE_NODE_STATUS) {
        ESP_LOGI(BRIDGE_TAG, "NODE DURUMU -> telefon: tabloda %s (seq %u, son yollanan %u)",
                 payload[4] ? "var" : "YOK", payload[5], payload[3]);
    } else if (len == 8 && payload[0] == CMD_TYPE_JOIN_STATUS) {
        ESP_LOGI(BRIDGE_TAG, "KATILMA DURUMU -> telefon: durum=%u sonuc=%u adres=0x%04X",
                 payload[1], payload[2], (unsigned)(payload[3] | (payload[4] << 8)));
    } else {
        ESP_LOGI(BRIDGE_TAG, "cerceve: %u byte, tip=0x%02X", len, len ? payload[0] : 0);
    }
    ble_notify_all(payload, len);
}

/* STM32 node'dan gelen UART1 akisini cozen gorev (STM32'deki FSM'in aynisi) */
static void node_uart_task(void *arg)
{
    enum { S_SYNC0, S_SYNC1, S_LEN, S_DATA, S_CRC } st = S_SYNC0;
    uint8_t  rx[64];
    uint8_t  payload[255];
    uint16_t plen = 0, pidx = 0;
    uint8_t  crc = 0;
    uint32_t crc_err = 0;

    ESP_LOGI(BRIDGE_TAG, "UART%d RX=GPIO%d %d baud dinleniyor", NODE_UART_NUM, NODE_UART_RX_PIN, NODE_UART_BAUD);
    for (;;) {
        int n = uart_read_bytes(NODE_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            uint8_t b = rx[i];
            switch (st) {
            case S_SYNC0:
                if (b == 0xAA) { st = S_SYNC1; }
                break;
            case S_SYNC1:
                if (b == 0x55)      { st = S_LEN; }
                else if (b != 0xAA) { st = S_SYNC0; }
                break;
            case S_LEN:
                if (b == 0) { st = S_SYNC0; break; }
                plen = b; pidx = 0;
                crc = crc8_update(0x00, b);
                st = S_DATA;
                break;
            case S_DATA:
                payload[pidx++] = b;
                crc = crc8_update(crc, b);
                if (pidx >= plen) { st = S_CRC; }
                break;
            case S_CRC:
                if (crc == b) {
                    on_node_frame(payload, plen);
                } else {
                    crc_err++;
                    ESP_LOGW(BRIDGE_TAG, "CRC hatasi (toplam %lu)", (unsigned long)crc_err);
                }
                st = S_SYNC0;
                break;
            }
        }
    }
}

static void node_uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = NODE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(NODE_UART_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(NODE_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(NODE_UART_NUM, NODE_UART_TX_PIN, NODE_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* Kablo takili degilken RX havada kalip cop uretmesin */
    gpio_pullup_en(NODE_UART_RX_PIN);
    xTaskCreate(node_uart_task, "nodeUart", 4096, NULL, 7, NULL);
}

/* Ornekten kalan UART0 (USB seri) -> BLE aktarimi. KAPATILDI: kartin "RX" pinine
   (GPIO20) gelen her seyi telefona yolluyordu, kablo yanlis pine takilinca kafa
   karistiriyordu. Bizim kopru UART1 (GPIO4/5) uzerinden calisiyor. */
#if 0
static void ble_spp_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_RTS,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    //Install UART driver, and get the queue.
    uart_driver_install(UART_NUM_0, 4096, 8192, 10, &spp_common_uart_queue, 0);
    //Set UART parameters
    uart_param_config(UART_NUM_0, &uart_config);
    //Set UART pins
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    xTaskCreate(ble_server_uart_task, "uTask", 4096, (void *)UART_NUM_0, 8, NULL);
}
#endif


void
app_main(void)
{
    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        MODLOG_DFLT(ERROR, "Failed to init nimble %d \n", ret);
        return;
    }

    /* Initialize connection_handle array */
    for (int i = 0; i <= CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {
        conn_handle_subs[i] = false;
    }

    /* (Ornegin UART0 aktarimi kapatildi, bkz. ble_spp_uart_init) */
    /* STM32 node -> ESP32 kopru UART'i (UART1, GPIO4) */
    node_uart_init();

    /* Initialize the NimBLE host configuration. */
    ble_hs_cfg.reset_cb = ble_spp_server_on_reset;
    ble_hs_cfg.sync_cb = ble_spp_server_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = CONFIG_EXAMPLE_IO_TYPE;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
#endif
#ifdef CONFIG_EXAMPLE_MITM
    ble_hs_cfg.sm_mitm = 1;
#endif
#ifdef CONFIG_EXAMPLE_USE_SC
    ble_hs_cfg.sm_sc = 1;
#else
    ble_hs_cfg.sm_sc = 0;
#endif
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_our_key_dist = 1;
    ble_hs_cfg.sm_their_key_dist = 1;
#endif

#if MYNEWT_VAL(BLE_GATTS)
    int rc;
    /* Register custom service */
    rc = gatt_svr_init();
    assert(rc == 0);

    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set("LoRaNode-BLE");
    assert(rc == 0);
#endif

    /* XXX Need to have template for store */
    ble_store_config_init();

    nimble_port_freertos_init(ble_spp_server_host_task);
}
