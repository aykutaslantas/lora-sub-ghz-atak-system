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
#if (SECURE_ENABLED == 1) && (GATEWAY_MODE == 0)
#include "lora_secure.h" /* AES-128 + CCM (header-only) */
#include "net_key.h"     /* NET_KEY_BYTES: git'e eklenmez */
#endif
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
  EV_POS_RELAY  = (1U << 9),   /* node: aktarim kuyrugundaki bir paketin zamani geldi (mesh) */
  EV_ACK        = (1U << 10),  /* node: bir ACK'in (ilk ya da tekrar) yollanma zamani geldi */
  EV_JOIN       = (1U << 11),  /* node: katilma istegi (0xA1) / durum zamani geldi */
  EV_SECTX      = (1U << 12),  /* gateway: Zybo'dan gelen guvenli paketin tekrar zamani */
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
#if (GATEWAY_MODE == 0) && (JOIN_ENABLED == 1) && ((VIRTUAL_NODES != 1) || (NODE_UART_BRIDGE != 1))
#error "JOIN_ENABLED=1 icin VIRTUAL_NODES=1 ve NODE_UART_BRIDGE=1 olmali"
#endif
#if (GATEWAY_MODE == 1) && (SECURE_ENABLED == 1) && (GATEWAY_LOCAL_TABLE == 1)
#error "SECURE_ENABLED=1 ile GATEWAY_LOCAL_TABLE=1 birlikte kullanilamaz (gateway anahtar tutmaz)"
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
#if (GATEWAY_MODE == 0) && (NODE_UART_BRIDGE == 1)
/* Telefondan gelen gercek konum (0xC4). posOvrValid=false -> sahte konum */
static bool     posOvrValid = false;
static int32_t  posOvrLat = 0;
static int32_t  posOvrLon = 0;
static uint32_t posOvrTime = 0;             /* son 0xC4'un geldigi an (ms) */
#endif
/* ---- Tek sekme mesh ---- */
#if (GATEWAY_MODE == 0)
static uint32_t bootTime = 0;               /* acilis ani (ms) */
/* Zybo tablosunda kendi kaydimizi DOGRUDAN ulasmis olarak (bit2 yok) son gordugumuz an.
   Aktarimla ulasmis kayit (bit2 var) "dogrudan ulasiyorum" sayilmaz. */
static uint32_t lastInTableTime = 0;
static bool     everInTable = false;
static uint32_t lastViaRelayTime = 0;       /* kaydimizi aktarimla ulasmis gordugumuz son an */
static bool     relayReqLogged = false;     /* "aktarim isteniyor" durumu (log gecisleri icin) */
/* Aktarim kuyrugu: 0xE1 (konum), 0xE2 (yukari kontrol), 0xE3 (asagi). Her slotun kendi
   rastgele gecikme timer'i var; ayni paketi baska komsu once aktarirsa slot iptal edilir. */
#define RELAY_Q_SIZE     3
typedef struct
{
  uint8_t  buf[MAX_APP_BUFFER_SIZE];
  uint8_t  len;
  uint32_t key;
  bool     active;
  volatile bool due;
  UTIL_TIMER_Object_t timer;
} RelaySlot_t;
static RelaySlot_t relayQ[RELAY_Q_SIZE];
/* Aktardigimiz ya da baskasinin aktardigini duydugumuz paket anahtarlari */
#define RELAY_SEEN_SIZE  24
static uint32_t relaySeen[RELAY_SEEN_SIZE];
static uint8_t  relaySeenIdx = 0;
/* Bizden aktarim isteyen node'lar ("musteriler") */
typedef struct
{
  uint16_t addr;
  uint32_t lastSeen;
} RelayClient_t;
static RelayClient_t relayClients[MAX_RELAY_CLIENTS];
/* Musterinin yukari aktardigimiz 0xC1'i: gateway'den donen kopyasini asagi aktaracagiz */
static uint32_t echoWaitKey = 0;
/* Su an islenen paket 0xE3'ten acildi (tekrar aktarilmaz) */
static bool     rxViaRelay = false;
/* Paket anahtari: [31]=1 [29..24]=etiket [23..0]=deger */
#define RKEY(tag, v)     (0x80000000U | ((uint32_t)(tag) << 24) | ((uint32_t)(v) & 0x00FFFFFFU))
#define RTAG_POS         0x01
#define RTAG_UL(t)       (0x10U | CTRL_TAG(t))
#define RTAG_DL(t)       (0x20U | CTRL_TAG(t))
#define RTAG_TABLE       0x30
#endif
#if (GATEWAY_MODE == 1)
/* Zybo'ya iletilen konumlar: ayni paket hem dogrudan hem aktarilmis gelirse bir kez ilet */
#define POS_SEEN_SIZE    16
static uint32_t posSeen[POS_SEEN_SIZE];
static uint8_t  posSeenIdx = 0;
#endif

#if (GATEWAY_MODE == 0)
/* Komut ACK'i (0xC5) kuyrugu: her slot bir ACK'i ACK_REPEAT kez yollar */
typedef struct
{
  uint16_t me;            /* ACK'i yollayan adres (node / sanal node) */
  uint16_t ackedSrc;      /* onaylanan komutun src'si */
  uint8_t  ackedId;       /* onaylanan komutun msg_id'si */
  uint8_t  kind;          /* ACK_KIND_DELIVERED / ACK_KIND_SEEN */
  uint8_t  left;          /* kalan gonderim */
  bool     active;
  volatile bool due;
  UTIL_TIMER_Object_t timer;
} AckSlot_t;
static AckSlot_t ackQ[ACK_Q_SIZE];
static uint8_t  ackTxBuf[CMD_PACKET_LEN];
static uint8_t  ackSeq = 0;
/* Aktarici: asagi aktarilan admin komutlarinin son aktarim zamani (Zybo yeniden denerse) */
#define DL_CMD_HIST      4
static struct { uint32_t key; uint32_t t; } dlCmdHist[DL_CMD_HIST];
static uint8_t  dlCmdHistIdx = 0;
#endif
/* ---- JOIN v1 ---- */
#if (GATEWAY_MODE == 0)
static uint32_t myDevId = 0;                /* UID'nin CRC32'si (A1 / A2 eslesmesi) */
static uint16_t uidAddr = 0;                /* UID'den turetilen adres (kimlik yokken) */
static uint8_t  joinState = JOIN_ST_DISABLED;
static uint8_t  joinResult = 0;
static uint8_t  joinTeam = 0;               /* istenen / verilen takim */
static uint8_t  joinNum = 0;                /* istenen / verilen numara (sonuc 4: onerilen) */
static uint16_t joinPin = 0;
static uint8_t  joinReqSeq = 0;
static volatile bool joinDue = false;
static UTIL_TIMER_Object_t timerJoin;
/* Kick / leave */
static bool     kickPending = false;        /* atildik: ULASTI'lar bitince kimlik silinecek */
static uint16_t kickedAddr = 0;             /* atildigimiz adres (gec gelen A3'e eski adresten ULASTI) */
static uint8_t  leaveTries = 0;
static uint8_t  leaveMsgId = 0;
typedef struct
{
  uint32_t magic;
  uint16_t addr;
  uint8_t  team;
  uint8_t  num;
  uint32_t devId;
  uint32_t chk;           /* ilk 12 byte'in CRC32'si */
} IdRec_t;
#endif
#if (GATEWAY_MODE == 1)
/* A1 tekrar eleme: (dev_id, req_seq) */
#define JOIN_SEEN_SIZE   8
static struct { uint32_t devId; uint8_t seq; bool used; } joinSeen[JOIN_SEEN_SIZE];
static uint8_t  joinSeenIdx = 0;
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
#if (SECURE_ENABLED == 1)
#if (GATEWAY_MODE == 0)
/* ---- Faz 1 guvenlik (node) ---- */
static uint32_t secCtr = 1;                 /* bir sonraki paketin sayaci */
static uint32_t secSaved = 0;               /* flash'a en son yazilan sayac */
static uint16_t secSlot = 0;                /* sayac sayfasindaki bir sonraki bos yuva */
static bool     secNeedErase = false;       /* sayfada bozuk yuva var: ilk yazmada silinecek */
typedef struct { uint32_t devId; uint32_t last; uint32_t bits; uint32_t used; } SecPeer_t;
static SecPeer_t secPeers[SEC_REPLAY_SIZE]; /* gonderen basina son sayac + 32'lik pencere */
static uint32_t secUseTick = 0;
static uint8_t  secRaw[MAX_APP_BUFFER_SIZE];/* son alinan guvenli paketin ham hali (aktarim icin) */
static uint16_t secRawLen = 0;
static bool     secRawValid = false;        /* true: son paket dogrudan guvenli (ic ice degil) */
static uint8_t  secTxBuf[MAX_APP_BUFFER_SIZE];
static uint8_t  secInner[MAX_APP_BUFFER_SIZE];
static uint32_t secMicFail = 0, secReplayDrop = 0;
/* Son dogrudan alinan guvenli paketin dis zarfi */
static uint8_t  secRxVer = 0;               /* 0x51 / 0x52 (0 = duz ya da ic ice) */
static uint32_t secRxDev = 0;               /* gonderenin dev_id'si */
static uint32_t secRxRelayDev = SEC_RELAY_ANY; /* 0x52: gonderenin sectigi aktarici */
/* Gateway baglanti kalitesi: Zybo'dan DOGRUDAN (E3 icinde degil) duydugumuz son paket */
static uint32_t zyboDirectTime = 0;
static int8_t   zyboDirectSnr = 0;
static bool     zyboDirectEver = false;
/* Aktarici secimi (uzak node): dogrudan duydugumuz komsular */
typedef struct
{
  uint32_t devId;
  uint16_t addr;
  uint8_t  link;          /* uzak <-> komsu, LINK_Q(SNR) */
  uint8_t  gwq;           /* komsunun kendi gateway kalitesi (bayrak bit3..6) */
  uint32_t lastSeen;
  uint32_t badTime;       /* kotu isaretlendigi an */
  bool     bad;
  bool     used;
} Nb_t;
static Nb_t     nbList[NB_MAX];
static uint32_t curRelayDev = SEC_RELAY_ANY;   /* su anki secili aktarici */
static uint32_t txRelayDev = SEC_RELAY_ANY;    /* yollanacak konum paketine yazilacak */
/* Fallback: adini koydugumuz son konum aktarildi mi (aktaricinin E1'ini duyarak) */
static bool     chkActive = false;
static uint32_t chkDev = 0;
static uint8_t  chkSeq = 0;
static uint8_t  chkMiss = 0;
/* Aktarildigini duydugumuz konumlarimiz (Zybo bit2 koymazsa tablo icin emniyet) */
static struct { uint8_t seq; uint32_t t; bool used; } myRelayed[MY_RELAYED_SEQ_N];
static uint8_t  myRelayedIdx = 0;
static bool     sawViaBit = false;          /* Zybo tabloda bit2'yi koyuyor: emniyete gerek yok */
#else
/* ---- Faz 1 guvenlik (gateway): Zybo'dan gelen hazir guvenli paketler, tx_repeat kez ---- */
typedef struct
{
  uint8_t  buf[MAX_APP_BUFFER_SIZE];
  uint8_t  len;
  uint8_t  left;          /* kalan yayin */
  uint32_t seq;           /* eklenme sirasi (eski once) */
  bool     active;
  volatile bool due;
  UTIL_TIMER_Object_t timer;
} SecTx_t;
static SecTx_t  secTxQ[SEC_TXQ_SIZE];
static uint32_t secTxSeq = 0;
#endif
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
#if (SECURE_ENABLED == 1) && (GATEWAY_MODE == 0)
static void Sec_Init(void);
static bool Sec_Incoming(void);
static void AirSend(const uint8_t *data, uint8_t len);
static void Nb_Update(uint32_t dev, uint16_t addr, int8_t snr, uint8_t gwq);
static uint32_t Relay_Choose(uint8_t seq);
static void Relay_ResetChoice(void);
static void Relay_NoteMine(uint16_t relayer, bool isPos, uint8_t seq);
static bool Relay_TableGuard(uint8_t seq);
#else
#define AirSend(d, l)   RadioSend((d), (l))
#endif
#if (SECURE_ENABLED == 1) && (GATEWAY_MODE == 1)
static void SecTx_Add(const uint8_t *pkt, uint16_t len, uint8_t repeat);
static bool SecTx_Service(void);
static void OnSecTxTimer(void *context);
#endif
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
#if (GATEWAY_MODE == 0)
static void Node_QueueAck(uint16_t me, uint16_t ackedSrc, uint8_t ackedId, uint8_t kind);
static void OnAckTimer(void *context);
static void RelaySeen_Forget(uint32_t key);
static bool DlCmd_RetryDue(uint32_t key);
static void DlCmd_Note(uint32_t key);
static bool Node_NeedsRelay(void);
static bool Node_CanRelay(void);
static bool RelaySeen_Mark(uint32_t key);
static void Relay_Enqueue(uint32_t key, uint8_t type, bool withLink, int16_t rssi, int8_t snr,
                          const uint8_t *body, uint16_t bodyLen,
                          uint32_t minMs, uint32_t randMs, const char *what);
