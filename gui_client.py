#!/usr/bin/env python3
"""Thin Tkinter chess client for the STM32F051 serial server.

Zero rule logic lives in the client. Every board render, every legal-move
hint, every terminal state comes from a server reply.
"""
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk

import serial

BAUD = 115200

PIECE_GLYPHS = {
    "K": "♔", "Q": "♕", "R": "♖", "B": "♗", "N": "♘", "P": "♙",
    "k": "♚", "q": "♛", "r": "♜", "b": "♝", "n": "♞", "p": "♟",
    ".": "",
}

LIGHT_SQ = "#ebecd0"
DARK_SQ = "#739552"
LAST_MOVE_TINT = "#f7f769"
SELECTED_TINT = "#829769"
LEGAL_DOT = "#2f2f2f"
LEGAL_CAPTURE = "#2f2f2f"

MIN_SQUARE = 32


def idx_to_sq(idx):
    return chr(ord("a") + (idx % 8)) + str(8 - idx // 8)


def sq_to_idx(sq):
    return (8 - int(sq[1])) * 8 + (ord(sq[0]) - ord("a"))


def piece_fill(p):
    return "#fafafa" if p.isupper() else "#151515"


def piece_shadow(p):
    return "#5a5a5a" if p.isupper() else None


class BoardView:
    def __init__(self, parent, on_click, initial_square=70):
        self.square = initial_square
        self.size = initial_square * 8
        self.on_click = on_click
        self.board = "." * 64
        self.selected = None
        self.legal_dests = set()
        self.last_move_squares: set[int] = set()

        self.canvas = tk.Canvas(parent, width=self.size, height=self.size,
                                highlightthickness=0, bg=LIGHT_SQ)
        self.canvas.place(relx=0.5, rely=0.5, anchor="center")
        self.canvas.bind("<Button-1>", self._on_click)
        self._draw_background()

    def _font(self):
        return ("DejaVu Sans", max(12, int(self.square * 0.63)))

    def set_square(self, sq):
        if sq == self.square or sq < MIN_SQUARE:
            return
        self.square = sq
        self.size = sq * 8
        self.canvas.config(width=self.size, height=self.size)
        self._redraw_all()

    def _redraw_all(self):
        self._draw_background()
        self.render(self.board)
        self.set_highlights(self.selected, self.legal_dests, self.last_move_squares)

    def _draw_background(self):
        self.canvas.delete("bg")
        for r in range(8):
            for c in range(8):
                x1, y1 = c * self.square, r * self.square
                self.canvas.create_rectangle(
                    x1, y1, x1 + self.square, y1 + self.square,
                    fill=LIGHT_SQ if (r + c) % 2 == 0 else DARK_SQ,
                    outline="", tags="bg",
                )

    def _center(self, idx):
        r, c = divmod(idx, 8)
        return (c * self.square + self.square // 2,
                r * self.square + self.square // 2)

    def _topleft(self, idx):
        r, c = divmod(idx, 8)
        return c * self.square, r * self.square

    def _on_click(self, event):
        c = event.x // self.square
        r = event.y // self.square
        if 0 <= r < 8 and 0 <= c < 8:
            self.on_click(r * 8 + c)

    def render(self, board):
        self.board = board
        self.canvas.delete("piece")
        font = self._font()
        for i, p in enumerate(board):
            if p == ".":
                continue
            cx, cy = self._center(i)
            shadow = piece_shadow(p)
            if shadow:
                self.canvas.create_text(
                    cx + 1, cy + 1, text=PIECE_GLYPHS[p],
                    font=font, fill=shadow, tags="piece"
                )
            self.canvas.create_text(
                cx, cy, text=PIECE_GLYPHS[p],
                font=font, fill=piece_fill(p), tags="piece"
            )

    def set_highlights(self, selected, legal_dests, last_move_squares):
        self.selected = selected
        self.legal_dests = legal_dests
        self.last_move_squares = last_move_squares
        self.canvas.delete("highlight")

        for sq in last_move_squares:
            x1, y1 = self._topleft(sq)
            self.canvas.create_rectangle(
                x1, y1, x1 + self.square, y1 + self.square,
                fill=LAST_MOVE_TINT, outline="",
                stipple="gray50", tags="highlight",
            )

        if selected is not None:
            x1, y1 = self._topleft(selected)
            self.canvas.create_rectangle(
                x1, y1, x1 + self.square, y1 + self.square,
                fill=SELECTED_TINT, outline="",
                stipple="gray50", tags="highlight",
            )

        for dst in legal_dests:
            cx, cy = self._center(dst)
            if self.board[dst] == ".":
                r = max(6, self.square // 9)
                self.canvas.create_oval(
                    cx - r, cy - r, cx + r, cy + r,
                    fill=LEGAL_DOT, outline="", tags="highlight",
                )
            else:
                r = self.square // 2 - 3
                self.canvas.create_oval(
                    cx - r, cy - r, cx + r, cy + r,
                    outline=LEGAL_CAPTURE, width=max(3, self.square // 18),
                    tags="highlight",
                )


class ChessClient:
    def __init__(self, root, port):
        self.root = root
        root.title("STM32 Chess — Serial Client")

        self.ser = None
        self.rx_thread = None
        self.running = False
        self.rx_queue = queue.Queue()

        self.board = "." * 64
        self.turn = "-"
        self.over = True
        self.selected = None
        self.legal_dests = set()
        self.last_move_squares: set[int] = set()

        self._build_ui(port)
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        root.after(50, self._poll_queue)

    def _build_ui(self, port):
        root = self.root
        root.grid_rowconfigure(0, weight=0)
        root.grid_rowconfigure(1, weight=0)
        root.grid_rowconfigure(2, weight=1)
        root.grid_rowconfigure(3, weight=0)
        root.grid_columnconfigure(0, weight=1)

        top = ttk.Frame(root, padding=(8, 8, 8, 4))
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

        bar = ttk.Frame(root, padding=(8, 0, 8, 6))
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

        board_frame = ttk.Frame(root, padding=8)
        board_frame.grid(row=2, column=0, sticky="nsew")
        self.board_frame = board_frame
        self.board_view = BoardView(board_frame, on_click=self.on_cell)
        board_frame.bind("<Configure>", self._on_board_resize)

        status = ttk.Frame(root, padding=(8, 4, 8, 8))
        status.grid(row=3, column=0, sticky="ew")
        self.status_var = tk.StringVar(value="Not connected")
        ttk.Label(status, textvariable=self.status_var, anchor="w").pack(fill="x")

    def _on_board_resize(self, event):
        side = min(event.width, event.height)
        new_sq = side // 8
        if new_sq >= MIN_SQUARE:
            self.board_view.set_square(new_sq)

    def _set_status(self, text):
        self.status_var.set(text)

    def _refresh_buttons(self):
        connected = self.ser is not None
        self.btn_connect.configure(state="disabled" if connected else "normal")
        self.port_entry.configure(state="disabled" if connected else "normal")
        self.btn_disconnect.configure(state="normal" if connected else "disabled")
        self.btn_new.configure(state="normal" if connected else "disabled")
        self.btn_new_ai.configure(state="normal" if connected else "disabled")
        self.btn_resign.configure(
            state="normal" if (connected and not self.over) else "disabled"
        )

    def _refresh_ui(self):
        self.board_view.set_highlights(
            self.selected, self.legal_dests, self.last_move_squares
        )
        self.board_view.render(self.board)
        self.turn_var.set(
            "Turn: —" if self.turn == "-"
            else f"Turn: {'White' if self.turn == 'W' else 'Black'}"
        )
        self._refresh_buttons()

    def connect(self):
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

    def disconnect(self):
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
        self.last_move_squares = set()
        self.board = "." * 64
        self.turn = "-"
        self._refresh_ui()
        self._set_status("Disconnected")

    def _close_serial(self):
        self.running = False
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.rx_thread = None

    def _reader_loop(self):
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

    def send(self, cmd):
        if self.ser is None:
            return
        try:
            self.ser.write((cmd + "\n").encode())
        except Exception as e:
            self._set_status(f"Send failed: {e}")
            self._close_serial()
            self._refresh_buttons()

    def new_game(self, ai):
        self.over = True
        self.selected = None
        self.legal_dests = set()
        self.last_move_squares = set()
        self.board = "." * 64
        self.turn = "-"
        self._refresh_ui()
        self._set_status("Starting new game…")
        self.send("DISCONNECT")
        time.sleep(0.05)
        self.send("NEW:AI" if ai else "NEW")

    def resign(self):
        if self.over:
            return
        self.send("RESIGN")
        self.over = True
        self.selected = None
        self.legal_dests = set()
        self._refresh_ui()

    def on_cell(self, idx):
        if self.over or self.ser is None:
            return

        if self.selected is None:
            self.selected = idx
            self.legal_dests = set()
            self._refresh_ui()
            self.send(f"MOVES:{idx_to_sq(idx)}")
            return

        if idx == self.selected:
            self.selected = None
            self.legal_dests = set()
            self._refresh_ui()
            return

        src = self.selected
        self.selected = None
        self.legal_dests = set()
        self._refresh_ui()
        self.send(f"MOVE:{idx_to_sq(src)}{idx_to_sq(idx)}")

    def _poll_queue(self):
        try:
            while True:
                self._handle_line(self.rx_queue.get_nowait())
        except queue.Empty:
            pass
        self.root.after(50, self._poll_queue)

    def _handle_line(self, line):
        if not line:
            return
        if line.startswith("OK:BOARD:"):
            self._apply_board(line, len("OK:BOARD:"))
            return
        if line.startswith("BOARD:"):
            self._apply_board(line, len("BOARD:"))
            return
        if line.startswith("WIN:"):
            parts = line.split(":")
            winner = parts[1]
            if "BOARD" in parts:
                idx = parts.index("BOARD") + 1
                if idx < len(parts):
                    self._apply_board("BOARD:" + parts[idx], len("BOARD:"))
            self._end_game(
                f"{'White' if winner == 'W' else 'Black'} wins by checkmate"
            )
            return
        if line.startswith("DRAW:"):
            parts = line.split(":")
            if "BOARD" in parts:
                idx = parts.index("BOARD") + 1
                if idx < len(parts):
                    self._apply_board("BOARD:" + parts[idx], len("BOARD:"))
            self._end_game("Draw (stalemate / 50-move / insufficient material)")
            return
        if line.startswith("OK:GAMEOVER:"):
            winner = line.split(":")[-1]
            self._end_game(
                f"{'White' if winner == 'W' else 'Black'} wins by resignation"
            )
            return
        if line.startswith("VALID:"):
            if self.selected is not None:
                payload = line[len("VALID:"):]
                self.legal_dests = {
                    sq_to_idx(s) for s in payload.split(",") if len(s) == 2
                }
                self._refresh_ui()
            return
        if line == "OK":
            self._set_status("Ready")
            return
        if line.startswith("NACK:"):
            self._show_nack(line.split(":", 1)[1])
            return
        self._set_status(f"Unhandled: {line}")

    def _apply_board(self, line, prefix_len):
        body = line[prefix_len:]
        parts = body.split(":")
        if not parts[0] or len(parts[0]) != 64:
            self._set_status(f"Bad BOARD payload: {line}")
            return

        new_board = parts[0]
        new_turn = self.turn
        if "TURN" in parts:
            idx = parts.index("TURN") + 1
            if idx < len(parts):
                new_turn = parts[idx]

        # Highlight every square that changed since the previous board.
        # For a single human move that is just {src, dst}. For a human-vs-AI
        # round trip that is 4 squares (our move + the AI's reply).
        if self.board == "." * 64:
            self.last_move_squares = set()
        else:
            self.last_move_squares = {
                i for i in range(64) if self.board[i] != new_board[i]
            }

        self.board = new_board
        self.turn = new_turn
        self.over = False
        self._refresh_ui()
        self._set_status(
            f"{'White' if self.turn == 'W' else 'Black'} to move"
        )

    def _end_game(self, reason):
        self.over = True
        self.selected = None
        self.legal_dests = set()
        self._refresh_ui()
        self._set_status(f"Game over — {reason}")

    def _show_nack(self, code):
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

    def on_close(self):
        try:
            if self.ser is not None:
                self.send("DISCONNECT")
                time.sleep(0.05)
        finally:
            self._close_serial()
            self.root.destroy()


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    root = tk.Tk()
    try:
        root.state("zoomed")
    except tk.TclError:
        try:
            root.attributes("-zoomed", True)
        except tk.TclError:
            sw, sh = root.winfo_screenwidth(), root.winfo_screenheight()
            root.geometry(f"{sw}x{sh}+0+0")

    ChessClient(root, port)
    root.mainloop()


if __name__ == "__main__":
    main()
