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
/* bit1: gonderen admin'e ulasamiyor (REACH_TIMEOUT_MS boyunca tabloda kendini gormedi),
   admin'e ulasan bir komsu bu paketi bir kez aktarsin (tek sekme mesh) */
#define POS_FLAG_RELAY_REQ   0x02
/* bit2: GATEWAY koyar = bu konum Zybo'ya komsu uzerinden (0xE1) ulasti. Zybo bayraklari
   oldugu gibi saklayip tabloda geri yolladigi icin node kendi kaydinda bunu gorur:
   aktarimla ulasiyorsa "beni aktarin" istegini acik tutar (salinim olmaz). */
#define POS_FLAG_VIA_RELAY   0x04

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
/* 0xC5 KOMUT ACK: node -> gateway -> Zybo (havada tekrar yayinlanmaz)
        [1..2]=node adresi [3..4]=0xFFFE [5]=node'un ACK sayaci (tekrar elemek icin)
        [6]=tur (1 ULASTI: node aldi, 2 OKUNDU: telefonda kullanici gordu)
        [7..8]=onaylanan komutun src (0xFFFE) [9]=onaylanan komutun msg_id [10..14]=0
   ULASTI: admin'in o node'a ozel (dst = node) her 0xC1 kopyasina yollanir.
   OKUNDU: telefon, admin komut uyarisinda kullanici Tamam/Bul'a basinca yollar. */
#define CMD_TYPE_ACK                                0xC5
#define ACK_KIND_DELIVERED                          1
#define ACK_KIND_SEEN                               2
/* ACK guvenilirligi: her ACK (ULASTI / OKUNDU) ACK_REPEAT kez yollanir (her seferinde yeni
   ack_seq). Ilki kisa rastgele gecikmeyle, sonrakiler gateway'in 3x yayin penceresinin
   (~1.2 sn) disina dussun diye daha uzun rastgele arayla. Zybo (src, hedef msg_id, tur)
   ile tekrarlari eler. Ayni ACK tekrarlanirken komutun yeni kopyalari yeni ACK baslatmaz;
   tekrarlar bittikten sonra komut yine gelirse (Zybo yeniden denedi) ACK yeniden baslar. */
#define ACK_REPEAT                                  3
#define ACK_Q_SIZE                                  3
#define ACK_FIRST_DELAY_MIN_MS                      30
#define ACK_FIRST_DELAY_RAND_MS                     170
#define ACK_REPEAT_GAP_MIN_MS                       500
#define ACK_REPEAT_GAP_RAND_MS                      1500
#define IS_CTRL_TYPE(t)                             (((t) == CMD_TYPE) || ((t) == CMD_TYPE_DEL_REQ) || ((t) == CMD_TYPE_DEL_ACK) || ((t) == CMD_TYPE_ACK) || \
                                                     ((t) == JOIN_TYPE_KICK) || ((t) == JOIN_TYPE_LEAVE))
/* 15 B kontrol tipinin 4 bitlik etiketi (tekrar eleme / aktarim anahtarlari): 0xC1..0xC5 -> 1..5,
   0xA2..0xA4 -> 0xA..0xC (0xC3 ile 0xA3 karismasin) */
#define CTRL_TAG(t)                                 ((((t) & 0xF0U) == 0xA0U) ? (0x08U | ((t) & 0x07U)) : ((t) & 0x0FU))
#define ADDR_ADMIN                                  0xFFFE
/* 0xC1 tur (kind): 1 ilgi, 2 tehlike, 3 toplanma, 4 buraya gel, 5 bekle, 6 kontrol,
   7 GERI DON (sadece admin -> tek node; telefon koyamaz) */
#define CMD_KIND_MIN                                1
#define CMD_KIND_PHONE_MAX                          6
#define CMD_KIND_GO_BACK                            7
/* 0xC4 KONUMUMU AYARLA: telefon -> ESP32 -> SADECE bagli node (LoRa'ya hic cikmaz).
   [0]=0xC4 [1..4] lat i32 x1e7 [5..8] lon i32 x1e7 [9] 1=ayarla / 0=sahte konuma don
   Node bu konumu fix=1 ile yayinlar. POS_OVERRIDE_TIMEOUT_MS boyunca yenisi
   gelmezse (telefon koptu) sahte konuma doner. */
