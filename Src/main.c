#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* --- STM32F051 Direct Register Definitions --- */
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

/* Cortex-M0 SysTick Registers (1 ms hardware tick) */
#define SYST_CSR      (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR      (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR      (*(volatile uint32_t *)0xE000E018)

volatile uint32_t ms_ticks = 0;
void SysTick_Handler(void) {
    ms_ticks++;
}

/* --- Server State Machine & Move Status Codes --- */
typedef enum {
    STATE_IDLE = 0,
    STATE_IN_PROGRESS,
    STATE_GAME_OVER
} ServerState_t;

typedef enum {
    MOVE_OK = 0,
    ERR_RANGE,
    ERR_EMPTY,
    ERR_NOTURN,
    ERR_OCCUPIED,
    ERR_ILLEGAL
} MoveStatus_t;

typedef struct {
    ServerState_t state;
    char board[65];         // 64 squares + null terminator
    char turn;              // 'W' or 'B'
    uint16_t halfmove_clk;  // For 50-move draw rule
    bool ai_enabled;        // Stretch Bonus: STM32 plays Black ('B')
} ChessGame_t;

static ChessGame_t game;

static const char INIT_BOARD[65] =
    "rnbqkbnr"
    "pppppppp"
    "........"
    "........"
    "........"
    "........"
    "PPPPPPPP"
    "RNBQKBNR";

/* --- UART Transmit Helpers --- */
static void uart_send_char(char c) {
    while (!(USART1_ISR & (1U << 7)));
    USART1_TDR = (uint8_t)c;
}

static void uart_send_str(const char *s) {
    while (*s) uart_send_char(*s++);
}

static void send_board_state(const char *prefix) {
    uart_send_str(prefix);
    uart_send_str(game.board);
    uart_send_str(":TURN:");
    uart_send_char(game.turn);
    uart_send_str("\n");
}

/* --- Piece & Geometry Helpers --- */
static int iabs(int x) { return (x < 0) ? -x : x; }
static int isign(int x) { return (x > 0) - (x < 0); }

static bool is_white(char p) { return (p >= 'A' && p <= 'Z'); }
static bool is_black(char p) { return (p >= 'a' && p <= 'z'); }

static bool is_own_piece(char p, char turn) {
    return (turn == 'W') ? is_white(p) : is_black(p);
}

static bool is_enemy_piece(char p, char turn) {
    return (turn == 'W') ? is_black(p) : is_white(p);
}

static char to_upper(char p) {
    return (p >= 'a' && p <= 'z') ? (p - 'a' + 'A') : p;
}

static bool is_path_clear(const char *b, int r1, int c1, int r2, int c2) {
    int sr = isign(r2 - r1);
    int sc = isign(c2 - c1);
    int r = r1 + sr;
    int c = c1 + sc;
    while (r != r2 || c != c2) {
        if (b[r * 8 + c] != '.') return false;
        r += sr;
        c += sc;
    }
    return true;
}

/* Pseudo-legal piece movement geometry on board b */
static bool check_piece_geometry(const char *b, int src, int dst) {
    if (src == dst) return false;
    char p = b[src];
    char target = b[dst];
    char type = to_upper(p);
    bool white = is_white(p);

    int r1 = src / 8, c1 = src % 8;
    int r2 = dst / 8, c2 = dst % 8;
    int dr = r2 - r1, dc = c2 - c1;
    int adr = iabs(dr), adc = iabs(dc);

    switch (type) {
        case 'P': {
            int dir = white ? -1 : 1;
            int start_row = white ? 6 : 1;
            if (dc == 0 && dr == dir && target == '.') return true;
            if (dc == 0 && r1 == start_row && dr == 2 * dir &&
                target == '.' && b[(r1 + dir) * 8 + c1] == '.') return true;
            if (adc == 1 && dr == dir && target != '.' &&
                is_enemy_piece(target, white ? 'W' : 'B')) return true;
            return false;
        }
        case 'N':
            return (adr == 2 && adc == 1) || (adr == 1 && adc == 2);
        case 'K':
            return (adr <= 1 && adc <= 1);
        case 'R':
            if (r1 != r2 && c1 != c2) return false;
            return is_path_clear(b, r1, c1, r2, c2);
        case 'B':
            if (adr != adc) return false;
            return is_path_clear(b, r1, c1, r2, c2);
        case 'Q':
            if (r1 != r2 && c1 != c2 && adr != adc) return false;
            return is_path_clear(b, r1, c1, r2, c2);
        default:
            return false;
    }
}

