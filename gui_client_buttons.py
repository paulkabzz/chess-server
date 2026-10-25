#!/usr/bin/env python3
"""Thin Tkinter chess client for the EEE3095S STM32F051 serial server.

The client contains zero chess rule logic. Every board render and outcome
is derived from the server's reply. Player clicks are only forwarded as
MOVE:<src><dst> requests; the STM32 is the sole authority on legality.
"""
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk

import serial

BAUD = 115200

PIECES = {
    "K": "♔", "Q": "♕", "R": "♖", "B": "♗", "N": "♘", "P": "♙",
    "k": "♚", "q": "♛", "r": "♜", "b": "♝", "n": "♞", "p": "♟",
    ".": "",
}

LIGHT = "#f0d9b5"
DARK = "#b58863"
SEL_BG = "#7fb069"
LEGAL_BG = "#8fb4d9"


def idx_to_sq(idx: int) -> str:
    return chr(ord("a") + (idx % 8)) + str(8 - idx // 8)


def sq_to_idx(sq: str) -> int:
    return (8 - int(sq[1])) * 8 + (ord(sq[0]) - ord("a"))


class ChessClient:
    def __init__(self, root: tk.Tk, port: str) -> None:
        self.root = root
        root.title("STM32 Chess — Serial Client")
        root.resizable(False, False)

        self.ser: serial.Serial | None = None
        self.rx_thread: threading.Thread | None = None
        self.running = False
        self.rx_queue: queue.Queue[str] = queue.Queue()

        # Client mirror of server state. Never used for validation.
        self.board = "." * 64
        self.turn = "-"
        self.over = True
        self.selected: int | None = None
        self.legal_dests: set[int] = set()
        self.pending_moves_src: int | None = None

        self._build_ui(port)
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        root.after(50, self._poll_queue)

    # ---------------- UI ----------------

    def _build_ui(self, port: str) -> None:
        top = ttk.Frame(self.root, padding=(8, 8, 8, 4))
        top.grid(row=0, column=0, sticky="ew")

        ttk.Label(top, text="Port:").pack(side="left")
        self.port_var = tk.StringVar(value=port)
        self.port_entry = ttk.Entry(top, textvariable=self.port_var, width=16)
        self.port_entry.pack(side="left", padx=4)

        self.btn_connect = ttk.Button(top, text="Connect", command=self.connect)
        self.btn_connect.pack(side="left")
        self.btn_disconnect = ttk.Button(
            top, text="Disconnect", command=self.disconnect, state="disabled"
        )
        self.btn_disconnect.pack(side="left", padx=4)

        bar = ttk.Frame(self.root, padding=(8, 0, 8, 6))
        bar.grid(row=1, column=0, sticky="ew")

        self.btn_new = ttk.Button(
            bar, text="New Game",
            command=lambda: self.new_game(ai=False), state="disabled",
        )
        self.btn_new.pack(side="left")
        self.btn_new_ai = ttk.Button(
            bar, text="New vs AI",
            command=lambda: self.new_game(ai=True), state="disabled",
        )
        self.btn_new_ai.pack(side="left", padx=4)
        self.btn_resign = ttk.Button(
            bar, text="Resign", command=self.resign, state="disabled"
        )
        self.btn_resign.pack(side="left")

        self.turn_var = tk.StringVar(value="Turn: —")
        ttk.Label(
            bar, textvariable=self.turn_var, font=("TkDefaultFont", 12, "bold")
        ).pack(side="right")

        board_frame = ttk.Frame(self.root, padding=8)
        board_frame.grid(row=2, column=0)
        self.cells: list[tk.Button] = []
        for r in range(8):
            for c in range(8):
                idx = r * 8 + c
                b = tk.Button(
                    board_frame, text="", width=3, height=1,
                    font=("DejaVu Sans", 24), relief="flat",
                    bg=LIGHT if (r + c) % 2 == 0 else DARK,
                    activebackground="#d9c3a5",
                    command=lambda i=idx: self.on_cell(i),
                )
                b.grid(row=r, column=c, sticky="nsew")
                self.cells.append(b)

        status = ttk.Frame(self.root, padding=(8, 4, 8, 8))
        status.grid(row=3, column=0, sticky="ew")
        self.status_var = tk.StringVar(value="Not connected")
        ttk.Label(status, textvariable=self.status_var, anchor="w").pack(fill="x")

    def _set_status(self, text: str) -> None:
        self.status_var.set(text)

    def _refresh_buttons(self) -> None:
        connected = self.ser is not None
        self.btn_connect.configure(state="disabled" if connected else "normal")
        self.port_entry.configure(state="disabled" if connected else "normal")
        self.btn_disconnect.configure(state="normal" if connected else "disabled")
        self.btn_new.configure(state="normal" if connected else "disabled")
        self.btn_new_ai.configure(state="normal" if connected else "disabled")
        self.btn_resign.configure(
            state="normal" if (connected and not self.over) else "disabled"
        )

    def _redraw(self) -> None:
        for i, ch in enumerate(self.board):
            btn = self.cells[i]
            btn.configure(text=PIECES.get(ch, ""))
            r, c = divmod(i, 8)
            bg = LIGHT if (r + c) % 2 == 0 else DARK
            if self.selected == i:
                bg = SEL_BG
            elif i in self.legal_dests:
                bg = LEGAL_BG
            btn.configure(bg=bg)

    # ---------------- Serial ----------------

    def connect(self) -> None:
        port = self.port_var.get().strip()
        try:
            self.ser = serial.Serial(port, BAUD, timeout=0.2)
        except Exception as e:
            self._set_status(f"Cannot open {port}: {e}")
            return
        self.running = True
        self.rx_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.rx_thread.start()
        time.sleep(0.3)
        self.send("CONNECT")
        self._set_status("Connected — press New Game or New vs AI")
        self._refresh_buttons()

    def disconnect(self) -> None:
        if self.ser is not None:
            try:
                self.send("DISCONNECT")
            except Exception:
                pass
            time.sleep(0.05)
        self._close_serial()
        self.over = True
        self.selected = None
        self.legal_dests = set()
        self.board = "." * 64
        self.turn = "-"
        self.turn_var.set("Turn: —")
        self._redraw()
        self._set_status("Disconnected")
        self._refresh_buttons()

    def _close_serial(self) -> None:
        self.running = False
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.rx_thread = None

    def _reader_loop(self) -> None:
        buf = b""
        while self.running and self.ser is not None:
            try:
                chunk = self.ser.read(64)
            except Exception:
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                self.rx_queue.put(line.decode(errors="replace").strip())

    def send(self, cmd: str) -> None:
        if self.ser is None:
            return
        try:
            self.ser.write((cmd + "\n").encode())
        except Exception as e:
            self._set_status(f"Send failed: {e}")
            self._close_serial()
            self._refresh_buttons()

    # ---------------- Game commands ----------------

    def new_game(self, ai: bool) -> None:
        self.over = True
        self.selected = None
        self.legal_dests = set()
        self.pending_moves_src = None
        self._redraw()
        self._set_status("Starting new game…")
        self.send("DISCONNECT")
        time.sleep(0.05)
        self.send("NEW:AI" if ai else "NEW")

    def resign(self) -> None:
        if self.over:
            return
        self.send("RESIGN")
        self.over = True
        self.selected = None
        self.legal_dests = set()
        self._refresh_buttons()

    def on_cell(self, idx: int) -> None:
        if self.over or self.ser is None:
            return

        if self.selected is None:
            self.selected = idx
            self.pending_moves_src = idx
            self.legal_dests = set()
            self._redraw()
            self.send(f"MOVES:{idx_to_sq(idx)}")
            return

        if idx == self.selected:
            self.selected = None
            self.pending_moves_src = None
            self.legal_dests = set()
            self._redraw()
            return

        src = self.selected
        self.selected = None
        self.pending_moves_src = None
        self.legal_dests = set()
        self._redraw()
        self.send(f"MOVE:{idx_to_sq(src)}{idx_to_sq(idx)}")

    # ---------------- Protocol handling ----------------

    def _poll_queue(self) -> None:
        try:
            while True:
                self._handle_line(self.rx_queue.get_nowait())
        except queue.Empty:
            pass
        self.root.after(50, self._poll_queue)

    def _handle_line(self, line: str) -> None:
        if not line:
            return

        if line.startswith("OK:BOARD:"):
            self._apply_board(line, len("OK:BOARD:"))
            self._set_status(
                f"Move accepted — {'White' if self.turn == 'W' else 'Black'} to move"
            )
            return

        if line.startswith("BOARD:"):
            self._apply_board(line, len("BOARD:"))
            return

        if line.startswith("WIN:"):
            parts = line.split(":")
            winner = parts[1]
            if "BOARD" in parts:
                self.board = parts[parts.index("BOARD") + 1]
            self._end_game(
                f"{'White' if winner == 'W' else 'Black'} wins by checkmate"
            )
            return

        if line.startswith("DRAW:"):
            parts = line.split(":")
            if "BOARD" in parts:
                self.board = parts[parts.index("BOARD") + 1]
            self._end_game("Draw (stalemate / 50-move / insufficient material)")
            return

        if line.startswith("OK:GAMEOVER:"):
            winner = line.split(":")[-1]
            self._end_game(
                f"{'White' if winner == 'W' else 'Black'} wins by resignation"
            )
            return

        if line.startswith("VALID:"):
            if self.selected is not None and self.pending_moves_src == self.selected:
                payload = line[len("VALID:"):]
                self.legal_dests = {
                    sq_to_idx(s) for s in payload.split(",") if len(s) == 2
                }
                self._redraw()
            return

        if line == "OK":
            self._set_status("Ready")
            return

        if line.startswith("NACK:"):
            self._show_nack(line.split(":", 1)[1])
            return

        self._set_status(f"Unhandled: {line}")

    def _apply_board(self, line: str, prefix_len: int) -> None:
        body = line[prefix_len:]
        parts = body.split(":")
        if not parts[0] or len(parts[0]) != 64:
            self._set_status(f"Bad BOARD payload: {line}")
            return
        self.board = parts[0]
        if "TURN" in parts:
            ti = parts.index("TURN")
            if ti + 1 < len(parts):
                self.turn = parts[ti + 1]
                self.turn_var.set(
                    f"Turn: {'White' if self.turn == 'W' else 'Black'}"
                )
        self.over = False
        self._redraw()
        self._refresh_buttons()

    def _end_game(self, reason: str) -> None:
        self.over = True
        self.selected = None
        self.legal_dests = set()
        self._redraw()
        self._refresh_buttons()
        self._set_status(f"Game over — {reason}")

    def _show_nack(self, code: str) -> None:
        messages = {
            "NOGAME": "No game in progress — press New Game.",
            "RANGE": "Move out of range — invalid square or format.",
            "EMPTY": "Source square is empty.",
            "NOTURN": "That piece is not yours to move.",
            "OCCUPIED": "Destination square is occupied by your piece.",
            "ILLEGAL": "Illegal move for that piece (or leaves king in check).",
            "BADCMD": "Malformed or unknown command.",
            "TIMEOUT": "Incomplete frame timed out.",
        }
        self._set_status(f"Rejected — {messages.get(code, code)}")

    # ---------------- Shutdown ----------------

    def on_close(self) -> None:
        try:
            if self.ser is not None:
                self.send("DISCONNECT")
                time.sleep(0.05)
        finally:
            self._close_serial()
            self.root.destroy()


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    root = tk.Tk()
    ChessClient(root, port)
    root.mainloop()


if __name__ == "__main__":
    main()
