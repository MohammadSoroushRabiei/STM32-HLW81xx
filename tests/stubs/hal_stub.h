/* Minimal host-side stand-in for the STM32 HAL (tests only). */
#ifndef HAL_STUB_H
#define HAL_STUB_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { volatile uint32_t RDR; } USART_TypeDef;
typedef struct { USART_TypeDef *Instance; } UART_HandleTypeDef;

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t);
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *h, uint8_t *p, uint16_t n);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *h);
void HAL_Delay(uint32_t ms);

#define __HAL_UART_CLEAR_OREFLAG(h) ((void)(h))
#define __HAL_UART_CLEAR_FEFLAG(h)  ((void)(h))
#define __HAL_UART_CLEAR_NEFLAG(h)  ((void)(h))
#define __HAL_UART_CLEAR_PEFLAG(h)  ((void)(h))
#endif
