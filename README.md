================================================================================
STM32F051 AUTHORITATIVE CHESS SERVER + QT 6 THIN CLIENT
================================================================================

An STM32F051R8T6 acts as the single source of truth for a full-rules chess
game over the ST-Link Virtual COM Port. A Qt 6 GUI on the host is a
strictly thin client: it forwards clicks and renders whatever board the
server returns. A pytest harness drives the same serial link headlessly.


ARCHITECTURE
--------------------------------------------------------------------------------

   PC host                          STM32F051 (UCT dev board)
   ---------                        -------------------------
   Qt6 chess_gui   <--- USB-CDC --->  USART1 PA9 (TX) / PA10 (RX)
   pytest          /dev/ttyACM0      115200 baud, 8N1
                                      bare-metal firmware (no HAL)


REPOSITORY LAYOUT
--------------------------------------------------------------------------------

  chess-server/
  |-- main.c                  STM32 firmware (bare-metal, no HAL)
  |-- main.h                  register addresses & hardware macros
  |-- startup_stm32f051r8tx.s vector table
  |-- STM32F051R8TX_FLASH.ld  linker script
  |-- syscalls.c, sysmem.c    CubeIDE boilerplate
  |-- .project / .cproject    STM32CubeIDE project files
  |-- main.cpp                Qt 6 C++ GUI (primary)
  |-- CMakeLists.txt          Qt CMake build
  |-- Makefile                Cross-platform build/test automation
  |-- test/
  |   |-- test_chess.py       29-vector pytest harness
  |-- README.txt              this file


PREREQUISITES
--------------------------------------------------------------------------------

Fedora / RHEL / Rocky:

    sudo dnf install qt6-qtbase-devel qt6-qtserialport-devel \
                     cmake gcc-c++ python3-pyserial python3-pytest

Debian / Ubuntu:

    sudo apt install qt6-base-dev libqt6serialport6-dev cmake g++ \
                     python3-pyserial python3-pytest

Serial port permissions (both distros):

    sudo usermod -aG dialout "$USER"
    # log out and back in for the group change to take effect

Firmware toolchain:

    Install STM32CubeIDE from ST's website (no package name --
    download the installer).


FLASH THE FIRMWARE
--------------------------------------------------------------------------------

1. Open STM32CubeIDE and select
   File -> Import -> Existing Projects into Workspace.
2. Browse to this folder and import the project.
3. Confirm the target is STM32F051R8Tx (it is, by .cproject).
4. Click the hammer to build, then Run (play button) to flash.
5. Wait for "Download verified successfully", then press the board's
   reset button once. The PB0 LED is off in IDLE.

The USB cable used for flashing also carries the serial port
(/dev/ttyACM0) -- you do NOT need the CP210x adapter on the newer UCT
boards.


MAKE COMMANDS QUICK REFERENCE
--------------------------------------------------------------------------------

We provide a cross-platform Makefile to automate the build and test
process. Run these from the root `chess-server/` directory:

    make gui         Build the Qt 6 C++ application
    make start_gui   Launch the built GUI (auto-detects the port)
    make test        Run the 29-vector pytest suite
    make check       Check if the serial port is currently in use
    make free        Kill any process holding the serial port
    make clean       Remove build artifacts


RUN THE GUI
--------------------------------------------------------------------------------

    make start_gui

Or leave off the argument and type the port into the text box.
The window opens maximised.


PLAYING
--------------------------------------------------------------------------------

1. Click Connect -- the button turns green and reads "Connected".
2. Click New Game (human vs human) or New vs AI (you are White).
3. Click a piece, then click a highlighted destination. Legal
   destinations come from the server via MOVES:<sq>.
4. Every move is validated by the STM32. If it is illegal, the
   status bar shows the specific rejection:

       NACK:NOTURN     source piece belongs to the inactive player
       NACK:OCCUPIED   destination occupied by a friendly piece
       NACK:ILLEGAL    geometry, path, or self-check violation
       NACK:RANGE      square out of range or wrong length
       NACK:EMPTY      source square has no piece

5. Hardware Buttons (on the STM32 board):
       SW0 (PA0)       Soft reset (preserves current AI/HvH mode)
       SW1 (PA1)       Force new Human vs Human game
       SW2 (PA2)       Force new Human vs AI game
       SW3 (PA3)       Resign current game

   Pressing these buttons updates the GUI immediately via the serial
   "EVENT:" broadcasts.