#define CMD_TYPE_SET_POS                            0xC4
#define POS_OVERRIDE_TIMEOUT_MS                     60000
/* 0xC6 NODE DURUMU: node -> ESP32 -> telefon (SADECE yerel, LoRa'ya cikmaz), her tablodan sonra.
   Dolayli onay: Zybo tablosunda (0xD1) kendi kaydimiz var mi, varsa hangi seq.
   [0]=0xC6 [1..2]=node adresi [3]=son yollanan konum seq'i [4]=tabloda var mi (1/0)
   [5]=tablodaki seq [6]=tablodaki flags [7]=0 */
#define CMD_TYPE_NODE_STATUS                        0xC6
#define NODE_STATUS_LEN                             8

/* ---- Tek sekme mesh: aktarilmis konum ----
   0xE1 AKTARILMIS KONUM: komsu node -> gateway (aktarilmis paket tekrar aktarilmaz)
   [0]=0xE1 [1..2]=aktaran node adresi [3]=aktaranin orijinali duydugu RSSI (i8)
   [4]=SNR (i8) [5..16]=orijinal pos_packet_t (degistirilmeden)
   Gateway icindeki 12 byte'i Zybo'ya normal konum cercevesiyle iletir (Zybo degismez). */
#define RELAY_TYPE_POS                              0xE1
#define RELAY_POS_HDR                               5
#define RELAY_POS_LEN                               (RELAY_POS_HDR + 12)   /* = 17 */
/* Gateway -> Zybo aktarilmis konum cercevesi (GW_FORWARD_RELAY_INFO=1):
   [AA 55 13][17 B 0xE1 paketi][RSSI][SNR][CRC8] = 23 byte */
#define GW_RELAY_FRAME_LEN_FIELD                    (RELAY_POS_LEN + 2)    /* = 19 = 0x13 */
/* Bu kadar suredir Zybo tablosunda kendini gormeyen node "ulasamiyorum" der (2 tablo + pay) */
#define REACH_TIMEOUT_MS                            25000
/* Aktarim oncesi rastgele bekleme: ayni paketi baska komsu aktarirsa iptal edilir */
#define POS_RELAY_DELAY_MIN_MS                      100
#define POS_RELAY_DELAY_RAND_MS                     500

/* 0xE2 AKTARILMIS UPLINK KONTROL: komsu -> gateway (musterinin 0xC1/0xC2/0xC5'i)
   [0]=0xE2 [1..2]=aktaran [3]=aktaranin duydugu RSSI (i8) [4]=SNR (i8) [5..19]=orijinal 15 B
   Gateway icini acip dogrudan gelmis gibi isler (Zybo degismez). */
#define RELAY_TYPE_UL_CTRL                          0xE2
#define RELAY_UL_HDR                                5
#define RELAY_UL_LEN                                (RELAY_UL_HDR + CMD_PACKET_LEN)   /* = 20 */
/* 0xE3 AKTARILMIS DOWNLINK: komsu -> uzak node (gateway'in 0xD1 tablo / 0xC1 / 0xC3'u)
   [0]=0xE3 [1..2]=aktaran [3..]=orijinal paket (degistirilmeden)
   Sadece aktarim modundaki (admin'e dogrudan ulasamayan) node'lar acar. */
#define RELAY_TYPE_DL                               0xE3
#define RELAY_DL_HDR                                3
/* "Musteri": bizden aktarim isteyen node. Son bayrakli paketinden bu kadar sonra musteri sayilmaz */
#define RELAY_CLIENT_TIMEOUT_MS                     30000
#define MAX_RELAY_CLIENTS                           4
#define DL_RELAY_DELAY_MIN_MS                       50
#define DL_RELAY_DELAY_RAND_MS                      250
/* Aktarici: ayni admin komutu bu kadar sure sonra yine duyulursa (Zybo ayni msg_id ile
   yeniden denedi) musterilere asagi yeniden aktarilir */
#define DL_CMD_RETRY_GAP_MS                         3000

