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
#if ROLE_RELAYER
  uint16_t FAR=0x0F53;
  /* 1) relayer sees itself in table directly -> can relay */
  mktable(b,&n,myAddr,0,0x1111,0); rx(b,n,-40); pump();
  CHECK(Node_CanRelay(),"aktarici tabloda kendini gordu -> CanRelay");
  CHECK(!Client_AnyActive(),"henuz musteri yok -> tablo asagi aktarilmaz");
  /* 2) flagged position from far node -> E1 queued + client */
  mkpos(b,FAR,5,POS_FLAG_RELAY_REQ); rx(b,12,-50);
  CHECK(Client_IsActive(FAR),"bayrakli konum -> musteri eklendi");
  advance(700); txCount=0; pump(); txdone();
  CHECK(sent_type()==0xE1 && lastTxLen==17,"0xE1 konum aktarildi (17 B)");
  /* 3) table again -> E3 table relayed */
  mktable(b,&n,myAddr,0,FAR,POS_FLAG_VIA_RELAY|POS_FLAG_RELAY_REQ); b[5]=8; rx(b,n,-40);
  advance(400); pump(); txdone();
  CHECK(sent_type()==0xE3 && lastTxLen==3+26 && lastTx[3]==0xD1,"tablo 0xE3 ile asagi aktarildi");
  /* same table again (dup) -> not relayed twice */
  int before=txCount; rx(b,n,-40); advance(400); pump(); txdone();
  CHECK(txCount==before,"ayni tablo ikinci kez aktarilmadi");
  /* 4) admin C1 to far (from gateway, fresh) -> E3 ; duplicates not */
  mkcmd(b,0xC1,0xFFFE,FAR,0x22,7); rx(b,15,-40); advance(400); pump(); txdone();
  CHECK(sent_type()==0xE3 && lastTx[3]==0xC1 && lastTxLen==18,"admin komutu 0xE3 ile asagi aktarildi");
  before=txCount; rx(b,15,-40); rx(b,15,-40); advance(400); pump(); txdone();
  CHECK(txCount==before,"gateway'in 2. ve 3. kopyasi tekrar aktarilmadi");
  advance(5000); mkcmd(b,0xC1,0xFFFE,FAR,0x22,7); rx(b,15,-40); advance(400); pump(); txdone();
  CHECK(sent_type()==0xE3 && lastTx[3]==0xC1 && lastTx[8]==0x22,"Zybo yeniden deneyince komut tekrar asagi aktarildi");
  { int dl=0; rx(b,15,-40); for(int k=0;k<8;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xE3 && lastTx[3]==0xC1 && lastTx[8]==0x22) dl++; printf("   (tx tip 0x%02X)\n", sent_type()); txdone(); } }
  CHECK(dl==0,"yeniden denemenin 2. kopyasi tekrar aktarilmadi"); }
  /* 5) far node's ACK C5 (fresh) -> E2 */
  mkcmd(b,0xC5,FAR,0xFFFE,0x00,1); b[7]=0xFE;b[8]=0xFF;b[9]=0x22; rx(b,15,-50); advance(700); pump(); txdone();
  CHECK(sent_type()==0xE2 && lastTxLen==20 && lastTx[5]==0xC5,"musterinin ACK'i 0xE2 ile yukari aktarildi");
  /* 6) far node's phone C1 (original from client) -> E2 up, NOT E3 down */
  mkcmd(b,0xC1,FAR,0xFFFF,0x40,2); rx(b,15,-50); advance(700); pump(); txdone();
  CHECK(sent_type()==0xE2 && lastTx[5]==0xC1,"musterinin komutu 0xE2 ile yukari aktarildi");
  before=txCount; advance(400); pump(); txdone();
  CHECK(txCount==before,"musterinin kendi komutunun ilk kopyasi asagi aktarilmadi (sahte ✓✓ yok)");
  /* gateway echo of same C1 (non-fresh) -> E3 down */
  rx(b,15,-40); advance(400); pump(); txdone();
  CHECK(sent_type()==0xE3 && lastTx[3]==0xC1 && lastTx[4]==(FAR&0xFF),"gateway'den donen kopya 0xE3 ile musteriye aktarildi");
  /* 7) suppression: another relayer's E2 for something we queued */
  mkcmd(b,0xC5,FAR,0xFFFE,0x01,2); rx(b,15,-50);   /* queue E2 */
  uint8_t e2[20]; e2[0]=0xE2; e2[1]=0x99; e2[2]=0x99; e2[3]=0xCE; e2[4]=5; memcpy(e2+5,b,15);
  rx(e2,20,-45); before=txCount; advance(700); pump(); txdone();
  CHECK(txCount==before,"baska aktaran ayni ACK'i aktarinca bizimki iptal edildi");
  /* 7b) far node's LEAVE A4 -> E2 up; admin KICK A3 for far node -> E3 down; retry later -> again */
  advance(1000); mkpos(b,FAR,9,POS_FLAG_RELAY_REQ); rx(b,12,-50); advance(700); pump(); txdone();  /* musteri tazele */
  { uint8_t a4[15]={0xA4,(uint8_t)(FAR&0xFF),(uint8_t)(FAR>>8),0xFE,0xFF,0x07,1,2,3,4,0,0,0,0,0};
    rx(a4,15,-50); int up=0; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xE2 && lastTx[5]==0xA4) up++; txdone(); } }
    CHECK(up==1,"musterinin A4 LEAVE'i 0xE2 ile yukari aktarildi");
    uint8_t a3[15]={0xA3,0xFE,0xFF,(uint8_t)(FAR&0xFF),(uint8_t)(FAR>>8),0x33,1,2,3,4,1,0,0,0,0};
    rx(a3,15,-40); int dn=0; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xE3 && lastTx[3]==0xA3) dn++; txdone(); } }
    CHECK(dn==1,"admin'in A3 KICK'i 0xE3 ile musteriye asagi aktarildi");
    rx(a3,15,-40); dn=0; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xE3 && lastTx[3]==0xA3) dn++; txdone(); } }
    CHECK(dn==0,"ayni A3'un gateway tekrari ikinci kez aktarilmadi");
    advance(5000); rx(a3,15,-40); dn=0; for(int k=0;k<10;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xE3 && lastTx[3]==0xA3) dn++; txdone(); } }
    CHECK(dn==1,"Zybo A3'u 5 sn sonra yeniden deneyince tekrar asagi aktarildi"); }
  /* 8) client expires after 30 s -> no downlink relay */
  advance(31000);
  mktable(b,&n,myAddr,0,0x1111,0); b[4]=33; rx(b,n,-40); before=txCount; advance(400); pump(); txdone();
  CHECK(txCount==before || sent_type()!=0xE3,"musteri 30 sn susunca tablo aktarimi durdu");
