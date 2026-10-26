
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <main.h>

volatile uint32_t millisecond_ticks = 0;

void SysTick_Handler(void) {
    millisecond_ticks++;
}

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
    char board[BOARD_STRING_LENGTH];
    char active_turn;               // 'W' or 'B'
    uint16_t halfmove_clock;
    bool computer_opponent_on;
} ChessGame_t;

static ChessGame_t current_game;

// --- User buttons (SW0..SW3 on PA0..PA3) --------------------------------
typedef struct {
    uint32_t pin_mask;
    bool     previous_state;
    uint32_t last_press_time;
} UserButton_t;

static UserButton_t user_buttons[] = {
    { BTN_SW0_PIN_MASK, true, 0 },   // SW0 -> soft reset (preserve mode)
    { BTN_SW1_PIN_MASK, true, 0 },   // SW1 -> new HvH
    { BTN_SW2_PIN_MASK, true, 0 },   // SW2 -> new vs AI
    { BTN_SW3_PIN_MASK, true, 0 },   // SW3 -> resign
};
#define NUM_USER_BUTTONS (sizeof(user_buttons) / sizeof(user_buttons[0]))

static const char INITIAL_BOARD[BOARD_STRING_LENGTH] =
    "rnbqkbnr"
    "pppppppp"
    "........"
    "........"
    "........"
    "........"
    "PPPPPPPP"
    "RNBQKBNR";


static void uart_send_character(char character) {
    while (!(USART1_ISR & USART1_ISR_TX_EMPTY));
    USART1_TDR = (uint8_t)character;
}

static void uart_send_string(const char *message) {
    while (*message) {
        uart_send_character(*message++);
    }
}

static void send_board_state(const char *prefix) {
    uart_send_string(prefix);
    uart_send_string(current_game.board);
    uart_send_string(":TURN:");
    uart_send_character(current_game.active_turn);
    uart_send_string("\n");
}


static int absolute_value(int number) {
    return (number < 0) ? -number : number;
}

static int get_step_direction(int difference) {
    return (difference > 0) - (difference < 0);
}

static bool is_white_piece(char piece) { 
    return (piece >= 'A' && piece <= 'Z'); 
}

static bool is_black_piece(char piece) { 
    return (piece >= 'a' && piece <= 'z'); 
}

static bool belongs_to_player(char piece, char turn) {
    return (turn == 'W') ? is_white_piece(piece) : is_black_piece(piece);
}
static bool is_opponent_piece(char piece, char turn) {
    return (turn == 'W') ? is_black_piece(piece) : is_white_piece(piece);
}
static char to_uppercase_piece(char piece) {
    return (piece >= 'a' && piece <= 'z') ? (piece - 'a' + 'A') : piece;
}

// Checks if all squares strictly between the start and destination are empty. /
static bool is_path_clear(const char *board, int start_row, int start_column, int end_row, int end_column) {
    int row_step = get_step_direction(end_row - start_row);
    int column_step = get_step_direction(end_column - start_column);
    int current_row = start_row + row_step;
    int current_column = start_column + column_step;

    while (current_row != end_row || current_column != end_column) {
        if (board[current_row * BOARD_SIZE + current_column] != '.') return false;
        current_row += row_step;
        current_column += column_step;
    }
    return true;
}

// Checks only how the piece moves (movement rules and clear paths).
   // Does NOT check whether the move leaves the mover's own king in check (see try_move for that).
