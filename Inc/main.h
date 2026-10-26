#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

// STM32F051 Direct Register Addresses 
#define RCC_AHBENR    (*(volatile uint32_t *)0x40021014)
#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018)

#define GPIOA_MODER   (*(volatile uint32_t *)0x48000000)
#define GPIOA_PUPDR   (*(volatile uint32_t *)0x4800000C)
#define GPIOA_IDR     (*(volatile uint32_t *)0x48000010)
#define GPIOA_AFRH    (*(volatile uint32_t *)0x48000024)

#define GPIOB_MODER   (*(volatile uint32_t *)0x48000400)
#define GPIOB_ODR     (*(volatile uint32_t *)0x48000414)

#define USART1_CR1    (*(volatile uint32_t *)0x40013800)
#define USART1_BRR    (*(volatile uint32_t *)0x4001380C)
#define USART1_ISR    (*(volatile uint32_t *)0x4001381C)
#define USART1_RDR    (*(volatile uint32_t *)0x40013824)
#define USART1_TDR    (*(volatile uint32_t *)0x40013828)
#define USART1_ICR    (*(volatile uint32_t *)0x40013820)

// Cortex-M0 SysTick Registers (1 ms hardware tick) 
#define SYST_CSR      (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR      (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR      (*(volatile uint32_t *)0xE000E018)


/* ============================================================================
 * 1. CHESS BOARD & GAME RULE CONSTANTS
 * ============================================================================ */

#define BOARD_SIZE                      8
#define TOTAL_BOARD_SQUARES             64
#define BOARD_STRING_LENGTH             65   // 64 squares + null terminator

#define WHITE_BACK_RANK_ROW             0    // Row 0 corresponds to rank 8 (Pawn promotion for White)
#define BLACK_STARTING_PAWN_ROW         1    // Row 1 corresponds to rank 7
#define WHITE_STARTING_PAWN_ROW         6    // Row 6 corresponds to rank 2
#define BLACK_BACK_RANK_ROW             7    // Row 7 corresponds to rank 1 (Pawn promotion for Black)

#define CENTER_BOARD_MIN_INDEX          2    // Rows/Columns 2..5 form the 4x4 center of the board
#define CENTER_BOARD_MAX_INDEX          5

#define FIFTY_MOVE_RULE_HALFMOVE_LIMIT  100  // 50 full turns = 100 half-moves

// Computer Opponent Scoring Constants
#define AI_INITIAL_LOWEST_SCORE         (-100)
#define AI_CAPTURE_SCORE_MULTIPLIER     10
#define AI_CENTER_SQUARE_BONUS          1

#define PAWN_VALUE                      1
#define KNIGHT_VALUE                    3
#define BISHOP_VALUE                    3
#define ROOK_VALUE                      5
#define QUEEN_VALUE                     9

/* ============================================================================
 * 2. PROTOCOL & TIMING CONSTANTS
 * ============================================================================ */
#define LED_BLINK_INTERVAL_MS           250
#define BUTTON_DEBOUNCE_MS              50
#define UART_RX_TIMEOUT_MS              500

#define UART_RX_BUFFER_SIZE             80
#define UART_MAX_COMMAND_LENGTH         75

#define CMD_LOAD_PREFIX_LENGTH          5    // Length of "LOAD:"
#define CMD_LOAD_TOTAL_LENGTH           71   // "LOAD:" (5) + 64 squares + ":" (1) + Turn (1)
#define CMD_LOAD_COLON_INDEX            69
#define CMD_LOAD_TURN_INDEX             70

#define CMD_MOVES_PREFIX_LENGTH         6    // Length of "MOVES:"
#define SQUARE_COORDINATE_TEXT_LENGTH   2    // e.g., "e2"

#define CMD_MOVE_PREFIX_LENGTH          5    // Length of "MOVE:"
#define MOVE_COORDINATE_TEXT_LENGTH     4    // e.g., "e2e4"

/* ============================================================================
 * 3. HARDWARE REGISTER & BIT-MASK MACROS (STM32F0)
 * ============================================================================ */
// Clock Enable Bits
#define RCC_AHBENR_GPIOA_CLOCK_EN       (1U << 17)
#define RCC_AHBENR_GPIOB_CLOCK_EN       (1U << 18)
#define RCC_APB2ENR_USART1_CLOCK_EN     (1U << 14)

// Port B LEDs (PB0 = White Turn LED, PB1 = Black Turn LED)
#define LED_WHITE_TURN_PIN_MASK         (1U << 0)
#define LED_BLACK_TURN_PIN_MASK         (1U << 1)
#define LED_BOTH_PINS_MASK              (LED_WHITE_TURN_PIN_MASK | LED_BLACK_TURN_PIN_MASK)

#define GPIOB_MODER_PB0_PB1_CLEAR_MASK  ((3U << 0) | (3U << 2))
#define GPIOB_MODER_PB0_PB1_OUTPUT_MODE ((1U << 0) | (1U << 2))

// Port A Reset Button (PA0 = Active-Low Input with Pull-Up)
#define RESET_BUTTON_PIN_MASK           (1U << 0)
#define GPIOA_MODER_PA0_CLEAR_MASK      (3U << 0)
#define GPIOA_PUPDR_PA0_CLEAR_MASK      (3U << 0)
#define GPIOA_PUPDR_PA0_PULLUP_ENABLE   (1U << 0)

// Port A UART Pins (PA9 = TX, PA10 = RX on Alternate Function 1)
#define GPIOA_MODER_PA9_PA10_CLEAR_MASK ((3U << 18) | (3U << 20))
#define GPIOA_MODER_PA9_PA10_ALT_MODE   ((2U << 18) | (2U << 20))
#define GPIOA_AFRH_PA9_PA10_CLEAR_MASK  ((0xFU << 4) | (0xFU << 8))
#define GPIOA_AFRH_PA9_PA10_AF1_USART1  ((1U << 4) | (1U << 8))

// USART1 Control, Status, and Baud Rate (8 MHz / 115200 baud ~= 69)
#define USART1_BAUD_115200_AT_8MHZ      69
#define USART1_CR1_ENABLE               (1U << 0)
#define USART1_CR1_RX_ENABLE            (1U << 2)
#define USART1_CR1_TX_ENABLE            (1U << 3)

#define USART1_ISR_PARITY_ERROR         (1U << 0)
#define USART1_ISR_FRAMING_ERROR        (1U << 1)
#define USART1_ISR_NOISE_ERROR          (1U << 2)
#define USART1_ISR_OVERRUN_ERROR        (1U << 3)
#define USART1_ISR_RX_NOT_EMPTY         (1U << 5)
#define USART1_ISR_TX_EMPTY             (1U << 7)
#define USART1_ERROR_FLAGS_MASK         (USART1_ISR_OVERRUN_ERROR | USART1_ISR_NOISE_ERROR | USART1_ISR_FRAMING_ERROR)
#define USART1_RDR_BYTE_MASK            0xFFU

// SysTick Timer (1 ms tick at 8 MHz CPU clock = 8000 cycles)
#define SYSTICK_1MS_RELOAD_AT_8MHZ      (8000U - 1U)
#define SYSTICK_CSR_ENABLE              (1U << 0)
#define SYSTICK_CSR_TICKINT_ENABLE      (1U << 1)
#define SYSTICK_CSR_CLKSOURCE_CPU       (1U << 2)

#endif // MAIN_H