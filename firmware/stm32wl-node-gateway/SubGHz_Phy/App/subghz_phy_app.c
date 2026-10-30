/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    subghz_phy_app.c
  * @author  MCD Application Team
  * @brief   Application of the SubGHz_Phy Middleware
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
/*
  * P2P konum yayini (PingPong orneginden turetildi)
  *  - Her node TX_PERIOD_MS + rastgele jitter araliklarla kendi konum paketini yollar.
  *  - Geri kalan butun zamanda dinler; gelen paketi cozup UART'tan basar.
  *  - Master/slave yok: iki karta da ayni kod yuklenir.
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "platform.h"
#include "sys_app.h"
#include "subghz_phy_app.h"
#include "radio.h"

/* USER CODE BEGIN Includes */
#include "stm32_timer.h"
#include "stm32_seq.h"
#include "utilities_def.h"
#include "app_version.h"
#include "subghz_phy_version.h"
#include "usart.h"   /* huart1: Zybo'ya giden USART1 (PB6 TX / PB7 RX) */
/* USER CODE END Includes */

/* External variables ---------------------------------------------------------*/
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* Uygulama olaylari (bit maskesi). Callback'ler bit set eder,
   Node_Process hepsini tek seferde okuyup temizler. */
typedef enum
{
  EV_RX_DONE    = (1U << 0),
  EV_RX_TIMEOUT = (1U << 1),
  EV_RX_ERROR   = (1U << 2),
  EV_TX_DONE    = (1U << 3),
  EV_TX_TIMEOUT = (1U << 4),
  EV_TX_REQUEST = (1U << 5),   /* periyodik TX zamani geldi */
  EV_DL_FRAME   = (1U << 6),   /* gateway: Zybo'dan gecerli downlink cercevesi geldi */
  EV_LOCAL_DL   = (1U << 7),   /* gateway (Zybo yok): yerel tabloyu yayinlama zamani */
  EV_CMD_RELAY  = (1U << 8),   /* gateway: komutun bir sonraki tekrarinin zamani */
} AppEvent_t;

/* Kayip paket takibi icin gorulen node tablosu */
typedef struct
{
  uint16_t addr;
  uint8_t  last_seq;
  uint32_t rx_count;
  uint32_t lost_count;
} PeerInfo_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Configurations */
#define TX_TIMEOUT_VALUE              3000
#define MAX_APP_BUFFER_SIZE           255
#define MAX_PEERS                     8

#if (VIRTUAL_NODES < 1) || (VIRTUAL_NODES > 8)
#error "VIRTUAL_NODES 1..8 araliginda olmali"
#endif
/* LED yanik kalma suresi */
#define LED_PULSE_MS                  50

/* USART1'den byte ALINACAK mi?
   - gateway + Zybo: Zybo'dan tablo cercevesi
   - node + ESP32 koprusu: telefondan komut cercevesi (Faz 2, ESP GPIO5 -> node D0/PB7) */
#define UART1_RX_ENABLED  (((GATEWAY_MODE == 1) && (GATEWAY_LOCAL_TABLE == 0)) || \
                           ((GATEWAY_MODE == 0) && (NODE_UART_BRIDGE == 1)))
/* UART cerceve cozucusu ve dlFrame tamponu derlensin mi */
#define UART1_PARSER_USED ((GATEWAY_MODE == 1) || (NODE_UART_BRIDGE == 1))

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* Radio events function pointer */
static RadioEvents_t RadioEvents;

/* USER CODE BEGIN PV */
/* Bekleyen olaylar (interrupt'tan yazilir, ana dongude okunur) */
static volatile uint32_t AppEvents = 0;
/* Radyo su an TX yapiyor mu */
static bool txBusy = false;

/* App Rx Buffer */
static uint8_t BufferRx[MAX_APP_BUFFER_SIZE];
static uint16_t RxBufferSize = 0;
static int16_t RssiValue = 0;
static int8_t  SnrValue = 0;

/* Bu node'un bilgileri */
static uint16_t myAddr = 0;
static pos_packet_t txPacket;

/* Sanal node'lar (VIRTUAL_NODES=1 iken sadece [0] kullanilir = normal node) */
static uint16_t vAddr[VIRTUAL_NODES];
static uint8_t  vSeq[VIRTUAL_NODES];
/* TX zamani gelmis ama henuz yollanmamis sanal node'lar (bit i = sanal node i) */
static volatile uint32_t pendingTx = 0;

/* Gorulen diger node'lar */
static PeerInfo_t peers[MAX_PEERS];

/* Timer'lar */
static UTIL_TIMER_Object_t timerTx[VIRTUAL_NODES];
static UTIL_TIMER_Object_t timerLed;

/* Basit sozde-rastgele uretec (jitter icin) */
static uint32_t rngState = 1;

/* ---- Faz 2: komut paketleri ---- */
#if (GATEWAY_MODE == 0)
static uint8_t  cmdTxBuf[CMD_PACKET_LEN];   /* telefondan gelen, LoRa'ya yollanacak 0xC1 / 0xC2 */
static bool     cmdTxPending = false;
#endif
static uint32_t cmdSeen[CMD_SEEN_SIZE];     /* gorulen (src,msg_id) anahtarlari, halka */
static uint8_t  cmdSeenIdx = 0;
#if (GATEWAY_MODE == 1)
/* Gateway tekrar yayin tamponlari: [0] node'lardan gelen 0xC1, [1] Zybo'dan gelen 0xC3,
   [2] Zybo'dan gelen admin 0xC1 (isaret / GERI DON).
   Ayri tamponlar: bir admin paketi yeni bir komut yuzunden kaybolmasin. */
#define RELAY_NODE_CMD   0
#define RELAY_ADMIN      1
#define RELAY_ADMIN_CMD  2
#define RELAY_COUNT_BUF  3
typedef struct
{
  uint8_t  buf[CMD_PACKET_LEN];
  uint8_t  left;          /* kalan tekrar */
  volatile bool due;      /* zamani geldi (timer'dan) */
  UTIL_TIMER_Object_t timer;
} Relay_t;
static Relay_t relays[RELAY_COUNT_BUF];
#endif

#if UART1_PARSER_USED
/* UART cerceve cozucu (kesme icinde calisir):
   gateway'de Zybo -> gateway, node'da ESP32 -> node */
