"""
3D Tic-Tac-Toe with Computer Vision Hand Gesture Recognition
============================================================
Requirements:
    pip install opencv-python mediapipe numpy
 
Model download (run once, then place next to game.py):
    curl -o hand_landmarker.task \
      https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task
 
Controls:
    SPACE  - Start game / restart
    ESC    - Quit
 
Game Flow per turn:
  1. Ready Check   → Thumbs Up to proceed
  2. Level Select  → Raise 1, 2, or 3 fingers
  3. Level Confirm → Thumbs Up to confirm  |  Cross arms to redo
  4. Grid Select   → Hover thumb over cell, Pinch to choose
  5. Grid Confirm  → Thumbs Up to place mark
"""
 
import cv2
import numpy as np
import mediapipe as mp
from mediapipe.tasks.python import vision as mp_vision
from mediapipe.tasks.python.core import base_options as mp_base
import time
import os
import sys
import serial
 
# ─────────────────────────────────────────────────────────────
# CONSTANTS
# ─────────────────────────────────────────────────────────────
MODEL_PATH  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hand_landmarker.task")
WIN_W, WIN_H = 1280, 720
FPS_TARGET   = 30
 
# Colors (BGR)
C_BG        = (15,  12,  10)
C_GRID      = (60,  60,  80)
C_GRID_HL   = (80, 200,  80)
C_P1        = (220, 160,  40)   # gold  – Player 1 (X)
C_P2        = (60,  100, 230)   # blue  – Player 2 (O)
C_TEXT      = (150, 150, 150)
C_DIM       = (90,  90,  90)
C_ACCENT    = (0,  200, 160)
C_WARN      = (30, 120, 240)
C_WIN       = (30, 220, 130)
 
# Gesture sustain thresholds (frames at ~30 fps)
SUSTAIN = {
    "thumbs_up":     35, #Time to reset
    "pinch":          40,  
    "finger_count":  40,   
    "forearm_cross": 40, 
}
 
PHASE_LABELS = {
    0: "START",
    1: "READY CHECK",
    2: "LEVEL SELECT",
    3: "LEVEL CONFIRM",
    4: "GRID SELECT",
    5: "PLACE CONFIRM",
    6: "GAME OVER",
}
 
 
# ─────────────────────────────────────────────────────────────
# GAME STATE
# ─────────────────────────────────────────────────────────────
class Game:
    def __init__(self):
        self.reset()
 
    def reset(self):
        # grid[level][row][col] : 0=empty, 1=P1, 2=P2
        self.grid           = [[[0]*3 for _ in range(3)] for _ in range(3)]
        self.current_player = 1
        self.current_level  = None   # 0-indexed (None until chosen each turn)
        self.hover_pos      = None   # (row,col) thumb is hovering over
        self.pending_pos    = None   # (row,col) selected by pinch, awaiting thumbs-up
        self.game_over      = False
        self.winner         = None   # 0=draw, 1 or 2
 
    def is_empty(self, level, row, col):
        return self.grid[level][row][col] == 0
 
    def make_move(self, level, row, col):
        if self.is_empty(level, row, col):
            self.grid[level][row][col] = self.current_player
            return True
        return False
 
    def check_win(self, player):
        g = self.grid
        # ── within a single level ────────────────────────────
        for lv in range(3):
            for r in range(3):
                if all(g[lv][r][c] == player for c in range(3)): return True
            for c in range(3):
                if all(g[lv][r][c] == player for r in range(3)): return True
            if all(g[lv][i][i]   == player for i in range(3)):   return True
            if all(g[lv][i][2-i] == player for i in range(3)):   return True
        # ── verticals through levels ────────────────────────
        for r in range(3):
            for c in range(3):
                if all(g[lv][r][c] == player for lv in range(3)): return True
        # ── space diagonals ──────────────────────────────────
        if all(g[i][i][i]     == player for i in range(3)): return True
        if all(g[i][i][2-i]   == player for i in range(3)): return True
        if all(g[i][2-i][i]   == player for i in range(3)): return True
        if all(g[i][2-i][2-i] == player for i in range(3)): return True
        return False
 
    def is_level_full(self, level):
        return all(self.grid[level][r][c] != 0 for r in range(3) for c in range(3))
 
    def is_board_full(self):
        return all(self.is_level_full(lv) for lv in range(3))
 
    def switch_player(self):
        self.current_player = 3 - self.current_player
 
 