static void Relay_Cancel(uint32_t key, uint16_t byWhom);
static void OnRelaySlotTimer(void *context);
static uint32_t Hash24(const uint8_t *p, uint16_t n);
static void Client_Touch(uint16_t addr);
static void Client_Drop(uint16_t addr);
static bool Client_IsActive(uint16_t addr);
typedef enum { RLY_ANY = 0, RLY_ME, RLY_NO } RlyMode_t;
static RlyMode_t Relay_RxMode(void);
static uint8_t Node_GwQuality(void);
static bool Client_AnyActive(void);
static void HandleRelayedDl(void);
#endif
#if (GATEWAY_MODE == 1)
static bool PosSeen_Mark(uint16_t addr, uint8_t seq);
#if (GW_FORWARD_RELAY_INFO == 1)
static void Gateway_ForwardRelayed(const uint8_t *relayPkt, int16_t rssi, int8_t snr);
#endif
#endif
static void HandleRelayedPos(void);
static void HandleRelayedUlCtrl(void);
static uint32_t Crc32(const uint8_t *p, uint32_t n);
#if (GATEWAY_MODE == 0)
static void Join_Init(void);
static void Join_SendStatus(void);
static void Join_HandleAnswer(void);
static void Join_Start(uint8_t team, uint8_t num, uint16_t pin);
static void Join_Clear(uint8_t result);
static void Join_Leave(void);
static void Join_HandleKick(void);
static bool Ack_Pending(uint16_t me);
static void OnJoinTimer(void *context);
static bool Id_Load(IdRec_t *r);
static bool Id_Save(uint16_t addr, uint8_t team, uint8_t num);
static void Id_Erase(void);
#endif
#if (GATEWAY_MODE == 1)
static void Gateway_HandleJoinReq(void);
static void Gateway_ForwardFrame(const uint8_t *payload, uint16_t len, int16_t rssi, int8_t snr, const char *what);
#endif
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
  APP_LOG(TS_OFF, VLEVEL_M, "APPLICATION_VERSION: V%u.%u.%u\r\n",
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
    /* bit15 = 1 araligi (0x8000+) JOIN ile verilen takim adreslerine ayrildi */
    myAddr &= (uint16_t)~ADDR_JOINED_FLAG;
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
#if (GATEWAY_MODE == 0)
  /* JOIN: flash'ta gecerli kimlik varsa adres onunla degisir */
  Join_Init();
#if (SECURE_ENABLED == 1)
  Sec_Init();   /* dev_id Join_Init'te hesaplandi */
#endif
#endif

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
#if (GATEWAY_MODE == 0)
  /* Tek sekme mesh: komsu paketini rastgele gecikmeyle aktarma timer'i */
  for (uint32_t i = 0; i < RELAY_Q_SIZE; i++)
  {
    UTIL_TIMER_Create(&relayQ[i].timer, POS_RELAY_DELAY_MIN_MS, UTIL_TIMER_ONESHOT, OnRelaySlotTimer, (void *)i);
  }
  for (uint32_t i = 0; i < ACK_Q_SIZE; i++)
  {
    UTIL_TIMER_Create(&ackQ[i].timer, ACK_FIRST_DELAY_MIN_MS, UTIL_TIMER_ONESHOT, OnAckTimer, (void *)i);
  }
  /* JOIN: kimlik yokken A1 / durum periyodu */
  UTIL_TIMER_Create(&timerJoin, JOIN_PERIOD_MS, UTIL_TIMER_ONESHOT, OnJoinTimer, NULL);
  if ((joinState == JOIN_ST_NEED_PARAMS) || (joinState == JOIN_ST_WAITING))
  {
    UTIL_TIMER_SetPeriod(&timerJoin, 2000);
    UTIL_TIMER_Start(&timerJoin);
  }
  bootTime = UTIL_TIMER_GetCurrentTime();
#if (TEST_FAR_NODE == 1)
  APP_LOG(TS_OFF, VLEVEL_M, "!!! TEST_FAR_NODE=1: gateway duyulmuyor sayiliyor (MASA TESTI) !!!\n\r");
#endif
#endif
#if (GATEWAY_MODE == 1) && (TEST_GW_IGNORE_DIRECT != 0)
  APP_LOG(TS_OFF, VLEVEL_M, "!!! TEST_GW_IGNORE_DIRECT=0x%04X: dogrudan paketleri atiliyor (MASA TESTI) !!!\n\r",
          (unsigned)TEST_GW_IGNORE_DIRECT);
#endif
#if (GATEWAY_MODE == 1)
  /* Komut tekrar yayini (Faz 2) */
  for (uint32_t r = 0; r < RELAY_COUNT_BUF; r++)
  {
    UTIL_TIMER_Create(&relays[r].timer, CMD_RELAY_GAP_MS, UTIL_TIMER_ONESHOT, OnCmdRelayTimerEvent, (void *)r);
  }
#if (SECURE_ENABLED == 1)
  for (uint32_t i = 0; i < SEC_TXQ_SIZE; i++)
  {
    UTIL_TIMER_Create(&secTxQ[i].timer, CMD_RELAY_GAP_MS, UTIL_TIMER_ONESHOT, OnSecTxTimer, (void *)i);
  }
#endif
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
#if (SECURE_ENABLED == 1) && (GATEWAY_MODE == 1)
  APP_LOG(TS_OFF, VLEVEL_M, "SECURE: gateway boru modu (guvenli paketler acilmadan Zybo'ya, %s)\n\r",
          (SECURE_ONLY == 1) ? "duz paketler atilir" : "duz paketler eski yolla");
#endif
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
#if (GATEWAY_MODE == 0) && (NODE_UART_BRIDGE == 1)
  /* Telefon konumunu yolluyorsa (0xC4) sahte konum yerine onu kullan (sadece asil adres) */
  if (posOvrValid && (vi == 0))
  {
    if ((UTIL_TIMER_GetCurrentTime() - posOvrTime) > POS_OVERRIDE_TIMEOUT_MS)
    {
      posOvrValid = false;
      APP_LOG(TS_ON, VLEVEL_L, "[POS] telefondan konum gelmiyor, sahte konuma donuldu\n\r");
    }
    else
    {
      pkt->lat   = posOvrLat;
      pkt->lon   = posOvrLon;
      pkt->flags = POS_FLAG_FIX;
    }
  }
#endif
#if (GATEWAY_MODE == 0)
  /* Tek sekme mesh: admin'e ulasamiyorsak komsulardan aktarim iste */
  if (Node_NeedsRelay())
  {
    pkt->flags |= POS_FLAG_RELAY_REQ;
    if (!relayReqLogged)
    {
      relayReqLogged = true;
      APP_LOG(TS_ON, VLEVEL_L, "[MESH] %u sn'dir tabloda yokum: komsulardan aktarim isteniyor\n\r",
              (unsigned)(REACH_TIMEOUT_MS / 1000));
    }
  }
  else if (relayReqLogged)
  {
    relayReqLogged = false;
    APP_LOG(TS_ON, VLEVEL_L, "[MESH] tekrar dogrudan ulasiyorum: aktarim istegi kapandi\n\r");
  }
  /* Komsular aktarici secerken baksin: bizim gateway baglanti kalitemiz (bit3..6) */
  pkt->flags |= (uint8_t)((Node_GwQuality() << POS_FLAG_GWQ_SHIFT) & POS_FLAG_GWQ_MASK);
#if (SECURE_ENABLED == 1)
  /* Aktarici secimi: asil adresin aktarim isteyen konumu 0x52 zarfla, secilen komsunun adiyla */
  if ((vi == 0) && (pkt->flags & POS_FLAG_RELAY_REQ))
  {
    txRelayDev = Relay_Choose(pkt->seq);
  }
  else if (vi == 0)
  {
    Relay_ResetChoice();
  }
#endif
#endif
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

#if (SECURE_ENABLED == 1)
#if (GATEWAY_MODE == 1)
  /* Gateway duz boru: guvenli paketi acmadan Zybo'ya ver (dogrulama / tekrar yayin Zybo'da) */
  if (((BufferRx[0] == SEC_VER) && (RxBufferSize >= SEC_PIPE_MIN_LEN)) ||
      ((BufferRx[0] == SEC_VER2) && (RxBufferSize >= SEC_PIPE_MIN_LEN2)))
  {
    Gateway_ForwardFrame(BufferRx, RxBufferSize, RssiValue, SnrValue, "guvenli paket");
    return;
  }
#if (SECURE_ONLY == 1)
  APP_LOG(TS_ON, VLEVEL_L, "[SEC] duz paket atildi (%u B, SECURE_ONLY)\n\r", (unsigned)RxBufferSize);
  return;
#endif
#else
  /* Node: zarfi ac, MIC + sayac kontrolu. Sonrasi bugunku duz paket isleme ile ayni. */
  if (!Sec_Incoming())
  {
    return;
  }
#endif
#endif

#if (GATEWAY_MODE == 0) && (TEST_FAR_NODE == 1)
  /* MASA TESTI: gateway'in yayinlarini (tablo, admin komut/karar) duymamis gibi yap */
  if (((RxBufferSize >= 2) && (BufferRx[0] == DL_TYPE_TABLE) &&
       (RxBufferSize == DL_PAYLOAD_LEN(BufferRx[1]))) ||
      ((RxBufferSize == CMD_PACKET_LEN) &&
       ((BufferRx[0] == CMD_TYPE) || (BufferRx[0] == CMD_TYPE_DEL_ACK) || (BufferRx[0] == JOIN_TYPE_KICK))))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[TEST] gateway yayini (tip 0x%02X) duyulmadi sayildi\n\r", BufferRx[0]);
    return;
  }
#endif

  /* JOIN: katilma istegi (0xA1, 17 B). Gateway Zybo'ya iletir, node'lar yok sayar (v1: aktarim yok) */
  if ((RxBufferSize == JOIN_REQ_LEN) && (BufferRx[0] == JOIN_TYPE_REQ))
  {
#if (GATEWAY_MODE == 1)
    Gateway_HandleJoinReq();
#endif
    return;
  }
  /* JOIN: katilma cevabi (0xA2, 15 B). Gateway'in kendi yayini; node'lar kendi dev_id'sine bakar */
  if ((RxBufferSize == JOIN_ANS_LEN) && (BufferRx[0] == JOIN_TYPE_ANS))
  {
#if (GATEWAY_MODE == 0)
    Join_HandleAnswer();
#endif
    return;
  }

  /* Tek sekme mesh: komsu uzerinden aktarilmis konum mu? (0xE1, 17 byte) */
  if ((RxBufferSize == RELAY_POS_LEN) && (BufferRx[0] == RELAY_TYPE_POS))
  {
    HandleRelayedPos();
    return;
  }
  /* Tek sekme mesh: aktarilmis yukari kontrol paketi mi? (0xE2, 20 byte) */
  if ((RxBufferSize == RELAY_UL_LEN) && (BufferRx[0] == RELAY_TYPE_UL_CTRL))
  {
    HandleRelayedUlCtrl();
    return;
  }
  /* Tek sekme mesh: aktarilmis downlink mi? (0xE3 + orijinal) */
  if ((RxBufferSize > RELAY_DL_HDR) && (BufferRx[0] == RELAY_TYPE_DL))
  {
#if (GATEWAY_MODE == 0)
    HandleRelayedDl();
#endif
    return; /* gateway kendi yayininin aktarilmis halini yok sayar */
  }

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
#if (GATEWAY_MODE == 1) && (TEST_GW_IGNORE_DIRECT != 0)
    /* MASA TESTI: bu node'un DOGRUDAN gelen kontrol paketini at (aktarilmisi 0xE2 ile gelir) */
    if ((uint16_t)(BufferRx[1] | (BufferRx[2] << 8)) == (uint16_t)TEST_GW_IGNORE_DIRECT)
    {
      APP_LOG(TS_ON, VLEVEL_L, "[TEST] 0x%04X dogrudan kontrol paketi (tip 0x%02X) atildi\n\r",
              (unsigned)TEST_GW_IGNORE_DIRECT, BufferRx[0]);
      return;
    }
#endif
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
#if (GATEWAY_MODE == 1) && (TEST_GW_IGNORE_DIRECT != 0)
  /* MASA TESTI: bu node menzil disindaymis gibi; dogrudan paketini at (aktarilmisi gelir) */
  if (pkt.addr == (uint16_t)TEST_GW_IGNORE_DIRECT)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[TEST] 0x%04X dogrudan paketi atildi (seq=%d)\n\r", pkt.addr, pkt.seq);
    return;
  }
#endif

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
  APP_LOG(TS_OFF, VLEVEL_L, " fix=%d%s rssi=%d snr=%d", (pkt.flags & POS_FLAG_FIX) ? 1 : 0,
          (pkt.flags & POS_FLAG_RELAY_REQ) ? " AKTARIM-ISTIYOR" : "", RssiValue, SnrValue);
  if (p != NULL)
  {
    APP_LOG(TS_OFF, VLEVEL_L, " alinan=%u kayip=%u", (unsigned)p->rx_count, (unsigned)p->lost_count);
  }
  APP_LOG(TS_OFF, VLEVEL_L, "\n\r");

#if (GATEWAY_MODE == 1)
  /* Ayni paket komsu uzerinden de gelmis olabilir: bir kez ilet */
  if (!PosSeen_Mark(pkt.addr, pkt.seq))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[GW] 0x%04X seq=%d zaten iletildi (aktarilmisi once geldi)\n\r",
            pkt.addr, pkt.seq);
    return;
  }
  /* Adres filtrelemesi YOK: hangi node'un kabul edilecegine Zybo PL karar verir */
  Gateway_ForwardPacket(&pkt, RssiValue, SnrValue);
#if (GATEWAY_LOCAL_TABLE == 1)
  LocalTable_Update(&pkt);
#endif
#else
#if (SECURE_ENABLED == 1)
  /* Aktarici secimi icin komsu listesi: dogrudan duydugumuz komsu, baglanti ve GW kalitesi */
  if (secRxVer != 0)
  {
    Nb_Update(secRxDev, pkt.addr, SnrValue, (uint8_t)((pkt.flags & POS_FLAG_GWQ_MASK) >> POS_FLAG_GWQ_SHIFT));
  }
#endif
  /* Tek sekme mesh: komsu aktarim istiyor. Onu "musteri" olarak kaydet (asagi yon icin);
     biz admin'e dogrudan ulasiyorsak konumunu aktar. 0x52 ile aktaricisini secmisse sadece
     secilen aktarir (hemen); baskasini sectiyse musterimiz degildir. */
  if (pkt.flags & POS_FLAG_RELAY_REQ)
  {
    RlyMode_t m = Relay_RxMode();
    if (m == RLY_NO)
    {
      Client_Drop(pkt.addr);
      APP_LOG(TS_ON, VLEVEL_L, "[MESH] 0x%04X aktarimi baska komsudan istedi, aktarilmadi\n\r", pkt.addr);
    }
    else
    {
      Client_Touch(pkt.addr);
      if (Node_CanRelay())
      {
        if (m == RLY_ME)
        {
          APP_LOG(TS_ON, VLEVEL_L, "[MESH] 0x%04X aktarici olarak beni secti\n\r", pkt.addr);
        }
        Relay_Enqueue(RKEY(RTAG_POS, ((uint32_t)pkt.addr << 8) | pkt.seq), RELAY_TYPE_POS,
                      true, RssiValue, SnrValue, (const uint8_t *)&pkt, sizeof(pkt),
                      (m == RLY_ME) ? RELAY_CHOSEN_DELAY_MIN_MS : POS_RELAY_DELAY_MIN_MS,
                      (m == RLY_ME) ? RELAY_CHOSEN_DELAY_RAND_MS : POS_RELAY_DELAY_RAND_MS, "konum");
      }
    }
  }
#endif
}

/* ---------------- Tek sekme mesh ---------------- */