typedef enum { DLP_SYNC0, DLP_SYNC1, DLP_LEN, DLP_DATA, DLP_CRC } DlParseState_t;
static DlParseState_t dlState = DLP_SYNC0;
static uint8_t  dlRxBuf[DL_UART_MAX_PAYLOAD];
static uint16_t dlRxLen = 0, dlRxIdx = 0;
static uint8_t  dlCrc = 0;
/* Tamamlanmis cerceve (kesme yazar, Node_Process okur) */
static uint8_t  dlFrame[DL_UART_MAX_PAYLOAD];
static volatile uint16_t dlFrameLen = 0;
static volatile bool dlFrameReady = false;
/* Hata sayaclari (log icin) */
static volatile uint32_t dlCrcErrors = 0, dlDropped = 0;
#if (GATEWAY_MODE == 1) && (GATEWAY_LOCAL_TABLE == 1)
/* Zybo taklidi: gateway'in kendi tuttugu "kim nerede" tablosu */
static pos_packet_t localTab[DL_MAX_RECORDS];
static uint8_t  localTabCount = 0;
static uint32_t localTabFull = 0;
static UTIL_TIMER_Object_t timerLocalDl;
#endif
#endif

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/*!
 * @brief Function to be executed on Radio Tx Done event
 */
static void OnTxDone(void);

/**
  * @brief Function to be executed on Radio Rx Done event
  * @param  payload ptr of buffer received
  * @param  size buffer size
  * @param  rssi
  * @param  LoraSnr_FskCfo
  */
static void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t LoraSnr_FskCfo);

/**
  * @brief Function executed on Radio Tx Timeout event
  */
static void OnTxTimeout(void);

/**
  * @brief Function executed on Radio Rx Timeout event
  */
static void OnRxTimeout(void);

/**
  * @brief Function executed on Radio Rx Error event
  */
static void OnRxError(void);

/* USER CODE BEGIN PFP */
static void Node_Process(void);
static void RadioSend(const uint8_t *data, uint8_t len);
static void RadioRx(void);
static void PostEvent(uint32_t ev);
static void OnTxTimerEvent(void *context);
static void OnLedTimerEvent(void *context);
static void ScheduleNextTx(uint32_t vi);
static void BuildPositionPacket(pos_packet_t *pkt, uint32_t vi);
static bool IsMyAddr(uint16_t addr);
static void HandleRxPacket(void);
static uint32_t Rand32(void);
static void LogCoord(const char *label, int32_t v);
static void HandleDownlinkTable(void);
static uint8_t Crc8(const uint8_t *data, uint32_t len) __attribute__((unused));
static uint8_t Crc8Update(uint8_t crc, uint8_t b) __attribute__((unused));
static void Uart1_SendFrame(const uint8_t *payload, uint16_t len) __attribute__((unused));
static void HandleCmdPacket(void);
static bool Cmd_MarkSeen(uint8_t type, uint16_t src, uint8_t id);
static void LogCmd(const char *tag, const uint8_t *p);
#if UART1_PARSER_USED
static void DL_ParseByte(uint8_t b);
#endif
#if (GATEWAY_MODE == 0) && (NODE_UART_BRIDGE == 1)
static void Node_HandleUartFrame(void);
#endif
#if (GATEWAY_MODE == 1)
static void OnCmdRelayTimerEvent(void *context);
static void Relay_Start(uint32_t r, const uint8_t *pkt);
#if (GW_FORWARD_CMD == 1)
static void Gateway_ForwardCmd(const uint8_t *cmd, int16_t rssi, int8_t snr);
#endif
static void Gateway_SendDownlink(void);
#if (GATEWAY_LOCAL_TABLE == 1)
static void OnLocalDlTimerEvent(void *context);
static void LocalTable_Update(const pos_packet_t *pkt);
static void LocalTable_BuildDownlink(void);
#endif
static void Gateway_ForwardPacket(const pos_packet_t *pkt, int16_t rssi, int8_t snr);
#endif

/* USER CODE END PFP */