static bool can_piece_move(const char *board, int from_square, int to_square) {
    if (from_square == to_square) return false;

    char moving_piece = board[from_square];
    char target_piece = board[to_square];
    char piece_type = to_uppercase_piece(moving_piece);
    bool is_white = is_white_piece(moving_piece);

    int start_row = from_square / BOARD_SIZE;
    int start_column = from_square % BOARD_SIZE;
    int end_row = to_square / BOARD_SIZE;
    int end_column = to_square % BOARD_SIZE;

    int row_difference = end_row - start_row;
    int column_difference = end_column - start_column;
    int row_distance = absolute_value(row_difference);
    int column_distance = absolute_value(column_difference);

    switch (piece_type) {
        case 'P': {
            int forward_direction = is_white ? -1 : 1;
            int starting_row = is_white ? WHITE_STARTING_PAWN_ROW : BLACK_STARTING_PAWN_ROW;

            // Standard 1-square forward move into an empty square
            if (column_difference == 0 && row_difference == forward_direction && target_piece == '.') {
                return true;
            }

            // Initial 2-square forward move from starting rank if both squares are empty
            if (column_difference == 0 && start_row == starting_row && row_difference == 2 * forward_direction && target_piece == '.' &&
                board[(start_row + forward_direction) * BOARD_SIZE + start_column] == '.') {
                return true;
            }

            // Diagonal 1-square capture of an opponent's piece
            if (column_distance == 1 && row_difference == forward_direction && target_piece != '.' && is_opponent_piece(target_piece, is_white ? 'W' : 'B')) {
                return true;
            }
            return false;
        }

        case 'N':
            return (row_distance == 2 && column_distance == 1) ||
                   (row_distance == 1 && column_distance == 2);
        case 'K':
            return (row_distance <= 1 && column_distance <= 1);
        case 'R':
            if (start_row != end_row && start_column != end_column) return false;
            return is_path_clear(board, start_row, start_column, end_row, end_column);
        case 'B':
            if (row_distance != column_distance) return false;
            return is_path_clear(board, start_row, start_column, end_row, end_column);
        case 'Q':
            if (start_row != end_row && start_column != end_column && row_distance != column_distance) {
                return false;
            }
            return is_path_clear(board, start_row, start_column, end_row, end_column);
        default:
            return false;
    }
}

// Checks if the given player's king can currently be captured by any enemy piece. 
static bool is_king_in_check(const char *board, char player_side) {
    char target_king = (player_side == 'W') ? 'K' : 'k';
    char opponent_side = (player_side == 'W') ? 'B' : 'W';
    int king_square = -1;

    for (int square = 0; square < TOTAL_BOARD_SQUARES; square++) {
        if (board[square] == target_king) { 
            king_square = square; break; 
        }
    }

    if (king_square < 0) return true;

    for (int square = 0; square < TOTAL_BOARD_SQUARES; square++) {
        if (belongs_to_player(board[square], opponent_side)) {
            if (can_piece_move(board, square, king_square)) {
                return true;
            }
        }
    }
    return false;
}

/// Moves a piece on a board array and automatically promotes pawns that reach the last rank to a Queen.
static void move_piece_on_board(char *board, int from_square, int to_square) {
    char moving_piece = board[from_square];
    board[to_square] = moving_piece;
    board[from_square] = '.';

    if (moving_piece == 'P' && (to_square / BOARD_SIZE) == WHITE_BACK_RANK_ROW) board[to_square] = 'Q';
    if (moving_piece == 'p' && (to_square / BOARD_SIZE) == BLACK_BACK_RANK_ROW) board[to_square] = 'q';
}


 // Validates a full move, including making sure the player's own king is not left in check.
  //  Only updates the actual game state if commit_move is true and the move is completely legal.
static MoveStatus_t try_move(ChessGame_t *game_instance, int from_square, int to_square, bool commit_move) {
    if (from_square < 0 || from_square >= TOTAL_BOARD_SQUARES ||
        to_square < 0 || to_square >= TOTAL_BOARD_SQUARES) {
        return ERR_RANGE;
    }

    char moving_piece = game_instance->board[from_square];
    if (moving_piece == '.') return ERR_EMPTY;
    if (!belongs_to_player(moving_piece, game_instance->active_turn)) return ERR_NOTURN;

    char target_piece = game_instance->board[to_square];
    if (target_piece != '.' && belongs_to_player(target_piece, game_instance->active_turn)) return ERR_OCCUPIED;
    if (!can_piece_move(game_instance->board, from_square, to_square)) return ERR_ILLEGAL;

    // Test the move on a temporary copy of the board to verify the king stays safe
    char test_board[BOARD_STRING_LENGTH];
    memcpy(test_board, game_instance->board, BOARD_STRING_LENGTH);
    move_piece_on_board(test_board, from_square, to_square);
    if (is_king_in_check(test_board, game_instance->active_turn)) return ERR_ILLEGAL;

    if (commit_move) {
        if (to_uppercase_piece(moving_piece) == 'P' || target_piece != '.') {
            game_instance->halfmove_clock = 0;
        } else {
            game_instance->halfmove_clock++;
        }
        memcpy(game_instance->board, test_board, BOARD_STRING_LENGTH);
        game_instance->active_turn = (game_instance->active_turn == 'W') ? 'B' : 'W';
    }
    return MOVE_OK;
}