#if (GATEWAY_MODE == 0)
/* Admin'e DOGRUDAN ulasamiyor muyuz? REACH_TIMEOUT_MS'dir tabloda kendimizi dogrudan
   ulasmis (bit2 yok) gormedik; hic gormediysek acilistan beri say. Aktarimla ulasiyorsak
   da istek acik kalir, yoksa aktarim durur ve konumumuz admin'de bayatlar (salinim). */
static bool Node_NeedsRelay(void)
{
  uint32_t ref = everInTable ? lastInTableTime : bootTime;
  return (UTIL_TIMER_GetCurrentTime() - ref) > REACH_TIMEOUT_MS;
}

/* Baskasinin paketini aktarabilir miyiz? Sadece son REACH_TIMEOUT_MS icinde admin'e
   DOGRUDAN ulastiysak (tek sekme: aktarimla ulasan node baskasini aktarmaz) */
static bool Node_CanRelay(void)
{
  return everInTable && ((UTIL_TIMER_GetCurrentTime() - lastInTableTime) <= REACH_TIMEOUT_MS);
}

/* Paket anahtari daha once aktarildi / aktarildigi duyuldu mu? Degilse kaydet, true don */
static bool RelaySeen_Mark(uint32_t key)
{
  for (uint32_t i = 0; i < RELAY_SEEN_SIZE; i++)
  {
    if (relaySeen[i] == key) { return false; }
  }
  relaySeen[relaySeenIdx] = key;
  relaySeenIdx = (uint8_t)((relaySeenIdx + 1) % RELAY_SEEN_SIZE);
  return true;
}

/* Paketi [type][aktaran][(RSSI SNR)][body] olarak sar ve rastgele gecikmeyle aktarmak
   uzere kuyruga al. Ayni anahtar daha once aktarildiysa / duyulduysa bir sey yapmaz. */
static void Relay_Enqueue(uint32_t key, uint8_t type, bool withLink, int16_t rssi, int8_t snr,
                          const uint8_t *body, uint16_t bodyLen,
                          uint32_t minMs, uint32_t randMs, const char *what)
{
  uint16_t hdr = withLink ? 5U : 3U;
  RelaySlot_t *s = NULL;
  uint16_t room = MAX_APP_BUFFER_SIZE;

#if (SECURE_ENABLED == 1)
  /* Ic ice aktarim: uzak node'un / Zybo'nun guvenli paketi ACILMADAN, aynen tasinir */
  if (secRawValid)
  {
    body = secRaw;
    bodyLen = secRawLen;
  }
  room = MAX_APP_BUFFER_SIZE - SEC_OVERHEAD;   /* bizim zarfimiza da sigmali */
#endif
  if ((uint32_t)hdr + bodyLen > room)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[MESH] %s cok buyuk (%u B), aktarilmadi\n\r", what, (unsigned)bodyLen);
    return;
  }
  if (!RelaySeen_Mark(key))
  {
    return; /* bu paketi zaten aktardik ya da baskasi aktardi */
  }
  for (uint32_t i = 0; i < RELAY_Q_SIZE; i++)
  {
    if (!relayQ[i].active) { s = &relayQ[i]; break; }
  }
  if (s == NULL)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[MESH] aktarim kuyrugu dolu, %s atlandi\n\r", what);
    return;
  }
  if (rssi < -128) { rssi = -128; }
  if (rssi > 127)  { rssi = 127; }
  s->buf[0] = type;
  s->buf[1] = (uint8_t)(myAddr & 0xFF);
  s->buf[2] = (uint8_t)(myAddr >> 8);
  if (withLink)
  {
    s->buf[3] = (uint8_t)(int8_t)rssi;
    s->buf[4] = (uint8_t)snr;
  }
  memcpy(&s->buf[hdr], body, bodyLen);
  s->len = (uint8_t)(hdr + bodyLen);
  s->key = key;
  s->active = true;
  s->due = false;
  uint32_t delay = minMs + (Rand32() % randMs);
  UTIL_TIMER_Stop(&s->timer);
  UTIL_TIMER_SetPeriod(&s->timer, delay);
  UTIL_TIMER_Start(&s->timer);
  APP_LOG(TS_ON, VLEVEL_L, "[MESH] %s %u ms sonra aktarilacak (tip 0x%02X)\n\r", what, (unsigned)delay, type);
}

/* Baska bir komsu bu paketi aktardi: bizim bekleyen aktarimimizi iptal et */
static void Relay_Cancel(uint32_t key, uint16_t byWhom)
{
  (void)RelaySeen_Mark(key);
  for (uint32_t i = 0; i < RELAY_Q_SIZE; i++)
  {
    if (relayQ[i].active && (relayQ[i].key == key))
    {
      relayQ[i].active = false;
      relayQ[i].due = false;
      UTIL_TIMER_Stop(&relayQ[i].timer);
      APP_LOG(TS_ON, VLEVEL_L, "[MESH] 0x%04X ayni paketi aktardi, bizimki iptal (tip 0x%02X)\n\r",
              byWhom, relayQ[i].buf[0]);
    }
  }
}

static void OnRelaySlotTimer(void *context)
{
  relayQ[(uint32_t)context].due = true;
  PostEvent(EV_POS_RELAY);
}

/* Tablonun kimligi icin kisa ozet (FNV-1a, 24 bite katlanmis) */
static uint32_t Hash24(const uint8_t *p, uint16_t n)
{
  uint32_t h = 2166136261U;
  for (uint16_t i = 0; i < n; i++)
  {
    h ^= p[i];
    h *= 16777619U;
  }
  return (h >> 24) ^ (h & 0x00FFFFFFU);
}

/* Bayrakli paketini duydugumuz node'u musteri listesine al / tazele */
static void Client_Touch(uint16_t addr)
{
  uint32_t now = UTIL_TIMER_GetCurrentTime();
  RelayClient_t *slot = NULL;
  for (uint32_t i = 0; i < MAX_RELAY_CLIENTS; i++)
  {
    if (relayClients[i].addr == addr) { slot = &relayClients[i]; break; }
  }
  if (slot == NULL)
  {
    /* bos ya da en eski yuva */
    slot = &relayClients[0];
    for (uint32_t i = 0; i < MAX_RELAY_CLIENTS; i++)
    {
      if (relayClients[i].addr == 0) { slot = &relayClients[i]; break; }
      if ((now - relayClients[i].lastSeen) > (now - slot->lastSeen)) { slot = &relayClients[i]; }
    }
    if (!Client_IsActive(addr))
    {
      APP_LOG(TS_ON, VLEVEL_L, "[MESH] 0x%04X aktarim istiyor: musteri olarak eklendi\n\r", addr);
    }
    slot->addr = addr;
  }
  slot->lastSeen = now;
}

static bool Client_IsActive(uint16_t addr)
{
  uint32_t now = UTIL_TIMER_GetCurrentTime();
  for (uint32_t i = 0; i < MAX_RELAY_CLIENTS; i++)
  {
    if ((relayClients[i].addr == addr) && (addr != 0) &&
        ((now - relayClients[i].lastSeen) <= RELAY_CLIENT_TIMEOUT_MS))
    {
      return true;
    }
  }
  return false;
}

static bool Client_AnyActive(void)
{
  uint32_t now = UTIL_TIMER_GetCurrentTime();
  for (uint32_t i = 0; i < MAX_RELAY_CLIENTS; i++)
  {
    if ((relayClients[i].addr != 0) && ((now - relayClients[i].lastSeen) <= RELAY_CLIENT_TIMEOUT_MS))
    {
      return true;
    }
  }
  return false;
}

/* Node aktarimi baska komsudan istedi: artik musterimiz degil (asagi aktarim da durur) */
static void Client_Drop(uint16_t addr)
{
  for (uint32_t i = 0; i < MAX_RELAY_CLIENTS; i++)
  {
    if ((relayClients[i].addr == addr) && (addr != 0))
    {
      relayClients[i].addr = 0;
    }
  }
}

/* Su an islenen (dogrudan gelen) paketin aktarim kurali: 0x52'deki relay_dev'e gore */
static RlyMode_t Relay_RxMode(void)
{
#if (SECURE_ENABLED == 1)
  if ((secRxVer != SEC_VER2) || (secRxRelayDev == SEC_RELAY_ANY))
  {
    return RLY_ANY;
  }
  return (secRxRelayDev == myDevId) ? RLY_ME : RLY_NO;
#else
  return RLY_ANY;
#endif
}

/* Bizim gateway baglanti kalitemiz (konum bayragi bit3..6). 0 = aktarici olamayiz:
   son 25 sn'de kendimizi tabloda dogrudan gormedik (uplink kaniti) ya da Zybo'yu dogrudan duymadik */
static uint8_t Node_GwQuality(void)
{
#if (SECURE_ENABLED == 1)
  if (!Node_CanRelay() || !zyboDirectEver ||
      ((UTIL_TIMER_GetCurrentTime() - zyboDirectTime) > REACH_TIMEOUT_MS))
  {
    return 0;
  }
  return (uint8_t)LINK_Q(zyboDirectSnr);
#else
  return 0;
#endif
}

/* Aktarilmis downlink (0xE3) geldi: aktarim modundaysak icini gateway'den gelmis gibi isle */
static void HandleRelayedDl(void)
{
  uint16_t relayer = (uint16_t)(BufferRx[1] | (BufferRx[2] << 8));
  uint16_t len = (uint16_t)(RxBufferSize - RELAY_DL_HDR);
  const uint8_t *in = &BufferRx[RELAY_DL_HDR];
  uint32_t key;

  /* Anahtar: baska aktaran yolladiysa bizim bekleyen ayni aktarimimizi iptal et */
  if ((in[0] == DL_TYPE_TABLE) && (len >= 2) && (len == DL_PAYLOAD_LEN(in[1])))
  {
    key = RKEY(RTAG_TABLE, Hash24(in, len));
  }
  else if ((len == CMD_PACKET_LEN) &&
           ((in[0] == CMD_TYPE) || (in[0] == CMD_TYPE_DEL_ACK) || (in[0] == JOIN_TYPE_KICK)))
  {
    key = RKEY(RTAG_DL(in[0]), ((uint32_t)(in[1] | (in[2] << 8)) << 8) | in[5]);
  }
  else
  {
    APP_LOG(TS_ON, VLEVEL_L, "[MESH] taninmayan 0xE3 icerigi (tip 0x%02X, %u B)\n\r", in[0], (unsigned)len);
    return;
  }
  Relay_Cancel(key, relayer);

  if (!Node_NeedsRelay())
  {
    return; /* admin'e dogrudan ulasiyoruz: orijinalini gateway'den zaten duyduk */
  }

  APP_LOG(TS_ON, VLEVEL_L, "[MESH] 0x%04X uzerinden downlink alindi (tip 0x%02X)\n\r", relayer, in[0]);
  memmove(BufferRx, in, len);
  RxBufferSize = len;
  rxViaRelay = true;
  if (BufferRx[0] == DL_TYPE_TABLE)
  {
    HandleDownlinkTable();
  }
  else
  {
    HandleCmdPacket();
  }
  rxViaRelay = false;
}
#endif

#if (GATEWAY_MODE == 1)
/* Bu (addr, seq) Zybo'ya iletildi mi? Iletilmediyse kaydet, true don */
static bool PosSeen_Mark(uint16_t addr, uint8_t seq)
{
  uint32_t key = 0x80000000U | ((uint32_t)addr << 8) | seq;
  for (uint32_t i = 0; i < POS_SEEN_SIZE; i++)
  {
    if (posSeen[i] == key) { return false; }
  }
  posSeen[posSeenIdx] = key;
  posSeenIdx = (uint8_t)((posSeenIdx + 1) % POS_SEEN_SIZE);
  return true;
}
#endif

/* Aktarilmis konum (0xE1) geldi */
static void HandleRelayedPos(void)
{
  pos_packet_t inner;
  uint16_t relayer = (uint16_t)(BufferRx[1] | (BufferRx[2] << 8));
  memcpy(&inner, &BufferRx[RELAY_POS_HDR], sizeof(inner));

  APP_LOG(TS_ON, VLEVEL_L, "[RELAY] 0x%04X seq=%d aktaran=0x%04X (aktaranin duydugu rssi=%d) rssi=%d\n\r",
          inner.addr, inner.seq, relayer, (int)(int8_t)BufferRx[3], RssiValue);

#if (GATEWAY_MODE == 1)
  /* Ayni paket dogrudan da gelmis olabilir: bir kez ilet */
  if (!PosSeen_Mark(inner.addr, inner.seq))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[RELAY] zaten iletildi, atlandi\n\r");
    return;
  }
  /* "Aktarimla geldi" bayragi: Zybo tabloda geri yollar, node bunu gorup istegi acik tutar */
  inner.flags |= POS_FLAG_VIA_RELAY;
  BufferRx[RELAY_POS_HDR + 11] = inner.flags;   /* 0xE1 paketindeki kopyayi da guncelle */
#if (GW_FORWARD_RELAY_INFO == 1)
  /* Aktaran bilgisiyle birlikte yeni cerceve (Zybo 0xE1'i taniyor) */
  Gateway_ForwardRelayed(BufferRx, RssiValue, SnrValue);
#else
  /* Eski yol: icindeki 12 byte normal konum cercevesiyle (Zybo aktarani bilmez).
     RSSI/SNR = aktaranin gateway'e geldigi baglanti. */
  Gateway_ForwardPacket(&inner, RssiValue, SnrValue);
#endif
#if (GATEWAY_LOCAL_TABLE == 1)
  LocalTable_Update(&inner);
#endif
#else
  /* Baska bir komsu bu paketi aktardi: bizim bekleyen aktarimimizi iptal et */
  Relay_Cancel(RKEY(RTAG_POS, ((uint32_t)inner.addr << 8) | inner.seq), relayer);
  if (IsMyAddr(inner.addr))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[MESH] konumum 0x%04X uzerinden aktarildi\n\r", relayer);
#if (SECURE_ENABLED == 1)
    if (inner.addr == myAddr)
    {
      Relay_NoteMine(relayer, true, inner.seq);
    }
#endif
  }
#endif
}

/* Aktarilmis yukari kontrol paketi (0xE2) geldi */
static void HandleRelayedUlCtrl(void)
{
  uint16_t relayer = (uint16_t)(BufferRx[1] | (BufferRx[2] << 8));
  const uint8_t *in = &BufferRx[RELAY_UL_HDR];
  uint16_t src = (uint16_t)(in[1] | (in[2] << 8));

  if (!((in[0] == CMD_TYPE) || (in[0] == CMD_TYPE_DEL_REQ) || (in[0] == CMD_TYPE_ACK) ||
        (in[0] == JOIN_TYPE_LEAVE)))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[RELAY] 0xE2 icinde beklenmeyen tip 0x%02X\n\r", in[0]);
    return;
  }
  APP_LOG(TS_ON, VLEVEL_L, "[RELAY] kontrol tip 0x%02X src=0x%04X id=%u aktaran=0x%04X\n\r",
          in[0], src, in[5], relayer);