/* Exported functions ---------------------------------------------------------*/
void SubghzApp_Init(void)
{
  /* USER CODE BEGIN SubghzApp_Init_1 */

  APP_LOG(TS_OFF, VLEVEL_M, "\n\rP2P KONUM NODE\n\r");
  APP_LOG(TS_OFF, VLEVEL_M, "APPLICATION_VERSION: V%X.%X.%X\r\n",
          (uint8_t)(APP_VERSION_MAIN),
          (uint8_t)(APP_VERSION_SUB1),
          (uint8_t)(APP_VERSION_SUB2));
  APP_LOG(TS_OFF, VLEVEL_M, "MW_RADIO_VERSION:    V%X.%X.%X\r\n",
          (uint8_t)(SUBGHZ_PHY_VERSION_MAIN),
          (uint8_t)(SUBGHZ_PHY_VERSION_SUB1),
          (uint8_t)(SUBGHZ_PHY_VERSION_SUB2));

  /* Node adresi: elle verilmediyse kartin 96-bit UID'sinden 16 bit'e katla */
#if (NODE_ADDR != 0)
  myAddr = NODE_ADDR;
#else
  {
    uint32_t uid = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
    myAddr = (uint16_t)(uid ^ (uid >> 16));
  }
#endif
  /* Adres korumasi: 0x0000 (telefon) ve 0xFFF0-0xFFFF (herkes/admin/ayrilmis) hicbir
     node'a verilmez. Sanal node'lar +1, +2... aldigi icin taban adres ona gore sinirlanir. */
  if (myAddr == 0) { myAddr = 1; }
  if (myAddr > (uint16_t)(ADDR_RESERVED_MIN - VIRTUAL_NODES))
  {
    myAddr = (uint16_t)(myAddr - 0x1000);   /* ornek: 0xFFF5 -> 0xEFF5 */
  }
  memset(peers, 0, sizeof(peers));

  /* Sanal node adresleri: kartin adresi, +1, +2, ... (0 adresi atlanir) */
  for (uint32_t i = 0; i < VIRTUAL_NODES; i++)
  {
    uint16_t a = (uint16_t)(myAddr + i);
    vAddr[i] = (a == 0) ? 1 : a;
    vSeq[i] = 0;
  }

  /* LED timer (tek atimlik, LED'i kisa sure yakip sondurmek icin) */
  UTIL_TIMER_Create(&timerLed, LED_PULSE_MS, UTIL_TIMER_ONESHOT, OnLedTimerEvent, NULL);
  /* Her sanal node icin ayri periyodik TX timer'i (tek atimlik, her TX'te yeni jitter
     ile yeniden kurulur). context = sanal node numarasi. */
  for (uint32_t i = 0; i < VIRTUAL_NODES; i++)
  {
    UTIL_TIMER_Create(&timerTx[i], TX_PERIOD_MS, UTIL_TIMER_ONESHOT, OnTxTimerEvent, (void *)i);
  }
#if (GATEWAY_MODE == 1)
  /* Komut tekrar yayini (Faz 2) */
  for (uint32_t r = 0; r < RELAY_COUNT_BUF; r++)
  {
    UTIL_TIMER_Create(&relays[r].timer, CMD_RELAY_GAP_MS, UTIL_TIMER_ONESHOT, OnCmdRelayTimerEvent, (void *)r);
  }
#endif
  /* USER CODE END SubghzApp_Init_1 */

  /* Radio initialization */
  RadioEvents.TxDone = OnTxDone;
  RadioEvents.RxDone = OnRxDone;
  RadioEvents.TxTimeout = OnTxTimeout;
  RadioEvents.RxTimeout = OnRxTimeout;
  RadioEvents.RxError = OnRxError;

  Radio.Init(&RadioEvents);

  /* USER CODE BEGIN SubghzApp_Init_2 */
  /* Jitter uretecini radyo gurultusunden gelen gercek rastgele sayi ile tohumla */
  rngState = Radio.Random() ^ ((uint32_t)myAddr << 16);
  if (rngState == 0) { rngState = 0x12345678; }

  Radio.SetChannel(RF_FREQUENCY);

  APP_LOG(TS_OFF, VLEVEL_M, "---------------\n\r");
#if (GATEWAY_MODE == 1)
  APP_LOG(TS_OFF, VLEVEL_M, "MODE=GATEWAY (TX yok, paketler USART1 -> Zybo)\n\r");
#else
  APP_LOG(TS_OFF, VLEVEL_M, "MODE=NODE\n\r");
#endif
  APP_LOG(TS_OFF, VLEVEL_M, "NODE_ADDR=0x%04X\n\r", myAddr);
#if (GATEWAY_MODE == 0) && (VIRTUAL_NODES > 1)
  APP_LOG(TS_OFF, VLEVEL_M, "VIRTUAL_NODES=%d:", VIRTUAL_NODES);
  for (uint32_t i = 0; i < VIRTUAL_NODES; i++)
  {
    APP_LOG(TS_OFF, VLEVEL_M, " v%d=0x%04X", (int)i, vAddr[i]);
  }
  APP_LOG(TS_OFF, VLEVEL_M, "\n\r");
#endif
  APP_LOG(TS_OFF, VLEVEL_M, "FREQ=%d Hz, LORA_SF=%d, BW=%d kHz, PWR=%d dBm\n\r",
          RF_FREQUENCY, LORA_SPREADING_FACTOR, (1 << LORA_BANDWIDTH) * 125, TX_OUTPUT_POWER);
  APP_LOG(TS_OFF, VLEVEL_M, "Paket boyu=%d byte, periyot=%d ms + [0..%d) ms jitter\n\r",
          (int)sizeof(pos_packet_t), TX_PERIOD_MS, TX_JITTER_MS);
  APP_LOG(TS_OFF, VLEVEL_M, "---------------\n\r");

  /* Ana donguden calisacak isleme gorevini kaydet */
  UTIL_SEQ_RegTask((1 << CFG_SEQ_Task_SubGHz_Phy_App_Process), UTIL_SEQ_RFU, Node_Process);

  /* Dinlemeye basla, (node modunda) ilk TX'i zamanla */
  RadioRx();
#if (GATEWAY_MODE == 0)
  for (uint32_t i = 0; i < VIRTUAL_NODES; i++)
  {
    ScheduleNextTx(i);
  }
#if (NODE_UART_BRIDGE == 1)
  APP_LOG(TS_OFF, VLEVEL_M, "NODE_UART_BRIDGE: tablo/komut USART1 D1(PB6) -> ESP32, komut ESP32 -> D0(PB7)\n\r");
#endif
#elif (GATEWAY_LOCAL_TABLE == 1)
  APP_LOG(TS_OFF, VLEVEL_M, "GATEWAY_LOCAL_TABLE: Zybo yok, tablo yerel, %d ms'de bir yayin\n\r",
          DL_LOCAL_PERIOD_MS);
  UTIL_TIMER_Create(&timerLocalDl, DL_LOCAL_PERIOD_MS, UTIL_TIMER_PERIODIC, OnLocalDlTimerEvent, NULL);
  UTIL_TIMER_Start(&timerLocalDl);
#endif
#if (GATEWAY_MODE == 1)
#if (GW_FORWARD_CMD == 1)
  APP_LOG(TS_OFF, VLEVEL_M, "Komut (0xC1): %d kez tekrar yayin + Zybo'ya iletim (21 B)\n\r", CMD_RELAY_COUNT);
#else
  APP_LOG(TS_OFF, VLEVEL_M, "Komut (0xC1): %d kez tekrar yayin, Zybo'ya iletilmez (GW_FORWARD_CMD=0)\n\r", CMD_RELAY_COUNT);
#endif
#endif
  /* USER CODE END SubghzApp_Init_2 */
}

/* USER CODE BEGIN EF */

/* USER CODE END EF */

/* Private functions ---------------------------------------------------------*/
static void OnTxDone(void)
{
  /* USER CODE BEGIN OnTxDone */
  PostEvent(EV_TX_DONE);
  /* USER CODE END OnTxDone */
}

static void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t LoraSnr_FskCfo)
{
  /* USER CODE BEGIN OnRxDone */
  RxBufferSize = (size <= MAX_APP_BUFFER_SIZE) ? size : MAX_APP_BUFFER_SIZE;
  memcpy(BufferRx, payload, RxBufferSize);
  RssiValue = rssi;
  SnrValue = LoraSnr_FskCfo;
  PostEvent(EV_RX_DONE);
  /* USER CODE END OnRxDone */
}

static void OnTxTimeout(void)
{
  /* USER CODE BEGIN OnTxTimeout */
  PostEvent(EV_TX_TIMEOUT);
  /* USER CODE END OnTxTimeout */
}

static void OnRxTimeout(void)
{
  /* USER CODE BEGIN OnRxTimeout */
  PostEvent(EV_RX_TIMEOUT);
  /* USER CODE END OnRxTimeout */
}

static void OnRxError(void)
{
  /* USER CODE BEGIN OnRxError */
  PostEvent(EV_RX_ERROR);
  /* USER CODE END OnRxError */
}