# ─────────────────────────────────────────────────────────────
# SUSTAIN TRACKER
# ─────────────────────────────────────────────────────────────
class SustainTracker:
    def __init__(self):
        self._counts = {}
 
    def update(self, key, active):
        if active:
            self._counts[key] = self._counts.get(key, 0) + 1
        else:
            self._counts[key] = 0
        return self._counts[key]
 
    def reset_all(self):
        self._counts = {}
 
    def get(self, key):
        return self._counts.get(key, 0)
 
    def fired(self, key, active, threshold):
        """Returns True exactly once the sustain threshold is reached."""
        count = self.update(key, active)
        return count == threshold
 
 
# ─────────────────────────────────────────────────────────────
# HAND GESTURE DETECTOR
# ─────────────────────────────────────────────────────────────
class HandDetector:
    # Landmark indices
    WRIST      =  0
    THUMB_IP   =  3; THUMB_TIP  =  4
    INDEX_PIP  =  6; INDEX_TIP  =  8
    MIDDLE_PIP = 10; MIDDLE_TIP = 12
    RING_PIP   = 14; RING_TIP   = 16
    PINKY_PIP  = 18; PINKY_TIP  = 20
 
    def __init__(self, model_path):
        options = mp_vision.HandLandmarkerOptions(
            base_options=mp_base.BaseOptions(model_asset_path=model_path),
            running_mode=mp_vision.RunningMode.VIDEO,
            num_hands=2,
            min_hand_detection_confidence=0.6,
            min_hand_presence_confidence=0.5,
            min_tracking_confidence=0.5,
        )
        self.landmarker  = mp_vision.HandLandmarker.create_from_options(options)
        self.timestamp_ms = 0
 
    def detect(self, bgr_frame):
        rgb = cv2.cvtColor(bgr_frame, cv2.COLOR_BGR2RGB)
        mp_img = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        self.timestamp_ms += int(1000 / FPS_TARGET)
        return self.landmarker.detect_for_video(mp_img, self.timestamp_ms)
 
    # ── gesture logic ─────────────────────────────────────────
    @staticmethod
    def _up(hand, tip, pip):
        return hand[tip].y < hand[pip].y
 
    def is_thumbs_up(self, hand):
        h = hand
        return (
            h[self.THUMB_TIP].y  < h[self.THUMB_IP].y  and
            h[self.INDEX_TIP].y  > h[self.INDEX_PIP].y  and
            h[self.MIDDLE_TIP].y > h[self.MIDDLE_PIP].y and
            h[self.RING_TIP].y   > h[self.RING_PIP].y   and
            h[self.PINKY_TIP].y  > h[self.PINKY_PIP].y
        )
 
    def count_fingers(self, hand):
        h = hand
        count = 0
        if h[self.THUMB_TIP].y  < h[self.THUMB_IP].y:   count += 1
        if h[self.INDEX_TIP].y  < h[self.INDEX_PIP].y:  count += 1
        if h[self.MIDDLE_TIP].y < h[self.MIDDLE_PIP].y: count += 1
        if h[self.RING_TIP].y   < h[self.RING_PIP].y:   count += 1
        if h[self.PINKY_TIP].y  < h[self.PINKY_PIP].y:  count += 1
        return count
 
    def is_pinch(self, hand, fw, fh, threshold=45):
        tx = int(hand[self.THUMB_TIP].x  * fw)
        ty = int(hand[self.THUMB_TIP].y  * fh)
        ix = int(hand[self.INDEX_TIP].x  * fw)
        iy = int(hand[self.INDEX_TIP].y  * fh)
        return ((tx-ix)**2 + (ty-iy)**2) ** 0.5 < threshold
 
    def thumb_px(self, hand, fw, fh):
        return int(hand[self.THUMB_TIP].x * fw), int(hand[self.THUMB_TIP].y * fh)
 
    def are_fingers_crossed(self, result, fw):
        """Detect crossed index fingers: both index tips close together."""
        if len(result.hand_landmarks) < 2:
            return False
        h0 = result.hand_landmarks[0]
        h1 = result.hand_landmarks[1]
        ix0 = h0[self.INDEX_TIP].x * fw
        iy0 = h0[self.INDEX_TIP].y
        ix1 = h1[self.INDEX_TIP].x * fw
        iy1 = h1[self.INDEX_TIP].y
        dist = ((ix0 - ix1)**2 + ((iy0 - iy1) * 720)**2) ** 0.5
        return dist < 60
 
 