#if (GATEWAY_MODE == 1)
  /* Icindeki 15 byte'i dogrudan gelmis gibi isle (Zybo'ya ilet / 0xC1 ise 3x yayinla) */
  memmove(BufferRx, in, CMD_PACKET_LEN);
  RxBufferSize = CMD_PACKET_LEN;
  HandleCmdPacket();
#else
  /* Baska komsu aktardi: bizim bekleyen ayni aktarimimizi iptal et */
  Relay_Cancel(RKEY(RTAG_UL(in[0]), ((uint32_t)src << 8) | in[5]), relayer);
#if (SECURE_ENABLED == 1)
  if (src == myAddr)
  {
    Relay_NoteMine(relayer, false, 0);   /* aktaricimiz calisiyor */
  }
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

#if (GATEWAY_MODE == 0)
  const char *via = rxViaRelay ? " (aktarimla)" : "";
#else
  const char *via = "";
#endif
  APP_LOG(TS_ON, VLEVEL_L, "[TABLO] %d node (rssi=%d snr=%d)%s:\n\r", n, RssiValue, SnrValue, via);
#if (GATEWAY_MODE == 0)
  /* Tek sekme mesh: musterimiz varsa tabloyu onlara asagi aktar (aktarilmis tablo tekrar aktarilmaz) */
  if (!rxViaRelay && Node_CanRelay() && Client_AnyActive())
  {
    Relay_Enqueue(RKEY(RTAG_TABLE, Hash24(BufferRx, RxBufferSize)), RELAY_TYPE_DL, false, 0, 0,
                  BufferRx, RxBufferSize, DL_RELAY_DELAY_MIN_MS, DL_RELAY_DELAY_RAND_MS, "tablo (asagi)");
  }
#endif
#if (GATEWAY_MODE == 0)
  /* Tek sekme mesh: tabloda kendimizi goruyorsak admin'e ulasiyoruz */
  for (uint8_t i = 0; i < n; i++)
  {
    memcpy(&rec, &BufferRx[2 + i * sizeof(pos_packet_t)], sizeof(rec));
    if (rec.addr == myAddr)
    {
      if (rec.flags & POS_FLAG_VIA_RELAY)
      {
        /* Admin bizi sadece komsu uzerinden goruyor: aktarim istegini kapatma */
        lastViaRelayTime = UTIL_TIMER_GetCurrentTime();
#if (SECURE_ENABLED == 1)
        sawViaBit = true;
#endif
        APP_LOG(TS_ON, VLEVEL_L, "[MESH] admin beni aktarimla goruyor (seq=%d)\n\r", rec.seq);
      }
#if (SECURE_ENABLED == 1)
      else if (Relay_TableGuard(rec.seq))
      {
        /* Emniyet: Zybo bit2'yi koymuyor ama bu konumumuzun komsu tarafindan aktarildigini
           duyduk. Dogrudan sayarsak istek kapanir, 25 sn sonra yine acilir (salinim). */
        lastViaRelayTime = UTIL_TIMER_GetCurrentTime();
        APP_LOG(TS_ON, VLEVEL_L, "[MESH] tabloda bit2 yok ama seq=%d aktarildi (duyduk): aktarimla sayildi\n\r", rec.seq);
      }
#endif
      else
      {
        lastInTableTime = UTIL_TIMER_GetCurrentTime();
        everInTable = true;
      }
      break;
    }
  }
#endif
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

  /* Dolayli onay: Zybo'nun tablosunda kendi kaydimiz var mi? Varsa son konumumuz
     admin'e ulasmis demektir. Sonucu telefona 0xC6 ile bildir (LoRa'ya gitmez). */
  {
    uint8_t st[NODE_STATUS_LEN] = {0};
    st[0] = CMD_TYPE_NODE_STATUS;
    st[1] = (uint8_t)(myAddr & 0xFF);
    st[2] = (uint8_t)(myAddr >> 8);
    st[3] = (uint8_t)(vSeq[0] - 1U);           /* son yollanan seq */
    for (uint8_t i = 0; i < n; i++)
    {
      memcpy(&rec, &BufferRx[2 + i * sizeof(pos_packet_t)], sizeof(rec));
      if (rec.addr == myAddr)
      {
        st[4] = 1;
        st[5] = rec.seq;
        st[6] = rec.flags;
        break;
      }
    }
    Uart1_SendFrame(st, NODE_STATUS_LEN);
    Join_SendStatus();
    if (st[4])
    {
      APP_LOG(TS_ON, VLEVEL_L, "[ONAY] admin son konumumu aldi: seq=%u (son yollanan %u)\n\r",
              st[5], st[3]);
    }
    else
    {
      APP_LOG(TS_ON, VLEVEL_L, "[ONAY] tabloda yokum (whitelist / menzil?)\n\r");
    }
  }
#endif
}

/* ---------------- Faz 2: isaret / komut paketleri ---------------- */