/* USER CODE BEGIN PrFD */

/* Olay bitini set et ve isleme gorevini tetikle (interrupt'tan guvenli) */
static void PostEvent(uint32_t ev)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  AppEvents |= ev;
  __set_PRIMASK(primask);
  UTIL_SEQ_SetTask((1 << CFG_SEQ_Task_SubGHz_Phy_App_Process), CFG_SEQ_Prio_0);
}

static void OnTxTimerEvent(void *context)
{
  uint32_t vi = (uint32_t)context;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  pendingTx |= (1U << vi);
  __set_PRIMASK(primask);
  PostEvent(EV_TX_REQUEST);
}

/* Adres bu karta (sanal node'lardan birine) mi ait? */
static bool IsMyAddr(uint16_t addr)
{
#if (GATEWAY_MODE == 1)
  return (addr == myAddr);   /* gateway TX yapmaz, sanal node'u yok */
#endif
  for (uint32_t i = 0; i < VIRTUAL_NODES; i++)
  {
    if (vAddr[i] == addr) { return true; }
  }
  return false;
}

static void OnLedTimerEvent(void *context)
{
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET); /* yesil */
  HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET); /* kirmizi */
}

/* xorshift32: hizli, kucuk, jitter icin yeterli */
static uint32_t Rand32(void)
{
  uint32_t x = rngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rngState = x;
  return x;
}

static void ScheduleNextTx(uint32_t vi)
{
  uint32_t period = TX_PERIOD_MS + (Rand32() % TX_JITTER_MS);
  UTIL_TIMER_Stop(&timerTx[vi]);
  UTIL_TIMER_SetPeriod(&timerTx[vi], period);
  UTIL_TIMER_Start(&timerTx[vi]);
}

/* Sahte konum: sabit bir nokta + adrese gore ofset + her pakette ~1 m kuzeye kayma.
   Sanal node i, ilkinin ~100 m dogusunda * i (0.0012 derece boylam ~ 100 m). */
static void BuildPositionPacket(pos_packet_t *pkt, uint32_t vi)
{
  pkt->addr  = vAddr[vi];
  pkt->seq   = vSeq[vi]++;
  pkt->lat   = FAKE_LAT_E7 + (int32_t)(myAddr & 0xFF) * 100 + (int32_t)pkt->seq * 90;
  pkt->lon   = FAKE_LON_E7 + (int32_t)((myAddr >> 8) & 0xFF) * 100 + (int32_t)vi * 12000;
  pkt->flags = 0; /* GPS yok -> fix yok. Gercek GPS gelince POS_FLAG_FIX set edilecek */
}

/* "lat=41.0082123" seklinde bas (tiny printf float desteklemiyor) */
static void LogCoord(const char *label, int32_t v)
{
  const char *sign = (v < 0) ? "-" : "";
  uint32_t a = (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
  APP_LOG(TS_OFF, VLEVEL_L, " %s=%s%u.%07u", label, sign,
          (unsigned)(a / 10000000U), (unsigned)(a % 10000000U));
}

static void HandleRxPacket(void)
{
  pos_packet_t pkt;

  /* Downlink tablo paketi mi? (ilk byte 0xD1, boy 2+12N) */
  if ((RxBufferSize >= 2) && (BufferRx[0] == DL_TYPE_TABLE) &&
      (BufferRx[1] >= 1) && (BufferRx[1] <= DL_MAX_RECORDS) &&
      (RxBufferSize == DL_PAYLOAD_LEN(BufferRx[1])))
  {
    HandleDownlinkTable();
    return;
  }

  /* Faz 2: isaret / komut paketi mi? (ilk byte 0xC1, boy 15) */
  if ((RxBufferSize == CMD_PACKET_LEN) && IS_CTRL_TYPE(BufferRx[0]))
  {
    HandleCmdPacket();
    return;
  }

  if (RxBufferSize != sizeof(pos_packet_t))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[RX] yabanci paket, boy=%d, atlandi (rssi=%d)\n\r",
            RxBufferSize, RssiValue);
    return;
  }
  memcpy(&pkt, BufferRx, sizeof(pkt));

  if (IsMyAddr(pkt.addr))
  {
    return; /* kendi yankimiz (olmamali ama yine de) */
  }

  /* Node tablosunda bul ya da ekle, kayip paketleri say */
  PeerInfo_t *p = NULL;
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (peers[i].rx_count != 0 && peers[i].addr == pkt.addr) { p = &peers[i]; break; }
  }
  if (p == NULL)
  {
    for (int i = 0; i < MAX_PEERS; i++)
    {
      if (peers[i].rx_count == 0) { p = &peers[i]; p->addr = pkt.addr; break; }
    }
  }
  if (p != NULL)
  {
    if (p->rx_count != 0)
    {
      uint8_t gap = (uint8_t)(pkt.seq - p->last_seq);
      if (gap > 1) { p->lost_count += (gap - 1); }
    }
    p->last_seq = pkt.seq;
    p->rx_count++;
  }

  HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_SET); /* kirmizi = RX */
  UTIL_TIMER_Start(&timerLed);

  APP_LOG(TS_ON, VLEVEL_L, "[RX] addr=0x%04X seq=%3d", pkt.addr, pkt.seq);
  LogCoord("lat", pkt.lat);
  LogCoord("lon", pkt.lon);
  APP_LOG(TS_OFF, VLEVEL_L, " fix=%d rssi=%d snr=%d", (pkt.flags & POS_FLAG_FIX) ? 1 : 0,
          RssiValue, SnrValue);
  if (p != NULL)
  {
    APP_LOG(TS_OFF, VLEVEL_L, " alinan=%u kayip=%u", (unsigned)p->rx_count, (unsigned)p->lost_count);
  }
  APP_LOG(TS_OFF, VLEVEL_L, "\n\r");

#if (GATEWAY_MODE == 1)
  /* Adres filtrelemesi YOK: hangi node'un kabul edilecegine Zybo PL karar verir */
  Gateway_ForwardPacket(&pkt, RssiValue, SnrValue);
#if (GATEWAY_LOCAL_TABLE == 1)
  LocalTable_Update(&pkt);
#endif
#endif
}