// Returns true if the current player has at least one legal move available./
static bool player_has_legal_moves(ChessGame_t *game_instance) {
    for (int from_square = 0; from_square < TOTAL_BOARD_SQUARES; from_square++) {
        if (belongs_to_player(game_instance->board[from_square], game_instance->active_turn)) {
            for (int to_square = 0; to_square < TOTAL_BOARD_SQUARES; to_square++) {
                if (try_move(game_instance, from_square, to_square, false) == MOVE_OK) return true;
            }
        }
    }
    return false;
}

// If both players have only Kings left on the board, automatic darw
static bool has_only_kings_left(const char *board) {
    for (int square = 0; square < TOTAL_BOARD_SQUARES; square++) {
        char piece_type = to_uppercase_piece(board[square]);
        if (piece_type != '.' && piece_type != 'K') return false;
    }
    return true;
}

static int get_piece_point_value(char piece) {
    switch (to_uppercase_piece(piece)) {
        case 'P': return PAWN_VALUE;
        case 'N': return KNIGHT_VALUE;
        case 'B': return BISHOP_VALUE;
        case 'R': return ROOK_VALUE;
        case 'Q': return QUEEN_VALUE;
        default:  return 0;
    }
}



/// Simple AI opponent: picks the legal move that captures the highest-value piece,
// with a small bonus for moving into the center of the board.
// Using a heristic search
static void make_computer_move(ChessGame_t *game_instance) {
    int best_from_square = -1;
    int best_to_square = -1;
    int highest_score = AI_INITIAL_LOWEST_SCORE;

    for (int from_square = 0; from_square < TOTAL_BOARD_SQUARES; from_square++) {
        if (belongs_to_player(game_instance->board[from_square], game_instance->active_turn)) {
            for (int to_square = 0; to_square < TOTAL_BOARD_SQUARES; to_square++) {
                if (try_move(game_instance, from_square, to_square, false) == MOVE_OK) {
                    int move_score = get_piece_point_value(game_instance->board[to_square]) * AI_CAPTURE_SCORE_MULTIPLIER;
                    int destination_row = to_square / BOARD_SIZE;
                    int destination_column = to_square % BOARD_SIZE;

                    if (destination_row >= CENTER_BOARD_MIN_INDEX && destination_row <= CENTER_BOARD_MAX_INDEX &&
                        destination_column >= CENTER_BOARD_MIN_INDEX && destination_column <= CENTER_BOARD_MAX_INDEX) {
                        move_score += AI_CENTER_SQUARE_BONUS;
                    }
                    if (move_score > highest_score) {
                        highest_score = move_score;
                        best_from_square = from_square;
                        best_to_square = to_square;
                    }
                }
            }
        }
    }
    if (best_from_square >= 0) {
        try_move(game_instance, best_from_square, best_to_square, true);
    }
}

// Converts a 2-character chess coordinate like "e2" into a board array index (0 to 63).
static int square_text_to_index(const char *square_text) {
    if (square_text[0] < 'a' || square_text[0] > 'h' ||
        square_text[1] < '1' || square_text[1] > '8') {
        return -1;
    }
    int column = square_text[0] - 'a';
    int row = BOARD_SIZE - (square_text[1] - '0');
    return row * BOARD_SIZE + column;
}

static void reset_game(bool enable_computer_opponent) {
    memcpy(current_game.board, INITIAL_BOARD, BOARD_STRING_LENGTH);
    current_game.active_turn = 'W';
    current_game.halfmove_clock = 0;
    current_game.computer_opponent_on = enable_computer_opponent;
    current_game.state = STATE_IN_PROGRESS;
}