# ─────────────────────────────────────────────────────────────
# UI RENDERER
# ─────────────────────────────────────────────────────────────
FONT       = cv2.FONT_HERSHEY_DUPLEX
FONT_BOLD  = cv2.FONT_HERSHEY_TRIPLEX
 
def txt(frame, s, x, y, scale=0.65, color=C_TEXT, thick=1, font=None):
    cv2.putText(frame, s, (x, y), font or FONT, scale, color, thick, cv2.LINE_AA)
 
def ctxt(frame, s, y, scale=0.9, color=C_TEXT, thick=1):
    (tw, _), _ = cv2.getTextSize(s, FONT_BOLD, scale, thick)
    x = (WIN_W - tw) // 2
    cv2.putText(frame, s, (x, y), FONT_BOLD, scale, color, thick, cv2.LINE_AA)
 
 
class UI:
    CONNECTIONS = [
        (0,1),(1,2),(2,3),(3,4),
        (0,5),(5,6),(6,7),(7,8),
        (5,9),(9,10),(10,11),(11,12),
        (9,13),(13,14),(14,15),(15,16),
        (13,17),(17,18),(18,19),(19,20),
        (0,17),
    ]
 
    def draw_skeleton(self, frame, result):
        fh, fw = frame.shape[:2]
        for hand in result.hand_landmarks:
            pts = [(int(lm.x*fw), int(lm.y*fh)) for lm in hand]
            for a, b in self.CONNECTIONS:
                cv2.line(frame, pts[a], pts[b], C_ACCENT, 1, cv2.LINE_AA)
            for p in pts:
                cv2.circle(frame, p, 3, C_TEXT, -1)
 
    def hud(self, frame, game, phase, frac=0.0, msg="", warn=""):
        pcolor = C_P1 if game.current_player == 1 else C_P2
        cv2.rectangle(frame, (0, 0), (WIN_W, 52), (18, 18, 26), -1)
        txt(frame, f"PLAYER {game.current_player}", 12, 36, 0.9, pcolor, 2)
 
        ph_lbl = PHASE_LABELS.get(phase, "")
        (tw,_),_ = cv2.getTextSize(ph_lbl, FONT, 0.5, 1)
        txt(frame, ph_lbl, (WIN_W-tw)//2, 34, 0.5, C_DIM)
 
        if game.current_level is not None:
            txt(frame, f"Level  {game.current_level+1}", WIN_W-140, 34, 0.75, C_ACCENT)
 
        if frac > 0:
            cv2.rectangle(frame, (0, 50), (int(WIN_W*frac), 53), C_ACCENT, -1)
 
        if msg:
            ctxt(frame, msg, 86, 0.7, C_TEXT, 1)
        if warn:
            ctxt(frame, warn, 116, 0.58, C_WARN, 1)
 
    def _x(self, frame, cx, cy, r, color):
        cv2.line(frame, (cx-r, cy-r), (cx+r, cy+r), color, 2, cv2.LINE_AA)
        cv2.line(frame, (cx+r, cy-r), (cx-r, cy+r), color, 2, cv2.LINE_AA)
 
    def _o(self, frame, cx, cy, r, color):
        cv2.circle(frame, (cx, cy), r, color, 2, cv2.LINE_AA)
 
    def all_levels(self, frame, game, active_lv=None, hover=None, pending=None):
        """Three small grids at the bottom."""
        cell = 60; gap = 28
        total = 3*(3*cell+gap) - gap
        sx = (WIN_W - total)//2
        sy = WIN_H - 3*cell - 55
 
        for lv in range(3):
            ox = sx + lv*(3*cell+gap)
            oy = sy
            active = (lv == active_lv)
            bc = C_ACCENT if active else C_GRID
 
            lbl = f"L{lv+1}" + (" <" if active else "")
            txt(frame, lbl, ox+4, oy-8, 0.45, C_ACCENT if active else C_DIM)
 
            for r in range(3):
                for c in range(3):
                    cx2, cy2 = ox+c*cell, oy+r*cell
                    val = game.grid[lv][r][c]
                    bg  = (32, 32, 42)
                    if active and hover  == (r,c) and val == 0: bg = (45, 65, 45)
                    if active and pending== (r,c):               bg = (35, 55, 55)
                    cv2.rectangle(frame, (cx2+1,cy2+1),(cx2+cell-2,cy2+cell-2), bg, -1)
                    edge = C_GRID_HL if active and hover==(r,c) else bc
                    cv2.rectangle(frame, (cx2,cy2),(cx2+cell,cy2+cell), edge, 1)
                    mid = cx2+cell//2, cy2+cell//2
                    if val == 1: self._x(frame, *mid, cell//2-10, C_P1)
                    elif val==2: self._o(frame, *mid, cell//2-12, C_P2)
 
    def big_grid(self, frame, game, lv, hover=None, pending=None):
        """Large interactive grid in the centre."""
        cell = 150
        ox   = (WIN_W - 3*cell)//2
        oy   = (WIN_H - 3*cell)//2 + 20
 
        for r in range(3):
            for c in range(3):
                cx2, cy2 = ox+c*cell, oy+r*cell
                val = game.grid[lv][r][c]
 
                bg = (26, 26, 36)
                if (r,c)==hover   and val==0: bg = (38, 60, 38)
                if (r,c)==pending and val==0: bg = (28, 52, 52)
                if val != 0:                  bg = (20, 20, 28)
                cv2.rectangle(frame, (cx2+2,cy2+2),(cx2+cell-3,cy2+cell-3), bg, -1)
 
                bc = C_GRID_HL if (r,c)==hover else C_GRID
                cv2.rectangle(frame, (cx2,cy2),(cx2+cell,cy2+cell), bc, 2)
 
                mid = cx2+cell//2, cy2+cell//2
                if val == 1: self._x(frame, *mid, 50, C_P1)
                elif val==2: self._o(frame, *mid, 50, C_P2)
 
        return ox, oy, cell
 
    def start_menu(self, frame):
        ov = np.zeros_like(frame); cv2.addWeighted(ov, 0.55, frame, 0.45, 0, frame)
        ctxt(frame, "3D  TIC-TAC-TOE",    WIN_H//2-80, 2.0, C_ACCENT, 3)
        ctxt(frame, "Computer Vision Edition", WIN_H//2-16, 0.72, C_DIM, 1)
        ctxt(frame, "Press  SPACE  to Play",    WIN_H//2+55, 1.0, C_TEXT, 2)
        ctxt(frame, "ESC to quit",               WIN_H//2+105, 0.48, C_DIM, 1)
 
    def game_over(self, frame, game):
        ov = np.zeros_like(frame); cv2.addWeighted(ov, 0.5, frame, 0.5, 0, frame)
        if game.winner == 0:
            ctxt(frame, "DRAW!",             WIN_H//2-40, 2.4, C_DIM, 3)
        else:
            pc = C_P1 if game.winner==1 else C_P2
            ctxt(frame, f"PLAYER {game.winner}  WINS!", WIN_H//2-40, 2.0, pc, 3)
        ctxt(frame, "Press  SPACE  to play again", WIN_H//2+55, 0.8, C_TEXT, 1)
 
    def legend(self, frame, phase):
        tips = {
            1: "Thumbs-up = READY",
            2: "Raise 1, 2 or 3 fingers = choose level",
            3: "Thumbs-up = confirm  |  Cross arms = redo",
            4: "Hover thumb over cell · Pinch = select",
            5: "Thumbs-up = place mark",
        }.get(phase, "")
        if tips:
            txt(frame, tips, 14, WIN_H-18, 0.46, C_DIM)
 
 
# ─────────────────────────────────────────────────────────────
# UTILITY
# ─────────────────────────────────────────────────────────────
def thumb_to_cell(tx, ty, ox, oy, cell):
    col = (tx - ox) // cell
    row = (ty - oy) // cell
    if 0 <= row < 3 and 0 <= col < 3:
        return int(row), int(col)
    return None
 
 
# ─────────────────────────────────────────────────────────────
# MAIN
# ─────────────────────────────────────────────────────────────
def main():
    if not os.path.exists(MODEL_PATH):
        print("\n[ERROR] hand_landmarker.task not found in the same folder as game.py!\n")
        print("Download it with:")
        print("  curl -o hand_landmarker.task \\")
        print("    https://storage.googleapis.com/mediapipe-models/"
              "hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task\n")
        sys.exit(1)
 
    cap = cv2.VideoCapture(0)
    ser = serial.Serial('COM3', 115200)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  WIN_W)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, WIN_H)
 
    cv2.namedWindow("3D Tic-Tac-Toe", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("3D Tic-Tac-Toe", WIN_W, WIN_H)
 
    detector = HandDetector(MODEL_PATH)
    game     = Game()
    ui       = UI()
    sustain  = SustainTracker()
 
    phase           = 0
    level_candidate = None   # 1-indexed, chosen in phase 2
    grid_geom       = None   # (ox, oy, cell) from big_grid()
    warn_msg        = ""
    warn_exp        = 0.0
 
    def set_warn(msg, secs=2.5):
        nonlocal warn_msg, warn_exp
        warn_msg = msg; warn_exp = time.time() + secs
 
    def go(p):
        nonlocal phase, warn_msg, warn_exp
        phase = p
        sustain.reset_all()
        warn_msg = ""; warn_exp = 0.0
 
    # ── main loop ─────────────────────────────────────────────
    while True:
        ret, frame = cap.read()
        if not ret: break
        frame = cv2.flip(frame, 1)
        fh, fw = frame.shape[:2]
 
        key = cv2.waitKey(1) & 0xFF
        if key == 27: break
        if key == 32 and phase in (0, 6):
            game.reset(); go(1)
 
        # ── detect ───────────────────────────────────────────
        result   = detector.detect(frame)
        has_hand = len(result.hand_landmarks) > 0
        hand0    = result.hand_landmarks[0] if has_hand else None
 
        if warn_exp and time.time() > warn_exp:
            warn_msg = ""; warn_exp = 0.0
 
        # ══════════════════════════════════════════════════════
        # PHASES
        # ══════════════════════════════════════════════════════
 
        # ── 0: Start menu ────────────────────────────────────
        if phase == 0:
            ui.start_menu(frame)
 
        # ── 1: Ready check ───────────────────────────────────
        elif phase == 1:
            thu  = has_hand and detector.is_thumbs_up(hand0)
            frac = sustain.get("thumbs_up") / SUSTAIN["thumbs_up"]
            ui.hud(frame, game, phase, min(frac,1),
                   f"Player {game.current_player} - Thumbs Up when ready!", warn_msg)
            ui.all_levels(frame, game)
            ui.legend(frame, phase)
            if has_hand: ui.draw_skeleton(frame, result)
            if sustain.fired("thumbs_up", thu, SUSTAIN["thumbs_up"]):
                go(2)
 
        # ── 2: Level selection ───────────────────────────────
        elif phase == 2:
            fc    = detector.count_fingers(hand0) if has_hand else 0
            valid = has_hand and 1 <= fc <= 3
            frac  = sustain.get("finger_count") / SUSTAIN["finger_count"]
            sub   = f"Fingers detected: {fc}" if has_hand else "No hand visible"
            ui.hud(frame, game, phase, min(frac,1),
                   f"Player {game.current_player} - Raise 1, 2 or 3 fingers", sub)
            ui.all_levels(frame, game)
            ui.legend(frame, phase)
            if has_hand: ui.draw_skeleton(frame, result)
            if sustain.fired("finger_count", valid, SUSTAIN["finger_count"]):
                if 1 <= fc <= 3:
                    level_candidate = fc
                    go(3)
                else:
                    set_warn("Show exactly 1, 2 or 3 fingers!")
 
        # ── 3: Level confirmation ────────────────────────────
        elif phase == 3:
            lv_idx = level_candidate - 1
            if game.is_level_full(lv_idx):
                set_warn(f"Level {level_candidate} is full! Pick another.")
                go(2)
            else:
                thu  = has_hand and detector.is_thumbs_up(hand0)
                crs  = detector.are_fingers_crossed(result, fw)
                ft   = sustain.get("thumbs_up")    / SUSTAIN["thumbs_up"]
                fc2  = sustain.get("forearm_cross") / SUSTAIN["forearm_cross"]
                frac = max(ft, fc2)
                ui.hud(frame, game, phase, min(frac,1),
                       f"Level {level_candidate} — Thumbs Up (GOOD) | Cross forefingers (RESELECT)", warn_msg)
                ui.all_levels(frame, game, active_lv=lv_idx)
                ui.legend(frame, phase)
                if has_hand: ui.draw_skeleton(frame, result)
                if sustain.fired("thumbs_up", thu, SUSTAIN["thumbs_up"]):
                    game.current_level = lv_idx; go(4)
                elif sustain.fired("forearm_cross", crs, SUSTAIN["forearm_cross"]):
                    go(2)
 
        # ── 4: Grid selection (thumb hover + pinch) ──────────
        elif phase == 4:
            lv = game.current_level
            grid_geom = ui.big_grid(frame, game, lv,
                                    hover=game.hover_pos, pending=game.pending_pos)
            ox, oy, cell = grid_geom
 
            if has_hand:
                tx, ty = detector.thumb_px(hand0, fw, fh)
                cell_rc = thumb_to_cell(tx, ty, ox, oy, cell)
                # only hover empty cells
                if cell_rc and not game.is_empty(lv, *cell_rc):
                    cell_rc = None
                game.hover_pos = cell_rc
 
                pinched = detector.is_pinch(hand0, fw, fh)
                valid_p = pinched and game.hover_pos is not None
                fp      = sustain.get("pinch") / SUSTAIN["pinch"]
                ui.hud(frame, game, phase, min(fp,1),
                       f"Player {game.current_player} - Hover thumb - Pinch to select", warn_msg)
                ui.draw_skeleton(frame, result)
                # draw thumb cursor dot
                cv2.circle(frame, (tx, ty), 10, C_ACCENT, 2, cv2.LINE_AA)
                if sustain.fired("pinch", valid_p, SUSTAIN["pinch"]):
                    if game.hover_pos:
                        game.pending_pos = game.hover_pos; go(5)
                    else:
                        set_warn("Point thumb at an empty cell first!")
            else:
                game.hover_pos = None
                ui.hud(frame, game, phase, 0,
                       f"Player {game.current_player} - Hover thumb - Pinch to select",
                       "No hand detected")
 
            ui.all_levels(frame, game, active_lv=lv,
                          hover=game.hover_pos, pending=game.pending_pos)
            ui.legend(frame, phase)
 
        # ── 5: Place confirmation (thumbs-up) ────────────────
        elif phase == 5:
            lv      = game.current_level
            pr, pc2 = game.pending_pos
            if grid_geom:
                ui.big_grid(frame, game, lv, pending=game.pending_pos)
 
            thu  = has_hand and detector.is_thumbs_up(hand0)
            frac = sustain.get("thumbs_up") / SUSTAIN["thumbs_up"]
            ui.hud(frame, game, phase, min(frac,1),
                   f"Place at row {pr+1}, col {pc2+1}? Thumbs Up ✓", warn_msg)
            ui.all_levels(frame, game, active_lv=lv, pending=game.pending_pos)
            ui.legend(frame, phase)
            if has_hand: ui.draw_skeleton(frame, result)
 
            if sustain.fired("thumbs_up", thu, SUSTAIN["thumbs_up"]):
                if game.make_move(lv, pr, pc2):
                    flat = [game.grid[lv][r][c] for lv in range(3) for r in range(3) for c in range(3)]
                    ser.write(bytes(flat))
                    game.hover_pos = game.pending_pos = None
                    if game.check_win(game.current_player):
                        game.game_over = True
                        game.winner    = game.current_player
                        go(6)
                    elif game.is_board_full():
                        game.game_over = True
                        game.winner    = 0
                        go(6)
                    else:
                        game.current_level = None
                        game.switch_player()
                        go(1)
                else:
                    set_warn("Cell already taken! Reselect.")
                    go(4)
 
        # ── 6: Game over ─────────────────────────────────────
        elif phase == 6:
            ui.all_levels(frame, game)
            ui.game_over(frame, game)
 
        # ── always: fps indicator ────────────────────────────
        txt(frame, f"{cap.get(cv2.CAP_PROP_FPS):.0f}fps",
            WIN_W-65, WIN_H-10, 0.4, C_DIM)
 
        cv2.imshow("3D Tic-Tac-Toe", frame)
 
    cap.release()
    cv2.destroyAllWindows()
 
 
if __name__ == "__main__":
    main()
 