/* Gateway'in yayinladigi "kim nerede" tablosunu coz ve bas */
static void HandleDownlinkTable(void)
{
  uint8_t n = BufferRx[1];
  pos_packet_t rec;

  HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_SET); /* kirmizi = RX */
  UTIL_TIMER_Start(&timerLed);

  APP_LOG(TS_ON, VLEVEL_L, "[TABLO] %d node (rssi=%d snr=%d):\n\r", n, RssiValue, SnrValue);
  for (uint8_t i = 0; i < n; i++)
  {
    memcpy(&rec, &BufferRx[2 + i * sizeof(pos_packet_t)], sizeof(rec));
    APP_LOG(TS_OFF, VLEVEL_L, "   0x%04X seq=%3d", rec.addr, rec.seq);
    LogCoord("lat", rec.lat);
    LogCoord("lon", rec.lon);
    APP_LOG(TS_OFF, VLEVEL_L, " fix=%d%s\n\r", (rec.flags & POS_FLAG_FIX) ? 1 : 0,
            IsMyAddr(rec.addr) ? "  (ben)" : "");
  }

#if (GATEWAY_MODE == 0) && (NODE_UART_BRIDGE == 1)
  /* Tabloyu oldugu gibi (D1 N kayitlar) cerceveleyip ESP32'ye yolla -> BLE -> telefon */
  Uart1_SendFrame(BufferRx, RxBufferSize);
  APP_LOG(TS_ON, VLEVEL_L, "[BLE] tablo -> ESP32 (%d byte payload)\n\r", RxBufferSize);
#endif
}

/* ---------------- Faz 2: isaret / komut paketleri ---------------- */

/* (src, msg_id) daha once goruldu mu? Gorulmediyse kaydet ve true don. */
static bool Cmd_MarkSeen(uint8_t type, uint16_t src, uint8_t id)
{
  /* 0 = bos yuva olmasin diye bit31 hep 1; tip de anahtarda (0xC1/C2/C3 ayri) */
  uint32_t key = 0x80000000U | ((uint32_t)(type & 0x0F) << 24) | ((uint32_t)src << 8) | id;
  for (uint32_t i = 0; i < CMD_SEEN_SIZE; i++)
  {
    if (cmdSeen[i] == key) { return false; }
  }
  cmdSeen[cmdSeenIdx] = key;
  cmdSeenIdx = (uint8_t)((cmdSeenIdx + 1) % CMD_SEEN_SIZE);
  return true;
}

static void LogCmd(const char *tag, const uint8_t *p)
{
  cmd_packet_t c;
  memcpy(&c, p, sizeof(c));
  if (c.type == CMD_TYPE)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[CMD] %s src=0x%04X dst=0x%04X id=%u tur=%u", tag,
            c.src, c.dst, c.msg_id, c.kind);
    LogCoord("lat", c.lat);
    LogCoord("lon", c.lon);
  }
  else
  {
    /* 0xC2 / 0xC3: hedef isaret = [7..8] src, [9] msg_id */
    APP_LOG(TS_ON, VLEVEL_L, "[%s] %s src=0x%04X dst=0x%04X id=%u hedef=0x%04X/%u", 
            (c.type == CMD_TYPE_DEL_REQ) ? "SIL-ISTEK" : "SIL-KARAR", tag,
            c.src, c.dst, c.msg_id, (unsigned)(p[7] | (p[8] << 8)), p[9]);
    if (c.type == CMD_TYPE_DEL_ACK)
    {
      APP_LOG(TS_OFF, VLEVEL_L, " karar=%s", (p[6] == 1) ? "ONAY" : "RED");
    }
  }
}

/* LoRa'dan komut paketi geldi */
static void HandleCmdPacket(void)
{
  cmd_packet_t c;
  memcpy(&c, BufferRx, sizeof(c));
  bool fresh = Cmd_MarkSeen(c.type, c.src, c.msg_id);

  LogCmd("rx", BufferRx);
  APP_LOG(TS_OFF, VLEVEL_L, " rssi=%d%s\n\r", RssiValue, fresh ? "" : " (tekrar, atlandi)");
  if (!fresh) { return; }

  HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_SET); /* kirmizi = RX */
  UTIL_TIMER_Start(&timerLed);

#if (GATEWAY_MODE == 1)
  if (c.type == CMD_TYPE)
  {
    /* 0xC1: Zybo'ya bildir (bayrak aciksa) + herkes duysun diye 3x tekrar yayinla */
#if (GW_FORWARD_CMD == 1)
    Gateway_ForwardCmd(BufferRx, RssiValue, SnrValue);
#endif
    Relay_Start(RELAY_NODE_CMD, BufferRx);
  }
  else if (c.type == CMD_TYPE_DEL_REQ)
  {
    /* 0xC2: karar admin'in, SADECE Zybo'ya gider, havada tekrar yayinlanmaz */
#if (GW_FORWARD_CMD == 1)
    Gateway_ForwardCmd(BufferRx, RssiValue, SnrValue);
#else
    APP_LOG(TS_ON, VLEVEL_L, "[SIL-ISTEK] Zybo'ya iletilmedi (GW_FORWARD_CMD=0)\n\r");
#endif
  }
  /* 0xC3'u gateway'in kendisi yayinlar; havadan duymaz (duyarsa yok sayilir) */
#elif (NODE_UART_BRIDGE == 1)
  if (c.type == CMD_TYPE_DEL_REQ)
  {
    return; /* baska node'un silme istegi: telefonlari ilgilendirmez, karari bekle */
  }
  /* Admin'in (0xFFFE) baska bir node'a ozel 0xC1'i (or. GERI DON, kind 7) bu telefonu
     ilgilendirmez. Telefonlardan gelen 0xC1'ler eskisi gibi herkese gider (hedef etiketiyle). */
  if ((c.type == CMD_TYPE) && (c.src == ADDR_ADMIN) &&
      (c.dst != CMD_DST_ALL) && !IsMyAddr(c.dst))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[BLE] admin komutu baska node'a (0x%04X), telefona iletilmedi\n\r", c.dst);
    return;
  }
  /* 0xC1 ve 0xC3: telefona ilet (kendi komutumuzun gateway'den donen ilk kopyasi da
     gider, telefon onu "aga ulasti" onayi olarak kullanir) */
  Uart1_SendFrame(BufferRx, CMD_PACKET_LEN);
  APP_LOG(TS_ON, VLEVEL_L, "[BLE] %s -> ESP32\n\r", (c.type == CMD_TYPE) ? "komut" : "silme karari");
#endif
}

#if (GATEWAY_MODE == 1)
static void OnCmdRelayTimerEvent(void *context)
{
  relays[(uint32_t)context].due = true;
  PostEvent(EV_CMD_RELAY);
}