static void update_status_leds(void) {
    if (current_game.state == STATE_IDLE) {
        GPIOB_ODR &= ~LED_BOTH_PINS_MASK;
    } else if (current_game.state == STATE_IN_PROGRESS) {
        if (current_game.active_turn == 'W') {
            GPIOB_ODR |=  LED_WHITE_TURN_PIN_MASK;
            GPIOB_ODR &= ~LED_BLACK_TURN_PIN_MASK;
        } else {
            GPIOB_ODR &= ~LED_WHITE_TURN_PIN_MASK;
            GPIOB_ODR |=  LED_BLACK_TURN_PIN_MASK;
        }
    } else {
        if ((millisecond_ticks / LED_BLINK_INTERVAL_MS) % 2 == 0) {
            GPIOB_ODR |=  LED_BOTH_PINS_MASK;
        } else {
            GPIOB_ODR &= ~LED_BOTH_PINS_MASK;
        }
    }
}

// --- User button dispatch ----------------------------------------------
static void handle_user_button(int button_index) {
    switch (button_index) {
        case 0:  // SW0 (PA0) - soft reset, preserves current AI/HvH mode
            reset_game(current_game.computer_opponent_on);
            send_board_state("OK:BOARD:");
            break;

        case 1:  // SW1 (PA1) - force human vs human
            reset_game(false);
            send_board_state("OK:BOARD:");
            uart_send_string("EVENT:NEW\n");
            break;

        case 2:  // SW2 (PA2) - force human vs AI
            reset_game(true);
            send_board_state("OK:BOARD:");
            uart_send_string("EVENT:NEW_AI\n");
            break;

        case 3:  // SW3 (PA3) - resign if a game is in progress
            if (current_game.state == STATE_IN_PROGRESS) {
                current_game.state = STATE_GAME_OVER;
                char winner = (current_game.active_turn == 'W') ? 'B' : 'W';
                uart_send_string("OK:GAMEOVER:WIN:");
                uart_send_character(winner);
                uart_send_string("\n");
            } else {
                uart_send_string("NACK:NOGAME\n");
            }
            break;
    }
}

