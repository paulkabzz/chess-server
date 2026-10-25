import os, time, pytest, serial

PORT = os.environ.get("SERIAL_PORT", "/dev/ttyACM0")
BAUD = 115200
INIT_BOARD = "rnbqkbnrpppppppp................................PPPPPPPPRNBQKBNR"


@pytest.fixture(scope="module")
def link():
    s = serial.Serial(PORT, BAUD, timeout=1.0)
    time.sleep(0.4)
    s.reset_input_buffer()
    yield s
    s.close()


def cmd(link, text, timeout=1.0):
    link.reset_input_buffer()
    link.write((text + "\n").encode())
    link.timeout = timeout
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = link.readline().decode(errors="replace").strip()
        if line:
            return line
    return ""


def fresh(link):
    cmd(link, "DISCONNECT")
    return cmd(link, "NEW")


def board_of(reply):
    parts = reply.split(":")
    for i, p in enumerate(parts):
        if p == "BOARD" and i + 1 < len(parts):
            return parts[i + 1]
    return None


# ---------- Session / state machine ----------

def test_connect_returns_ok(link):
    assert cmd(link, "CONNECT") == "OK"


def test_state_in_idle(link):
    cmd(link, "DISCONNECT")
    assert cmd(link, "STATE") == "NACK:NOGAME"


def test_new_returns_initial_board(link):
    assert fresh(link) == f"OK:BOARD:{INIT_BOARD}:TURN:W"


def test_bad_command(link):
    fresh(link)
    assert cmd(link, "HELLO") == "NACK:BADCMD"


def test_disconnect_returns_to_idle(link):
    fresh(link)
    assert cmd(link, "DISCONNECT") == "OK"
    assert cmd(link, "STATE") == "NACK:NOGAME"


# ---------- Move validation ----------

def test_legal_pawn_move(link):
    fresh(link)
    r = cmd(link, "MOVE:e2e4")
    assert r.startswith("OK:BOARD:")
    b = board_of(r)
    # a8=0, e2=52, e4=36
    assert b[52] == "." and b[36] == "P"
    assert ":TURN:B" in r


def test_empty_source(link):
    fresh(link)
    assert cmd(link, "MOVE:e4e5") == "NACK:EMPTY"


def test_out_of_range(link):
    fresh(link)
    assert cmd(link, "MOVE:a1a9") == "NACK:RANGE"
    assert cmd(link, "MOVE:z1a2") == "NACK:RANGE"
    assert cmd(link, "MOVE:e2e")   == "NACK:RANGE"    # wrong length


def test_wrong_side_piece(link):
    fresh(link)
    # White to move, but try to move a black pawn
    assert cmd(link, "MOVE:e7e5") == "NACK:NOTURN"


def test_own_piece_target(link):
    fresh(link)
    # Rook to own pawn — target is own piece
    assert cmd(link, "MOVE:a1a2") == "NACK:OCCUPIED"


def test_illegal_geometry(link):
    fresh(link)
    # Pawn cannot move 3 squares forward from the starting position.
    # e5 is empty (so friendly-occupancy passes) and the pawn is White's
    # (so turn-ownership passes), leaving geometry as the failing guard.
    assert cmd(link, "MOVE:e2e5") == "NACK:ILLEGAL"


def test_blocked_slider(link):
    fresh(link)
    # Bishop c1 is blocked by pawn b2 / d2
    assert cmd(link, "MOVE:c1e3") == "NACK:ILLEGAL"


def test_rejected_move_leaves_state_unchanged(link):
    fresh(link)
    cmd(link, "MOVE:e2e4")                       # now Black's turn
    before = board_of(cmd(link, "STATE"))
    # Black pawn cannot move 3 squares forward (e4 target passes OCCUPIED
    # because it holds an enemy piece, so the failing guard is geometry).
    assert cmd(link, "MOVE:e7e4") == "NACK:ILLEGAL"
    after = board_of(cmd(link, "STATE"))
    assert before == after


# ---------- Legal-move query ----------

def test_moves_query_initial_pawn(link):
    fresh(link)
    r = cmd(link, "MOVES:e2")
    assert r.startswith("VALID:")
    assert set(r[6:].split(",")) == {"e3", "e4"}


def test_moves_query_blocked_rook(link):
    fresh(link)
    r = cmd(link, "MOVES:a1")
    assert r == "VALID:"    # no legal destinations


def test_moves_query_bad_square(link):
    fresh(link)
    assert cmd(link, "MOVES:z9") == "NACK:RANGE"


