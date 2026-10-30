/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    subghz_phy_app.h
  * @author  MCD Application Team
  * @brief   Header of application of the SubGHz_Phy Middleware
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __SUBGHZ_PHY_APP_H__
#define __SUBGHZ_PHY_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_mode.h"   /* GATEWAY_MODE burada tanimli */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
#include <stdint.h>

/* Konum paketi: havaya giden 12 byte (little-endian, padding yok) */
typedef struct __attribute__((packed))
{
  uint16_t addr;   /* gonderen node adresi                         */
  uint8_t  seq;    /* her pakette +1, kayip tespiti icin           */
  int32_t  lat;    /* enlem  x 1e7 (41.0082000 -> 410082000)       */
  int32_t  lon;    /* boylam x 1e7                                 */
  uint8_t  flags;  /* bit0: GPS fix var (1) / yok (0)              */
} pos_packet_t;

#define POS_FLAG_FIX   0x01

/* Faz 2: isaret / komut paketi, havaya giden 15 byte (little-endian, padding yok).
   Telefon -> BLE -> ESP32 -> UART -> node -> LoRa -> gateway (tekrar yayin) -> herkes */
typedef struct __attribute__((packed))
{
  uint8_t  type;    /* CMD_TYPE (0xC1)                                     */
  uint16_t src;     /* gonderen node (telefon 0 yollar, node kendi adresini yazar) */
  uint16_t dst;     /* hedef node, CMD_DST_ALL = herkes                     */
  uint8_t  msg_id;  /* gonderen basina sayac; (src, msg_id) tekrar elemek icin */
  uint8_t  kind;    /* 1 ilgi noktasi, 2 tehlike, 3 toplanma,
                       4 buraya gel, 5 burada bekle, 6 bolgeyi kontrol et   */
  int32_t  lat;     /* enlem  x 1e7                                         */
  int32_t  lon;     /* boylam x 1e7                                         */
} cmd_packet_t;
/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* MODEM type: one shall be 1 the other shall be 0 */
#define USE_MODEM_LORA  1
#define USE_MODEM_FSK   0

#define RF_FREQUENCY                                868000000 /* Hz */

#ifndef TX_OUTPUT_POWER   /* please, to change this value, redefine it in USER CODE SECTION */
#define TX_OUTPUT_POWER                             17        /* dBm (KG200Z: >15 dBm -> HP PA) */
#endif /* TX_OUTPUT_POWER */

#if (( USE_MODEM_LORA == 1 ) && ( USE_MODEM_FSK == 0 ))
#define LORA_BANDWIDTH                              0         /* [0: 125 kHz, 1: 250 kHz, 2: 500 kHz, 3: Reserved] */
#define LORA_SPREADING_FACTOR                       7         /* [SF7..SF12] */
#define LORA_CODINGRATE                             1         /* [1: 4/5, 2: 4/6, 3: 4/7, 4: 4/8] */
#define LORA_PREAMBLE_LENGTH                        8         /* Same for Tx and Rx */
#define LORA_SYMBOL_TIMEOUT                         5         /* Symbols */
#define LORA_FIX_LENGTH_PAYLOAD_ON                  false
#define LORA_IQ_INVERSION_ON                        false

#elif (( USE_MODEM_LORA == 0 ) && ( USE_MODEM_FSK == 1 ))

#define FSK_FDEV                                    25000     /* Hz */
#define FSK_DATARATE                                50000     /* bps */
#define FSK_BANDWIDTH                               50000     /* Hz */
#define FSK_PREAMBLE_LENGTH                         5         /* Same for Tx and Rx */
#define FSK_FIX_LENGTH_PAYLOAD_ON                   false

#else
#error "Please define a modem in the compiler subghz_phy_app.h."
#endif /* USE_MODEM_LORA | USE_MODEM_FSK */

#define PAYLOAD_LEN                                 64

/* USER CODE BEGIN EC */
/* RF_FREQUENCY CubeMX tarafindan 868000000 olarak uretiliyor (Generate Code her
   seferinde geri yazar). Burada, USER CODE blogu icinde ezdigimiz icin kalici. */
#undef  RF_FREQUENCY
#define RF_FREQUENCY                                869525000 /* Hz, 869.4-869.65 MHz alt bandi: 500 mW e.r.p., %10 duty cycle */

/* Node adresi. 0 = kartin benzersiz ID'sinden (UID) otomatik uret,
   boylece iki karta da AYNI kodu yukleyebilirsin. Elle vermek icin 0x0001 gibi yaz. */
#ifndef NODE_ADDR
#define NODE_ADDR                                   0x0000
#endif

/* Konum yayini periyodu: TX_PERIOD_MS + [0, TX_JITTER_MS) rastgele */
#define TX_PERIOD_MS                                5000
#define TX_JITTER_MS                                1000

/* Sahte konum (GPS gelene kadar): Istanbul, Bakirkoy merkezi (40.98306 K, 28.85361 D).
   Offline harita testi icin; harita dosyasi (map.mbtiles) Bakirkoy'u kapsiyor. */
#define FAKE_LAT_E7                                 409830600
#define FAKE_LON_E7                                 288536100