/* (src, msg_id) daha once goruldu mu? Gorulmediyse kaydet ve true don. */
static bool Cmd_MarkSeen(uint8_t type, uint16_t src, uint8_t id)
{
  /* 0 = bos yuva olmasin diye bit31 hep 1; tip de anahtarda (0xC1/C2/C3 ayri) */
  uint32_t key = 0x80000000U | ((uint32_t)CTRL_TAG(type) << 24) | ((uint32_t)src << 8) | id;
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
            (c.type == CMD_TYPE_DEL_REQ) ? "SIL-ISTEK" :
            (c.type == CMD_TYPE_ACK) ? "ACK" :
            (c.type == JOIN_TYPE_ANS) ? "JOIN-CEVAP" :
            (c.type == JOIN_TYPE_KICK) ? "KICK" :
            (c.type == JOIN_TYPE_LEAVE) ? "LEAVE" : "SIL-KARAR", tag,
            c.src, c.dst, c.msg_id, (unsigned)(p[7] | (p[8] << 8)), p[9]);
    if (c.type == CMD_TYPE_DEL_ACK)
    {
      APP_LOG(TS_OFF, VLEVEL_L, " karar=%s", (p[6] == 1) ? "ONAY" : "RED");
    }
    else if (c.type == CMD_TYPE_ACK)
    {
      APP_LOG(TS_OFF, VLEVEL_L, " %s", (p[6] == ACK_KIND_SEEN) ? "OKUNDU" : "ULASTI");
    }
    else if (c.type == JOIN_TYPE_KICK)
    {
      APP_LOG(TS_OFF, VLEVEL_L, " sebep=%s", (p[10] == KICK_REASON_ADMIN) ? "ADMIN" : "AYRILMA-ONAYI");
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

#if (GATEWAY_MODE == 0)
  if (!rxViaRelay && Node_CanRelay())
  {
    /* Tek sekme mesh, yukari: musterinin 0xC1 / 0xC2 / 0xC5'ini gateway'e aktar.
       0x52: aktarici olarak bizi sectiyse hemen, baskasini sectiyse hic. */
    bool ulType = ((c.type == CMD_TYPE) || (c.type == CMD_TYPE_DEL_REQ) || (c.type == CMD_TYPE_ACK) ||
                   (c.type == JOIN_TYPE_LEAVE));
    RlyMode_t m = Relay_RxMode();
    if (ulType && (m == RLY_ME)) { Client_Touch(c.src); }
    if (ulType && (m == RLY_NO)) { Client_Drop(c.src); }
    if (fresh && ulType && (m != RLY_NO) && Client_IsActive(c.src))
    {
      Relay_Enqueue(RKEY(RTAG_UL(c.type), ((uint32_t)c.src << 8) | c.msg_id), RELAY_TYPE_UL_CTRL,
                    true, RssiValue, SnrValue, BufferRx, CMD_PACKET_LEN,
                    (m == RLY_ME) ? RELAY_CHOSEN_DELAY_MIN_MS : POS_RELAY_DELAY_MIN_MS,
                    (m == RLY_ME) ? RELAY_CHOSEN_DELAY_RAND_MS : POS_RELAY_DELAY_RAND_MS, "kontrol (yukari)");
      if (c.type == CMD_TYPE)
      {
        /* Gateway bunu 3x yayinlayinca donen kopyayi musteriye asagi aktaracagiz (✓✓ icin) */
        echoWaitKey = RKEY(RTAG_DL(CMD_TYPE), ((uint32_t)c.src << 8) | c.msg_id);
      }
    }
    /* Tek sekme mesh, asagi: 0xC1 / 0xC3'u musterilere aktar. Musterinin kendi 0xC1'inin
       ilk kopyasi musteriden gelir (onu aktarmak sahte "aga ulasti" olur); gateway'den
       donen tekrar kopyasini aktariyoruz. */
    if (Client_AnyActive() &&
        ((c.type == CMD_TYPE) || (c.type == CMD_TYPE_DEL_ACK) || (c.type == JOIN_TYPE_KICK)))
    {
      uint32_t k = RKEY(RTAG_DL(c.type), ((uint32_t)c.src << 8) | c.msg_id);
      bool fromClient = Client_IsActive(c.src);
      /* Zybo ayni komutu (ayni msg_id) yeniden denediyse: gateway'in 3x tekrarindan sonra
         (DL_CMD_RETRY_GAP_MS'den uzun aradan sonra) yine duyulursa tekrar aktar */
      bool retry = (!fromClient && !fresh && ((c.type == CMD_TYPE) || (c.type == JOIN_TYPE_KICK)) &&
                    DlCmd_RetryDue(k));
      if ((!fromClient && fresh) || retry || (fromClient && !fresh && (k == echoWaitKey)))
      {
        if (fromClient) { echoWaitKey = 0; }
        if (retry)
        {
          RelaySeen_Forget(k);
          APP_LOG(TS_ON, VLEVEL_L, "[MESH] admin komutu yeniden denendi, tekrar asagi aktariliyor\n\r");
        }
        if (!fromClient) { DlCmd_Note(k); }
        Relay_Enqueue(k, RELAY_TYPE_DL, false, 0, 0, BufferRx, CMD_PACKET_LEN,
                      DL_RELAY_DELAY_MIN_MS, DL_RELAY_DELAY_RAND_MS, "kontrol (asagi)");
      }
    }
  }
  /* Baska node'un ACK'i / ayrilma istegi: bizi ilgilendirmez */
  if ((c.type == CMD_TYPE_ACK) || (c.type == JOIN_TYPE_LEAVE)) { return; }
  /* KICK / ayrilma onayi: tekrar kopyalari da islenir (ULASTI tekrar yollanabilsin) */
  if (c.type == JOIN_TYPE_KICK)
  {
    Join_HandleKick();
    return;
  }
  /* Admin'in BANA ozel komutu: gateway'in her kopyasina "ULASTI" ACK'i yolla. Tekrar eden
     kopyalara da yollanir; boylece ilk ACK kaybolursa ya da Zybo ayni msg_id ile tekrar
     yollarsa da admin onayi alir. Telefona ise sadece ilk kopya gider (asagida). */
  if ((c.type == CMD_TYPE) && (c.src == ADDR_ADMIN) && IsMyAddr(c.dst))
  {
    Node_QueueAck(c.dst, c.src, c.msg_id, ACK_KIND_DELIVERED);
  }
#endif
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
  else if (c.type == JOIN_TYPE_LEAVE)
  {
    /* 0xA4: node takimdan ayriliyor, SADECE Zybo'ya gider (cevabi Zybo 0xA3 sebep 2 ile verir) */
#if (GW_FORWARD_CMD == 1)
    Gateway_ForwardCmd(BufferRx, RssiValue, SnrValue);
#else
    APP_LOG(TS_ON, VLEVEL_L, "[LEAVE] Zybo'ya iletilmedi (GW_FORWARD_CMD=0)\n\r");
#endif
  }
  else if (c.type == CMD_TYPE_ACK)
  {
    /* 0xC5: node'un komut onayi, SADECE Zybo'ya gider, havada tekrar yayinlanmaz */
#if (GW_FORWARD_CMD == 1)
    Gateway_ForwardCmd(BufferRx, RssiValue, SnrValue);
#else
    APP_LOG(TS_ON, VLEVEL_L, "[ACK] Zybo'ya iletilmedi (GW_FORWARD_CMD=0)\n\r");
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

  /* 0xC9 / 0xCC: katilma parametreleri / takimdan ayril (yerel; ayrilmayi node 0xA4 ile bildirir) */
  if ((len == CMD_PACKET_LEN) && (dlFrame[0] == CMD_TYPE_JOIN_PARAMS))
  {
    Join_Start(dlFrame[1], dlFrame[2], (uint16_t)(dlFrame[3] | (dlFrame[4] << 8)));
    dlFrameReady = false;
    return;
  }
  if ((len == CMD_PACKET_LEN) && (dlFrame[0] == CMD_TYPE_LEAVE))
  {
    Join_Leave();
    dlFrameReady = false;
    return;
  }

  /* 0xC4: telefonun konumu. LoRa'ya gitmez, sadece bu node'un yayinladigi konumu degistirir */
  if ((len == CMD_PACKET_LEN) && (dlFrame[0] == CMD_TYPE_SET_POS))
  {
    if (dlFrame[9] == 1)
    {
      memcpy(&posOvrLat, &dlFrame[1], 4);   /* little-endian, Cortex-M4 ile ayni */
      memcpy(&posOvrLon, &dlFrame[5], 4);
      posOvrTime = UTIL_TIMER_GetCurrentTime();
      if (!posOvrValid)
      {
        APP_LOG(TS_ON, VLEVEL_L, "[POS] telefon konumu kullaniliyor:");
        LogCoord("lat", posOvrLat);
        LogCoord("lon", posOvrLon);
        APP_LOG(TS_OFF, VLEVEL_L, "\n\r");
      }
      posOvrValid = true;
    }
    else
    {
      posOvrValid = false;
      APP_LOG(TS_ON, VLEVEL_L, "[POS] telefon konum paylasimini kapatti, sahte konuma donuldu\n\r");
    }
    dlFrameReady = false;
    return;
  }

  /* 0xC5 OKUNDU: telefonda kullanici admin komutunu gordu. Node adres ve sayaci doldurup yollar */
  if ((len == CMD_PACKET_LEN) && (dlFrame[0] == CMD_TYPE_ACK))
  {
    if (dlFrame[6] == ACK_KIND_SEEN)
    {
      Node_QueueAck(myAddr, (uint16_t)(dlFrame[7] | (dlFrame[8] << 8)), dlFrame[9], ACK_KIND_SEEN);
    }
    dlFrameReady = false;
    return;
  }

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

#if (GATEWAY_MODE == 0)
/* 0xC5 ACK'ini kuyruga al: ACK_REPEAT kez yollanir (ilki kisa rastgele gecikmeyle).
   Ayni ACK hala tekrarlaniyorsa yeni bir sey yapmaz (komutun 2./3. kopyasi). */
static void Node_QueueAck(uint16_t me, uint16_t ackedSrc, uint8_t ackedId, uint8_t kind)
{
  AckSlot_t *s = NULL;

  for (uint32_t i = 0; i < ACK_Q_SIZE; i++)
  {
    if (ackQ[i].active && (ackQ[i].ackedSrc == ackedSrc) && (ackQ[i].ackedId == ackedId) &&
        (ackQ[i].kind == kind))
    {
      return; /* bu ACK zaten tekrarlaniyor */
    }
  }
  for (uint32_t i = 0; i < ACK_Q_SIZE; i++)
  {
    if (!ackQ[i].active) { s = &ackQ[i]; break; }
  }
  if (s == NULL)
  {
    /* Kuyruk dolu: en az tekrari kalan (en cok yollanmis) ACK'in yerini al */
    s = &ackQ[0];
    for (uint32_t i = 1; i < ACK_Q_SIZE; i++)
    {
      if (ackQ[i].left < s->left) { s = &ackQ[i]; }
    }
    UTIL_TIMER_Stop(&s->timer);
    APP_LOG(TS_ON, VLEVEL_L, "[ACK] kuyruk dolu, en eski ACK'in kalan tekrarlari birakildi\n\r");
  }
  s->me = me;
  s->ackedSrc = ackedSrc;
  s->ackedId = ackedId;
  s->kind = kind;
  s->left = ACK_REPEAT;
  s->active = true;
  s->due = false;
  UTIL_TIMER_SetPeriod(&s->timer, ACK_FIRST_DELAY_MIN_MS + (Rand32() % ACK_FIRST_DELAY_RAND_MS));
  UTIL_TIMER_Start(&s->timer);
}

static void OnAckTimer(void *context)
{
  ackQ[(uint32_t)context].due = true;
  PostEvent(EV_ACK);
}

/* Anahtari "goruldu" listesinden cikar (yeniden aktarim icin) */
static void RelaySeen_Forget(uint32_t key)
{
  for (uint32_t i = 0; i < RELAY_SEEN_SIZE; i++)
  {
    if (relaySeen[i] == key) { relaySeen[i] = 0; }
  }
}

/* Bu admin komutu son aktarimindan DL_CMD_RETRY_GAP_MS sonra mi yine duyuldu? (Zybo yeniden denedi) */
static bool DlCmd_RetryDue(uint32_t key)
{
  for (uint32_t i = 0; i < DL_CMD_HIST; i++)
  {
    if (dlCmdHist[i].key == key)
    {
      return (UTIL_TIMER_GetCurrentTime() - dlCmdHist[i].t) > DL_CMD_RETRY_GAP_MS;
    }
  }
  return false;
}

static void DlCmd_Note(uint32_t key)
{
  for (uint32_t i = 0; i < DL_CMD_HIST; i++)
  {
    if (dlCmdHist[i].key == key)
    {
      dlCmdHist[i].t = UTIL_TIMER_GetCurrentTime();
      return;
    }
  }
  dlCmdHist[dlCmdHistIdx].key = key;
  dlCmdHist[dlCmdHistIdx].t = UTIL_TIMER_GetCurrentTime();
  dlCmdHistIdx = (uint8_t)((dlCmdHistIdx + 1) % DL_CMD_HIST);
}
#endif

/* ======================= JOIN v1: takimli kimlik ======================= */

/* CRC-32 (IEEE 802.3 / zlib, yansitilmis, poly 0xEDB88320). dev_id = CRC32(UID 12 byte) */
static uint32_t Crc32(const uint8_t *p, uint32_t n)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (uint32_t i = 0; i < n; i++)
  {
    crc ^= p[i];
    for (int b = 0; b < 8; b++)
    {
      crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
  }
  return ~crc;
}

#if (GATEWAY_MODE == 0)
/* Kimlik sayfasini okuma adresi (test ortaminda degistirilebilsin diye makro) */
#ifndef ID_FLASH_READ_PTR
#define ID_FLASH_READ_PTR   ((const void *)ID_FLASH_ADDR)
#endif

/* dev_id'yi hesapla, flash'ta kimlik varsa adresi ona cevir */
static void Join_Init(void)
{
  uint8_t u[12];
  uint32_t w;
  w = HAL_GetUIDw0(); memcpy(&u[0], &w, 4);
  w = HAL_GetUIDw1(); memcpy(&u[4], &w, 4);
  w = HAL_GetUIDw2(); memcpy(&u[8], &w, 4);
  myDevId = Crc32(u, 12);
  uidAddr = myAddr;
#if (JOIN_ENABLED == 1)
  {
    IdRec_t r;
    if (Id_Load(&r))
    {
      myAddr = r.addr;
      joinTeam = r.team;
      joinNum = r.num;
      joinState = JOIN_ST_JOINED;
      APP_LOG(TS_OFF, VLEVEL_M, "JOIN: kayitli kimlik takim=%u numara=%u adres=0x%04X\n\r",
              r.team, r.num, r.addr);
    }
    else
    {
      joinState = JOIN_ST_NEED_PARAMS;
      APP_LOG(TS_OFF, VLEVEL_M, "JOIN: kimlik yok (dev_id=0x%08X), telefondan takim/numara/PIN bekleniyor, konum yayini KAPALI\n\r",
              (unsigned int)myDevId);
    }
  }
#else
  joinState = JOIN_ST_DISABLED;
#endif
}

/* Katilma durumunu telefona bildir (0xCB, yerel) */
static void Join_SendStatus(void)
{
#if (NODE_UART_BRIDGE == 1)
  uint8_t st[JOIN_STATUS_LEN];
  st[0] = CMD_TYPE_JOIN_STATUS;
  st[1] = joinState;
  st[2] = joinResult;
  st[3] = (uint8_t)(myAddr & 0xFF);
  st[4] = (uint8_t)(myAddr >> 8);
  st[5] = joinTeam;
  st[6] = joinNum;
  st[7] = joinReqSeq;
  Uart1_SendFrame(st, JOIN_STATUS_LEN);
#endif
}

/* Telefondan katilma parametreleri geldi (0xC9) */
static void Join_Start(uint8_t team, uint8_t num, uint16_t pin)
{
#if (JOIN_ENABLED == 1)
  if ((joinState == JOIN_ST_JOINED) || (joinState == JOIN_ST_LEAVING))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[JOIN] zaten kimlik var (0x%04X); once takimdan ayrilin\n\r", myAddr);
    Join_SendStatus();
    return;
  }
  if ((team < 1) || (team > JOIN_TEAM_MAX) || (num > JOIN_NUM_MAX) || (pin > 9999))
  {
    APP_LOG(TS_ON, VLEVEL_L, "[JOIN] gecersiz parametre: takim=%u numara=%u\n\r", team, num);
    Join_SendStatus();
    return;
  }
  joinTeam = team;
  joinNum = num;
  joinPin = pin;
  joinResult = 0;
  joinState = JOIN_ST_WAITING;
  joinDue = false;
  UTIL_TIMER_Stop(&timerJoin);
  UTIL_TIMER_SetPeriod(&timerJoin, 100 + (Rand32() % 400));
  UTIL_TIMER_Start(&timerJoin);
  APP_LOG(TS_ON, VLEVEL_L, "[JOIN] parametreler alindi: takim=%u numara=%u, admin onayi istenecek\n\r", team, num);
  Join_SendStatus();
#else
  (void)team; (void)num; (void)pin;
  APP_LOG(TS_ON, VLEVEL_L, "[JOIN] bu node'da JOIN kapali (JOIN_ENABLED=0)\n\r");
  Join_SendStatus();
#endif
}

/* Kimligi sil, UID adresine ve katilma moduna don (atildi / ayrildi). result telefona gider. */
static void Join_Clear(uint8_t result)
{
#if (JOIN_ENABLED == 1)
  Id_Erase();
  myAddr = uidAddr;
  vAddr[0] = uidAddr;
  joinState = JOIN_ST_NEED_PARAMS;
  joinResult = result;
  joinTeam = 0;
  joinNum = 0;
  joinDue = false;
  kickPending = false;
  leaveTries = 0;
  everInTable = false;                      /* mesh durumu bastan */
  bootTime = UTIL_TIMER_GetCurrentTime();
  UTIL_TIMER_Stop(&timerJoin);
  UTIL_TIMER_SetPeriod(&timerJoin, 2000);
  UTIL_TIMER_Start(&timerJoin);
  APP_LOG(TS_ON, VLEVEL_L, "[JOIN] kimlik silindi (%s): adres 0x%04X, konum yayini KAPALI, katilma modu\n\r",
          (result == JOIN_RES_KICKED) ? "admin cikardi" : "takimdan ayrildi", myAddr);
  Join_SendStatus();
#else
  (void)result;
#endif
}

/* Telefondan "takimdan ayril" (0xCC): 0xA4'u 5 sn arayla en fazla 3 kez yolla */
static void Join_Leave(void)
{
#if (JOIN_ENABLED == 1)
  if ((joinState != JOIN_ST_JOINED) || kickPending)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[JOIN] ayrilma istegi yok sayildi (durum %u)\n\r", joinState);
    Join_SendStatus();
    return;
  }
  joinState = JOIN_ST_LEAVING;
  joinResult = 0;
  leaveTries = 0;
  leaveMsgId = (uint8_t)Rand32();
  joinDue = false;
  UTIL_TIMER_Stop(&timerJoin);
  UTIL_TIMER_SetPeriod(&timerJoin, 100 + (Rand32() % 400));
  UTIL_TIMER_Start(&timerJoin);
  APP_LOG(TS_ON, VLEVEL_L, "[JOIN] takimdan ayriliniyor (0x%04X), admin'e bildirilecek\n\r", myAddr);
  Join_SendStatus();
#else
  APP_LOG(TS_ON, VLEVEL_L, "[JOIN] bu node'da JOIN kapali (JOIN_ENABLED=0)\n\r");
  Join_SendStatus();
#endif
}

/* 0xA3 geldi (dogrudan ya da aktarimla). Sadece kendi dev_id'mize bakariz. */
static void Join_HandleKick(void)
{
#if (JOIN_ENABLED == 1)
  uint32_t dev;
  memcpy(&dev, &BufferRx[6], 4);
  if (dev != myDevId)
  {
    return; /* baska cihaz */
  }
  uint16_t dst    = (uint16_t)(BufferRx[3] | (BufferRx[4] << 8));
  uint8_t  msgId  = BufferRx[5];
  uint8_t  reason = BufferRx[10];

  if (reason == KICK_REASON_ADMIN)
  {
    if (((joinState == JOIN_ST_JOINED) || (joinState == JOIN_ST_LEAVING)) && (dst == myAddr))
    {
      /* ULASTI mevcut adresten gider; kimlik ULASTI'lar havaya cikinca silinir (Node_Process) */
      if (!kickPending)
      {
        APP_LOG(TS_ON, VLEVEL_L, "[JOIN] ADMIN TAKIMDAN CIKARDI (0x%04X), ULASTI yollanip kimlik silinecek\n\r", myAddr);
      }
      kickPending = true;
      kickedAddr = myAddr;
      joinDue = false;
      UTIL_TIMER_Stop(&timerJoin);          /* ayriliyorduk: A4 denemeleri biter */
      Node_QueueAck(myAddr, ADDR_ADMIN, msgId, ACK_KIND_DELIVERED);
    }
    else if ((kickedAddr != 0) && (dst == kickedAddr))
    {
      /* Kimligi zaten sildik ama Zybo ULASTI'yi alamamis, tekrar denedi: eski adresten cevap ver */
      APP_LOG(TS_ON, VLEVEL_L, "[JOIN] gec gelen KICK tekrari: eski adres 0x%04X'ten ULASTI\n\r", kickedAddr);
      Node_QueueAck(kickedAddr, ADDR_ADMIN, msgId, ACK_KIND_DELIVERED);
    }
  }
  else if (reason == KICK_REASON_LEAVE_OK)
  {
    if ((joinState == JOIN_ST_LEAVING) && (dst == myAddr))
    {
      APP_LOG(TS_ON, VLEVEL_L, "[JOIN] admin ayrilmayi onayladi\n\r");
      Join_Clear(JOIN_RES_LEFT);
    }
  }
#endif
}

/* Bu adresten yollanacak ACK kaldi mi */
static bool Ack_Pending(uint16_t me)
{
  for (uint32_t i = 0; i < ACK_Q_SIZE; i++)
  {
    if (ackQ[i].active && (ackQ[i].me == me)) { return true; }
  }
  return false;
}

static void OnJoinTimer(void *context)
{
  joinDue = true;
  PostEvent(EV_JOIN);
}