/* Paketi tampon r'ye al ve CMD_RELAY_COUNT kez yayin zamanla (ilki hemen) */
static void Relay_Start(uint32_t r, const uint8_t *pkt)
{
  Relay_t *rl = &relays[r];
  if (rl->left != 0)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[CMD] onceki paketin %d tekrari iptal (yenisi geldi)\n\r", rl->left);
  }
  memcpy(rl->buf, pkt, CMD_PACKET_LEN);
  rl->left = CMD_RELAY_COUNT;
  rl->due = false;
  UTIL_TIMER_Stop(&rl->timer);
  UTIL_TIMER_SetPeriod(&rl->timer, 30 + (Rand32() % 100));   /* ilk tekrar hemen */
  UTIL_TIMER_Start(&rl->timer);
}

#if (GW_FORWARD_CMD == 1)
/* Komutu [AA 55 11][15 B][RSSI][SNR][CRC8] cercevesiyle Zybo'ya yolla (konum cercevesiyle ayni stil) */
static void Gateway_ForwardCmd(const uint8_t *cmd, int16_t rssi, int8_t snr)
{
  uint8_t frame[3 + GW_CMD_FRAME_LEN_FIELD + 1];
  uint32_t i = 0;

  if (rssi < -128) { rssi = -128; }
  if (rssi > 127)  { rssi = 127; }

  frame[i++] = GW_SYNC0;
  frame[i++] = GW_SYNC1;
  frame[i++] = (uint8_t)GW_CMD_FRAME_LEN_FIELD;
  memcpy(&frame[i], cmd, CMD_PACKET_LEN);
  i += CMD_PACKET_LEN;
  frame[i++] = (uint8_t)(int8_t)rssi;
  frame[i++] = (uint8_t)snr;
  frame[i] = Crc8(&frame[2], i - 2);   /* LEN..SNR */
  i++;

  if (HAL_UART_Transmit(&huart1, frame, (uint16_t)i, 20) == HAL_OK)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[GW] komut -> Zybo %d byte\n\r", (int)i);
  }
  else
  {
    APP_LOG(TS_ON, VLEVEL_L, "[GW] komut USART1 gonderim HATASI\n\r");
  }
}
#endif
#endif

#if (GATEWAY_MODE == 0) && (NODE_UART_BRIDGE == 1)
/* ESP32'den (telefondan) UART cercevesi geldi: komutsa LoRa'ya hazirla */
static void Node_HandleUartFrame(void)
{
  uint16_t len = dlFrameLen;

  if ((len != CMD_PACKET_LEN) || ((dlFrame[0] != CMD_TYPE) && (dlFrame[0] != CMD_TYPE_DEL_REQ)))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[CMD] ESP32'den gecersiz cerceve (boy=%d, tip=0x%02X)\n\r",
            len, (len > 0) ? dlFrame[0] : 0);
    dlFrameReady = false;
    return;
  }
  /* Telefon sadece tur 1..6 isaret/komut koyabilir; 7 (GERI DON) admin'e ozel */
  if ((dlFrame[0] == CMD_TYPE) && ((dlFrame[6] < CMD_KIND_MIN) || (dlFrame[6] > CMD_KIND_PHONE_MAX)))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[CMD] telefondan izinsiz tur %u, atlandi\n\r", dlFrame[6]);
    dlFrameReady = false;
    return;
  }
  if (cmdTxPending)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[CMD] onceki komut henuz yollanmadi, yenisi uzerine yazildi\n\r");
  }
  memcpy(cmdTxBuf, dlFrame, CMD_PACKET_LEN);
  dlFrameReady = false;
  /* Kaynak adresini node doldurur (telefon hangi node'a bagli oldugunu bilmek zorunda degil) */
  cmdTxBuf[1] = (uint8_t)(myAddr & 0xFF);
  cmdTxBuf[2] = (uint8_t)(myAddr >> 8);
  cmdTxPending = true;
  LogCmd("telefondan", cmdTxBuf);
  APP_LOG(TS_OFF, VLEVEL_L, " -> LoRa\n\r");
}
#endif

/* [AA][55][LEN][payload][CRC8] cercevesini USART1'den yolla (Zybo / ESP32 ile ayni format) */
static void Uart1_SendFrame(const uint8_t *payload, uint16_t len)
{
  uint8_t frame[3 + 255 + 1];
  uint8_t crc;
  if (len > 255) { return; }
  frame[0] = 0xAA;
  frame[1] = 0x55;
  frame[2] = (uint8_t)len;
  memcpy(&frame[3], payload, len);
  crc = Crc8(&frame[2], 1 + len);
  frame[3 + len] = crc;
  (void)HAL_UART_Transmit(&huart1, frame, (uint16_t)(4 + len), 50);
}