#else
  /* FAR NODE (TEST_FAR_NODE=1) */
  uint16_t REL=0x157C;
  advance(26000); 
  CHECK(Node_NeedsRelay(),"25 sn tablo gormedi -> aktarim modu");
  /* original table from gateway ignored by test mode */
  mktable(b,&n,REL,0,myAddr,0); int uc=uartCount; rx(b,n,-40);
  CHECK(Node_NeedsRelay(),"orijinal tablo (test modu) yok sayildi");
  /* E3 table with my record via relay */
  uint8_t e3[64]; e3[0]=0xE3; e3[1]=REL&0xFF; e3[2]=REL>>8; mktable(e3+3,&n,REL,0,myAddr,POS_FLAG_VIA_RELAY|POS_FLAG_RELAY_REQ);
  uc=uartCount; rx(e3,3+n,-45);
  CHECK(uartCount>uc,"0xE3 tablo acildi ve telefona (ESP32) iletildi");
  CHECK(Node_NeedsRelay(),"kaydim bit2 ile -> aktarim modunda kaldim");
  /* E3 admin cmd to me -> ACK delivered queued and sent as C5 */
  mkcmd(e3+3,0xC1,0xFFFE,myAddr,0x22,7); txCount=0; rx(e3,18,-45);
  int acks=0; uint8_t seqs[8];
  for(int k=0;k<40;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xC5 && lastTx[9]==0x22 && lastTx[6]==1){ seqs[acks++]=lastTx[5]; } txdone(); } }
  CHECK(acks==3,"aktarilmis admin komutuna ULASTI ACK'i 3 kez yollandi");
  CHECK(acks==3 && seqs[0]!=seqs[1] && seqs[1]!=seqs[2],"her tekrarda yeni ack_seq");
  /* same command copy again within repeat window -> no extra ACK series */
  mkcmd(e3+3,0xC1,0xFFFE,myAddr,0x33,5); rx(e3,18,-45); advance(300); pump(); txdone();
  rx(e3,18,-45); /* 2. kopya (tekrar) */
  acks=0; for(int k=0;k<40;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xC5 && lastTx[9]==0x33) acks++; txdone(); } }
  CHECK(acks==2,"ilk ACK'ten sonra gelen kopya yeni ACK serisi baslatmadi (toplam 3)");
  /* Zybo retry later -> new ACK series */
  advance(5000); rx(e3,18,-45);
  acks=0; for(int k=0;k<40;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xC5 && lastTx[9]==0x33) acks++; txdone(); } }
  CHECK(acks==3,"Zybo yeniden deneyince yeni ULASTI serisi (3) yollandi");
  /* SEEN from phone -> 3 times */
  memset(dlFrame,0,15); dlFrame[0]=0xC5; dlFrame[6]=2; dlFrame[7]=0xFE; dlFrame[8]=0xFF; dlFrame[9]=0x33; dlFrameLen=15; dlFrameReady=true;
  Node_HandleUartFrame();
  acks=0; for(int k=0;k<40;k++){ advance(100); pump(); if(txBusy){ if(sent_type()==0xC5 && lastTx[6]==2) acks++; txdone(); } }
  CHECK(acks==3,"telefondan OKUNDU 3 kez yollandi");
  /* position packet has relay flag */
  BuildPositionPacket(&txPacket,0);
  CHECK(txPacket.flags & POS_FLAG_RELAY_REQ,"konum paketinde 'beni aktarin' bayragi var");
  /* table with my record WITHOUT bit2 via E3 -> still relay? (direct copy) */
  e3[0]=0xE3; mktable(e3+3,&n,REL,0,myAddr,0); e3[3+2+12+2]=99; rx(e3,3+n,-45);
  CHECK(!Node_NeedsRelay(),"kaydim bit2'siz (dogrudan ulasmis) -> aktarim modundan cikti");
  /* now not needing relay: further E3 ignored */
  uc=uartCount; e3[3+2+12+2]=100; rx(e3,3+n,-45);
  CHECK(uartCount==uc,"dogrudan ulasirken 0xE3 yok sayildi");
#endif
  printf(fails? "\n%d HATA\n" : "\nHEPSI GECTI\n", fails);
  return fails;
}
