#include <string.h>
#include <stdbool.h>
#include <main.h>


/* --- Server States --- */
typedef enum {
    STATE_IDLE = 0,
    STATE_IN_PROGRESS,
    STATE_GAME_OVER
} ServerState_t;

static ServerState_t server_state = STATE_IDLE;
static char current_turn = 'W'; // 'W' = White, 'B' = Black

/* 64-char board: index 0 = a8 (top-left), index 63 = h1 (bottom-right)
 * Lowercase = Black pieces, Uppercase = White pieces, '.' = Empty square */
static const char DEFAULT_BOARD[65] =
    "rnbqkbnr"
    "pppppppp"
    "........"
    "........"
    "........"
    "........"
    "PPPPPPPP"
    "RNBQKBNR";

static char board[65];

/* --- UART Transmit Helpers --- */
static void uart_send_char(char c) {
    while (!(USART1_ISR & (1U << 7))); // Wait for TXE
    USART1_TDR = (uint8_t)c;
}

static void uart_send_str(const char *s) {
    while (*s) {
        uart_send_char(*s++);
    }
}

static void send_board_response(const char *prefix) {
    uart_send_str(prefix);
    uart_send_str(board);
    uart_send_str(":TURN:");
    uart_send_char(current_turn);
    uart_send_str("\n");
}

/* --- Minimal Command Handler for GUI Rendering Tests --- */
static void handle_command(const char *cmd) {
    if (strcmp(cmd, "CONNECT") == 0) {
        server_state = STATE_IDLE;
        GPIOB_ODR &= ~(1U << 0); // LED0 OFF in IDLE
        uart_send_str("OK\n");
    }
    else if (strcmp(cmd, "NEW") == 0) {
        memcpy(board, DEFAULT_BOARD, 65);
        current_turn = 'W';
        server_state = STATE_IN_PROGRESS;
        GPIOB_ODR |= (1U << 0);  // LED0 ON when game is active
        send_board_response("OK:BOARD:");
    }
    else if (strcmp(cmd, "STATE") == 0) {
        if (server_state == STATE_IDLE) {
            uart_send_str("NACK:NOGAME\n");
        } else {
            send_board_response("BOARD:");
        }
    }
    else if (strcmp(cmd, "DISCONNECT") == 0) {
        server_state = STATE_IDLE;
        GPIOB_ODR &= ~(1U << 0);
        uart_send_str("OK\n");
    }
    else {
        uart_send_str("NACK:BADCMD\n");
    }
}

int main(void) {
    /* 1. Enable Clocks: GPIOA, GPIOB, USART1 */
    RCC_AHBENR  |= (1U << 17) | (1U << 18);
    RCC_APB2ENR |= (1U << 14);

    /* 2. Set PB0 as Output (LED0) */
    GPIOB_MODER &= ~(3U << 0);
    GPIOB_MODER |=  (1U << 0);

    /* 3. Set PA9 (TX) & PA10 (RX) to Alternate Function 1 (USART1) */
    GPIOA_MODER &= ~((3U << 18) | (3U << 20));
    GPIOA_MODER |=  ((2U << 18) | (2U << 20));
    GPIOA_AFRH  &= ~((0xFU << 4) | (0xFU << 8));
    GPIOA_AFRH  |=  ((1U << 4)   | (1U << 8));

    /* 4. Configure USART1: 115200 8N1 @ 8 MHz HSI */
    USART1_BRR = 69;
    USART1_CR1 = (1U << 3) | (1U << 2) | (1U << 0); // TE | RE | UE

    memcpy(board, DEFAULT_BOARD, 65);

    char rx_buf[64];
    uint8_t rx_idx = 0;

    while (1) {
        /* Clear any UART hardware overrun/framing errors */
        if (USART1_ISR & ((1U << 3) | (1U << 1) | (1U << 2))) {
            USART1_ICR = (1U << 3) | (1U << 1) | (1U << 2);
        }

        /* Read incoming byte */
        if (USART1_ISR & (1U << 5)) { // RXNE
            char c = (char)(USART1_RDR & 0xFF);

            if (c == '\r') {
                continue; // Ignore carriage return
            } else if (c == '\n') {
                if (rx_idx > 0) {
                    rx_buf[rx_idx] = '\0';
                    handle_command(rx_buf);
                    rx_idx = 0;
                }
            } else if (rx_idx < sizeof(rx_buf) - 1) {
                rx_buf[rx_idx++] = c;
            }
        }
    }
}