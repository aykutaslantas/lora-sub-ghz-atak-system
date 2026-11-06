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
static uint8_t secFlash[2048];
static int secErases=0;
#define ID_FLASH_READ_PTR ((const void*)fakeFlash)
#define SEC_FLASH_READ_PTR ((const void*)secFlash)

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
static int uartType[256];
int HAL_UART_Transmit(UART_HandleTypeDef*h,const uint8_t*d,uint16_t n,uint32_t t){memcpy(lastUart,d,n);lastUartLen=n;uartCount++; if(n>3) uartType[d[3]]++; return 0;}
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
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef*e,uint32_t*err){ if(e->Page==127) memset(fakeFlash,0xFF,sizeof fakeFlash); if(e->Page==126){memset(secFlash,0xFF,sizeof secFlash);secErases++;} return 0;}
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t t,uint32_t a,uint64_t d){
  uint8_t *base; uint32_t off;
  if(a>=0x0803F800U){base=fakeFlash;off=a-0x0803F800U;} else if(a>=0x0803F000U){base=secFlash;off=a-0x0803F000U;} else return 1;
  if(off+8>2048) return 1;
  for(int i=0;i<8;i++) if(base[off+i]!=0xFF) return 1;   /* gercek flash gibi: bos olmayan yere yazilamaz */
  memcpy(base+off,&d,8); return 0;}
#include "../SubGHz_Phy/App/subghz_phy_app.c"