/* ======================= JOIN v1: takimli kimlik =======================
   Adres semasi: UID'den turetilen adresler bit15 = 0 (0x0001..0x7FFF).
   Katilan cihaz: 0x8000 | (takim << 8) | numara  (ALPHA-2 = 0x8102, ZULU-99 = 0x9A63).
   0xFF00 | takim ileride takim adresleri icin ayrilmis, hic verilmez.
   Takimlar NATO alfabesi: 1 = ALPHA ... 26 = ZULU. Numara 1..99. */
#define ADDR_JOINED_FLAG                            0x8000
#define JOIN_ADDR(team, num)                        ((uint16_t)(ADDR_JOINED_FLAG | ((uint16_t)(team) << 8) | (num)))
#define JOIN_TEAM_MAX                               26
#define JOIN_NUM_MAX                                99
/* 0xA1 KATILMA ISTEGI: node -> gateway -> Zybo (17 B; gateway Zybo'ya LEN 0x13 ile iletir)
   [0]=A1 [1..2]=src 0x0000 [3..4]=dst 0xFFFE [5]=req_seq [6..9]=dev_id (UID CRC32)
   [10]=takim [11]=numara (0 = bos olan herhangi biri) [12..13]=PIN [14]=fw ana [15]=fw alt
   [16]=pil % (0xFF = bilinmiyor). Tekrar eleme (dev_id, req_seq) ile. */
#define JOIN_TYPE_REQ                               0xA1
#define JOIN_REQ_LEN                                17
/* 0xA2 KATILMA CEVABI: Zybo -> gateway (3x yayin) -> node (15 B)
   [0]=A2 [1..2]=0xFFFE [3..4]=0xFFFF [5]=msg_id [6..9]=dev_id [10..11]=verilen adres
   [12]=takim [13]=numara (sonuc 4 ise Zybo'nun onerdigi bos numara) [14]=sonuc */
#define JOIN_TYPE_ANS                               0xA2
#define JOIN_ANS_LEN                                15
#define JOIN_RES_OK                                 0
#define JOIN_RES_WRONG_PIN                          1
#define JOIN_RES_REJECTED                           2
#define JOIN_RES_TEAM_FULL                          3
#define JOIN_RES_NUM_TAKEN                          4
#define JOIN_RES_KICKED                             5      /* sadece telefona (0xCB): admin takimdan cikardi */
#define JOIN_RES_LEFT                               6      /* sadece telefona (0xCB): takimdan ayrildi */
/* 0xA3 KICK: Zybo -> gateway (3x yayin) -> node (15 B)
   [0]=A3 [1..2]=0xFFFE [3..4]=dst (node'un adresi) [5]=msg_id [6..9]=dev_id [10]=sebep [11..14]=0
   sebep 1: admin cikardi -> node mevcut adresinden ULASTI (0xC5) yollar, sonra kimligi siler.
            Zybo ULASTI gelene kadar 5 sn arayla en fazla 3 kez yollar.
   sebep 2: node'un kendi LEAVE'ine (0xA4) cevap, ACK yok. */
#define JOIN_TYPE_KICK                              0xA3
#define KICK_REASON_ADMIN                           1
#define KICK_REASON_LEAVE_OK                        2
/* 0xA4 LEAVE: node -> gateway -> SADECE Zybo (15 B, gateway LEN 0x11 ile iletir)
   [0]=A4 [1..2]=src (node'un adresi) [3..4]=0xFFFE [5]=msg_id (denemelerde ayni) [6..9]=dev_id
   [10..14]=0. 5 sn arayla en fazla 3 kez; A3 sebep 2 gelince ya da denemeler bitince kimlik silinir. */