/* CRC-8: polinom 0x07, init 0x00, yansitma yok, son XOR yok */
static uint8_t Crc8Update(uint8_t crc, uint8_t b)
{
  crc ^= b;
  for (int i = 0; i < 8; i++)
  {
    crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

static uint8_t Crc8(const uint8_t *data, uint32_t len)
{
  uint8_t crc = 0x00;
  for (uint32_t i = 0; i < len; i++)
  {
    crc = Crc8Update(crc, data[i]);
  }
  return crc;
}

#if (GATEWAY_MODE == 1)

/* Paketi [AA 55 LEN payload RSSI SNR CRC8] cercevesine sarip USART1'den yolla */
static void Gateway_ForwardPacket(const pos_packet_t *pkt, int16_t rssi, int8_t snr)
{
  uint8_t frame[GW_FRAME_SIZE];
  uint32_t i = 0;

  /* RSSI radyodan 16 bit gelir, cercevede int8: [-128, 127] araligina kirp */
  if (rssi < -128) { rssi = -128; }
  if (rssi > 127)  { rssi = 127; }

  frame[i++] = GW_SYNC0;
  frame[i++] = GW_SYNC1;
  frame[i++] = (uint8_t)GW_FRAME_LEN_FIELD;
  memcpy(&frame[i], pkt, sizeof(pos_packet_t));
  i += sizeof(pos_packet_t);
  frame[i++] = (uint8_t)(int8_t)rssi;
  frame[i++] = (uint8_t)snr;
  frame[i] = Crc8(&frame[2], i - 2);   /* LEN..SNR, sync haric */
  i++;

  /* 18 byte @115200 ~1.6 ms; ana donguden cagrildigi icin bloklayan gonderim yeterli */
  if (HAL_UART_Transmit(&huart1, frame, (uint16_t)i, 20) == HAL_OK)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[GW] -> USART1 %d byte, crc=0x%02X\n\r", (int)i, frame[i - 1]);
  }
  else
  {
    APP_LOG(TS_ON, VLEVEL_L, "[GW] USART1 gonderim HATASI\n\r");
  }
}

#endif /* GATEWAY_MODE == 1 (ForwardPacket) */

#if UART1_PARSER_USED
/* UART cerceve cozucu (Zybo -> gateway / ESP32 -> node): her byte icin kesmeden cagrilir.
   AA -> 55 -> LEN -> PAYLOAD -> CRC8. Bozuk cercevede basa doner. */
static void DL_ParseByte(uint8_t b)
{
  switch (dlState)
  {
    case DLP_SYNC0:
      if (b == GW_SYNC0) { dlState = DLP_SYNC1; }
      break;
    case DLP_SYNC1:
      if (b == GW_SYNC1)      { dlState = DLP_LEN; }
      else if (b != GW_SYNC0) { dlState = DLP_SYNC0; }  /* AA AA 55 gibi durumlarda kayma */
      break;
    case DLP_LEN:
      if (b == 0) { dlState = DLP_SYNC0; break; }
      dlRxLen = b;
      dlRxIdx = 0;
      dlCrc = Crc8Update(0x00, b);      /* CRC, LEN byte'indan baslar */
      dlState = DLP_DATA;
      break;
    case DLP_DATA:
      dlRxBuf[dlRxIdx++] = b;
      dlCrc = Crc8Update(dlCrc, b);
      if (dlRxIdx >= dlRxLen) { dlState = DLP_CRC; }
      break;
    case DLP_CRC:
    {
      if (dlCrc != b)
      {
        dlCrcErrors++;
      }
      else if (dlFrameReady)
      {
        dlDropped++;          /* onceki cerceve henuz yayinlanmadi */
      }
      else
      {
        memcpy(dlFrame, dlRxBuf, dlRxLen);
        dlFrameLen = dlRxLen;
        dlFrameReady = true;
        PostEvent(EV_DL_FRAME);
      }
      dlState = DLP_SYNC0;
      break;
    }
    default:
      dlState = DLP_SYNC0;
      break;
  }
}
#endif /* UART1_PARSER_USED */

#if (GATEWAY_MODE == 1)

/* Zybo'dan gelen tabloyu kontrol et ve LoRa ile herkese yayinla */
static void Gateway_SendDownlink(void)
{
  uint16_t len = dlFrameLen;
  uint8_t n = (len >= 2) ? dlFrame[1] : 0;

  if ((len < 2) || (dlFrame[0] != DL_TYPE_TABLE) || (n < 1) || (n > DL_MAX_RECORDS) ||
      (len != DL_PAYLOAD_LEN(n)))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[DL] gecersiz paket (boy=%d, tip=0x%02X), yayinlanmadi\n\r",
            len, (len > 0) ? dlFrame[0] : 0);
    dlFrameReady = false;
    return;
  }

  APP_LOG(TS_ON, VLEVEL_L, "[DL] %d byte, N=%d -> yayin (crc hata=%u, atlanan=%u)\n\r",
          len, n, (unsigned)dlCrcErrors, (unsigned)dlDropped);

  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
  UTIL_TIMER_Start(&timerLed);

  txBusy = true;
  RadioSend(dlFrame, (uint8_t)len);
  dlFrameReady = false;   /* dlFrame artik radyo tamponuna kopyalandi */
}

#if (GATEWAY_LOCAL_TABLE == 1)
/* ---- Zybo taklidi (Zybo yokken test icin) ---- */
static void OnLocalDlTimerEvent(void *context)
{
  PostEvent(EV_LOCAL_DL);
}

/* Zybo'daki mantigin aynisi: adres varsa uzerine yaz, yoksa ekle, doluysa say */
static void LocalTable_Update(const pos_packet_t *pkt)
{
  for (uint8_t i = 0; i < localTabCount; i++)
  {
    if (localTab[i].addr == pkt->addr)
    {
      localTab[i] = *pkt;
      return;
    }
  }
  if (localTabCount < DL_MAX_RECORDS)
  {
    localTab[localTabCount++] = *pkt;
  }
  else
  {
    localTabFull++;
  }
}

/* Tabloyu downlink paketine (D1 N kayitlar) cevir; normal yayin yolu (dlFrame) kullanilir */
static void LocalTable_BuildDownlink(void)
{
  if (localTabCount == 0) { return; }          /* bos tablo yayinlanmaz (Zybo gibi) */
  if (dlFrameReady)       { return; }          /* onceki henuz yayinlanmadi */
  dlFrame[0] = DL_TYPE_TABLE;
  dlFrame[1] = localTabCount;
  memcpy(&dlFrame[2], localTab, localTabCount * sizeof(pos_packet_t));
  dlFrameLen = (uint16_t)DL_PAYLOAD_LEN(localTabCount);
  dlFrameReady = true;
  APP_LOG(TS_ON, VLEVEL_L, "[LT] yerel tablo hazir: N=%d (dolu=%u)\n\r",
          localTabCount, (unsigned)localTabFull);
}
#endif
#endif

/* ---- USART1 RX (Zybo -> gateway). HAL callback'leri log sistemi kullaniyor,
        o yuzden USART1 kesmesini dogrudan register'lardan okuyoruz. ---- */
void Gateway_UartRxStart(void)
{
#if UART1_RX_ENABLED
  dlState = DLP_SYNC0;
  __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
  HAL_NVIC_SetPriority(USART1_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
#endif
}

void Gateway_UsartIrq(void)
{
  uint32_t isr = USART1->ISR;

  /* Hata bayraklari (overrun, framing, noise, parity) temizlenmezse kesme takilir */
  if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE))
  {
    USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
  }
  if (isr & USART_ISR_RXNE_RXFNE)
  {
    uint8_t b = (uint8_t)USART1->RDR;   /* okumak RXNE'yi temizler */
#if UART1_PARSER_USED
    DL_ParseByte(b);
#else
    (void)b;
#endif
  }
}

static void RadioSend(const uint8_t *data, uint8_t len)
{
  Radio.Sleep();
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                    LORA_SPREADING_FACTOR, LORA_CODINGRATE,
                    LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
                    true, 0, 0, LORA_IQ_INVERSION_ON, TX_TIMEOUT_VALUE);
  Radio.SetMaxPayloadLength(MODEM_LORA, MAX_APP_BUFFER_SIZE);
  Radio.Send((uint8_t *)data, len);
}