static bool is_in_check(const char *b, char side) {
    char king_char = (side == 'W') ? 'K' : 'k';
    char enemy_side = (side == 'W') ? 'B' : 'W';
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (b[i] == king_char) { king_sq = i; break; }
    }
    if (king_sq < 0) return true;

    for (int i = 0; i < 64; i++) {
        if (is_own_piece(b[i], enemy_side)) {
            if (check_piece_geometry(b, i, king_sq)) return true;
        }
    }
    return false;
}

static void apply_move_on_buf(char *b, int src, int dst) {
    char p = b[src];
    b[dst] = p;
    b[src] = '.';
    /* Auto-promote Pawn to Queen on reaching back rank */
    if (p == 'P' && (dst / 8) == 0) b[dst] = 'Q';
    if (p == 'p' && (dst / 8) == 7) b[dst] = 'q';
}

/* Transactional Validator: never mutates g->board unless commit == true and MOVE_OK */
static MoveStatus_t validate_move(ChessGame_t *g, int src, int dst, bool commit) {
    if (src < 0 || src > 63 || dst < 0 || dst > 63) return ERR_RANGE;
    char piece = g->board[src];
    if (piece == '.') return ERR_EMPTY;
    if (!is_own_piece(piece, g->turn)) return ERR_NOTURN;
    char target = g->board[dst];
    if (target != '.' && is_own_piece(target, g->turn)) return ERR_OCCUPIED;
    if (!check_piece_geometry(g->board, src, dst)) return ERR_ILLEGAL;

    /* Simulate on temporary buffer to guarantee King safety */
    char temp[65];
    memcpy(temp, g->board, 65);
    apply_move_on_buf(temp, src, dst);
    if (is_in_check(temp, g->turn)) return ERR_ILLEGAL;

    if (commit) {
        if (to_upper(piece) == 'P' || target != '.') g->halfmove_clk = 0;
        else g->halfmove_clk++;
        memcpy(g->board, temp, 65);
        g->turn = (g->turn == 'W') ? 'B' : 'W';
    }
    return MOVE_OK;
}

static bool has_any_legal_move(ChessGame_t *g) {
    for (int src = 0; src < 64; src++) {
        if (is_own_piece(g->board[src], g->turn)) {
            for (int dst = 0; dst < 64; dst++) {
                if (validate_move(g, src, dst, false) == MOVE_OK) return true;
            }
        }
    }
    return false;
}

static bool is_insufficient_material(const char *b) {
    for (int i = 0; i < 64; i++) {
        char c = to_upper(b[i]);
        if (c != '.' && c != 'K') return false;
    }
    return true;
}

/* --- Bonus: 1-Ply Greedy AI for Black ('B') --- */
static int piece_value(char c) {
    switch (to_upper(c)) {
        case 'P': return 1;
        case 'N': case 'B': return 3;
        case 'R': return 5;
        case 'Q': return 9;
        default: return 0;
    }
}

static void play_ai_move(ChessGame_t *g) {
    int best_src = -1, best_dst = -1, best_score = -100;
    for (int src = 0; src < 64; src++) {
        if (is_own_piece(g->board[src], g->turn)) {
            for (int dst = 0; dst < 64; dst++) {
                if (validate_move(g, src, dst, false) == MOVE_OK) {
                    int score = piece_value(g->board[dst]) * 10;
                    int r = dst / 8, c = dst % 8;
                    if (r >= 2 && r <= 5 && c >= 2 && c <= 5) score += 1;
                    if (score > best_score) {
                        best_score = score;
                        best_src = src;
                        best_dst = dst;
                    }
                }
            }
        }
    }
    if (best_src >= 0) {
        validate_move(g, best_src, best_dst, true);
    }
}

static int parse_sq(const char *s) {
    if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return -1;
    int col = s[0] - 'a';
    int row = 8 - (s[1] - '0');
    return row * 8 + col;
}

static void reset_game(bool ai) {
    memcpy(game.board, INIT_BOARD, 65);
    game.turn = 'W';
    game.halfmove_clk = 0;
    game.ai_enabled = ai;
    game.state = STATE_IN_PROGRESS;
}

