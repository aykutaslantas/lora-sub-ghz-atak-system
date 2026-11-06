#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define CFG_SEQ_Prio_0 0
#define CFG_SEQ_Task_SubGHz_Phy_App_Process 1
typedef enum {GPIO_PIN_RESET=0, GPIO_PIN_SET} GPIO_PinState;
#define HAL_OK 0
typedef struct {int x;} GPIO_TypeDef;
extern GPIO_TypeDef *LED2_GPIO_Port, *LED3_GPIO_Port;
#define LED2_Pin 1
#define LED3_Pin 2
#define MODEM_LORA 1
#define SUBGHZ_PHY_VERSION_MAIN 1
#define SUBGHZ_PHY_VERSION_SUB1 5
#define SUBGHZ_PHY_VERSION_SUB2 0
#define TS_OFF 0
#define TS_ON 1
#define VLEVEL_L 1
#define VLEVEL_M 2
#define APP_LOG(ts, lvl, ...) printf(__VA_ARGS__)
int printf(const char*, ...);
#define UART_IT_RXNE 1
typedef struct { volatile uint32_t ISR, ICR, RDR; } USART_TypeDef;
extern USART_TypeDef *USART1;
#define USART1_IRQn 37
#define USART_ICR_FECF 1
#define USART_ICR_NECF 2
#define USART_ICR_ORECF 4
#define USART_ICR_PECF 8
#define USART_ISR_FE 1
#define USART_ISR_NE 2
#define USART_ISR_ORE 4
#define USART_ISR_PE 8
#define USART_ISR_RXNE_RXFNE 16
#define UTIL_SEQ_RFU 0
typedef enum {UTIL_TIMER_ONESHOT=0, UTIL_TIMER_PERIODIC} UTIL_TIMER_Mode_t;
typedef struct {int x;} UTIL_TIMER_Object_t;
typedef uint32_t UTIL_TIMER_Time_t;
int UTIL_TIMER_Create(UTIL_TIMER_Object_t*, uint32_t, UTIL_TIMER_Mode_t, void (*)(void*), void*);
int UTIL_TIMER_SetPeriod(UTIL_TIMER_Object_t*, uint32_t);
int UTIL_TIMER_Start(UTIL_TIMER_Object_t*);
int UTIL_TIMER_Stop(UTIL_TIMER_Object_t*);
UTIL_TIMER_Time_t UTIL_TIMER_GetCurrentTime(void);
void UTIL_SEQ_RegTask(uint32_t, uint32_t, void (*)(void));
void UTIL_SEQ_SetTask(uint32_t, uint32_t);
typedef struct { int x; } UART_HandleTypeDef;
extern UART_HandleTypeDef huart1;
int HAL_UART_Transmit(UART_HandleTypeDef*, const uint8_t*, uint16_t, uint32_t);
#define __HAL_UART_ENABLE_IT(h, it) ((void)(h))
void HAL_GPIO_WritePin(GPIO_TypeDef*, uint16_t, GPIO_PinState);
uint32_t HAL_GetUIDw0(void); uint32_t HAL_GetUIDw1(void); uint32_t HAL_GetUIDw2(void);
void HAL_NVIC_SetPriority(int, uint32_t, uint32_t); void HAL_NVIC_EnableIRQ(int);
void __disable_irq(void); uint32_t __get_PRIMASK(void); void __set_PRIMASK(uint32_t);
typedef struct {
  void (*TxDone)(void); void (*TxTimeout)(void);
  void (*RxDone)(uint8_t*, uint16_t, int16_t, int8_t);
  void (*RxTimeout)(void); void (*RxError)(void);
} RadioEvents_t;
struct Radio_s {
  void (*Init)(RadioEvents_t*); void (*Sleep)(void); void (*SetChannel)(uint32_t);
  void (*SetTxConfig)(int, int8_t, uint32_t, uint32_t, uint32_t, uint8_t, uint16_t, bool, bool, bool, uint8_t, bool, uint32_t);
  void (*SetRxConfig)(int, uint32_t, uint32_t, uint8_t, uint32_t, uint16_t, uint16_t, bool, uint8_t, bool, bool, uint8_t, bool, bool);
  void (*SetMaxPayloadLength)(int, uint8_t); void (*Send)(uint8_t*, uint8_t); void (*Rx)(uint32_t);
  uint32_t (*Random)(void);
};
extern const struct Radio_s Radio;
typedef int HAL_StatusTypeDef;
typedef struct { uint32_t TypeErase; uint32_t Page; uint32_t NbPages; } FLASH_EraseInitTypeDef;
#define FLASH_TYPEERASE_PAGES 2
#define FLASH_TYPEPROGRAM_DOUBLEWORD 1
HAL_StatusTypeDef HAL_FLASH_Unlock(void); HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef*, uint32_t*);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t, uint32_t, uint64_t);
