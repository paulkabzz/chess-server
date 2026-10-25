import os
import time
import glob
import serial
import pytest

START_BOARD = "rnbqkbnrpppppppp................................PPPPPPPPRNBQKBNR"

def detect_port():
    env_port = os.environ.get("SERIAL_PORT")
    if env_port:
        return env_port
    candidates = (
        glob.glob("/dev/cu.usbmodem*")
        + glob.glob("/dev/cu.usbserial*")
        + glob.glob("/dev/ttyACM*")
    )
    if candidates:
        return candidates[0]
    for p in serial.tools.list_ports.comports():
        if "STLink" in p.description or "COM" in p.device:
            return p.device
    raise RuntimeError("No STM32 serial port found.")

@pytest.fixture(scope="module")
def stm32():
    port = detect_port()
    ser = serial.Serial(port, 115200, timeout=1.2)
    time.sleep(0.2)
    ser.reset_input_buffer()
    yield ser
    ser.close()

def send_cmd(ser, cmd):
    ser.reset_input_buffer()
    ser.write(f"{cmd}\n".encode("ascii"))
    return ser.readline().decode("ascii", errors="replace").strip()

def test_connect_and_new(stm32):
    """1. CONNECT returns OK; NEW returns starting 64-char board and TURN:W."""
    assert send_cmd(stm32, "CONNECT") == "OK"
    assert send_cmd(stm32, "NEW") == f"OK:BOARD:{START_BOARD}:TURN:W"

def test_legal_move(stm32):
    """2. Legal move (e2e4) updates squares 52 and 36 and flips turn to B."""
    send_cmd(stm32, "NEW")
    resp = send_cmd(stm32, "MOVE:e2e4")
    assert resp.startswith("OK:BOARD:") and resp.endswith(":TURN:B")
    board = resp.split("OK:BOARD:")[1].split(":TURN:")[0]
    assert board[52] == "." and board[36] == "P"

def test_server_move_highlighting(stm32):
    """3. MOVES:<sq> returns comma-separated legal target squares computed by STM32."""
    send_cmd(stm32, "NEW")
    assert send_cmd(stm32, "MOVES:e2") == "VALID:e4,e3"
    assert send_cmd(stm32, "MOVES:b1") == "VALID:a3,c3"

def test_occupied_cell_leaves_state_unchanged(stm32):
    """4. Moving onto friendly piece (d1d2) returns NACK:OCCUPIED and preserves board."""
    send_cmd(stm32, "NEW")
    assert send_cmd(stm32, "MOVE:d1d2") == "NACK:OCCUPIED"
    assert send_cmd(stm32, "STATE") == f"BOARD:{START_BOARD}:TURN:W"

def test_out_of_range_move(stm32):
    """5. Out-of-range coordinates return NACK:RANGE and preserve board."""
    send_cmd(stm32, "NEW")
    assert send_cmd(stm32, "MOVE:z9a1") == "NACK:RANGE"
    assert send_cmd(stm32, "MOVE:e2") == "NACK:RANGE"
    assert send_cmd(stm32, "STATE") == f"BOARD:{START_BOARD}:TURN:W"

def test_out_of_turn_empty_and_illegal_geometry(stm32):
    """6. Out-of-turn -> NACK:NOTURN; empty -> NACK:EMPTY; bad geometry -> NACK:ILLEGAL."""
    send_cmd(stm32, "NEW")
    assert send_cmd(stm32, "MOVE:e7e5") == "NACK:NOTURN"
    assert send_cmd(stm32, "MOVE:e4e5") == "NACK:EMPTY"
    assert send_cmd(stm32, "MOVE:e2e5") == "NACK:ILLEGAL"
    assert send_cmd(stm32, "STATE") == f"BOARD:{START_BOARD}:TURN:W"

def test_fools_mate_win_and_post_game_rejection(stm32):
    """7. Scripted 4-ply Fool's Mate returns WIN:B and rejects subsequent moves with NACK:NOGAME."""
    send_cmd(stm32, "NEW")
    assert send_cmd(stm32, "MOVE:f2f3").startswith("OK:BOARD:")
    assert send_cmd(stm32, "MOVE:e7e5").startswith("OK:BOARD:")
    assert send_cmd(stm32, "MOVE:g2g4").startswith("OK:BOARD:")
    win_resp = send_cmd(stm32, "MOVE:d8h4")
    assert win_resp.startswith("WIN:B:BOARD:")
    assert send_cmd(stm32, "MOVE:a2a3") == "NACK:NOGAME"

def test_scripted_stalemate_draw(stm32):
    """8. Scripted stalemate position returns DRAW:BOARD:..."""
    # Index 0 = a8 ('k'), Index 2 = c8 ('K'), Index 18 = c6 ('Q') -> 15 dots between K and Q
    custom = "k.K" + ("." * 15) + "Q" + ("." * 45)
    assert send_cmd(stm32, f"LOAD:{custom}:W").startswith("OK:BOARD:")
    draw_resp = send_cmd(stm32, "MOVE:c6b6")
    assert draw_resp.startswith("DRAW:BOARD:")

def test_protocol_robustness_garbage_and_timeout(stm32):
    """9. Garbage commands -> NACK:BADCMD; partial unterminated frame -> NACK:TIMEOUT after 500ms."""
    send_cmd(stm32, "NEW")
    assert send_cmd(stm32, "RANDOM_GARBAGE_123") == "NACK:BADCMD"
    # Send partial frame without '\n' and wait >500ms
    stm32.reset_input_buffer()
    stm32.write(b"MOVE:e2")
    time.sleep(0.65)
    assert stm32.readline().decode("ascii", errors="replace").strip() == "NACK:TIMEOUT"
    # Verify board state is completely untouched
    assert send_cmd(stm32, "STATE") == f"BOARD:{START_BOARD}:TURN:W"

def test_ai_opponent_makes_legal_move(stm32):
    """10. Stretch Bonus: STM32 AI opponent responds legally to human move."""
    assert send_cmd(stm32, "NEW:AI").startswith("OK:BOARD:")
    resp = send_cmd(stm32, "MOVE:e2e4")
    assert resp.startswith("OK:BOARD:") and resp.endswith(":TURN:W")
