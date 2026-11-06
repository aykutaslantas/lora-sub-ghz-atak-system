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
  /* GATEWAY: TEST_GW_IGNORE_DIRECT=0x0F53, GW_FORWARD_CMD=1, GW_FORWARD_RELAY_INFO=1 */
  int uc;
  /* direct ctrl from far node dropped */
  mkcmd(b,0xC5,0x0F53,0xFFFE,0,1); uc=uartCount; rx(b,15,-50);
  CHECK(uartCount==uc,"0x0F53'ten dogrudan ACK atildi (test modu)");
  /* E2 with that ACK -> forwarded to Zybo as 21-byte frame */
  uint8_t e2[20]; e2[0]=0xE2; e2[1]=0x7C; e2[2]=0x15; e2[3]=0xCF; e2[4]=13; memcpy(e2+5,b,15);
  uc=uartCount; rx(e2,20,-46);
  CHECK(uartCount==uc+1 && lastUartLen==21 && lastUart[2]==0x11 && lastUart[3]==0xC5,"0xE2 icindeki ACK Zybo'ya 21 B cerceveyle iletildi");
  /* duplicate E2 -> not forwarded again */
  uc=uartCount; rx(e2,20,-46);
  CHECK(uartCount==uc,"ayni ACK ikinci kez iletilmedi");
  /* E2 with far node phone C1 -> forwarded + relayed 3x */
  mkcmd(b,0xC1,0x0F53,0xFFFF,0x40,2); memcpy(e2+5,b,15); uc=uartCount; txCount=0; rx(e2,20,-46);
  CHECK(uartCount==uc+1 && lastUart[3]==0xC1,"0xE2 icindeki komut Zybo'ya iletildi");
  for(int k=0;k<4;k++){ advance(700); pump(); txdone(); }
  CHECK(txCount==3 && sent_type()==0xC1,"komut gateway tarafindan 3 kez yayinlandi");
  /* E1 position -> 23-byte relay frame with bit2 */
  uint8_t e1[17]; e1[0]=0xE1; e1[1]=0x7C; e1[2]=0x15; e1[3]=0xCF; e1[4]=13; mkpos(e1+5,0x0F53,5,POS_FLAG_RELAY_REQ);
  uc=uartCount; rx(e1,17,-46);
  CHECK(uartCount==uc+1 && lastUartLen==23 && lastUart[2]==0x13 && (lastUart[3+16]&0x04),"0xE1 konum 23 B cerceveyle, bit2 isaretli iletildi");
  /* E3 ignored by gateway */
  uint8_t e3[18]; e3[0]=0xE3; e3[1]=0x7C; e3[2]=0x15; mkcmd(e3+3,0xC1,0xFFFE,0x0F53,0x55,7); uc=uartCount; txCount=0; rx(e3,18,-46); advance(800); pump();
  CHECK(uartCount==uc && txCount==0,"gateway 0xE3'u yok saydi");
  printf(fails? "\n%d HATA\n" : "\nHEPSI GECTI\n", fails);
  return fails;
}
