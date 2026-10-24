#include <stdint.h>

/* Direct Register Base Addresses for STM32F051 */
#define RCC_AHBENR    (*(volatile uint32_t *)0x40021014)
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018)

#define GPIOA_MODER   (*(volatile uint32_t *)0x48000000)
#define GPIOA_AFRH    (*(volatile uint32_t *)0x48000024)

#define GPIOB_MODER   (*(volatile uint32_t *)0x48000400)
#define GPIOB_ODR     (*(volatile uint32_t *)0x48000414)

#define USART1_CR1    (*(volatile uint32_t *)0x40013800)
#define USART1_BRR    (*(volatile uint32_t *)0x4001380C)
#define USART1_ISR    (*(volatile uint32_t *)0x4001381C)
#define USART1_TDR    (*(volatile uint32_t *)0x40013828)

static void uart_send_str(const char *s) {
    while (*s) {
        while (!(USART1_ISR & (1 << 7))); // Wait for TXE (Transmit Data Register Empty)
        USART1_TDR = (uint8_t)(*s++);
    }
}

int main(void) {
    /* 1. Enable GPIOA, GPIOB, and USART1 clocks */
    RCC_AHBENR  |= (1 << 17) | (1 << 18); // IOPAEN (bit 17), IOPBEN (bit 18)
    RCC_APB2ENR |= (1 << 14);             // USART1EN (bit 14)

    /* 2. Set PB0 as Output (LED0) */
    GPIOB_MODER &= ~(3U << 0);
    GPIOB_MODER |=  (1U << 0);

    /* 3. Set PA9 (TX) and PA10 (RX) to Alternate Function 1 (USART1) */
    GPIOA_MODER &= ~((3U << 18) | (3U << 20));
    GPIOA_MODER |=  ((2U << 18) | (2U << 20)); // AF mode
    GPIOA_AFRH  &= ~((0xFU << 4) | (0xFU << 8));
    GPIOA_AFRH  |=  ((1U << 4)   | (1U << 8)); // AF1 = USART1

    /* 4. Configure USART1 for 115200 8N1 (Default HSI clock = 8 MHz) */
    /* BRR = 8,000,000 / 115,200 = 69 (0x45) */
    USART1_BRR = 69;
    USART1_CR1 = (1 << 3) | (1 << 2) | (1 << 0); // TE | RE | UE

    while (1) {
        uart_send_str("Welcome to Binary\r\n");
        GPIOB_ODR ^= (1 << 0); // Toggle PB0 LED
        for (volatile int i = 0; i < 500000; i++); // Simple delay
    }
}