/* Calisma modu (GATEWAY_MODE) artik Core/Inc/app_mode.h icinde */

/* Gateway -> Zybo UART cercevesi:
   [0xAA][0x55][LEN][PAYLOAD 12B][RSSI int8][SNR int8][CRC8]
   LEN  = PAYLOAD + RSSI + SNR byte sayisi (= 14)
   CRC8 = poly 0x07, init 0x00; LEN..SNR uzerinden (sync byte'lari haric) */
#define GW_SYNC0                                    0xAA
#define GW_SYNC1                                    0x55
#define GW_FRAME_LEN_FIELD                          (sizeof(pos_packet_t) + 2)
#define GW_FRAME_SIZE                               (3 + GW_FRAME_LEN_FIELD + 1)

/* Downlink (gateway -> node'lar) LoRa paketi:
   [DL_TYPE_TABLE][N][kayit 1 (12B)]...[kayit N]   boy = 2 + 12*N
   Kayit = pos_packet_t (uplink paketinin aynisi).
   Uplink hep 12 byte, downlink 14/26/38... -> boydan ayirt edilir. */
#define DL_TYPE_TABLE                               0xD1
#define DL_MAX_RECORDS                              21     /* 2 + 12*21 = 254 <= 255 */
#define DL_PAYLOAD_LEN(n)                           (2 + sizeof(pos_packet_t) * (n))

/* Zybo -> gateway UART cercevesi: [0xAA][0x55][LEN][PAYLOAD (LEN byte)][CRC8]
   CRC8 uplink ile ayni (poly 0x07, init 0), LEN + PAYLOAD uzerinden. */
#define DL_UART_MAX_PAYLOAD                         255

/* Faz 2: komut paketi (cmd_packet_t). Boylar cakismaz: uplink 12, tablo 14/26/38..., komut 15 */
#define CMD_TYPE                                    0xC1   /* isaret / komut */
/* Silme (admin onayli). Ayni 15 byte duzen, sadece tip farkli:
   0xC2 SILME ISTEGI : node -> gateway -> SADECE Zybo (havada tekrar yayinlanmaz)
        [6]=0  [7..8]=hedef src  [9]=hedef msg_id
   0xC3 ADMIN KARARI : Zybo -> gateway -> 3x yayin -> tum node'lar -> telefonlar
        src=0xFFFE  [6]=karar (1 onay, 2 red)  [7..8]=hedef src  [9]=hedef msg_id  [10]=istek msg_id */
#define CMD_TYPE_DEL_REQ                            0xC2
#define CMD_TYPE_DEL_ACK                            0xC3
#define IS_CTRL_TYPE(t)                             (((t) == CMD_TYPE) || ((t) == CMD_TYPE_DEL_REQ) || ((t) == CMD_TYPE_DEL_ACK))
#define ADDR_ADMIN                                  0xFFFE
/* 0xC1 tur (kind): 1 ilgi, 2 tehlike, 3 toplanma, 4 buraya gel, 5 bekle, 6 kontrol,
   7 GERI DON (sadece admin -> tek node; telefon koyamaz) */
#define CMD_KIND_MIN                                1
#define CMD_KIND_PHONE_MAX                          6
#define CMD_KIND_GO_BACK                            7
#define CMD_PACKET_LEN                              15
#define CMD_DST_ALL                                 0xFFFF
#define CMD_RELAY_COUNT                             3      /* gateway her komutu kac kez yayinlar */
#define CMD_RELAY_GAP_MS                            400    /* tekrarlar arasi + [0..200) ms rastgele */
#define CMD_SEEN_SIZE                               16     /* son gorulen (src,msg_id) sayisi */
/* Gateway -> Zybo komut cercevesi (GW_FORWARD_CMD=1): [AA 55 11][15 B][RSSI][SNR][CRC8] */
#define GW_CMD_FRAME_LEN_FIELD                      (CMD_PACKET_LEN + 2)   /* = 17 = 0x11 */

/* Ayrilmis adresler: hicbir node adresi (sanal olanlar dahil) bu araliga dusmez.
   0xFFFF = herkes, 0xFFFE = admin (Zybo), digerleri ileride. 0x0000 = telefon (sadece BLE/UART). */
#define ADDR_RESERVED_MIN                           0xFFF0
/* USER CODE END EC */

/* External variables --------------------------------------------------------*/
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/* Exported macros -----------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
/**
  * @brief  Init Subghz Application
  */
void SubghzApp_Init(void);

/* USER CODE BEGIN EFP */
/* Gateway: Zybo'dan gelen downlink cercevelerini USART1 RX (D0/PB7) ile almayi baslatir.
   usart.c icinde MX_USART1_UART_Init() sonunda cagrilir. Node modunda bos. */
void Gateway_UartRxStart(void);
/* USART1 kesme fonksiyonu: stm32wlxx_it.c icindeki USART1_IRQHandler cagirir. */
void Gateway_UsartIrq(void);

/* USER CODE END EFP */

#ifdef __cplusplus
}
#endif

#endif /*__SUBGHZ_PHY_APP_H__*/