#define JOIN_TYPE_LEAVE                             0xA4
#define LEAVE_TRIES                                 3
#define LEAVE_RETRY_MS                              5000
/* A1 tekrar araligi */
#define JOIN_PERIOD_MS                              10000
#define JOIN_PERIOD_RAND_MS                         5000
/* Telefon <-> node yerel paketler (LoRa'ya cikmaz) */
#define CMD_TYPE_JOIN_PARAMS                        0xC9   /* tel->node 15 B: [1]=takim [2]=numara [3..4]=PIN */
#define CMD_TYPE_LEAVE                              0xCC   /* tel->node 15 B: takimdan ayril (node 0xA4 yollar) */
#define CMD_TYPE_JOIN_STATUS                        0xCB   /* node->tel 8 B, asagida */
#define JOIN_STATUS_LEN                             8
/* 0xCB: [1]=durum [2]=son sonuc [3..4]=adres [5]=takim [6]=numara (sonuc 4: onerilen) [7]=req_seq */
#define JOIN_ST_DISABLED                            0      /* JOIN_ENABLED=0: UID adresi */
#define JOIN_ST_NEED_PARAMS                         1      /* kimlik yok, telefondan takim/PIN bekleniyor */
#define JOIN_ST_WAITING                             2      /* A1 yollaniyor, admin onayi bekleniyor */
#define JOIN_ST_JOINED                              3      /* kimlik var, normal calisma */
#define JOIN_ST_FAILED                              4      /* reddedildi; sebep [2]'de */
#define JOIN_ST_LEAVING                             5      /* 0xA4 yollaniyor, Zybo cevabi bekleniyor */
/* Kimlik kaydi: flash'in son 2 KB sayfasi (linker'da kod alanindan cikarildi) */
#define ID_FLASH_ADDR                               0x0803F800U
#define ID_FLASH_PAGE                               127U
#define ID_MAGIC                                    0x4E494F4AU   /* "JOIN" */

/* ---------------- Faz 1 guvenlik (SECURE_ENABLED) ----------------
   Havadaki her paket: [0]=0x51 [1..4]=dev_id (u32 LE, UID'nin CRC32'si; Zybo 0xFFFFFFFE)
   [5..8]=sayac (u32 LE, gonderenin) [9..N+8]=sifreli ic paket (bugunku paket, degismeden)
   [N+9..N+12]=MIC. AES-128-CCM, MIC 4 B.
   nonce (13) = dev_id(4) | sayac(4) | dir(1) | 00 00 00 00   dir: 0 = node yollar, 1 = Zybo yollar
   AAD = [0..8]. Alici once MIC'e, sonra sayaca bakar (32'lik pencere, dev_id basina).
   Aktarim (ic ice): E1/E2 = [E1|E2][aktaran u16][RSSI][SNR][uzak node'un guvenli paketi, AYNEN]
                     E3    = [E3][aktaran u16][Zybo'nun guvenli paketi, AYNEN]
   Gateway -> Zybo: AA 55 LEN [havadaki guvenli paket] RSSI SNR CRC8   (LEN = boy + 2)
   Zybo -> gateway: AA 55 LEN [tx_repeat][guvenli paket] CRC8          (LEN = 1 + boy) */
#define SEC_VER                                     0x51
#define SEC_HDR_LEN                                 9
#define SEC_MIC_LEN                                 4
#define SEC_OVERHEAD                                (SEC_HDR_LEN + SEC_MIC_LEN)   /* = 13 */
#define SEC_DEVID_ZYBO                              0xFFFFFFFEU
#define SEC_DIR_NODE                                0x00
#define SEC_DIR_ZYBO                                0x01
#define SEC_PIPE_MIN_LEN                            25     /* gateway: 0x51 ile baslayan, en az bu boyda = guvenli */
#define SEC_WIN                                     32     /* tekrar penceresi (sayac) */
#define SEC_REPLAY_SIZE                             16     /* takip edilen gonderen sayisi */
#define SEC_CTR_STEP                                256    /* sayac her 256 pakette flash'a yazilir, acilista +256 */
#define SEC_CTR_FLASH_ADDR                          0x0803F000U   /* sondan bir onceki 2 KB sayfa */
#define SEC_CTR_PAGE                                126U
#define SEC_CTR_SLOTS                               256    /* 8 B'lik yuva: [sayac][~sayac], sayfa dolunca silinir */
#define SEC_TXQ_SIZE                                4      /* gateway: Zybo'dan gelen guvenli paket kuyrugu */
#define SEC_TX_REPEAT_MAX                           5
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