# ---------- Game-over paths ----------

def test_fools_mate(link):
    fresh(link)
    assert cmd(link, "MOVE:f2f3").startswith("OK:BOARD:")
    assert cmd(link, "MOVE:e7e5").startswith("OK:BOARD:")
    assert cmd(link, "MOVE:g2g4").startswith("OK:BOARD:")
    r = cmd(link, "MOVE:d8h4")
    assert r.startswith("WIN:B"), r


def test_move_after_mate_rejected(link):
    fresh(link)
    for m in ("MOVE:f2f3", "MOVE:e7e5", "MOVE:g2g4", "MOVE:d8h4"):
        cmd(link, m)
    assert cmd(link, "MOVE:a2a3") == "NACK:NOGAME"


def test_resign(link):
    fresh(link)
    assert cmd(link, "RESIGN") == "OK:GAMEOVER:WIN:B"


def test_resign_after_white_move(link):
    fresh(link)
    cmd(link, "MOVE:e2e4")                       # now black to move
    assert cmd(link, "RESIGN") == "OK:GAMEOVER:WIN:W"


def test_state_after_game_over_reports_board(link):
    fresh(link)
    cmd(link, "RESIGN")
    r = cmd(link, "STATE")
    assert r.startswith("BOARD:")


def test_stalemate_via_load(link):
    # White Kf6 Qg7, Black Kh8, White to move. White plays Qg7-g6 → stalemate.
    board = (".......k"   # rank 8
             "......Q."   # rank 7
             ".....K.."   # rank 6
             "........"
             "........"
             "........"
             "........"
             "........")  # rank 1
    assert cmd(link, f"LOAD:{board}:W").startswith("OK:BOARD:")
    assert cmd(link, "MOVE:g7g6").startswith("DRAW:BOARD:")


def test_insufficient_material_via_load(link):
    # Kings only — any move triggers the draw
    board = ("....k..."   # rank 8
             "........"
             "........"
             "........"
             "........"
             "........"
             "........"
             "....K...")  # rank 1
    assert cmd(link, f"LOAD:{board}:W").startswith("OK:BOARD:")
    assert cmd(link, "MOVE:e1e2").startswith("DRAW:BOARD:")


# ---------- Protocol integrity ----------

def test_oversized_frame_rejected_and_recovers(link):
    fresh(link)
    assert cmd(link, "X" * 100) == "NACK:BADCMD"
    assert cmd(link, "STATE").startswith("BOARD:")


def test_incomplete_frame_timeout(link):
    fresh(link)
    link.reset_input_buffer()
    link.write(b"MOV")
    link.timeout = 1.5
    assert link.readline().decode().strip() == "NACK:TIMEOUT"
    assert cmd(link, "STATE").startswith("BOARD:")


def test_empty_line_rejected(link):
    fresh(link)
    assert cmd(link, "") == "NACK:BADCMD"


# ---------- AI (stretch) ----------

def test_ai_makes_move(link):
    cmd(link, "DISCONNECT")
    assert cmd(link, "NEW:AI").startswith("OK:BOARD:")
    r = cmd(link, "MOVE:e2e4", timeout=6.0)
    assert r.startswith("OK:BOARD:"), r
    assert ":TURN:W" in r                        # proves the AI replied
    b = board_of(r)
    assert len(b) == 64
    assert sum(1 for c in b if c != ".") == 32   # no captures possible yet


def test_ai_never_returns_illegal(link):
    cmd(link, "DISCONNECT")
    cmd(link, "NEW:AI")
    r = cmd(link, "MOVE:e2e4", timeout=6.0)
    assert r.startswith("OK:BOARD:"), r
    b = board_of(r)
    assert len(b) == 64
    assert 'K' in b and 'k' in b                 # both kings still present


def test_ai_punishes_passive_move(link):
    """The greedy AI should capture an undefended central pawn."""
    cmd(link, "DISCONNECT")
    cmd(link, "NEW:AI")
    cmd(link, "MOVE:e2e4", timeout=6.0)          # AI replies (expect Nb8-c6)
    r = cmd(link, "MOVE:d2d4", timeout=6.0)      # hangs the d4 pawn
    assert r.startswith("OK:BOARD:"), r
    b = board_of(r)
    # d4 = index 27; if it is still White's pawn, the AI missed the capture
    assert b[27] != 'P', f"AI missed free pawn at d4; board={b}"