/* Surekli alim: timeout 0 = paket gelene kadar sonsuza dek dinle */
static void RadioRx(void)
{
  Radio.Sleep();
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetRxConfig(MODEM_LORA, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
                    LORA_CODINGRATE, 0, LORA_PREAMBLE_LENGTH,
                    LORA_SYMBOL_TIMEOUT, LORA_FIX_LENGTH_PAYLOAD_ON,
                    0, true, 0, 0, LORA_IQ_INVERSION_ON, true);
  Radio.SetMaxPayloadLength(MODEM_LORA, MAX_APP_BUFFER_SIZE);
  Radio.Rx(0);
}

/* Node durum makinesi: ana donguden (sequencer) cagrilir */
static void Node_Process(void)
{
  uint32_t ev;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  ev = AppEvents;
  AppEvents = 0;
  __set_PRIMASK(primask);

  /* Bozuk paket surucuden hem RX_DONE hem RX_ERROR olarak gelebiliyor:
     ikisi birlikteyse icerige guvenme, paketi hic isleme */
  if ((ev & EV_RX_DONE) && !(ev & EV_RX_ERROR))
  {
    HandleRxPacket();
  }
  if (ev & EV_RX_ERROR)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[RX] CRC/baslik hatasi (bozuk paket)\n\r");
  }
  if (ev & EV_TX_DONE)
  {
    txBusy = false;
  }
  if (ev & EV_TX_TIMEOUT)
  {
    txBusy = false;
    APP_LOG(TS_ON, VLEVEL_L, "[TX] timeout!\n\r");
  }

#if (GATEWAY_MODE == 1)
#if (GATEWAY_LOCAL_TABLE == 1)
  if (ev & EV_LOCAL_DL)
  {
    LocalTable_BuildDownlink();
  }
#endif
  /* Bekleyen downlink varsa yayinla. Radyo mesgulse (TX suruyor) dokunma:
     TX bitince gelen EV_TX_DONE bu fonksiyonu tekrar calistirir, o zaman yollanir. */
  /* Zybo'dan admin karari (0xC3) ya da admin isareti/komutu (0xC1, src 0xFFFE) geldiyse
     kendi tamponuna al, 3x yayinlanacak. Zybo'ya geri iletilmez. */
  if (dlFrameReady && (dlFrameLen == CMD_PACKET_LEN) &&
      ((dlFrame[0] == CMD_TYPE_DEL_ACK) || (dlFrame[0] == CMD_TYPE)))
  {
    uint32_t r = (dlFrame[0] == CMD_TYPE) ? RELAY_ADMIN_CMD : RELAY_ADMIN;
    /* Kendi yayinimizi geri duyarsak (olmamali) ikinci kez islemeyelim */
    (void)Cmd_MarkSeen(dlFrame[0], (uint16_t)(dlFrame[1] | (dlFrame[2] << 8)), dlFrame[5]);
    LogCmd("Zybo'dan", dlFrame);
    APP_LOG(TS_OFF, VLEVEL_L, " -> 3x yayin\n\r");
    Relay_Start(r, dlFrame);
    dlFrameReady = false;
  }
  if (dlFrameReady && !txBusy)
  {
    Gateway_SendDownlink();
    if (txBusy) { return; } /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
  }
  /* Tekrar yayin zamani gelmis paket varsa (once admin paketleri) */
  for (int32_t r = RELAY_COUNT_BUF - 1; (r >= 0) && !txBusy; r--)
  {
    Relay_t *rl = &relays[r];
    if (!rl->due || (rl->left == 0)) { continue; }
    rl->due = false;
    rl->left--;
    LogCmd("tekrar yayin", rl->buf);
    APP_LOG(TS_OFF, VLEVEL_L, " (kalan %d)\n\r", rl->left);
    if (rl->left != 0)
    {
      UTIL_TIMER_SetPeriod(&rl->timer, CMD_RELAY_GAP_MS + (Rand32() % 200));
      UTIL_TIMER_Start(&rl->timer);
    }
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
    UTIL_TIMER_Start(&timerLed);
    txBusy = true;
    RadioSend(rl->buf, CMD_PACKET_LEN);
    return; /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
  }
#elif (NODE_UART_BRIDGE == 1)
  /* Telefondan (ESP32 uzerinden) cerceve geldi mi */
  if (dlFrameReady)
  {
    Node_HandleUartFrame();
  }
#endif

#if (GATEWAY_MODE == 0)
  /* Telefondan gelen komut / silme istegi varsa (radyo bossa) yolla */
  if (cmdTxPending && !txBusy)
  {
    cmdTxPending = false;
    APP_LOG(TS_ON, VLEVEL_L, "[CMD] LoRa'ya yollandi (tip 0x%02X)\n\r", cmdTxBuf[0]);
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
    UTIL_TIMER_Start(&timerLed);
    txBusy = true;
    RadioSend(cmdTxBuf, CMD_PACKET_LEN);
    return; /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
  }
#endif

  /* Zamani gelmis bir sanal node varsa (ve radyo bossa) onun paketini yolla.
     Birden fazlasi ayni anda bekliyorsa siradakiler bu TX bitince (EV_TX_DONE) yollanir. */
  if ((pendingTx != 0) && !txBusy)
  {
    uint32_t vi = 0;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    while (((pendingTx >> vi) & 1U) == 0U) { vi++; }
    pendingTx &= ~(1U << vi);
    __set_PRIMASK(primask);

    BuildPositionPacket(&txPacket, vi);
#if (VIRTUAL_NODES > 1)
    APP_LOG(TS_ON, VLEVEL_L, "[TX v%d] addr=0x%04X seq=%3d", (int)vi, txPacket.addr, txPacket.seq);
#else
    APP_LOG(TS_ON, VLEVEL_L, "[TX] addr=0x%04X seq=%3d", txPacket.addr, txPacket.seq);
#endif
    LogCoord("lat", txPacket.lat);
    LogCoord("lon", txPacket.lon);
    APP_LOG(TS_OFF, VLEVEL_L, "\n\r");

    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
    UTIL_TIMER_Start(&timerLed);

    txBusy = true;
    RadioSend((const uint8_t *)&txPacket, sizeof(txPacket));
    ScheduleNextTx(vi);
    return; /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
  }

  /* TX yoksa her durumda tekrar dinle (RX done, hata, timeout, TX bitti...) */
  if (!txBusy)
  {
    RadioRx();
  }
}

/* USER CODE END PrFD */