/* Katilma cevabi (0xA2) geldi: sadece kendi dev_id'mize bakariz */
static void Join_HandleAnswer(void)
{
#if (JOIN_ENABLED == 1)
  uint32_t dev;
  memcpy(&dev, &BufferRx[6], 4);
  if (dev != myDevId)
  {
    return; /* baska cihazin cevabi */
  }
  uint8_t  msgId = BufferRx[5];
  uint16_t addr  = (uint16_t)(BufferRx[10] | (BufferRx[11] << 8));
  uint8_t  team  = BufferRx[12];
  uint8_t  num   = BufferRx[13];
  uint8_t  res   = BufferRx[14];

  APP_LOG(TS_ON, VLEVEL_L, "[JOIN] cevap: sonuc=%u adres=0x%04X takim=%u numara=%u (msg %u) rssi=%d\n\r",
          res, addr, team, num, msgId, RssiValue);

  if (res == JOIN_RES_OK)
  {
    if (((addr & ADDR_JOINED_FLAG) == 0) || (addr >= 0xFF00))
    {
      APP_LOG(TS_ON, VLEVEL_L, "[JOIN] gecersiz adres, yok sayildi\n\r");
      return;
    }
    if (joinState == JOIN_ST_JOINED)
    {
      if (myAddr == addr)
      {
        /* Ayni cevap tekrar geldi (3x yayin / Zybo tekrar denedi): ULASTI'yi tekrar yolla */
        Node_QueueAck(myAddr, ADDR_ADMIN, msgId, ACK_KIND_DELIVERED);
      }
      else
      {
        APP_LOG(TS_ON, VLEVEL_L, "[JOIN] baska adres icin cevap (kimlik 0x%04X), yok sayildi\n\r", myAddr);
      }
      return;
    }
    if (!Id_Save(addr, team, num))
    {
      APP_LOG(TS_ON, VLEVEL_L, "[JOIN] UYARI: kimlik flash'a yazilamadi (yeniden baslayinca kaybolur)\n\r");
    }
    myAddr = addr;
    vAddr[0] = addr;
    joinTeam = team;
    joinNum = num;
    joinResult = JOIN_RES_OK;
    joinState = JOIN_ST_JOINED;
    joinDue = false;
    UTIL_TIMER_Stop(&timerJoin);
    APP_LOG(TS_ON, VLEVEL_L, "[JOIN] ONAYLANDI: yeni adres 0x%04X, konum yayini basliyor\n\r", myAddr);
    Node_QueueAck(myAddr, ADDR_ADMIN, msgId, ACK_KIND_DELIVERED);
    Join_SendStatus();
  }
  else
  {
    if (joinState != JOIN_ST_WAITING)
    {
      return;
    }
    joinState = JOIN_ST_FAILED;
    joinResult = res;
    if (res == JOIN_RES_NUM_TAKEN) { joinNum = num; }   /* Zybo'nun onerdigi bos numara */
    joinDue = false;
    UTIL_TIMER_Stop(&timerJoin);
    APP_LOG(TS_ON, VLEVEL_L, "[JOIN] REDDEDILDI: sebep %u (1 PIN, 2 admin, 3 takim dolu, 4 numara dolu)\n\r", res);
    Join_SendStatus();
  }
#endif
}

/* Kimlik kaydini oku: sihirli sayi, CRC ve bu kartin dev_id'si tutmali */
static bool Id_Load(IdRec_t *r)
{
  memcpy(r, ID_FLASH_READ_PTR, sizeof(*r));
  if (r->magic != ID_MAGIC) { return false; }
  if (r->chk != Crc32((const uint8_t *)r, 12)) { return false; }
  if (r->devId != myDevId) { return false; }          /* baska karttan kopyalanmis */
  if (((r->addr & ADDR_JOINED_FLAG) == 0) || (r->addr >= 0xFF00)) { return false; }
  return true;
}

/* Kimligi flash'in son sayfasina yaz (sayfa silinir, 2 x 64 bit yazilir, geri okunup kontrol edilir) */
static bool Id_Save(uint16_t addr, uint8_t team, uint8_t num)
{
  IdRec_t r;
  uint64_t dw[2];
  uint32_t err = 0;
  FLASH_EraseInitTypeDef e;
  HAL_StatusTypeDef st;

  r.magic = ID_MAGIC;
  r.addr = addr;
  r.team = team;
  r.num = num;
  r.devId = myDevId;
  r.chk = Crc32((const uint8_t *)&r, 12);
  memcpy(dw, &r, sizeof(dw));

  HAL_FLASH_Unlock();
  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.Page = ID_FLASH_PAGE;
  e.NbPages = 1;
  st = HAL_FLASHEx_Erase(&e, &err);
  if (st == HAL_OK) { st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, ID_FLASH_ADDR, dw[0]); }
  if (st == HAL_OK) { st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, ID_FLASH_ADDR + 8U, dw[1]); }
  HAL_FLASH_Lock();

  IdRec_t chk;
  return (st == HAL_OK) && Id_Load(&chk) && (chk.addr == addr);
}

static void Id_Erase(void)
{
  uint32_t err = 0;
  FLASH_EraseInitTypeDef e;
  HAL_FLASH_Unlock();
  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.Page = ID_FLASH_PAGE;
  e.NbPages = 1;
  (void)HAL_FLASHEx_Erase(&e, &err);
  HAL_FLASH_Lock();
}
#endif /* GATEWAY_MODE == 0 */

#if (GATEWAY_MODE == 1)
/* Havadan katilma istegi (0xA1) geldi: (dev_id, req_seq) ile tekrar ele, Zybo'ya ilet */
static void Gateway_HandleJoinReq(void)
{
  uint32_t dev;
  uint8_t seq = BufferRx[5];
  memcpy(&dev, &BufferRx[6], 4);
  for (uint32_t i = 0; i < JOIN_SEEN_SIZE; i++)
  {
    if (joinSeen[i].used && (joinSeen[i].devId == dev) && (joinSeen[i].seq == seq))
    {
      return; /* ayni istegin kopyasi */
    }
  }
  joinSeen[joinSeenIdx].devId = dev;
  joinSeen[joinSeenIdx].seq = seq;
  joinSeen[joinSeenIdx].used = true;
  joinSeenIdx = (uint8_t)((joinSeenIdx + 1) % JOIN_SEEN_SIZE);

  APP_LOG(TS_ON, VLEVEL_L, "[JOIN] istek: dev_id=0x%08X takim=%u numara=%u seq=%u fw=%u.%u rssi=%d\n\r",
          (unsigned int)dev, BufferRx[10], BufferRx[11], seq, BufferRx[14], BufferRx[15], RssiValue);
  Gateway_ForwardFrame(BufferRx, JOIN_REQ_LEN, RssiValue, SnrValue, "katilma istegi");
}

/* [AA 55 LEN | payload | RSSI | SNR | CRC8] ile Zybo'ya yolla (LEN = payload + 2) */
static void Gateway_ForwardFrame(const uint8_t *payload, uint16_t len, int16_t rssi, int8_t snr, const char *what)
{
  static uint8_t frame[3 + (MAX_APP_BUFFER_SIZE - 2) + 2 + 1];
  uint32_t i = 0;
  if (len > (MAX_APP_BUFFER_SIZE - 2)) { return; }   /* LEN = len + 2 <= 255 */
  if (rssi < -128) { rssi = -128; }
  if (rssi > 127)  { rssi = 127; }
  frame[i++] = GW_SYNC0;
  frame[i++] = GW_SYNC1;
  frame[i++] = (uint8_t)(len + 2);
  memcpy(&frame[i], payload, len);
  i += len;
  frame[i++] = (uint8_t)(int8_t)rssi;
  frame[i++] = (uint8_t)snr;
  frame[i] = Crc8(&frame[2], i - 2);   /* LEN..SNR */
  i++;
  if (HAL_UART_Transmit(&huart1, frame, (uint16_t)i, 60) == HAL_OK)   /* 260 B @115200 ~ 23 ms */
  {
    APP_LOG(TS_ON, VLEVEL_L, "[GW] %s -> Zybo %d byte\n\r", what, (int)i);
  }
  else
  {
    APP_LOG(TS_ON, VLEVEL_L, "[GW] %s USART1 gonderim HATASI\n\r", what);
  }
}
#endif /* GATEWAY_MODE == 1 */

/* ======================== Faz 1 guvenlik ======================== */
#if (SECURE_ENABLED == 1) && (GATEWAY_MODE == 0)
/* Sayac sayfasini okuma adresi (test ortaminda degistirilebilsin diye makro) */
#ifndef SEC_FLASH_READ_PTR
#define SEC_FLASH_READ_PTR   ((const void *)SEC_CTR_FLASH_ADDR)
#endif

typedef enum { SEC_OK = 0, SEC_BAD, SEC_REPLAY } SecRes_t;
typedef enum { SEC_RP_NONE = 0, SEC_RP_ENFORCE, SEC_RP_NOTE } SecRp_t;

static void Sec_EraseCtrPage(void)
{
  uint32_t err = 0;
  FLASH_EraseInitTypeDef e;
  HAL_FLASH_Unlock();
  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.Page = SEC_CTR_PAGE;
  e.NbPages = 1;
  (void)HAL_FLASHEx_Erase(&e, &err);
  HAL_FLASH_Lock();
  secSlot = 0;
  secNeedErase = false;
}

/* Sayaci sayfadaki bir sonraki bos yuvaya yaz: [sayac][~sayac]. Sayfa dolunca (256 yuva) silinir. */
static void Sec_SaveCtr(uint32_t v)
{
  uint64_t dw = (uint64_t)v | ((uint64_t)(~v) << 32);
  HAL_StatusTypeDef st;
  if (secNeedErase || (secSlot >= SEC_CTR_SLOTS))
  {
    Sec_EraseCtrPage();
  }
  HAL_FLASH_Unlock();
  st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, SEC_CTR_FLASH_ADDR + 8U * secSlot, dw);
  HAL_FLASH_Lock();
  if (st != HAL_OK)
  {
    /* yuva bos degilmis: sayfayi sil, bastan yaz */
    Sec_EraseCtrPage();
    HAL_FLASH_Unlock();
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, SEC_CTR_FLASH_ADDR, dw);
    HAL_FLASH_Lock();
  }
  secSlot++;
  secSaved = v;
  if (st != HAL_OK)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[SEC] sayac flash'a yazilamadi!\n\r");
  }
}

/* Anahtari yukle, sayaci flash'tan geri getir (+256) ve hemen kaydet */
static void Sec_Init(void)
{
  static const uint8_t key[16] = NET_KEY_BYTES;
  const uint32_t *p = (const uint32_t *)SEC_FLASH_READ_PTR;
  uint32_t last = 0;
  bool found = false;

  Aes_SetKey(key);
  secSlot = SEC_CTR_SLOTS;   /* bos yuva bulunmazsa: sayfa dolu, ilk yazmada silinir */
  secNeedErase = false;
  for (uint32_t i = 0; i < SEC_CTR_SLOTS; i++)
  {
    uint32_t lo = p[2U * i];
    uint32_t hi = p[2U * i + 1U];
    if ((lo == 0xFFFFFFFFU) && (hi == 0xFFFFFFFFU))
    {
      secSlot = (uint16_t)i;
      break;
    }
    if (hi == (uint32_t)~lo)
    {
      last = lo;
      found = true;
    }
    else
    {
      secNeedErase = true;   /* bozuk yuva */
    }
  }
  secCtr = found ? (last + SEC_CTR_STEP) : 1U;
  Sec_SaveCtr(secCtr);
  APP_LOG(TS_OFF, VLEVEL_M, "SECURE: acik (%s), dev_id=0x%08X, sayac=%u\n\r",
          (SECURE_ONLY == 1) ? "sadece guvenli" : "duz + guvenli", (unsigned int)myDevId, (unsigned int)secCtr);
  if ((key[0] == 0x00) && (key[1] == 0x01) && (key[2] == 0x02) && (key[15] == 0x0F))
  {
    APP_LOG(TS_OFF, VLEVEL_M, "!!! net_key.h TEST anahtari: sahada kullanmadan once degistirin !!!\n\r");
  }
}

static uint32_t Sec_NextCtr(void)
{
  if (secCtr >= (secSaved + SEC_CTR_STEP))
  {
    Sec_SaveCtr(secCtr);   /* acilista +256 yapilacagi icin bu deger bir daha kullanilmaz */
  }
  return secCtr++;
}

static void Sec_Nonce(const uint8_t *hdr, uint8_t nonce[CCM_NONCE_LEN])
{
  uint32_t dev;
  memcpy(&dev, &hdr[1], 4);
  memcpy(nonce, &hdr[1], 8);                 /* dev_id(4) | sayac(4) */
  nonce[8] = (dev == SEC_DEVID_ZYBO) ? SEC_DIR_ZYBO : SEC_DIR_NODE;
  memset(&nonce[9], 0, 4);
}

/* Ic paketi zarfla. v2 = false: [0x51][dev_id][sayac][sifreli][MIC]
                    v2 = true:  [0x52][dev_id][sayac][relay_dev][sifreli][MIC]. Donus: toplam boy */
static uint16_t Sec_Wrap2(const uint8_t *in, uint16_t len, uint8_t *out, bool v2, uint32_t relayDev)
{
  uint8_t nonce[CCM_NONCE_LEN];
  uint32_t c = Sec_NextCtr();
  uint16_t hl = v2 ? SEC_HDR2_LEN : SEC_HDR_LEN;
  out[0] = v2 ? SEC_VER2 : SEC_VER;
  memcpy(&out[1], &myDevId, 4);
  memcpy(&out[5], &c, 4);
  if (v2)
  {
    memcpy(&out[9], &relayDev, 4);
  }
  Sec_Nonce(out, nonce);
  Ccm_Encrypt(nonce, out, hl, in, &out[hl], len, &out[hl + len]);
  return (uint16_t)(len + hl + SEC_MIC_LEN);
}

static uint16_t Sec_Wrap(const uint8_t *in, uint16_t len, uint8_t *out) __attribute__((unused));
static uint16_t Sec_Wrap(const uint8_t *in, uint16_t len, uint8_t *out)
{
  return Sec_Wrap2(in, len, out, false, SEC_RELAY_ANY);
}

/* Gonderen basina 32'lik pencere: daha once gorulmemis ve pencere icindeki sayac kabul */
static bool Sec_ReplayOk(uint32_t dev, uint32_t ctr)
{
  SecPeer_t *p = NULL;
  SecPeer_t *old = &secPeers[0];
  for (uint32_t i = 0; i < SEC_REPLAY_SIZE; i++)
  {
    if ((secPeers[i].used != 0U) && (secPeers[i].devId == dev)) { p = &secPeers[i]; break; }
    if (secPeers[i].used < old->used) { old = &secPeers[i]; }
  }
  secUseTick++;
  if (p == NULL)
  {
    /* yeni gonderen (ya da en eski kaydin yerine): ilk paket kabul */
    old->devId = dev;
    old->last = ctr;
    old->bits = 1U;
    old->used = secUseTick;
    return true;
  }
  p->used = secUseTick;
  if (ctr > p->last)
  {
    uint32_t dlt = ctr - p->last;
    p->bits = (dlt >= SEC_WIN) ? 1U : ((p->bits << dlt) | 1U);
    p->last = ctr;
    return true;
  }
  uint32_t back = p->last - ctr;
  if (back >= SEC_WIN) { return false; }          /* cok eski */
  if (p->bits & (1U << back)) { return false; }   /* zaten goruldu */
  p->bits |= (1U << back);
  return true;
}

/* Zarfi ac (0x51 ya da 0x52): MIC dogrula, istenirse sayaci kontrol et. out ile in ayri
   tampon olmali. relayDev != NULL ise 0x52'nin relay_dev'i (0x51: SEC_RELAY_ANY) yazilir. */