static void update_leds(void) {
    if (game.state == STATE_IDLE) {
        GPIOB_ODR &= ~((1U << 0) | (1U << 1));
    } else if (game.state == STATE_IN_PROGRESS) {
        if (game.turn == 'W') {
            GPIOB_ODR |=  (1U << 0); // PB0 ON for White
            GPIOB_ODR &= ~(1U << 1);
        } else {
            GPIOB_ODR &= ~(1U << 0);
            GPIOB_ODR |=  (1U << 1); // PB1 ON for Black
        }
    } else {
        if ((ms_ticks / 250) % 2 == 0) GPIOB_ODR |=  ((1U << 0) | (1U << 1));
        else                           GPIOB_ODR &= ~((1U << 0) | (1U << 1));
    }
}

/* --- Command Dispatcher --- */
static void process_command(const char *cmd) {
    if (strcmp(cmd, "CONNECT") == 0) {
        game.state = STATE_IDLE;
        uart_send_str("OK\n");
        return;
    }
    if (strcmp(cmd, "DISCONNECT") == 0) {
        game.state = STATE_IDLE;
        uart_send_str("OK\n");
        return;
    }
    if (strcmp(cmd, "NEW") == 0 || strcmp(cmd, "NEW:AI") == 0) {
        reset_game(strcmp(cmd, "NEW:AI") == 0);
        send_board_state("OK:BOARD:");
        return;
    }
    /* Test helper: Load custom 64-char board + turn to test stalemate/draws rapidly */
    if (strncmp(cmd, "LOAD:", 5) == 0 && strlen(cmd) == 71 && cmd[69] == ':') {
        memcpy(game.board, cmd + 5, 64);
        game.board[64] = '\0';
        game.turn = cmd[70];
        game.state = STATE_IN_PROGRESS;
        send_board_state("OK:BOARD:");
        return;
    }
    if (strcmp(cmd, "STATE") == 0) {
        if (game.state == STATE_IDLE) uart_send_str("NACK:NOGAME\n");
        else send_board_state("BOARD:");
        return;
    }
    if (strcmp(cmd, "RESIGN") == 0) {
        if (game.state != STATE_IN_PROGRESS) {
            uart_send_str("NACK:NOGAME\n");
        } else {
            game.state = STATE_GAME_OVER;
            char winner = (game.turn == 'W') ? 'B' : 'W';
            uart_send_str("OK:GAMEOVER:WIN:");
            uart_send_char(winner);
            uart_send_str("\n");
        }
        return;
    }
    /* Server-side move highlighting query: MOVES:e2 -> VALID:e3,e4 */
    if (strncmp(cmd, "MOVES:", 6) == 0) {
        if (game.state != STATE_IN_PROGRESS) {
            uart_send_str("NACK:NOGAME\n");
            return;
        }
        int src = parse_sq(cmd + 6);
        if (src < 0 || strlen(cmd + 6) != 2) {
            uart_send_str("NACK:RANGE\n");
            return;
        }
        uart_send_str("VALID:");
        bool first = true;
        if (is_own_piece(game.board[src], game.turn)) {
            for (int dst = 0; dst < 64; dst++) {
                if (validate_move(&game, src, dst, false) == MOVE_OK) {
                    if (!first) uart_send_char(',');
                    uart_send_char('a' + (dst % 8));
                    uart_send_char('8' - (dst / 8));
                    first = false;
                }
            }
        }
        uart_send_str("\n");
        return;
    }
    /* MOVE:e2e4 */
    if (strncmp(cmd, "MOVE:", 5) == 0) {
        if (game.state != STATE_IN_PROGRESS) {
            uart_send_str("NACK:NOGAME\n");
            return;
        }
        const char *m = cmd + 5;
        if (strlen(m) != 4) {
            uart_send_str("NACK:RANGE\n");
            return;
        }
        int src = parse_sq(m);
        int dst = parse_sq(m + 2);
        if (src < 0 || dst < 0) {
            uart_send_str("NACK:RANGE\n");
            return;
        }

        MoveStatus_t st = validate_move(&game, src, dst, true);
        if (st == ERR_EMPTY)    { uart_send_str("NACK:EMPTY\n"); return; }
        if (st == ERR_NOTURN)   { uart_send_str("NACK:NOTURN\n"); return; }
        if (st == ERR_OCCUPIED) { uart_send_str("NACK:OCCUPIED\n"); return; }
        if (st == ERR_ILLEGAL)  { uart_send_str("NACK:ILLEGAL\n"); return; }

        /* Check if human move ended the game */
        if (!has_any_legal_move(&game)) {
            game.state = STATE_GAME_OVER;
            if (is_in_check(game.board, game.turn)) {
                char winner = (game.turn == 'W') ? 'B' : 'W';
                uart_send_str("WIN:");
                uart_send_char(winner);
                send_board_state(":BOARD:");
            } else {
                send_board_state("DRAW:BOARD:");
            }
            return;
        }
        if (is_insufficient_material(game.board) || game.halfmove_clk >= 100) {
            game.state = STATE_GAME_OVER;
            send_board_state("DRAW:BOARD:");
            return;
        }

        /* If AI mode is enabled and it is Black's turn, STM32 plays immediately */
        if (game.ai_enabled && game.turn == 'B') {
            play_ai_move(&game);
            if (!has_any_legal_move(&game)) {
                game.state = STATE_GAME_OVER;
                if (is_in_check(game.board, game.turn)) {
                    uart_send_str("WIN:B");
                    send_board_state(":BOARD:");
                } else {
                    send_board_state("DRAW:BOARD:");
                }
                return;
            }
        }

        send_board_state("OK:BOARD:");
        return;
    }

    uart_send_str("NACK:BADCMD\n");
}

