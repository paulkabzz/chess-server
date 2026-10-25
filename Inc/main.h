#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

/* --- STM32F051 Direct Register Addresses --- */

#define RCC_AHBENR    (*(volatile uint32_t *)0x40021014)
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018)

#define GPIOA_MODER   (*(volatile uint32_t *)0x48000000)
#define GPIOA_AFRH    (*(volatile uint32_t *)0x48000024)

#define GPIOB_MODER   (*(volatile uint32_t *)0x48000400)
#define GPIOB_ODR     (*(volatile uint32_t *)0x48000414)

#define USART1_CR1    (*(volatile uint32_t *)0x40013800)
#define USART1_BRR    (*(volatile uint32_t *)0x4001380C)
#define USART1_ISR    (*(volatile uint32_t *)0x4001381C)
#define USART1_RDR    (*(volatile uint32_t *)0x40013824)
#define USART1_TDR    (*(volatile uint32_t *)0x40013828)
#define USART1_ICR    (*(volatile uint32_t *)0x40013820)

#endif // MAIN_H