static void handle_uart_command(const char *command) {
    if (strcmp(command, "CONNECT") == 0 || strcmp(command, "DISCONNECT") == 0) {
        current_game.state = STATE_IDLE;
        uart_send_string("OK\n");
        return;
    }

    if (strcmp(command, "NEW") == 0 || strcmp(command, "NEW:AI") == 0) {
        reset_game(strcmp(command, "NEW:AI") == 0);
        send_board_state("OK:BOARD:");
        return;
    }

    // Test helper: Load a custom 64-character board and turn to test endgames rapidly
    if (strncmp(command, "LOAD:", CMD_LOAD_PREFIX_LENGTH) == 0 &&
        strlen(command) == CMD_LOAD_TOTAL_LENGTH &&
        command[CMD_LOAD_COLON_INDEX] == ':') {
        memcpy(current_game.board, command + CMD_LOAD_PREFIX_LENGTH, TOTAL_BOARD_SQUARES);
        current_game.board[TOTAL_BOARD_SQUARES] = '\0';
        current_game.active_turn = command[CMD_LOAD_TURN_INDEX];
        current_game.state = STATE_IN_PROGRESS;
        send_board_state("OK:BOARD:");
        return;
    }

    
    if (strcmp(command, "STATE") == 0) {
        if (current_game.state == STATE_IDLE) uart_send_string("NACK:NOGAME\n");
        else send_board_state("BOARD:");
        return;
    }

    if (strcmp(command, "RESIGN") == 0) {
        if (current_game.state != STATE_IN_PROGRESS) {
            uart_send_string("NACK:NOGAME\n");
        } else {
            current_game.state = STATE_GAME_OVER;
            char winner = (current_game.active_turn == 'W') ? 'B' : 'W';
            uart_send_string("OK:GAMEOVER:WIN:");
            uart_send_character(winner);
            uart_send_string("\n");
        }
        return;
    }

    // Returns all legal destination squares for a chosen square (e.g., "MOVES:e2" -> "VALID:e3,e4")
    if (strncmp(command, "MOVES:", CMD_MOVES_PREFIX_LENGTH) == 0) {
        if (current_game.state != STATE_IN_PROGRESS) {
            uart_send_string("NACK:NOGAME\n");
            return;
        }
        const char *square_text = command + CMD_MOVES_PREFIX_LENGTH;
        int from_square = square_text_to_index(square_text);
        if (from_square < 0 || strlen(square_text) != SQUARE_COORDINATE_TEXT_LENGTH) {
            uart_send_string("NACK:RANGE\n");
            return;
        }
        uart_send_string("VALID:");
        bool is_first_move_printed = true;
        if (belongs_to_player(current_game.board[from_square], current_game.active_turn)) {
            for (int to_square = 0; to_square < TOTAL_BOARD_SQUARES; to_square++) {
                if (try_move(&current_game, from_square, to_square, false) == MOVE_OK) {
                    if (!is_first_move_printed) uart_send_character(',');
                    uart_send_character('a' + (to_square % BOARD_SIZE));
                    uart_send_character('8' - (to_square / BOARD_SIZE));
                    is_first_move_printed = false;
                }
            }
        }
        uart_send_string("\n");
        return;
    }

    // Executes a player move (e.g., "MOVE:e2e4")
    if (strncmp(command, "MOVE:", CMD_MOVE_PREFIX_LENGTH) == 0) {
        if (current_game.state != STATE_IN_PROGRESS) {
            uart_send_string("NACK:NOGAME\n");
            return;
        }
        const char *move_text = command + CMD_MOVE_PREFIX_LENGTH;
        if (strlen(move_text) != MOVE_COORDINATE_TEXT_LENGTH) {
            uart_send_string("NACK:RANGE\n");
            return;
        }
        int from_square = square_text_to_index(move_text);
        int to_square = square_text_to_index(move_text + SQUARE_COORDINATE_TEXT_LENGTH);
        if (from_square < 0 || to_square < 0) {
            uart_send_string("NACK:RANGE\n");
            return;
        }

        MoveStatus_t move_status = try_move(&current_game, from_square, to_square, true);
        if (move_status == ERR_EMPTY)    { uart_send_string("NACK:EMPTY\n"); return; }
        if (move_status == ERR_NOTURN)   { uart_send_string("NACK:NOTURN\n"); return; }
        if (move_status == ERR_OCCUPIED) { uart_send_string("NACK:OCCUPIED\n"); return; }
        if (move_status == ERR_ILLEGAL)  { uart_send_string("NACK:ILLEGAL\n"); return; }

        // Check if the player's move ended the game by checkmate or stalemate
        if (!player_has_legal_moves(&current_game)) {
            current_game.state = STATE_GAME_OVER;
            if (is_king_in_check(current_game.board, current_game.active_turn)) {
                char winner = (current_game.active_turn == 'W') ? 'B' : 'W';
                uart_send_string("WIN:");
                uart_send_character(winner);
                send_board_state(":BOARD:");
            } else {
                send_board_state("DRAW:BOARD:");
            }
            return;
        }


        if (has_only_kings_left(current_game.board) ||
            current_game.halfmove_clock >= FIFTY_MOVE_RULE_HALFMOVE_LIMIT) {
            current_game.state = STATE_GAME_OVER;
            send_board_state("DRAW:BOARD:");
            return;
        }

        // If the computer opponent is enabled and it is Black's turn, respond immediately
        if (current_game.computer_opponent_on && current_game.active_turn == 'B') {
            make_computer_move(&current_game);
            if (!player_has_legal_moves(&current_game)) {
                current_game.state = STATE_GAME_OVER;
                if (is_king_in_check(current_game.board, current_game.active_turn)) {
                    uart_send_string("WIN:B");
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

    uart_send_string("NACK:BADCMD\n");
}


int main(void) {
    // Enable peripheral clocks for GPIOA, GPIOB, and USART1
    RCC_AHBENR  |= (RCC_AHBENR_GPIOA_CLOCK_EN | RCC_AHBENR_GPIOB_CLOCK_EN);
    RCC_APB2ENR |= RCC_APB2ENR_USART1_CLOCK_EN;

    // Configre PB0 and PB1 as general-purpose outputs for status LEDs
    GPIOB_MODER &= ~GPIOB_MODER_PB0_PB1_CLEAR_MASK;
    GPIOB_MODER |=  GPIOB_MODER_PB0_PB1_OUTPUT_MODE;

    // Configure PA0 as input with pull-up resistor for the hardware reset button
    GPIOA_MODER &= ~GPIOA_MODER_PA0_CLEAR_MASK;
    GPIOA_PUPDR &= ~GPIOA_PUPDR_PA0_CLEAR_MASK;
    GPIOA_PUPDR |=  GPIOA_PUPDR_PA0_PULLUP_ENABLE;

    // SW1..SW3 (PA1..PA3): input, pull-up
    GPIOA_MODER &= ~((3U << 2) | (3U << 4) | (3U << 6));   // input mode
    GPIOA_PUPDR &= ~GPIOA_PUPDR_PA1_PA3_CLEAR_MASK;
    GPIOA_PUPDR |=  GPIOA_PUPDR_PA1_PA3_PULLUP;

    // Configure PA9 (TX) and PA10 (RX) for USART1 alternate function
    GPIOA_MODER &= ~GPIOA_MODER_PA9_PA10_CLEAR_MASK;
    GPIOA_MODER |=  GPIOA_MODER_PA9_PA10_ALT_MODE;
    GPIOA_AFRH  &= ~GPIOA_AFRH_PA9_PA10_CLEAR_MASK;
    GPIOA_AFRH  |=  GPIOA_AFRH_PA9_PA10_AF1_USART1;

    // Configure USART1 for 115200 baud (at 8 MHz clock) and enable TX, RX, and USART, 8N1
    USART1_BRR = USART1_BAUD_115200_AT_8MHZ;
    USART1_CR1 = (USART1_CR1_TX_ENABLE | USART1_CR1_RX_ENABLE | USART1_CR1_ENABLE);

    // Configure SysTick to trigger an interrupt every 1 millisecond
    SYST_RVR = SYSTICK_1MS_RELOAD_AT_8MHZ;
    SYST_CVR = 0;
    SYST_CSR = (SYSTICK_CSR_CLKSOURCE_CPU | SYSTICK_CSR_TICKINT_ENABLE | SYSTICK_CSR_ENABLE);

    reset_game(false);
    current_game.state = STATE_IDLE;

    char receive_buffer[UART_RX_BUFFER_SIZE];
    uint8_t buffer_index = 0;
    bool buffer_overflowed = false;
    uint32_t last_byte_received_time = 0;

    while (1) {
        update_status_leds();

        // Poll all user buttons SW0..SW3 (PA0..PA3), each debounced independently
        for (int i = 0; i < (int)NUM_USER_BUTTONS; i++) {
            bool current_state = (GPIOA_IDR & user_buttons[i].pin_mask) != 0;
            if (!current_state && user_buttons[i].previous_state &&
                (millisecond_ticks - user_buttons[i].last_press_time > BUTTON_DEBOUNCE_MS)) {
                user_buttons[i].last_press_time = millisecond_ticks;
                handle_user_button(i);
            }
            user_buttons[i].previous_state = current_state;
        }

        // Clear UART overrun, noise, framing errors
        if (USART1_ISR & USART1_ERROR_FLAGS_MASK) {
            USART1_ICR = USART1_ERROR_FLAGS_MASK;
        }

        // Read incoming UART character when the recieve data register is not empty
        if (USART1_ISR & USART1_ISR_RX_NOT_EMPTY) {
            char incoming_char = (char)(USART1_RDR & USART1_RDR_BYTE_MASK);
            last_byte_received_time = millisecond_ticks;

            if (incoming_char == '\r') {
                // ignore
            } else if (incoming_char == '\n') {
                if (buffer_overflowed || buffer_index == 0) {
                    uart_send_string("NACK:BADCMD\n");
                } else {
                    receive_buffer[buffer_index] = '\0';
                    handle_uart_command(receive_buffer);
                }
                buffer_index = 0;
                buffer_overflowed = false;
            } else {
                if (buffer_index < UART_MAX_COMMAND_LENGTH) {
                    receive_buffer[buffer_index++] = incoming_char;
                } else {
                    buffer_overflowed = true;
                }
            }
        }

        //reject incomplete commands that stall for more than 500ms without a newline
        if ((buffer_index > 0 || buffer_overflowed) &&
            (millisecond_ticks - last_byte_received_time > UART_RX_TIMEOUT_MS)) {
            buffer_index = 0;
            buffer_overflowed = false;
            uart_send_string("NACK:TIMEOUT\n");
        }
    }
}
