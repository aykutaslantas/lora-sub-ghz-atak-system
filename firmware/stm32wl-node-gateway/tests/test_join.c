#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
/* ---- stub implementations ---- */
static uint32_t g_now = 1000;
typedef void (*tcb)(void*);
#define MAXT 32
static struct { void *obj; tcb cb; void *ctx; uint32_t period, due; int running; } T[MAXT]; static int nT=0;
#include "stubs/stubs.h"
static uint8_t fakeFlash[2048];
#define ID_FLASH_READ_PTR ((const void*)fakeFlash)

GPIO_TypeDef g1,g2; GPIO_TypeDef *LED2_GPIO_Port=&g1,*LED3_GPIO_Port=&g2;
USART_TypeDef u1; USART_TypeDef *USART1=&u1; UART_HandleTypeDef huart1;
static uint8_t lastTx[256]; static int lastTxLen=-1; static int txCount=0;
static uint8_t lastUart[300]; static int lastUartLen=-1; static int uartCount=0;
static int findT(void*o){for(int i=0;i<nT;i++) if(T[i].obj==o) return i; return -1;}
int UTIL_TIMER_Create(UTIL_TIMER_Object_t*o,uint32_t p,UTIL_TIMER_Mode_t m,void(*cb)(void*),void*c){int i=findT(o); if(i<0){i=nT++;} T[i].obj=o;T[i].cb=cb;T[i].ctx=c;T[i].period=p;T[i].running=0;return 0;}
int UTIL_TIMER_SetPeriod(UTIL_TIMER_Object_t*o,uint32_t p){int i=findT(o); if(i>=0)T[i].period=p;return 0;}
int UTIL_TIMER_Start(UTIL_TIMER_Object_t*o){int i=findT(o); if(i>=0){T[i].running=1;T[i].due=g_now+T[i].period;}return 0;}
int UTIL_TIMER_Stop(UTIL_TIMER_Object_t*o){int i=findT(o); if(i>=0)T[i].running=0;return 0;}
UTIL_TIMER_Time_t UTIL_TIMER_GetCurrentTime(void){return g_now;}
void UTIL_SEQ_RegTask(uint32_t a,uint32_t b,void(*f)(void)){}
void UTIL_SEQ_SetTask(uint32_t a,uint32_t b){}
int HAL_UART_Transmit(UART_HandleTypeDef*h,const uint8_t*d,uint16_t n,uint32_t t){memcpy(lastUart,d,n);lastUartLen=n;uartCount++;return 0;}
void HAL_GPIO_WritePin(GPIO_TypeDef*g,uint16_t p,GPIO_PinState s){}
uint32_t HAL_GetUIDw0(void){return UIDW0;} uint32_t HAL_GetUIDw1(void){return 0;} uint32_t HAL_GetUIDw2(void){return 0;}
void HAL_NVIC_SetPriority(int a,uint32_t b,uint32_t c){} void HAL_NVIC_EnableIRQ(int a){}
void __disable_irq(void){} uint32_t __get_PRIMASK(void){return 0;} void __set_PRIMASK(uint32_t x){}
static void r_init(RadioEvents_t*e){} static void r_sleep(void){} static void r_ch(uint32_t f){}
static void r_txc(int a,int8_t b,uint32_t c,uint32_t d,uint32_t e,uint8_t f,uint16_t g,bool h,bool i,bool j,uint8_t k,bool l,uint32_t m){}
static void r_rxc(int a,uint32_t b,uint32_t c,uint8_t d,uint32_t e,uint16_t f,uint16_t g,bool h,uint8_t i,bool j,bool k,uint8_t l,bool m,bool n){}
static void r_max(int a,uint8_t b){} static void r_send(uint8_t*d,uint8_t n){memcpy(lastTx,d,n);lastTxLen=n;txCount++;} static void r_rx(uint32_t t){}
static uint32_t r_rand(void){return 0x1234;}
const struct Radio_s Radio={r_init,r_sleep,r_ch,r_txc,r_rxc,r_max,r_send,r_rx,r_rand};
HAL_StatusTypeDef HAL_FLASH_Unlock(void){return 0;} HAL_StatusTypeDef HAL_FLASH_Lock(void){return 0;}
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef*e,uint32_t*err){ if(e->Page==127) memset(fakeFlash,0xFF,sizeof fakeFlash); return 0;}
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t t,uint32_t a,uint64_t d){ uint32_t off=a-0x0803F800U; if(off+8>2048) return 1; memcpy(fakeFlash+off,&d,8); return 0;}
#include "../SubGHz_Phy/App/subghz_phy_app.c"
/* ---- helpers ---- */
static void advance(uint32_t ms){ uint32_t end=g_now+ms; while(g_now<end){ g_now++; for(int i=0;i<nT;i++) if(T[i].running && g_now>=T[i].due){ T[i].running=0; T[i].cb(T[i].ctx);} } }
static void rx(const uint8_t*p,int n,int16_t rssi){ memcpy(BufferRx,p,n); RxBufferSize=n; RssiValue=rssi; SnrValue=10; HandleRxPacket(); }
static void txdone(void){ txBusy=false; }
static void pump(void){ Node_Process(); }
static int sent_type(void){ return lastTxLen>0? lastTx[0] : -1; }
static void mkpos(uint8_t*b,uint16_t addr,uint8_t seq,uint8_t flags){ b[0]=addr&0xFF;b[1]=addr>>8;b[2]=seq; memset(b+3,0x11,8); b[11]=flags; }
static void mktable(uint8_t*b,int*len,uint16_t a1,uint8_t f1,uint16_t a2,uint8_t f2){ b[0]=0xD1;b[1]=2; mkpos(b+2,a1,7,f1); mkpos(b+14,a2,9,f2); *len=26; }
static void mkcmd(uint8_t*b,uint8_t type,uint16_t src,uint16_t dst,uint8_t id,uint8_t kind){ memset(b,0,15); b[0]=type;b[1]=src&0xFF;b[2]=src>>8;b[3]=dst&0xFF;b[4]=dst>>8;b[5]=id;b[6]=kind; }
#define CHECK(c,msg) do{ if(c) printf("  OK   %s\n",msg); else { printf("  FAIL %s\n",msg); fails++; } }while(0)
int main(void){
  int fails=0; uint8_t b[256]; int n;
  SubghzApp_Init();
  printf("myAddr=0x%04X\n", myAddr);
#if ROLE_GW
  /* GATEWAY */
  uint8_t a1[17]; memset(a1,0,17); a1[0]=0xA1; a1[3]=0xFE;a1[4]=0xFF; a1[5]=0; uint32_t dv=0x9B9D0B15; memcpy(a1+6,&dv,4); a1[10]=1;a1[11]=2;a1[12]=0xD2;a1[13]=0x04;a1[14]=1;a1[15]=14;a1[16]=0xFF;
  int uc=uartCount; rx(a1,17,-52); SnrValue=11;
  CHECK(uartCount==uc+1 && lastUartLen==23 && lastUart[2]==0x13 && lastUart[3]==0xA1,"A1 Zybo'ya LEN 0x13 (23 B) cerceveyle iletildi");
  { uint8_t gold[]={0xAA,0x55,0x13,0xA1,0x00,0x00,0xFE,0xFF,0x00,0x15,0x0B,0x9D,0x9B,0x01,0x02,0xD2,0x04,0x01,0x0E,0xFF,0xCC,0x0B,0xFE};
    uint8_t buf[23]; memcpy(buf,lastUart,23); /* SNR rx'te 10 verildi; altin ornekle ayni olsun diye yeniden uret */
    memcpy(BufferRx,a1,17); RxBufferSize=17; RssiValue=-52; SnrValue=11; joinSeen[0].used=false; joinSeen[(joinSeenIdx+JOIN_SEEN_SIZE-1)%JOIN_SEEN_SIZE].used=false;
    HandleRxPacket(); CHECK(memcmp(lastUart,gold,23)==0,"A1 cercevesi altin ornekle birebir ayni (CRC 0xFE)"); }
  uc=uartCount; rx(a1,17,-52);
  CHECK(uartCount==uc,"ayni (dev_id, req_seq) ikinci kez iletilmedi");
  a1[6]^=0x55; uc=uartCount; rx(a1,17,-52);
  CHECK(uartCount==uc+1,"baska dev_id ayni req_seq ile iletildi (karismadi)");
  /* A2 from Zybo -> 3x broadcast */
  uint8_t a2[15]={0xA2,0xFE,0xFF,0xFF,0xFF,0x30,0x15,0x0B,0x9D,0x9B,0x02,0x81,0x01,0x02,0x00};
  memcpy(dlFrame,a2,15); dlFrameLen=15; dlFrameReady=true; txCount=0;
  for(int k=0;k<6;k++){ pump(); txdone(); advance(500); }
  CHECK(txCount==3 && sent_type()==0xA2,"A2 gateway tarafindan 3 kez yayinlandi");
  /* A3 from Zybo -> 3x */
  { uint8_t a3[15]={0xA3,0xFE,0xFF,0x01,0x81,0x31,0x15,0x0B,0x9D,0x9B,1,0,0,0,0};
    memcpy(dlFrame,a3,15); dlFrameLen=15; dlFrameReady=true; txCount=0;
    for(int k=0;k<6;k++){ pump(); txdone(); advance(500); }
    CHECK(txCount==3 && sent_type()==0xA3,"A3 KICK gateway tarafindan 3 kez yayinlandi"); }
  /* A4 from air -> Zybo 21 B, golden */
  { uint8_t a4[15]={0xA4,0x01,0x81,0xFE,0xFF,0x05,0x15,0x0B,0x9D,0x9B,0,0,0,0,0};
    uint8_t gold[]={0xAA,0x55,0x11,0xA4,0x01,0x81,0xFE,0xFF,0x05,0x15,0x0B,0x9D,0x9B,0,0,0,0,0,0xCC,0x0B,0x7B};
    int uc=uartCount; txCount=0; memcpy(BufferRx,a4,15); RxBufferSize=15; RssiValue=-52; SnrValue=11; HandleRxPacket();
    CHECK(uartCount==uc+1 && lastUartLen==21 && memcmp(lastUart,gold,21)==0,"A4 Zybo'ya altin ornekle birebir (CRC 0x7B)");
    for(int k=0;k<6;k++){ pump(); txdone(); advance(500); }
    CHECK(txCount==0,"A4 havada tekrar yayinlanmadi");
    uc=uartCount; rx(a4,15,-52);
    CHECK(uartCount==uc,"ayni A4 (ayni msg_id) ikinci kez iletilmedi");
    /* relayed A4 inside E2 */
    uint8_t e2[20]; e2[0]=0xE2; e2[1]=0x7C; e2[2]=0x15; e2[3]=0xCF; e2[4]=13; memcpy(e2+5,a4,15); e2[10]=0x06;
    uc=uartCount; rx(e2,20,-46);
    CHECK(uartCount==uc+1 && lastUart[3]==0xA4 && lastUart[4]==0x01 && lastUart[5]==0x81,"E2 icindeki A4 uzak node'un src'siyle Zybo'ya iletildi"); }
  /* golden C5 for kick */
  { uint8_t c5[15]={0xC5,0x01,0x81,0xFE,0xFF,0x00,0x01,0xFE,0xFF,0x31,0,0,0,0,0};
    uint8_t gold[]={0xAA,0x55,0x11,0xC5,0x01,0x81,0xFE,0xFF,0x00,0x01,0xFE,0xFF,0x31,0,0,0,0,0,0xCC,0x0B,0xA0};
    memcpy(BufferRx,c5,15); RxBufferSize=15; RssiValue=-52; SnrValue=11; HandleRxPacket();
    CHECK(lastUartLen==21 && memcmp(lastUart,gold,21)==0,"kick ULASTI'si altin ornekle birebir (CRC 0xA0)"); }
#else
  /* NODE, JOIN_ENABLED=1 */
  CHECK((uidAddr & 0x8000)==0,"UID adresinde bit15 = 0");
  CHECK(joinState==JOIN_ST_NEED_PARAMS,"kimlik yok -> katilma modu (parametre bekliyor)");
  txCount=0; int uc=uartCount;
  for(int k=0;k<130;k++){ advance(100); pump(); txdone(); }
  CHECK(txCount==0,"onaysiz node 13 sn boyunca hic konum / A1 yollamadi");
  CHECK(uartCount>uc && lastUart[3]==0xCB && lastUart[4]==JOIN_ST_NEED_PARAMS,"telefona 'parametre bekleniyor' durumu (0xCB) gitti");
  /* phone params */
  memset(dlFrame,0,15); dlFrame[0]=0xC9; dlFrame[1]=1; dlFrame[2]=2; dlFrame[3]=0xD2; dlFrame[4]=0x04; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
  CHECK(joinState==JOIN_ST_WAITING,"0xC9 ile katilma basladi");
  int a1s=0; uint8_t firstA1[17];
  for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xA1){ if(!a1s) memcpy(firstA1,lastTx,17); a1s++; } txdone(); } }
  CHECK(a1s==1 && lastTxLen==17,"ilk A1 (17 B) yollandi");
  uint32_t dv; memcpy(&dv,firstA1+6,4);
  CHECK(firstA1[1]==0&&firstA1[2]==0&&firstA1[3]==0xFE&&firstA1[4]==0xFF&&dv==myDevId&&firstA1[10]==1&&firstA1[11]==2&&firstA1[12]==0xD2&&firstA1[13]==0x04&&firstA1[16]==0xFF,"A1 alanlari dogru (src 0, dst FFFE, dev_id, takim, numara, PIN LE)");
  for(int k=0;k<160;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xA1) a1s++; txdone(); } }
  CHECK(a1s==2 && lastTx[5]==firstA1[5]+1,"A1 10-15 sn sonra tekrarlandi, req_seq +1");
  /* answer for other device */
  uint8_t a2[15]={0xA2,0xFE,0xFF,0xFF,0xFF,0x40, 0,0,0,0, 0x02,0x81,1,2,0};
  uint32_t other=myDevId^1; memcpy(a2+6,&other,4); rx(a2,15,-40);
  CHECK(joinState==JOIN_ST_WAITING,"baska dev_id'nin cevabi yok sayildi");
  /* number taken, suggest 5 */
  memcpy(a2+6,&myDevId,4); a2[5]=0x41; a2[10]=0;a2[11]=0; a2[13]=5; a2[14]=4; uc=uartCount; rx(a2,15,-40);
  CHECK(joinState==JOIN_ST_FAILED && joinResult==4 && joinNum==5,"'numara dolu' -> basarisiz, onerilen numara 5");
  CHECK(lastUart[3]==0xCB && lastUart[4]==JOIN_ST_FAILED && lastUart[5]==4 && lastUart[9]==5,"telefona sebep ve onerilen numara gitti");
  a1s=0; for(int k=0;k<200;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xA1) a1s++; txdone(); } }
  CHECK(a1s==0,"reddedildikten sonra A1 durdu");
  /* retry with 5, accepted */
  dlFrame[0]=0xC9; dlFrame[2]=5; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
  for(int k=0;k<10;k++){ advance(100); pump(); txdone(); }
  a2[5]=0x42; a2[10]=0x05; a2[11]=0x81; a2[13]=5; a2[14]=0; rx(a2,15,-40);
  CHECK(joinState==JOIN_ST_JOINED && myAddr==0x8105 && vAddr[0]==0x8105,"onaylandi: adres 0x8105 (ALPHA-5)");
  { IdRec_t r; memcpy(&r,fakeFlash,16); CHECK(r.magic==ID_MAGIC && r.addr==0x8105 && r.team==1 && r.num==5 && r.devId==myDevId,"kimlik flash'a yazildi"); }
  int acks=0,possent=0; uint16_t posaddr=0;
  for(int k=0;k<80;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xC5 && lastTx[1]==0x05 && lastTx[2]==0x81 && lastTx[9]==0x42 && lastTx[6]==1) acks++; if(lastTxLen==12){possent++; posaddr=lastTx[0]|(lastTx[1]<<8);} txdone(); } }
  CHECK(acks==3,"yeni adresle (0x8105) A2'ye ULASTI 3 kez yollandi");
  CHECK(possent>=1 && posaddr==0x8105,"konum yayini yeni adresle basladi");
  /* repeat A2 -> DELIVERED again */
  rx(a2,15,-40); acks=0; for(int k=0;k<40;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xC5 && lastTx[9]==0x42) acks++; txdone(); } }
  CHECK(acks==3,"ayni A2 tekrar gelince ULASTI tekrar yollandi");
  /* reboot: identity restored */
  myAddr=uidAddr; joinState=0; Join_Init();
  CHECK(joinState==JOIN_ST_JOINED && myAddr==0x8105,"yeniden acilista kimlik flash'tan geri geldi");
  /* foreign record (other devId) rejected */
  { IdRec_t r; memcpy(&r,fakeFlash,16); r.devId^=1; r.chk=Crc32((uint8_t*)&r,12); uint8_t save[16]; memcpy(save,fakeFlash,16); memcpy(fakeFlash,&r,16);
    myAddr=uidAddr; Join_Init(); CHECK(joinState==JOIN_ST_NEED_PARAMS,"baska kartin kimlik kaydi kabul edilmedi"); memcpy(fakeFlash,save,16); myAddr=uidAddr; Join_Init(); }
  /* corrupted record rejected */
  { fakeFlash[4]^=0xFF; myAddr=uidAddr; Join_Init(); CHECK(joinState==JOIN_ST_NEED_PARAMS,"bozuk kimlik kaydi (CRC) kabul edilmedi"); fakeFlash[4]^=0xFF; myAddr=uidAddr; Join_Init(); }
  /* LEAVE, admin onayliyor (A3 sebep 2) */
  memset(dlFrame,0,15); dlFrame[0]=0xCC; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
  CHECK(joinState==JOIN_ST_LEAVING,"0xCC: ayriliniyor durumu");
  { int a4s=0; uint8_t a4[15]; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xA4){a4s++; memcpy(a4,lastTx,15);} txdone(); } }
    uint32_t dv2; memcpy(&dv2,a4+6,4);
    CHECK(a4s==1 && a4[1]==0x05 && a4[2]==0x81 && a4[3]==0xFE && a4[4]==0xFF && dv2==myDevId && a4[10]==0,"A4 dogru: src 0x8105, dst FFFE, dev_id");
    int pos=0; for(int k=0;k<60;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xA4) a4s++; if(lastTxLen==12) pos++; txdone(); } }
    CHECK(a4s==2 && lastTx[5]==a4[5],"A4 5 sn sonra ayni msg_id ile tekrarlandi");
    CHECK(pos==0,"ayrilirken konum yayini durdu");
    uint8_t a3[15]={0xA3,0xFE,0xFF,0x05,0x81,0x50, 0,0,0,0, 2,0,0,0,0}; memcpy(a3+6,&myDevId,4);
    int acks=0; rx(a3,15,-40);
    CHECK(joinState==JOIN_ST_NEED_PARAMS && joinResult==JOIN_RES_LEFT && myAddr==uidAddr && fakeFlash[0]==0xFF,"A3 sebep 2: kimlik silindi, sonuc 'ayrildi'");
    CHECK(lastUart[3]==0xCB && lastUart[4]==JOIN_ST_NEED_PARAMS && lastUart[5]==6,"telefona 'takimdan ayrildin' gitti");
    a4s=0; for(int k=0;k<120;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xA4) a4s++; if(sent_type()==0xC5) acks++; txdone(); } }
    CHECK(a4s==0 && acks==0,"ayrilma onayina ACK yok, A4 durdu");
  }
  /* wrong pin */
  dlFrame[0]=0xC9; dlFrame[1]=1; dlFrame[2]=2; dlFrame[3]=0xD2; dlFrame[4]=0x04; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame(); for(int k=0;k<10;k++){ advance(100); pump(); txdone(); }
  a2[5]=0x43; a2[10]=0;a2[11]=0; a2[14]=1; rx(a2,15,-40);
  CHECK(joinState==JOIN_ST_FAILED && joinResult==1,"yanlis PIN -> basarisiz (sebep 1)");
  /* invalid params */
  dlFrame[0]=0xC9; dlFrame[1]=30; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
  CHECK(joinState==JOIN_ST_FAILED,"gecersiz takim (30) reddedildi");
  /* tekrar katil (ALPHA-5) */
  { dlFrame[0]=0xC9; dlFrame[1]=1; dlFrame[2]=5; dlFrame[3]=0xD2; dlFrame[4]=0x04; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
    for(int k=0;k<10;k++){ advance(100); pump(); txdone(); }
    uint8_t ok[15]={0xA2,0xFE,0xFF,0xFF,0xFF,0x60, 0,0,0,0, 0x05,0x81,1,5,0}; memcpy(ok+6,&myDevId,4); rx(ok,15,-40);
    for(int k=0;k<40;k++){ advance(100); pump(); txdone(); }
    CHECK(joinState==JOIN_ST_JOINED && myAddr==0x8105,"tekrar katildi (0x8105)"); }
  /* KICK: baska dev_id -> yok sayilir */
  { uint8_t k3[15]={0xA3,0xFE,0xFF,0x05,0x81,0x31, 0,0,0,0, 1,0,0,0,0}; uint32_t o=myDevId^7; memcpy(k3+6,&o,4); rx(k3,15,-40);
    CHECK(joinState==JOIN_ST_JOINED && !kickPending,"baska dev_id'li KICK yok sayildi");
    memcpy(k3+6,&myDevId,4); rx(k3,15,-40);
    CHECK(kickPending && joinState==JOIN_ST_JOINED && myAddr==0x8105,"KICK: once ULASTI, kimlik henuz silinmedi");
    int acks=0,pos=0; uint8_t ack[15];
    for(int k=0;k<60;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xC5){acks++; memcpy(ack,lastTx,15);} if(lastTxLen==12) pos++; txdone(); } }
    CHECK(acks==3 && ack[1]==0x05 && ack[2]==0x81 && ack[6]==1 && ack[7]==0xFE && ack[8]==0xFF && ack[9]==0x31,"3x ULASTI eski adresten (0x8105), acked FFFE/0x31");
    CHECK(pos==0,"atilinca konum yayini durdu");
    CHECK(joinState==JOIN_ST_NEED_PARAMS && joinResult==JOIN_RES_KICKED && myAddr==uidAddr && fakeFlash[0]==0xFF,"ULASTI'lardan sonra kimlik silindi, sonuc 'atildi'");
    CHECK(lastUart[3]==0xCB && lastUart[5]==5,"telefona 'admin cikardi' gitti");
    /* Zybo tekrar denedi (ayni msg_id) */
    rx(k3,15,-40); acks=0;
    for(int k=0;k<60;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xC5){acks++; memcpy(ack,lastTx,15);} txdone(); } }
    CHECK(acks==3 && ack[1]==0x05 && ack[2]==0x81 && ack[9]==0x31,"gec gelen KICK tekrarina eski adresten ULASTI");
    CHECK(joinState==JOIN_ST_NEED_PARAMS,"durum bozulmadi"); }
  /* tekrar katil, sonra LEAVE cevapsiz */
  { dlFrame[0]=0xC9; dlFrame[1]=1; dlFrame[2]=5; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
    for(int k=0;k<10;k++){ advance(100); pump(); txdone(); }
    uint8_t ok[15]={0xA2,0xFE,0xFF,0xFF,0xFF,0x61, 0,0,0,0, 0x05,0x81,1,5,0}; memcpy(ok+6,&myDevId,4); rx(ok,15,-40);
    for(int k=0;k<40;k++){ advance(100); pump(); txdone(); }
    memset(dlFrame,0,15); dlFrame[0]=0xCC; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
    int a4s=0; for(int k=0;k<200;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xA4) a4s++; txdone(); } }
    CHECK(a4s==3 && joinState==JOIN_ST_NEED_PARAMS && joinResult==JOIN_RES_LEFT,"cevap gelmezse 3 A4'ten sonra yine de ayrildi"); }
  /* kimliksizken 0xCC yok sayilir */
  { memset(dlFrame,0,15); dlFrame[0]=0xCC; dlFrameLen=15; dlFrameReady=true; Node_HandleUartFrame();
    CHECK(joinState==JOIN_ST_NEED_PARAMS,"kimlik yokken ayrilma istegi yok sayildi"); }
#endif
  printf(fails? "\n%d HATA\n" : "\nHEPSI GECTI\n", fails);
  return fails;
}