6. Click Resign in the GUI to forfeit, or play to checkmate / stalemate.
   A modal popup will appear declaring the winner and offering a
   "Play Again" button.


RUN THE AUTOMATED TESTS
--------------------------------------------------------------------------------

    make test

The suite opens /dev/ttyACM0 (or /dev/cu.usbmodem* on macOS), drives 29
assertions against the firmware, and closes the port.

IMPORTANT: do not have the GUI open at the same time -- the OS only
allows one process to hold the serial port.

Expected output (truncated):

    test/test_chess.py::test_connect_returns_ok PASSED
    test/test_chess.py::test_new_returns_initial_board PASSED
    test/test_chess.py::test_legal_pawn_move PASSED
    test/test_chess.py::test_illegal_geometry PASSED
    test/test_chess.py::test_fools_mate PASSED
    test/test_chess.py::test_stalemate_via_load PASSED
    test/test_chess.py::test_ai_punishes_passive_move PASSED
    ...
    ===================== 29 passed in 3.06s =====================


PROTOCOL REFERENCE
--------------------------------------------------------------------------------

All frames are ASCII, \n-terminated, max 75 bytes, \r stripped.
Board encoding is 64 characters, row-major from a8 (index 0) to h1
(index 63). Uppercase = White, lowercase = Black, '.' = empty square.

REQUESTS AND REPLIES

    CONNECT              ->  OK
    DISCONNECT           ->  OK
    NEW / NEW:AI         ->  OK:BOARD:<64>:TURN:W
    MOVE:<sq1><sq2>      ->  OK:BOARD:<64>:TURN:<W|B>
                             or WIN:<W|B>:BOARD:<64>    (checkmate)
                             or DRAW:BOARD:<64>          (stalemate / 50-move / KvK)
                             or a NACK
    MOVES:<sq>           ->  VALID:<sq>[,<sq>...]
                             or NACK:RANGE
    STATE                ->  BOARD:<64>:TURN:<W|B>
                             or NACK:NOGAME
    RESIGN               ->  OK:GAMEOVER:WIN:<W|B>
    LOAD:<64>:<W|B>      ->  OK:BOARD:<64>:TURN:<W|B>   (test-only)


ERROR CODES

    NACK:NOGAME      command requires an active game
    NACK:RANGE       square out of range or wrong length
    NACK:EMPTY       source square has no piece
    NACK:NOTURN      source piece belongs to the inactive player
    NACK:OCCUPIED    destination occupied by a friendly piece
    NACK:ILLEGAL     geometry, path, or self-check violation
    NACK:BADCMD      unknown verb or malformed frame
    NACK:TIMEOUT     partial frame buffered >500 ms


TROUBLESHOOTING
--------------------------------------------------------------------------------

Cannot open /dev/ttyACM0: Permission denied
    Run:  sudo usermod -aG dialout $USER
    Then log out and log back in.

Device or resource busy
    Another process holds the port.
    Run:  make check
    Then: make free

No /dev/ttyACM* appears on plug-in
    Try another USB cable (some are charge-only).
    Confirm the board is powered.

GUI connects but no replies
    Wrong USART -- the new UCT board wires USART1 (PA9/PA10) to the
    ST-Link VCP. Do not use the CP210x adapter.

qt.qpa.plugin: could not load
    Missing Qt6 base package.
    Run:  sudo dnf install qt6-qtbase-devel

Board resets every 0.5 s
    Another process is spamming the port. Close it.

pytest fails with PermissionError
    Same as first row: add yourself to dialout.

GUI window appears but no serial port field
    Rebuild with `make clean` and then `make gui`.


KNOWN LIMITATIONS
--------------------------------------------------------------------------------

The following chess rules are intentionally omitted to keep the
firmware within the STM32F051's 8 KB SRAM budget:

    - Castling
    - En passant
    - Threefold-repetition draws
    - K+B vs K / K+N vs K draws

Pawn promotion is forced to Queen. The AI opponent is 1-ply greedy
(piece value + centre bonus); it does not plan ahead.


LICENSE / ATTRIBUTION
--------------------------------------------------------------------------------

Coursework for EEE3095S 2026, University of Cape Town.
Chess rule engine, firmware, GUI, and test harness written by the
team. Third-party dependencies:

    Qt 6       LGPLv3
    pySerial   BSD
    pytest     MIT

startup_stm32f051r8tx.s, syscalls.c, and sysmem.c are
STM32CubeIDE-generated boilerplate.

================================================================================