static SecRes_t Sec_OpenEx(const uint8_t *in, uint16_t len, uint8_t *out, uint16_t *outLen, SecRp_t rp,
                           uint32_t *relayDev)
{
  uint8_t nonce[CCM_NONCE_LEN];
  uint32_t dev, ctr;
  uint16_t n, hl;
  if (len < 1U) { return SEC_BAD; }
  if (in[0] == SEC_VER)       { hl = SEC_HDR_LEN; }
  else if (in[0] == SEC_VER2) { hl = SEC_HDR2_LEN; }
  else                        { return SEC_BAD; }
  if (len < (hl + SEC_MIC_LEN + 1U)) { return SEC_BAD; }
  n = (uint16_t)(len - hl - SEC_MIC_LEN);
  memcpy(&dev, &in[1], 4);
  memcpy(&ctr, &in[5], 4);
  Sec_Nonce(in, nonce);
  if (!Ccm_Decrypt(nonce, in, hl, &in[hl], out, n, &in[hl + n]))
  {
    return SEC_BAD;
  }
  if (relayDev != NULL)
  {
    if (hl == SEC_HDR2_LEN) { memcpy(relayDev, &in[9], 4); }
    else                    { *relayDev = SEC_RELAY_ANY; }
  }
  if (rp != SEC_RP_NONE)
  {
    bool ok = Sec_ReplayOk(dev, ctr);
    if (!ok && (rp == SEC_RP_ENFORCE)) { return SEC_REPLAY; }
  }
  *outLen = n;
  return SEC_OK;
}

static SecRes_t Sec_Open(const uint8_t *in, uint16_t len, uint8_t *out, uint16_t *outLen, SecRp_t rp)
{
  return Sec_OpenEx(in, len, out, outLen, rp, NULL);
}

/* Alinan paketi coz, BufferRx'e duz (bugunku) haliyle koy. false = paketi at. */
static bool Sec_Incoming(void)
{
  uint16_t n = 0;
  uint16_t m = 0;
  uint32_t rd = SEC_RELAY_ANY;
  SecRes_t r;
  secRawValid = false;
  secRxVer = 0;
  secRxRelayDev = SEC_RELAY_ANY;
  if (!(((BufferRx[0] == SEC_VER) && (RxBufferSize >= (SEC_OVERHEAD + 1U))) ||
        ((BufferRx[0] == SEC_VER2) && (RxBufferSize >= (SEC_OVERHEAD2 + 1U)))))
  {
#if (SECURE_ONLY == 1)
    APP_LOG(TS_ON, VLEVEL_L, "[SEC] duz paket atildi (%u B)\n\r", (unsigned)RxBufferSize);
    return false;
#else
    return true;   /* gecis: duz paket, eski yolla islenir */
#endif
  }
  memcpy(secRaw, BufferRx, RxBufferSize);
  secRawLen = RxBufferSize;
  r = Sec_OpenEx(secRaw, secRawLen, BufferRx, &n, SEC_RP_ENFORCE, &rd);
  if (r == SEC_REPLAY)
  {
    secReplayDrop++;
    return false;   /* tekrar (gateway'in ayni kopyasi ya da kayit) - sessizce at */
  }
  if (r != SEC_OK)
  {
    secMicFail++;
#if (SECURE_ONLY == 0)
    memcpy(BufferRx, secRaw, secRawLen);   /* 0x51 ile baslayan duz paket olabilir */
    if (secRawLen >= SEC_PIPE_MIN_LEN)
    {
      APP_LOG(TS_ON, VLEVEL_L, "[SEC] MIC hatasi (%u B), duz paket sayildi (toplam %u)\n\r",
              (unsigned)secRawLen, (unsigned int)secMicFail);
    }
    return true;
#else
    APP_LOG(TS_ON, VLEVEL_L, "[SEC] MIC hatasi, paket atildi (%u B, toplam %u)\n\r",
            (unsigned)secRawLen, (unsigned int)secMicFail);
    return false;
#endif
  }
  RxBufferSize = n;
  memcpy(&secRxDev, &secRaw[1], 4);
  if (secRxDev == SEC_DEVID_ZYBO)
  {
    /* Zybo'yu DOGRUDAN duyduk (E3 icindekinin dis zarfi aktaranin): GW kalitesi icin SNR */
    zyboDirectTime = UTIL_TIMER_GetCurrentTime();
    zyboDirectSnr = SnrValue;
    zyboDirectEver = true;
  }

  /* Ic ice aktarim: icteki guvenli paketi de ac (dis zarfin sayaci zaten tekrari engelliyor) */
  if (((BufferRx[0] == RELAY_TYPE_POS) || (BufferRx[0] == RELAY_TYPE_UL_CTRL)) &&
      (n > (RELAY_POS_HDR + SEC_OVERHEAD)) &&
      ((BufferRx[RELAY_POS_HDR] == SEC_VER) || (BufferRx[RELAY_POS_HDR] == SEC_VER2)))
  {
    if (Sec_Open(&BufferRx[RELAY_POS_HDR], (uint16_t)(n - RELAY_POS_HDR), secInner, &m, SEC_RP_NONE) != SEC_OK)
    {
      APP_LOG(TS_ON, VLEVEL_L, "[SEC] aktarilan ic paketin MIC'i hatali, atildi\n\r");
      return false;
    }
    memcpy(&BufferRx[RELAY_POS_HDR], secInner, m);
    RxBufferSize = (uint16_t)(RELAY_POS_HDR + m);
  }
  else if ((BufferRx[0] == RELAY_TYPE_DL) && (n > (RELAY_DL_HDR + SEC_OVERHEAD)) &&
           (BufferRx[RELAY_DL_HDR] == SEC_VER))
  {
    /* Zybo'nun paketi: pencereye isle (dogrudan gelen kopya ile ayni sayac), ama kopyayi
       atma: bugunku tekrar eleme ve aktarim bastirma kurallari ayni sekilde calissin */
    if (Sec_Open(&BufferRx[RELAY_DL_HDR], (uint16_t)(n - RELAY_DL_HDR), secInner, &m, SEC_RP_NOTE) != SEC_OK)
    {
      APP_LOG(TS_ON, VLEVEL_L, "[SEC] aktarilan downlink'in MIC'i hatali, atildi\n\r");
      return false;
    }
    memcpy(&BufferRx[RELAY_DL_HDR], secInner, m);
    RxBufferSize = (uint16_t)(RELAY_DL_HDR + m);
  }
  else
  {
    secRawValid = true;   /* dogrudan gelen guvenli paket: aktarilacaksa aynen tasinir */
    secRxVer = secRaw[0];
    secRxRelayDev = rd;
  }
  return true;
}

/* Node'un havaya cikan her paketi buradan gecer. Aktarim istedigimiz paketler (bit1'li konum,
   aktarim modundayken C1/C2/C5/A4) 0x52 zarfla, secilen aktaricinin dev_id'siyle gider. */
static void AirSend(const uint8_t *data, uint8_t len)
{
  bool v2 = false;
  uint32_t rd = SEC_RELAY_ANY;
  if ((len == sizeof(pos_packet_t)) && (data[11] & POS_FLAG_RELAY_REQ))
  {
    v2 = true;
    if ((uint16_t)(data[0] | (data[1] << 8)) == myAddr) { rd = txRelayDev; }
  }
  else if ((len == CMD_PACKET_LEN) && Node_NeedsRelay() &&
           ((data[0] == CMD_TYPE) || (data[0] == CMD_TYPE_DEL_REQ) || (data[0] == CMD_TYPE_ACK) ||
            (data[0] == JOIN_TYPE_LEAVE)))
  {
    v2 = true;
    rd = curRelayDev;
  }
  if (((uint16_t)len + (v2 ? SEC_OVERHEAD2 : SEC_OVERHEAD)) > MAX_APP_BUFFER_SIZE)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[SEC] paket zarfa sigmiyor (%u B), yollanmadi\n\r", (unsigned)len);
    PostEvent(EV_TX_DONE);   /* txBusy takili kalmasin */
    return;
  }
  uint16_t n = Sec_Wrap2(data, len, secTxBuf, v2, rd);
  RadioSend(secTxBuf, (uint8_t)n);
}

/* ---------------- Aktarici secimi (uzak node) ---------------- */

static Nb_t *Nb_Find(uint32_t dev)
{
  for (uint32_t i = 0; i < NB_MAX; i++)
  {
    if (nbList[i].used && (nbList[i].devId == dev)) { return &nbList[i]; }
  }
  return NULL;
}

/* Dogrudan duydugumuz komsunun konumu: baglanti (bizim duydugumuz SNR) ve GW kalitesi */
static void Nb_Update(uint32_t dev, uint16_t addr, int8_t snr, uint8_t gwq)
{
  uint32_t now = UTIL_TIMER_GetCurrentTime();
  Nb_t *e = Nb_Find(dev);
  if (e == NULL)
  {
    /* bos ya da en uzun suredir duyulmayan yuva */
    e = &nbList[0];
    for (uint32_t i = 0; i < NB_MAX; i++)
    {
      if (!nbList[i].used) { e = &nbList[i]; break; }
      if ((now - nbList[i].lastSeen) > (now - e->lastSeen)) { e = &nbList[i]; }
    }
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->devId = dev;
  }
  e->addr = addr;
  e->link = (uint8_t)LINK_Q(snr);
  e->gwq = gwq;
  e->lastSeen = now;
}

/* Aday mi: son 30 sn'de duyuldu, gateway'e dogrudan ulasiyor (gwq > 0), kotu isaretli degil */
static bool Nb_Usable(Nb_t *e, uint32_t now)
{
  if (!e->used || (e->gwq == 0) || ((now - e->lastSeen) > NB_TIMEOUT_MS)) { return false; }
  if (e->bad && ((now - e->badTime) < NB_BAD_MS)) { return false; }
  e->bad = false;
  return true;
}

/* Puan: zayif halka belirler */
static uint8_t Nb_Score(const Nb_t *e)
{
  return (e->link < e->gwq) ? e->link : e->gwq;
}

/* Aktarim isteyen konum paketimiz icin aktarici sec. Donus: dev_id ya da SEC_RELAY_ANY */
static uint32_t Relay_Choose(uint8_t seq)
{
  uint32_t now = UTIL_TIMER_GetCurrentTime();
  uint32_t prev = curRelayDev;
  Nb_t *cur = NULL;
  Nb_t *best = NULL;
  Nb_t *sel;
  uint32_t dev;

  /* Fallback: bir onceki konumumuzu secili aktarici aktarmadi (E1'ini duymadik) */
  if (chkActive)
  {
    chkActive = false;
    if (++chkMiss >= NB_FAIL_MAX)
    {
      Nb_t *b = Nb_Find(chkDev);
      if (b != NULL)
      {
        b->bad = true;
        b->badTime = now;
        APP_LOG(TS_ON, VLEVEL_L, "[MESH] aktarici 0x%04X %u kez aktarmadi: %u sn aday degil\n\r",
                b->addr, (unsigned)chkMiss, (unsigned)(NB_BAD_MS / 1000));
      }
      if (curRelayDev == chkDev) { curRelayDev = SEC_RELAY_ANY; }
      chkMiss = 0;
    }
  }

  if (curRelayDev != SEC_RELAY_ANY)
  {
    cur = Nb_Find(curRelayDev);
    if ((cur != NULL) && !Nb_Usable(cur, now)) { cur = NULL; }
  }
  for (uint32_t i = 0; i < NB_MAX; i++)
  {
    Nb_t *e = &nbList[i];
    if (!Nb_Usable(e, now)) { continue; }
    if ((best == NULL) || (Nb_Score(e) > Nb_Score(best)) ||
        ((Nb_Score(e) == Nb_Score(best)) && (e->link > best->link)))
    {
      best = e;
    }
  }
  /* Histerezis: mevcut aktarici gidene ya da digeri en az 2 basamak iyi olana kadar kalir */
  if ((cur != NULL) && (best != NULL) && (Nb_Score(best) >= (uint8_t)(Nb_Score(cur) + NB_HYST_STEPS)))
  {
    sel = best;
  }
  else if (cur != NULL)
  {
    sel = cur;
  }
  else
  {
    sel = best;
  }
  dev = (sel != NULL) ? sel->devId : SEC_RELAY_ANY;
  if (dev != prev)
  {
    if (sel != NULL)
    {
      APP_LOG(TS_ON, VLEVEL_L, "[MESH] aktarici secildi: 0x%04X (baglanti %u, GW %u)\n\r",
              sel->addr, sel->link, sel->gwq);
    }
    else
    {
      APP_LOG(TS_ON, VLEVEL_L, "[MESH] uygun aktarici yok: herhangi bir komsu (FFFFFFFF)\n\r");
    }
    chkMiss = 0;
  }
  curRelayDev = dev;
  if (dev != SEC_RELAY_ANY)
  {
    chkActive = true;
    chkDev = dev;
    chkSeq = seq;
  }
  return dev;
}

/* Tekrar dogrudan ulasiyoruz: secim durumunu sifirla */
static void Relay_ResetChoice(void)
{
  curRelayDev = SEC_RELAY_ANY;
  txRelayDev = SEC_RELAY_ANY;
  chkActive = false;
  chkMiss = 0;
}

/* Komsunun bizim paketimizi aktardigini duyduk (E1 konum / E2 kontrol) */
static void Relay_NoteMine(uint16_t relayer, bool isPos, uint8_t seq)
{
  if (isPos)
  {
    myRelayed[myRelayedIdx].seq = seq;
    myRelayed[myRelayedIdx].t = UTIL_TIMER_GetCurrentTime();
    myRelayed[myRelayedIdx].used = true;
    myRelayedIdx = (uint8_t)((myRelayedIdx + 1U) % MY_RELAYED_SEQ_N);
  }
  if (curRelayDev != SEC_RELAY_ANY)
  {
    Nb_t *b = Nb_Find(curRelayDev);
    if ((b != NULL) && (b->addr == relayer))
    {
      if (isPos && chkActive && (seq == chkSeq)) { chkActive = false; }
      chkMiss = 0;
    }
  }
}

/* Tabloda bit2'siz kaydimiz: Zybo bit2'yi hic koymadiysa ve bu seq'in aktarildigini
   duyduysak aktarimla gelmis say (eski PL icin emniyet; Zybo bit2'yi koyunca devre disi) */
static bool Relay_TableGuard(uint8_t seq)
{
  uint32_t now = UTIL_TIMER_GetCurrentTime();
  if (sawViaBit) { return false; }
  for (uint32_t i = 0; i < MY_RELAYED_SEQ_N; i++)
  {
    if (myRelayed[i].used && (myRelayed[i].seq == seq) && ((now - myRelayed[i].t) <= MY_RELAYED_SEQ_MS))
    {
      return true;
    }
  }
  return false;
}
#endif /* SECURE_ENABLED && node */

