
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

#define WHITE_BACK_RANK_ROW             0    // Row 0 = rank 8 (White promotes here)
#define BLACK_STARTING_PAWN_ROW         1    // Row 1 = rank 7
#define WHITE_STARTING_PAWN_ROW         6    // Row 6 = rank 2
#define BLACK_BACK_RANK_ROW             7    // Row 7 = rank 1 (Black promotes here)

#define CENTER_BOARD_MIN_INDEX          2    // 4x4 centre
#define CENTER_BOARD_MAX_INDEX          5

#define FIFTY_MOVE_RULE_HALFMOVE_LIMIT  100  // 50 full turns

// Computer opponent scoring
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

#define CMD_LOAD_PREFIX_LENGTH          5    // "LOAD:"
#define CMD_LOAD_TOTAL_LENGTH           71   // 5 + 64 + 1 + 1
#define CMD_LOAD_COLON_INDEX            69
#define CMD_LOAD_TURN_INDEX             70

#define CMD_MOVES_PREFIX_LENGTH         6    // "MOVES:"
#define SQUARE_COORDINATE_TEXT_LENGTH   2    // "e2"

#define CMD_MOVE_PREFIX_LENGTH          5    // "MOVE:"
#define MOVE_COORDINATE_TEXT_LENGTH     4    // "e2e4"

/* ============================================================================
 * 3. HARDWARE REGISTER & BIT-MASK MACROS (STM32F0)
 * ============================================================================ */
// Clock enable
#define RCC_AHBENR_GPIOA_CLOCK_EN       (1U << 17)
#define RCC_AHBENR_GPIOB_CLOCK_EN       (1U << 18)
#define RCC_APB2ENR_USART1_CLOCK_EN     (1U << 14)

// Port B LEDs (PB0 = White turn, PB1 = Black turn)
#define LED_WHITE_TURN_PIN_MASK         (1U << 0)
#define LED_BLACK_TURN_PIN_MASK         (1U << 1)
#define LED_BOTH_PINS_MASK              (LED_WHITE_TURN_PIN_MASK | LED_BLACK_TURN_PIN_MASK)

#define GPIOB_MODER_PB0_PB1_CLEAR_MASK  ((3U << 0) | (3U << 2))
#define GPIOB_MODER_PB0_PB1_OUTPUT_MODE ((1U << 0) | (1U << 2))

// Port A buttons (SW0..SW3 on PA0..PA3, active-low, internal pull-up)
#define RESET_BUTTON_PIN_MASK           (1U << 0)   // legacy alias for SW0

#define GPIOA_MODER_PA0_CLEAR_MASK      (3U << 0)
#define GPIOA_PUPDR_PA0_CLEAR_MASK      (3U << 0)
#define GPIOA_PUPDR_PA0_PULLUP_ENABLE   (1U << 0)

// Port A UART (PA9 = TX, PA10 = RX on AF1)
#define GPIOA_MODER_PA9_PA10_CLEAR_MASK ((3U << 18) | (3U << 20))
#define GPIOA_MODER_PA9_PA10_ALT_MODE   ((2U << 18) | (2U << 20))
#define GPIOA_AFRH_PA9_PA10_CLEAR_MASK  ((0xFU << 4) | (0xFU << 8))
#define GPIOA_AFRH_PA9_PA10_AF1_USART1  ((1U << 4) | (1U << 8))

// USART1 (8 MHz / 115200 baud ≈ 69)
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

// SysTick (1 ms tick at 8 MHz = 8000 cycles)
#define SYSTICK_1MS_RELOAD_AT_8MHZ      (8000U - 1U)
#define SYSTICK_CSR_ENABLE              (1U << 0)
#define SYSTICK_CSR_TICKINT_ENABLE      (1U << 1)
#define SYSTICK_CSR_CLKSOURCE_CPU       (1U << 2)

/* ============================================================================
 * 4. USER BUTTONS (SW0..SW3, PA0..PA3, active-low, internal pull-up)
 *    SW0 = soft reset (preserves current AI/HvH mode) -- legacy reset button.
 *    SW1 = force human vs human game
 *    SW2 = force human vs AI game
 *    SW3 = resign current game (mid-game only)
 * ============================================================================ */
#define BTN_SW0_PIN_MASK                (1U << 0)
#define BTN_SW1_PIN_MASK                (1U << 1)
#define BTN_SW2_PIN_MASK                (1U << 2)
#define BTN_SW3_PIN_MASK                (1U << 3)

// Pull-up config for PA1..PA3 (PA0 already handled by GPIOA_PUPDR_PA0_* above)
#define GPIOA_PUPDR_PA1_PA3_CLEAR_MASK  ((3U << 2) | (3U << 4) | (3U << 6))
#define GPIOA_PUPDR_PA1_PA3_PULLUP      ((1U << 2) | (1U << 4) | (1U << 6))

#endif // MAIN_H#endif // MAIN_H
