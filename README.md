# STM32F051 Authoritative Chess Server & Qt 6 C++ Thin Client

**EEE3095S Embedded Systems II — CS Project 2026 (University of Cape Town)**  
**Authors:** Paul Kabulu (`KBLPAU002`) & Mlungisi Sibeko (`SBKMLU001`)  
**Demo Video:** [Watch on YouTube](https://youtu.be/YgMaVd8k1dc)

---

## Overview

An **STM32F051R8T6** microcontroller acts as the isolated referee and single source of truth for a Chess game over the onboard ST-Link Virtual COM Port (`115200` baud, `8N1`). 

A **Qt 6 C++ GUI** on the host PC acts as a strictly thin client: it verifies physical MCU presence via a `1500 ms` application-layer handshake, queries the server for legal move highlights (`MOVES:<sq>`), forwards algebraic move requests (`MOVE:e2e4`), and renders the board exclusively by diffing consecutive 64-character board strings returned by the STM32. An automated 29-vector **Python `pytest` + `pySerial`** test harness drives the same serial link headlessly for hardware-in-the-loop verification.

```text
+-----------------------+                      +------------------------------------+
|        PC Host        |                      |    STM32F051R8T6 (UCT Dev Board)   |
|                       |                      |                                    |
|  - Qt 6 C++ GUI       | <--- ST-Link VCP --> |  - Bare-Metal C Firmware (No HAL)  |
|  - pytest Suite (29)  |     /dev/ttyACM0     |  - USART1: PA9 (TX) / PA10 (RX)    |
|                       |   115200 Baud, 8N1   |  - Buttons: SW0..SW3 (PA0..PA3)    |
|                       |                      |  - Status LEDs: D0..D1 (PB0..PB1)  |
+-----------------------+                      +------------------------------------+
```

---

## Repository Structure

```text
chess-server/
├── main.c                    # Bare-metal STM32F051 firmware (UART, FSM, Chess engine, 1-ply AI)
├── main.h                    # Register addresses, bitmasks & hardware macros
├── startup_stm32f051r8tx.s   # Cortex-M0 vector table
├── STM32F051R8TX_FLASH.ld    # Linker script (64 KB Flash, 8 KB SRAM)
├── syscalls.c, sysmem.c      # STM32CubeIDE runtime boilerplate
├── .project / .cproject      # STM32CubeIDE project configuration
├── main.cpp                  # Qt 6 C++ thin-client GUI
├── CMakeLists.txt            # CMake build configuration for the Qt 6 GUI
├── Makefile                  # Cross-platform build, port detection & test automation
├── test/
│   └── test_chess.py         # 29-vector automated pytest hardware verification suite
└── README.md                 # Project documentation & setup guide
```

---

## Prerequisites

### 1. Host PC Dependencies (Qt 6, CMake, Python Test Suite)

**Fedora / RHEL / Rocky Linux:**
```bash
sudo dnf install qt6-qtbase-devel qt6-qtserialport-devel \
                 cmake gcc-c++ python3-pyserial python3-pytest
```

**Debian / Ubuntu:**
```bash
sudo apt install qt6-base-dev libqt6serialport6-dev \
                 cmake g++ python3-pyserial python3-pytest
```

**macOS (Homebrew):**
```bash
brew install qt cmake
pip3 install pyserial pytest
```

### 2. Linux Serial Port Permissions
Add your user to the `dialout` group so you can open `/dev/ttyACM0` without `sudo`:
```bash
sudo usermod -aG dialout "$USER"
# Log out and log back in for the group change to take effect.
```

### 3. Embedded Toolchain
Install **STM32CubeIDE** from STMicroelectronics to compile and flash the firmware onto the STM32F051R8T6 target.

---

## Flashing the STM32F051 Firmware

1. Connect the **UCT STM32F051 Development Board** to your PC via the onboard USB ST-Link port.
2. Open **STM32CubeIDE** and select **File $\rightarrow$ Import $\rightarrow$ General $\rightarrow$ Existing Projects into Workspace**.
3. Select the root `chess-server/` directory and import the project.
4. Confirm the target MCU is `STM32F051R8Tx` (pre-configured in `.cproject`).
5. Click the **Build** icon (Hammer), then click **Run** (Play icon) to flash the board over SWD.
6. Once `"Download verified successfully"` appears, press the board's **RESET** button (`SW4`). Both `PB0` and `PB1` LEDs will turn **OFF** (`STATE_IDLE`).

> **Note:** The single USB cable connected to the onboard ST-Link V2.1 provides power, SWD flashing, and the Virtual COM Port (`/dev/ttyACM0` on Linux, `/dev/cu.usbmodem*` on macOS). You do **not** need an external CP210x USB-UART adapter.

---

## Makefile Quick Reference

We provide a cross-platform `Makefile` that auto-detects your OS (`Linux`, `macOS`, `Windows`), CPU core count, and active serial port. Run these commands from the root `chess-server/` directory:

| Command | Description |
| :--- | :--- |
| `make help` | Show available commands and the auto-detected serial port |
| `make gui` | Compile the Qt 6 C++ GUI into `./build/chess_gui` |
| `make start_gui` | Launch the built GUI (automatically passes the detected serial port) |
| `make test` | Run all 29 hardware-in-the-loop `pytest` vectors against the STM32 |
| `make check` | Check if another process is currently holding the serial port |
| `make free` | Terminate any stuck process holding the serial port |
| `make clean` | Remove `build/` and `pytest` cache artifacts |

**Overriding the serial port manually (if needed):**
```bash
make start_gui PORT=/dev/ttyACM1
make test PORT=/dev/cu.usbmodem14103
```

---

## Building & Running the Qt 6 GUI

Build and launch the GUI in two commands:
```bash
make gui
make start_gui
```

### How to Play & Hardware Controls

1. **Connect & Handshake:** Click **Connect**. The button turns orange (`"Connecting..."`) while sending `CONNECT\n` and starting a `1500 ms` timer. Once the STM32 replies with `OK`, the button turns green (`"Connected"`) and unlocks the game buttons.
2. **Select Mode:** Click **New Game** (Human vs. Human) or **New vs AI** (you play White; the STM32's 1-ply heuristic engine plays Black).
3. **Move Pieces:** Click a friendly piece to query the server (`MOVES:<sq>`) and display valid destination dots/capture rings. Click a destination square to send `MOVE:<src><dst>`.
4. **Server Rejections:** Every move is validated by the STM32 before committing to state. Illegal requests leave the board untouched and display the specific rejection in the GUI status bar.
5. **Onboard Hardware Pushbuttons (`PA0`–`PA3`, active-low, 50 ms debounced):**
   * **`SW0` (`PA0`):** Soft reset (restarts the game in the current HvH or AI mode).
   * **`SW1` (`PA1`):** Starts a new Human vs. Human game (broadcasts `EVENT:NEW`).
   * **`SW2` (`PA2`):** Starts a new Human vs. AI game (broadcasts `EVENT:NEW_AI`).
   * **`SW3` (`PA3`):** Hardware resign during an active game (broadcasts `OK:GAMEOVER:WIN:<Opponent>`).
6. **Onboard Status LEDs (`PB0`–`PB1`):**
   * **`STATE_IDLE`:** `PB0` = OFF, `PB1` = OFF.
   * **`STATE_IN_PROGRESS` (HvH):** `PB0` = ON (White's turn), `PB1` = ON (Black's turn).
   * **`STATE_GAME_OVER`:** `PB0` and `PB1` blink synchronously at `2 Hz` (`250 ms` interval).

---

## Running the Automated Test Suite (`pytest`)

> **Important:** Make sure the GUI is closed before running the test suite so the serial port is not locked (or run `make free` first).

```bash
make test
```

The test harness (`test/test_chess.py`) opens the serial port at `115200 8N1`, clears any ST-Link `DTR`/`RTS` boot glitches, and executes 29 deterministic hardware-in-the-loop tests:

```text
$ make test
Running pytest suite on port /dev/ttyACM0...
============================== test session starts ===============================
platform linux -- Python 3.14.8, pytest-9.1.1, pluggy-1.7.0
collected 29 items
test_chess.py::test_connect_returns_ok PASSED                    [  3%]
test_chess.py::test_state_in_idle PASSED                         [  6%]
test_chess.py::test_new_returns_initial_board PASSED             [ 10%]
...
test_chess.py::test_ai_makes_move PASSED                         [ 93%]
test_chess.py::test_ai_never_returns_illegal PASSED              [ 96%]
test_chess.py::test_ai_punishes_passive_move PASSED              [100%]
=============================== 29 passed in 3.06s ===============================
```

---

## Serial Protocol Reference

* **Framing:** Printable ASCII terminated by `\n` (`0x0A`); preceding `\r` (`0x0D`) is ignored.
* **Frame Bounds:** Max `75` payload bytes before `\n` (overflow latches and replies `NACK:BADCMD`); `500 ms` inter-byte receive timeout (incomplete frame flushes and replies `NACK:TIMEOUT`).
* **Board Encoding:** Fixed 64-character ASCII string in row-major order from `a8` (index `0`) to `h1` (index `63`). Uppercase = White (`P, N, B, R, Q, K`), lowercase = Black (`p, n, b, r, q, k`), and `.` = empty square.

### Commands & Server Responses

| Command / Event | Valid State(s) | Server Response |
| :--- | :--- | :--- |
| `CONNECT` | All | `OK\n` (resets server to `STATE_IDLE`, turns LEDs OFF) |
| `NEW` | All | `OK:BOARD:<64>:TURN:W\n` (2-player HvH mode, enters `IN_PROGRESS`) |
| `NEW:AI` | All | `OK:BOARD:<64>:TURN:W\n` (enables 1-ply STM32 AI for Black) |
| `MOVE:<s1><s2>` | `IN_PROGRESS` | `OK:BOARD:<64>:TURN:<W\|B>\n` <br>or `WIN:<W\|B>:BOARD:<64>:TURN:<B\|W>\n` (Checkmate) <br>or `DRAW:BOARD:<64>:TURN:<B\|W>\n` (Stalemate / 50-move / KvK) <br>or `NACK:<REASON>\n` |
| `MOVES:<sq>` | `IN_PROGRESS` | `VALID:<sq1>,<sq2>,...\n` (legal target squares for `<sq>`) |
| `LOAD:<64>:<W\|B>` | All | `OK:BOARD:<64>:TURN:<W\|B>\n` (loads custom board state for testing) |
| `STATE` | `IN_PROG`, `OVER` | `BOARD:<64>:TURN:<W\|B>\n` |
| `RESIGN` | `IN_PROGRESS` | `OK:GAMEOVER:WIN:<B\|W>\n` (transitions to `STATE_GAME_OVER`) |
| `DISCONNECT` | All | `OK\n` (returns server to `STATE_IDLE`) |
| `SW0`–`SW2` Press | All | `OK:BOARD:<64>:TURN:W\n` (plus `EVENT:NEW` or `EVENT:NEW_AI`) |
| `SW3` Press | `IN_PROGRESS` | `OK:GAMEOVER:WIN:<B\|W>\n` (hardware resign button) |

### Error Evaluation Priority (`NACK` Codes)

| Priority | Response | Triggering Condition |
| :--- | :--- | :--- |
| **1** | `NACK:TIMEOUT` | Partial frame buffered for `>500 ms` without `\n` |
| **2** | `NACK:BADCMD` | Frame exceeds `75` bytes, is empty, or has an unrecognised command verb |
| **3** | `NACK:NOGAME` | Game command sent in `STATE_IDLE`, or move/resign sent in `STATE_GAME_OVER` |
| **4** | `NACK:RANGE` | Coordinates outside `a1`–`h8` or invalid payload length |
| **5** | `NACK:EMPTY` | Source square contains no piece (`.`) |
| **6** | `NACK:NOTURN` | Source square holds a piece belonging to the inactive player |
| **7** | `NACK:OCCUPIED` | Destination square holds a friendly piece belonging to the active player |
| **8** | `NACK:ILLEGAL` | Violates piece geometry, raycast path is blocked, or leaves friendly King in check |

---

## Troubleshooting

* **`Cannot open /dev/ttyACM0: Permission denied`**  
  Run `sudo usermod -aG dialout "$USER"`, then log out and log back in.
* **`Device or resource busy`**  
  Another process (such as an open GUI window or serial monitor) is holding the port. Run `make check` to inspect it and `make free` to release it.
* **No `/dev/ttyACM*` or `/dev/cu.usbmodem*` appears when plugged in**  
  Check that you are using a data-capable USB cable (not a charge-only cable) connected to the onboard ST-Link port.
* **`GUI connects ("Connecting...") then times out after 1.5 s`**  
  Press the `RESET` button (`SW4`) on the STM32 board and ensure the firmware has been flashed via STM32CubeIDE.
* **`qt.qpa.plugin: could not load`**  
  Ensure the Qt 6 base development packages are installed (`qt6-qtbase-devel` on Fedora or `qt6-base-dev` on Ubuntu).

---

## Known Scope Limitations

To keep the firmware compact, fast, and strictly within the STM32F051's **8 KB SRAM** budget, the following rules are intentionally omitted:
* Castling
* *En passant* captures
* Threefold-repetition detection
* `K+B vs K` / `K+N vs K` insufficient-material draws (`K vs K` and the 50-move rule are enforced)

Pawns automatically promote to a Queen (`Q`/`q`) upon reaching rank 8 or rank 1. The embedded AI opponent evaluates all strictly legal Black moves using a 1-ply greedy heuristic (capture value $\times 10 +$ centre-square bonus).

---

## License & Attribution

Developed for **EEE3095S (2026)** at the **University of Cape Town**.  
All bare-metal STM32 firmware, Chess validation logic, Qt 6 GUI code, and `pytest` verification scripts were written by **Paul Kabulu** and **Mlungisi Sibeko**.

* **Qt 6:** LGPLv3
* **pySerial:** BSD License
* **pytest:** MIT License
* `startup_stm32f051r8tx.s`, `syscalls.c`, and `sysmem.c` are STM32CubeIDE-generated target files.