int main(void) {
    RCC_AHBENR  |= (1U << 17) | (1U << 18);
    RCC_APB2ENR |= (1U << 14);

    GPIOB_MODER &= ~((3U << 0) | (3U << 2));
    GPIOB_MODER |=  ((1U << 0) | (1U << 2));

    GPIOA_MODER &= ~(3U << 0);
    GPIOA_PUPDR &= ~(3U << 0);
    GPIOA_PUPDR |=  (1U << 0);

    GPIOA_MODER &= ~((3U << 18) | (3U << 20));
    GPIOA_MODER |=  ((2U << 18) | (2U << 20));
    GPIOA_AFRH  &= ~((0xFU << 4) | (0xFU << 8));
    GPIOA_AFRH  |=  ((1U << 4)   | (1U << 8));

    USART1_BRR = 69;
    USART1_CR1 = (1U << 3) | (1U << 2) | (1U << 0);

    SYST_RVR = 8000 - 1;
    SYST_CVR = 0;
    SYST_CSR = (1U << 2) | (1U << 1) | (1U << 0);

    reset_game(false);
    game.state = STATE_IDLE;

    char rx_buf[80];
    uint8_t rx_idx = 0;
    bool overflow = false;
    uint32_t last_rx_time = 0;
    uint32_t last_btn_time = 0;
    bool btn_prev = true;

    while (1) {
        update_leds();

        /* PA0 Hardware Reset Button */
        bool btn_curr = (GPIOA_IDR & (1U << 0)) != 0;
        if (!btn_curr && btn_prev && (ms_ticks - last_btn_time > 50)) {
            last_btn_time = ms_ticks;
            reset_game(game.ai_enabled);
            send_board_state("OK:BOARD:");
        }
        btn_prev = btn_curr;

        if (USART1_ISR & ((1U << 3) | (1U << 1) | (1U << 2))) {
            USART1_ICR = (1U << 3) | (1U << 1) | (1U << 2);
        }

        if (USART1_ISR & (1U << 5)) {
            char c = (char)(USART1_RDR & 0xFF);
            last_rx_time = ms_ticks;

            if (c == '\r') {
                /* Ignore CR */
            } else if (c == '\n') {
                if (overflow || rx_idx == 0) {
                    uart_send_str("NACK:BADCMD\n");
                } else {
                    rx_buf[rx_idx] = '\0';
                    process_command(rx_buf);
                }
                rx_idx = 0;
                overflow = false;
            } else {
                if (rx_idx < 75) rx_buf[rx_idx++] = c;
                else overflow = true;
            }
        }

        if ((rx_idx > 0 || overflow) && (ms_ticks - last_rx_time > 500)) {
            rx_idx = 0;
            overflow = false;
            uart_send_str("NACK:TIMEOUT\n");
        }
    }
}