#if (SECURE_ENABLED == 1) && (GATEWAY_MODE == 1)
/* Zybo'dan gelen hazir guvenli paketi kuyruga al. Kuyruk doluysa en eskinin kalan tekrarlari atilir. */
static void SecTx_Add(const uint8_t *pkt, uint16_t len, uint8_t repeat)
{
  SecTx_t *s = NULL;
  SecTx_t *old = NULL;
  if (len > MAX_APP_BUFFER_SIZE) { return; }
  for (uint32_t i = 0; i < SEC_TXQ_SIZE; i++)
  {
    if (!secTxQ[i].active) { s = &secTxQ[i]; break; }
    if ((old == NULL) || (secTxQ[i].seq < old->seq)) { old = &secTxQ[i]; }
  }
  if (s == NULL)
  {
    s = old;
    UTIL_TIMER_Stop(&s->timer);
    APP_LOG(TS_ON, VLEVEL_L, "[GW] guvenli kuyruk dolu: en eski paketin kalan %u yayini atildi\n\r", s->left);
  }
  memcpy(s->buf, pkt, len);
  s->len = (uint8_t)len;
  s->left = repeat;
  s->seq = ++secTxSeq;
  s->active = true;
  s->due = true;
  APP_LOG(TS_ON, VLEVEL_L, "[GW] Zybo'dan guvenli paket %u B, %u kez yayinlanacak\n\r", (unsigned)len, repeat);
}

/* Zamani gelmis en eski guvenli paketi yayinla. true = TX basladi */
static bool SecTx_Service(void)
{
  SecTx_t *pick = NULL;
  for (uint32_t i = 0; i < SEC_TXQ_SIZE; i++)
  {
    SecTx_t *q = &secTxQ[i];
    if (q->active && q->due && ((pick == NULL) || (q->seq < pick->seq))) { pick = q; }
  }
  if (pick == NULL) { return false; }
  pick->due = false;
  pick->left--;
  if (pick->left == 0)
  {
    pick->active = false;
  }
  else
  {
    UTIL_TIMER_SetPeriod(&pick->timer, CMD_RELAY_GAP_MS + (Rand32() % 200));
    UTIL_TIMER_Start(&pick->timer);
  }
  APP_LOG(TS_ON, VLEVEL_L, "[GW] guvenli paket yayinlandi (%u B, kalan %u)\n\r", pick->len, pick->left);
  HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
  UTIL_TIMER_Start(&timerLed);
  txBusy = true;
  RadioSend(pick->buf, pick->len);
  return true;
}

static void OnSecTxTimer(void *context)
{
  secTxQ[(uint32_t)context].due = true;
  PostEvent(EV_SECTX);
}
#endif /* SECURE_ENABLED && gateway */


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

#if (GW_FORWARD_RELAY_INFO == 1)
/* Aktarilmis konumu (0xE1, 17 B) [AA 55 13 | 17 B | RSSI | SNR | CRC8] ile Zybo'ya yolla */
static void Gateway_ForwardRelayed(const uint8_t *relayPkt, int16_t rssi, int8_t snr)
{
  uint8_t frame[3 + GW_RELAY_FRAME_LEN_FIELD + 1];
  uint32_t i = 0;

  if (rssi < -128) { rssi = -128; }
  if (rssi > 127)  { rssi = 127; }

  frame[i++] = GW_SYNC0;
  frame[i++] = GW_SYNC1;
  frame[i++] = (uint8_t)GW_RELAY_FRAME_LEN_FIELD;
  memcpy(&frame[i], relayPkt, RELAY_POS_LEN);
  i += RELAY_POS_LEN;
  frame[i++] = (uint8_t)(int8_t)rssi;
  frame[i++] = (uint8_t)snr;
  frame[i] = Crc8(&frame[2], i - 2);   /* LEN..SNR */
  i++;

  if (HAL_UART_Transmit(&huart1, frame, (uint16_t)i, 20) == HAL_OK)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[GW] aktarilmis konum -> Zybo %d byte\n\r", (int)i);
  }
  else
  {
    APP_LOG(TS_ON, VLEVEL_L, "[GW] aktarilmis konum USART1 gonderim HATASI\n\r");
  }
}
#endif

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
#if (SECURE_ENABLED == 1)
  /* Zybo'dan hazir guvenli paket: [tx_repeat][0x51 ...]. Acilmaz, tx_repeat kez yayinlanir. */
  if (dlFrameReady && (dlFrameLen >= (1U + SEC_OVERHEAD + 1U)) &&
      (dlFrame[0] >= 1U) && (dlFrame[0] <= SEC_TX_REPEAT_MAX) && (dlFrame[1] == SEC_VER))
  {
    SecTx_Add(&dlFrame[1], (uint16_t)(dlFrameLen - 1U), dlFrame[0]);
    dlFrameReady = false;
  }
#if (SECURE_ONLY == 1)
  if (dlFrameReady)
  {
    APP_LOG(TS_ON, VLEVEL_L, "[SEC] Zybo'dan duz downlink atildi (tip 0x%02X, SECURE_ONLY)\n\r", dlFrame[0]);
    dlFrameReady = false;
  }
#endif
#endif
  if (dlFrameReady && (dlFrameLen == CMD_PACKET_LEN) &&
      ((dlFrame[0] == CMD_TYPE_DEL_ACK) || (dlFrame[0] == CMD_TYPE) || (dlFrame[0] == JOIN_TYPE_ANS) ||
       (dlFrame[0] == JOIN_TYPE_KICK)))
  {
    /* 0xA2 katilma cevabi / 0xA3 kick: admin karari gibi 3x yayin (0xC3 ile ayni tampon) */
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
#if (SECURE_ENABLED == 1)
  /* Zybo'nun guvenli paketlerinden zamani gelen varsa yayinla */
  if (!txBusy && SecTx_Service())
  {
    return; /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
  }
#endif
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
  /* Zamani gelmis ACK (ilk ya da tekrar) varsa yolla */
  for (uint32_t i = 0; (i < ACK_Q_SIZE) && !txBusy; i++)
  {
    AckSlot_t *s = &ackQ[i];
    if (!s->active || !s->due) { continue; }
    s->due = false;
    memset(ackTxBuf, 0, sizeof(ackTxBuf));
    ackTxBuf[0] = CMD_TYPE_ACK;
    ackTxBuf[1] = (uint8_t)(s->me & 0xFF);
    ackTxBuf[2] = (uint8_t)(s->me >> 8);
    ackTxBuf[3] = (uint8_t)(ADDR_ADMIN & 0xFF);
    ackTxBuf[4] = (uint8_t)(ADDR_ADMIN >> 8);
    ackTxBuf[5] = ackSeq++;
    ackTxBuf[6] = s->kind;
    ackTxBuf[7] = (uint8_t)(s->ackedSrc & 0xFF);
    ackTxBuf[8] = (uint8_t)(s->ackedSrc >> 8);
    ackTxBuf[9] = s->ackedId;
    s->left--;
    if (s->left == 0)
    {
      s->active = false;
    }
    else
    {
      UTIL_TIMER_SetPeriod(&s->timer, ACK_REPEAT_GAP_MIN_MS + (Rand32() % ACK_REPEAT_GAP_RAND_MS));
      UTIL_TIMER_Start(&s->timer);
    }
    LogCmd("ACK yollandi", ackTxBuf);
    APP_LOG(TS_OFF, VLEVEL_L, " (%d/%d)\n\r", (int)(ACK_REPEAT - s->left), ACK_REPEAT);
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
    UTIL_TIMER_Start(&timerLed);
    txBusy = true;
    AirSend(ackTxBuf, CMD_PACKET_LEN);
    return; /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
  }
  /* Tek sekme mesh: zamani gelmis aktarim varsa yolla */
  for (uint32_t i = 0; (i < RELAY_Q_SIZE) && !txBusy; i++)
  {
    RelaySlot_t *s = &relayQ[i];
    if (!s->active || !s->due) { continue; }
    s->active = false;
    s->due = false;
    if (s->buf[0] == RELAY_TYPE_POS)
    {
      /* adres/seq anahtardan: guvenli modda icerik uzak node'un sifreli paketi */
      APP_LOG(TS_ON, VLEVEL_L, "[MESH] aktarildi: konum 0x%04X seq=%d (%d byte)\n\r",
              (unsigned)((s->key >> 8) & 0xFFFFU), (int)(s->key & 0xFFU), s->len);
    }
    else
    {
      APP_LOG(TS_ON, VLEVEL_L, "[MESH] aktarildi: tip 0x%02X (ic tip 0x%02X, %d byte)\n\r",
              s->buf[0], s->buf[(s->buf[0] == RELAY_TYPE_UL_CTRL) ? RELAY_UL_HDR : RELAY_DL_HDR], s->len);
    }
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
    UTIL_TIMER_Start(&timerLed);
    txBusy = true;
    AirSend(s->buf, s->len);
    return; /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
  }
  /* Telefondan gelen komut / silme istegi varsa (radyo bossa) yolla */
  if (cmdTxPending && !txBusy)
  {
    cmdTxPending = false;
    APP_LOG(TS_ON, VLEVEL_L, "[CMD] LoRa'ya yollandi (tip 0x%02X)\n\r", cmdTxBuf[0]);
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
    UTIL_TIMER_Start(&timerLed);
    txBusy = true;
    AirSend(cmdTxBuf, CMD_PACKET_LEN);
    return; /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
  }
#endif

  /* Zamani gelmis bir sanal node varsa (ve radyo bossa) onun paketini yolla.
     Birden fazlasi ayni anda bekliyorsa siradakiler bu TX bitince (EV_TX_DONE) yollanir. */
#if (GATEWAY_MODE == 0)
  /* KICK: ULASTI'lar havaya ciktiysa kimligi sil */
  if (kickPending && !Ack_Pending(kickedAddr))
  {
    Join_Clear(JOIN_RES_KICKED);
  }
  /* LEAVE: 0xA4'u yolla (5 sn arayla en fazla 3 kez); cevap gelmezse yine de ayril */
  if ((joinState == JOIN_ST_LEAVING) && joinDue && !txBusy)
  {
    joinDue = false;
    if (leaveTries >= LEAVE_TRIES)
    {
      APP_LOG(TS_ON, VLEVEL_L, "[JOIN] ayrilma onayi gelmedi, yine de ayriliniyor\n\r");
      Join_Clear(JOIN_RES_LEFT);
    }
    else
    {
      uint8_t a4[CMD_PACKET_LEN];
      memset(a4, 0, sizeof(a4));
      a4[0] = JOIN_TYPE_LEAVE;
      a4[1] = (uint8_t)(myAddr & 0xFF);
      a4[2] = (uint8_t)(myAddr >> 8);
      a4[3] = (uint8_t)(ADDR_ADMIN & 0xFF);
      a4[4] = (uint8_t)(ADDR_ADMIN >> 8);
      a4[5] = leaveMsgId;                       /* denemelerde ayni (gateway / Zybo tekrar eler) */
      memcpy(&a4[6], &myDevId, 4);
      leaveTries++;
      LogCmd("ayrilma yollandi", a4);
      APP_LOG(TS_OFF, VLEVEL_L, " (%u/%u)\n\r", leaveTries, LEAVE_TRIES);
      UTIL_TIMER_SetPeriod(&timerJoin, LEAVE_RETRY_MS);
      UTIL_TIMER_Start(&timerJoin);
      HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
      UTIL_TIMER_Start(&timerLed);
      txBusy = true;
      AirSend(a4, CMD_PACKET_LEN);
      return; /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
    }
  }
  /* JOIN: kimlik yokken sadece katilma istegi (0xA1) yollanir */
  if ((joinState == JOIN_ST_WAITING) && joinDue && !txBusy)
  {
    uint8_t a1[JOIN_REQ_LEN];
    joinDue = false;
    memset(a1, 0, sizeof(a1));
    a1[0] = JOIN_TYPE_REQ;
    a1[1] = 0x00;                               /* src: henuz adres yok */
    a1[2] = 0x00;
    a1[3] = (uint8_t)(ADDR_ADMIN & 0xFF);
    a1[4] = (uint8_t)(ADDR_ADMIN >> 8);
    a1[5] = joinReqSeq++;
    memcpy(&a1[6], &myDevId, 4);                /* little-endian */
    a1[10] = joinTeam;
    a1[11] = joinNum;
    a1[12] = (uint8_t)(joinPin & 0xFF);
    a1[13] = (uint8_t)(joinPin >> 8);
    a1[14] = (uint8_t)APP_VERSION_MAIN;
    a1[15] = (uint8_t)APP_VERSION_SUB1;
    a1[16] = 0xFF;                              /* pil olcumu yok */
    APP_LOG(TS_ON, VLEVEL_L, "[JOIN] katilma istegi #%u: takim=%u numara=%u dev_id=0x%08X\n\r",
            a1[5], joinTeam, joinNum, (unsigned int)myDevId);
    Join_SendStatus();
    UTIL_TIMER_SetPeriod(&timerJoin, JOIN_PERIOD_MS + (Rand32() % JOIN_PERIOD_RAND_MS));
    UTIL_TIMER_Start(&timerJoin);
    HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_SET); /* yesil = TX */
    UTIL_TIMER_Start(&timerLed);
    txBusy = true;
    AirSend(a1, JOIN_REQ_LEN);
    return; /* TX bitince (EV_TX_DONE) tekrar dinlemeye gececegiz */
  }
  if (joinDue && (joinState == JOIN_ST_NEED_PARAMS))
  {
    /* Telefon parametreleri bilmiyorsa (yeniden baglandi vb.) durumu hatirlat */
    joinDue = false;
    Join_SendStatus();
    UTIL_TIMER_SetPeriod(&timerJoin, JOIN_PERIOD_MS);
    UTIL_TIMER_Start(&timerJoin);
  }
  /* JOIN: onay yokken konum yayinlanmaz (zamanlayici donmeye devam eder) */
  if ((pendingTx != 0) &&
      (((joinState != JOIN_ST_DISABLED) && (joinState != JOIN_ST_JOINED)) || kickPending))
  {
    for (uint32_t vi = 0; vi < VIRTUAL_NODES; vi++)
    {
      if (pendingTx & (1U << vi)) { ScheduleNextTx(vi); }
    }
    pendingTx = 0;
  }
#endif
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
    AirSend((const uint8_t *)&txPacket, sizeof(txPacket));
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
