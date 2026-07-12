#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════════════════════╗
║  K.A.R.E.N. HUD — THE SPIDER-SENSE SENTRY (ESP32-CAM Edition)              ║
║                                                                            ║
║  Trigger sources:                                                          ║
║    1. CLAP / audio anomaly — Master ESP32 sends [TINGLE] → auto-capture   ║
║    2. SPACEBAR or on-screen FIRE button — sends [CAPTURE] over Serial      ║
║    3. Type [CAPTURE] in PC terminal directly into Serial Monitor            ║
║                                                                            ║
║  Usage:                                                                    ║
║    python KAREN.py --port COMx                                    ║
╚══════════════════════════════════════════════════════════════════════════════╝
"""

import sys
import time
import argparse
import threading
import serial
import serial.tools.list_ports
import pygame
import cv2
import numpy as np

# ═══════════════════════════════════════════════════════════════════════════════
# CONFIGURATION
# ═══════════════════════════════════════════════════════════════════════════════
WEB_TETHER_BAUD = 921600
WINDOW_WIDTH    = 1280
WINDOW_HEIGHT   = 800
FPS             = 60

# ── Monochrome tactical palette ───────────────────────────────────────────────
COLOR_BG             = (6,  6,  6)       # near-black canvas
COLOR_BG_PANEL       = (12, 12, 12)      # panel background
COLOR_BG_PANEL_DARK  = (8,  8,  8)       # deeper panel background
COLOR_GRID           = (22, 22, 22)      # subtle dot grid
COLOR_TEXT           = (210, 210, 210)   # primary text
COLOR_TEXT_DIM       = (90,  90,  90)    # secondary / labels
COLOR_TEXT_BRIGHT    = (255, 255, 255)   # headings / highlights

# Accent — single pure white + strict status colours, no purple/blue
COLOR_ACCENT         = (255, 255, 255)   # white accent / borders bright
COLOR_BORDER         = (32,  32,  32)    # panel borders
COLOR_BORDER_BRIGHT  = (80,  80,  80)    # active panel borders

# Status colours (kept functional, muted so they pop against black)
COLOR_THREAT_CRIT    = (230,  40,  40)   # red
COLOR_THREAT_HIGH    = (220, 120,  20)   # amber
COLOR_THREAT_MED     = (200, 180,   0)   # yellow
COLOR_THREAT_SECURE  = (60,  200, 100)   # green

# Button
COLOR_TRIGGER_BTN    = (22,  22,  22)
COLOR_TRIGGER_HOV    = (36,  36,  36)
COLOR_TRIGGER_PRESS  = (52,  52,  52)

# Tingle flash — single sharp white strobe, no spider-red
COLOR_TINGLE         = (255, 255, 255)

FFT_BAR_COUNT    = 64
FFT_PANEL_HEIGHT = 130
FFT_MAX_VALUE    = 2500
TRIGGER_LOG_MAX  = 6

# ═══════════════════════════════════════════════════════════════════════════════
# SHARED STATE
# ═══════════════════════════════════════════════════════════════════════════════
class SharedState:
    def __init__(self):
        self.lock = threading.Lock()
        self.fft_bins = [0] * FFT_BAR_COUNT
        self.camera_surface = None
        self.threat_string = "WAITING"
        self.threat_level = -1
        self.faces_found = 0
        self.serial_connected = False
        self.ser_ref = None
        self.last_capture_time = 0
        self.capture_count = 0
        self.trigger_log = []
        self.tingle_flash = 0
        self.last_frame_size = 0
        self.last_frame_time_ms = 0

    def _add_log(self, msg, color=None):
        if color is None:
            color = COLOR_TEXT_DIM
        ts = time.strftime("%H:%M:%S")
        self.trigger_log.append((ts, msg, color))
        if len(self.trigger_log) > TRIGGER_LOG_MAX:
            self.trigger_log.pop(0)

    def update_fft(self, bins):
        with self.lock:
            self.fft_bins = bins[:]

    def update_camera_jpeg(self, jpeg_bytes, rx_time_ms=0):
        try:
            nparr = np.frombuffer(jpeg_bytes, np.uint8)
            img_np = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            if img_np is not None:
                frame_rgb = cv2.cvtColor(img_np, cv2.COLOR_BGR2RGB)
                surface = pygame.surfarray.make_surface(frame_rgb.swapaxes(0, 1))
                with self.lock:
                    self.camera_surface = surface
                    self.last_frame_size = len(jpeg_bytes)
                    self.last_frame_time_ms = rx_time_ms
                    self._add_log(f"Frame recv: {len(jpeg_bytes)//1024:.1f} KB  {rx_time_ms} ms",
                                  COLOR_THREAT_SECURE)
        except Exception as e:
            with self.lock:
                self._add_log(f"JPEG decode error: {e}", COLOR_THREAT_CRIT)

    def update_threat(self, level, faces):
        with self.lock:
            self.threat_level = level
            self.faces_found = faces
            labels = {-1:"ERROR", 0:"SECURE", 1:"GUARDED", 2:"ELEVATED",
                      3:"HIGH", 4:"SEVERE", 5:"CRITICAL"}
            self.threat_string = labels.get(level, "UNKNOWN")
            color = COLOR_THREAT_CRIT if level >= 4 else \
                    COLOR_THREAT_HIGH  if level == 3 else \
                    COLOR_THREAT_MED   if level == 2 else \
                    COLOR_THREAT_SECURE
            self._add_log(f"Threat → {self.threat_string}  faces={faces}", color)

    def trigger_capture(self, source="MANUAL"):
        with self.lock:
            if self.ser_ref and self.serial_connected:
                try:
                    self.ser_ref.write(b"[CAPTURE]\n")
                    self.capture_count += 1
                    self.last_capture_time = time.time()
                    self._add_log(f"[CAPTURE] sent via {source}", COLOR_TEXT_BRIGHT)
                    return True
                except Exception as e:
                    self._add_log(f"Send failed: {e}", COLOR_THREAT_CRIT)
                    return False
            else:
                self._add_log("No serial — trigger ignored", COLOR_THREAT_CRIT)
                return False

    def mark_tingle(self):
        with self.lock:
            self.tingle_flash = 45
            self._add_log("SPIDER-SENSE TINGLE!", COLOR_TEXT_BRIGHT)

    def get_data(self):
        with self.lock:
            return {
                "fft": self.fft_bins[:],
                "cam": self.camera_surface,
                "threat": self.threat_string,
                "threat_level": self.threat_level,
                "faces": self.faces_found,
                "conn": self.serial_connected,
                "cap_count": self.capture_count,
                "last_cap": self.last_capture_time,
                "log": list(self.trigger_log),
                "tingle_flash": self.tingle_flash,
                "frame_size": self.last_frame_size,
                "frame_time": self.last_frame_time_ms,
            }

    def tick_tingle(self):
        with self.lock:
            if self.tingle_flash > 0:
                self.tingle_flash -= 1

# ═══════════════════════════════════════════════════════════════════════════════
# THREAD: Serial Parser
# ═══════════════════════════════════════════════════════════════════════════════
class SerialListener(threading.Thread):
    def __init__(self, port, baud, shared_state):
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.state = shared_state
        self.running = True

    def run(self):
        try:
            ser = serial.Serial(self.port, self.baud, timeout=0.05)
            with self.state.lock:
                self.state.serial_connected = True
                self.state.ser_ref = ser
                self.state._add_log(f"Serial {self.port} @ {self.baud}", COLOR_THREAT_SECURE)
            print(f"[TETHER] Connected: {self.port} @ {self.baud} baud")
        except Exception as e:
            print(f"[TETHER] ERROR: {e}")
            with self.state.lock:
                self.state._add_log(f"Serial failed: {e}", COLOR_THREAT_CRIT)
            return

        buf = bytearray()
        frame_rx_start = 0

        while self.running:
            try:
                waiting = ser.in_waiting
                if waiting > 0:
                    buf.extend(ser.read(min(waiting, 4096)))
                else:
                    chunk = ser.read(1)
                    if chunk:
                        buf.extend(chunk)

                while True:
                    frame_idx = buf.find(b'[FRAME_S]')
                    nl_idx    = buf.find(b'\n')
                    has_frame = frame_idx >= 0
                    has_line  = nl_idx >= 0

                    if has_frame and (not has_line or frame_idx < nl_idx):
                        need = frame_idx + 9 + 4
                        if len(buf) < need:
                            break
                        jpeg_len = int.from_bytes(buf[frame_idx+9:frame_idx+13], 'big')
                        if jpeg_len == 0 or jpeg_len > 200_000:
                            buf = buf[frame_idx+9:]
                            continue
                        total_need = frame_idx + 13 + jpeg_len + 9
                        if len(buf) < total_need:
                            break
                        jpeg_data  = bytes(buf[frame_idx+13 : frame_idx+13+jpeg_len])
                        end_marker = bytes(buf[frame_idx+13+jpeg_len : frame_idx+13+jpeg_len+9])
                        rx_ms = int((time.time() - frame_rx_start) * 1000) if frame_rx_start else 0
                        if end_marker == b'[FRAME_E]':
                            self.state.update_camera_jpeg(jpeg_data, rx_ms)
                        else:
                            with self.state.lock:
                                self.state._add_log("Frame end-marker mismatch", COLOR_THREAT_CRIT)
                        buf = buf[frame_idx+13+jpeg_len+9:]
                        frame_rx_start = 0
                        continue

                    elif has_line:
                        raw_line = buf[:nl_idx]
                        buf = buf[nl_idx+1:]
                        line = raw_line.decode('utf-8', errors='ignore').strip()
                        if not line:
                            continue
                        if b'[FRAME_S]' in raw_line:
                            frame_rx_start = time.time()
                        if line == "[TINGLE]":
                            self.state.mark_tingle()
                            print("[K.A.R.E.N.] TINGLE!")
                        elif line.startswith("[FFT]") and "[/FFT]" in line:
                            csv = line[5:line.index("[/FFT]")]
                            try:
                                raw = [int(x) for x in csv.split(",") if x.strip()]
                                if len(raw) >= FFT_BAR_COUNT:
                                    step = len(raw) // FFT_BAR_COUNT
                                    bars = [sum(raw[i*step:(i+1)*step])//step
                                            for i in range(FFT_BAR_COUNT)]
                                    self.state.update_fft(bars)
                            except:
                                pass
                        elif line.startswith("[CAM_THREAT]"):
                            level, faces = 0, 0
                            for part in line.split():
                                if part.startswith("level="): level = int(part.split("=")[1])
                                if part.startswith("faces="): faces = int(part.split("=")[1])
                            self.state.update_threat(level, faces)
                        else:
                            print(f"ESP32> {line}")
                        continue
                    else:
                        break

            except Exception as e:
                print(f"[TETHER] Exception: {e}")
                time.sleep(0.2)

        try:
            ser.close()
        except:
            pass

# ═══════════════════════════════════════════════════════════════════════════════
# PYGAME HELPERS
# ═══════════════════════════════════════════════════════════════════════════════
def draw_panel(surf, rect, bg=COLOR_BG_PANEL, border=COLOR_BORDER, radius=2):
    pygame.draw.rect(surf, bg, rect, border_radius=radius)
    pygame.draw.rect(surf, border, rect, 1, border_radius=radius)

def draw_label(surf, font, text, x, y, color=COLOR_TEXT_DIM):
    s = font.render(text, True, color)
    surf.blit(s, (x, y))
    return s.get_width()

def draw_rule(surf, rect_or_y, x1=None, x2=None, color=COLOR_BORDER):
    """Draw a 1px horizontal rule."""
    if isinstance(rect_or_y, int):
        pygame.draw.line(surf, color, (x1, rect_or_y), (x2, rect_or_y))
    else:
        r = rect_or_y
        pygame.draw.line(surf, color, (r.x, r.bottom), (r.right, r.bottom))

def threat_color(level):
    if level >= 4: return COLOR_THREAT_CRIT
    if level == 3: return COLOR_THREAT_HIGH
    if level == 2: return COLOR_THREAT_MED
    if level == 0: return COLOR_THREAT_SECURE
    return COLOR_TEXT_DIM

# ═══════════════════════════════════════════════════════════════════════════════
# MAIN DISPLAY
# ═══════════════════════════════════════════════════════════════════════════════
class SentryDisplay:
    def __init__(self, shared_state):
        self.state = shared_state
        self.smooth_fft = [0.0] * FFT_BAR_COUNT
        self.frame_count = 0
        self.btn_hover = False
        self.btn_press_frames = 0

    def run(self):
        pygame.init()
        screen = pygame.display.set_mode((WINDOW_WIDTH, WINDOW_HEIGHT))
        pygame.display.set_caption("K.A.R.E.N. — Spider-Sense Sentry")
        clock = pygame.time.Clock()

        # ── Fonts: monospace for HUD feel ────────────────────────────────────
        try:
            font_huge  = pygame.font.SysFont("Consolas", 48, bold=True)
            font_large = pygame.font.SysFont("Consolas", 22, bold=True)
            font_med   = pygame.font.SysFont("Consolas", 15, bold=True)
            font_small = pygame.font.SysFont("Consolas", 12)
            font_tiny  = pygame.font.SysFont("Consolas", 10)
        except:
            font_huge  = pygame.font.SysFont("monospace", 48, bold=True)
            font_large = pygame.font.SysFont("monospace", 22, bold=True)
            font_med   = pygame.font.SysFont("monospace", 15, bold=True)
            font_small = pygame.font.SysFont("monospace", 12)
            font_tiny  = pygame.font.SysFont("monospace", 10)

        # ── Layout ───────────────────────────────────────────────────────────
        HEADER_H  = 40
        FOOTER_H  = FFT_PANEL_HEIGHT
        BODY_TOP  = HEADER_H + 1
        BODY_H    = WINDOW_HEIGHT - HEADER_H - FOOTER_H - 2
        BODY_BOT  = BODY_TOP + BODY_H

        CAM_W     = int(WINDOW_WIDTH * 0.63)
        CAM_H     = BODY_H
        CAM_RECT  = pygame.Rect(0, BODY_TOP, CAM_W, CAM_H)

        SIDE_X    = CAM_RECT.right + 1
        SIDE_W    = WINDOW_WIDTH - SIDE_X
        SIDE_H    = BODY_H

        THREAT_H  = 162
        STATUS_H  = 86
        BTN_H     = 68
        LOG_H     = SIDE_H - THREAT_H - STATUS_H - BTN_H - 3

        THREAT_RECT = pygame.Rect(SIDE_X, BODY_TOP,               SIDE_W, THREAT_H)
        STATUS_RECT = pygame.Rect(SIDE_X, BODY_TOP+THREAT_H+1,    SIDE_W, STATUS_H)
        BTN_RECT    = pygame.Rect(SIDE_X, STATUS_RECT.bottom+1,   SIDE_W, BTN_H)
        LOG_RECT    = pygame.Rect(SIDE_X, BTN_RECT.bottom+1,      SIDE_W, LOG_H)

        FFT_RECT  = pygame.Rect(0, BODY_BOT+1, WINDOW_WIDTH, FOOTER_H-2)

        # Pre-build scanline overlay for camera panel
        scanline_surf = pygame.Surface((CAM_W, CAM_H), pygame.SRCALPHA)
        for y in range(0, CAM_H, 4):
            pygame.draw.line(scanline_surf, (0, 0, 0, 28), (0, y), (CAM_W, y))

        running = True
        while running:
            self.frame_count += 1
            self.state.tick_tingle()

            mx, my = pygame.mouse.get_pos()
            self.btn_hover = BTN_RECT.collidepoint(mx, my)
            if self.btn_press_frames > 0:
                self.btn_press_frames -= 1

            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    running = False
                elif event.type == pygame.KEYDOWN:
                    if event.key in (pygame.K_SPACE, pygame.K_RETURN):
                        self.state.trigger_capture("KEYBOARD")
                        self.btn_press_frames = 14
                elif event.type == pygame.MOUSEBUTTONDOWN:
                    if event.button == 1 and self.btn_hover:
                        self.state.trigger_capture("BUTTON")
                        self.btn_press_frames = 14

            data = self.state.get_data()
            screen.fill(COLOR_BG)

            # ── Faint dot grid ────────────────────────────────────────────
            for gx in range(0, WINDOW_WIDTH, 28):
                for gy in range(BODY_TOP, BODY_BOT, 28):
                    screen.set_at((gx, gy), COLOR_GRID)

            # ══════════════════════════════════════════════════════════════
            # HEADER — single bar
            # ══════════════════════════════════════════════════════════════
            pygame.draw.rect(screen, COLOR_BG_PANEL, (0, 0, WINDOW_WIDTH, HEADER_H))
            # Thin white bottom border on header
            pygame.draw.line(screen, (40, 40, 40), (0, HEADER_H-1), (WINDOW_WIDTH, HEADER_H-1))

            # Left: system name
            name_s = font_large.render("K.A.R.E.N.  SPIDER-SENSE SENTRY", True, COLOR_TEXT_BRIGHT)
            screen.blit(name_s, (14, (HEADER_H - name_s.get_height()) // 2))

            # Right: connection pill + clock
            conn_color = COLOR_THREAT_SECURE if data["conn"] else COLOR_THREAT_CRIT
            conn_str   = "● TETHER LIVE" if data["conn"] else "● NO TETHER"
            conn_s = font_small.render(conn_str, True, conn_color)
            ts_s   = font_small.render(time.strftime("%H:%M:%S"), True, COLOR_TEXT_DIM)
            rx = WINDOW_WIDTH - conn_s.get_width() - 14
            ty = (HEADER_H - conn_s.get_height()) // 2
            screen.blit(conn_s, (rx, ty))
            screen.blit(ts_s,   (rx - ts_s.get_width() - 18, ty))

            # Vertical divider between header sections
            pygame.draw.line(screen, COLOR_BORDER,
                             (rx - ts_s.get_width() - 26, 8),
                             (rx - ts_s.get_width() - 26, HEADER_H - 8))

            # ══════════════════════════════════════════════════════════════
            # CAMERA PANEL
            # ══════════════════════════════════════════════════════════════
            tingle_active = data["tingle_flash"] > 0
            cam_border = (200, 200, 200) if (tingle_active and self.frame_count % 6 < 3) \
                        else (50, 50, 50) if data["cam"] else COLOR_BORDER

            draw_panel(screen, CAM_RECT, bg=(0, 0, 0), border=cam_border, radius=0)

            if data["cam"]:
                cw, ch = data["cam"].get_size()
                scale  = min((CAM_RECT.width  - 2) / cw,
                             (CAM_RECT.height - 2) / ch)
                sw, sh = int(cw * scale), int(ch * scale)
                scaled = pygame.transform.smoothscale(data["cam"], (sw, sh))
                cx = CAM_RECT.x + (CAM_RECT.width  - sw) // 2
                cy = CAM_RECT.y + (CAM_RECT.height - sh) // 2
                screen.blit(scaled, (cx, cy))

                # Scanline overlay
                screen.blit(scanline_surf, (CAM_RECT.x, CAM_RECT.y))

                # Crosshair — white, thin
                mid_x = cx + sw // 2
                mid_y = cy + sh // 2
                pygame.draw.line(screen, (255, 255, 255, 60),
                                 (mid_x, cy + 4), (mid_x, cy + sh - 4), 1)
                pygame.draw.line(screen, (255, 255, 255, 60),
                                 (cx + 4, mid_y), (cx + sw - 4, mid_y), 1)
                pygame.draw.circle(screen, (255, 255, 255), (mid_x, mid_y), 14, 1)
                pygame.draw.circle(screen, (255, 255, 255), (mid_x, mid_y), 3,  1)

                # Corner brackets — white
                L = 16
                for bx, by, sx, sy in [(cx,cy,1,1),(cx+sw,cy,-1,1),
                                        (cx,cy+sh,1,-1),(cx+sw,cy+sh,-1,-1)]:
                    pygame.draw.line(screen, COLOR_TEXT_BRIGHT, (bx, by), (bx+sx*L, by), 2)
                    pygame.draw.line(screen, COLOR_TEXT_BRIGHT, (bx, by), (bx, by+sy*L), 2)

                # Frame info — bottom left overlay
                if data["frame_size"] > 0:
                    meta = f"{data['frame_size']//1024:.1f} KB  {data['frame_time']} ms"
                    ms = font_tiny.render(meta, True, (130, 130, 130))
                    # Small dark backing
                    bg_r = pygame.Rect(CAM_RECT.x + 6, CAM_RECT.bottom - ms.get_height() - 8,
                                       ms.get_width() + 8, ms.get_height() + 4)
                    pygame.draw.rect(screen, (0, 0, 0), bg_r)
                    screen.blit(ms, (bg_r.x + 4, bg_r.y + 2))

            else:
                # No image placeholder
                lines = [
                    ("NO IMAGE RECEIVED", font_med, COLOR_TEXT),
                    ("", None, None),
                    ("Trigger sources:", font_small, COLOR_TEXT_DIM),
                    ("  SPACE / ENTER   keyboard trigger", font_small, COLOR_TEXT_DIM),
                    ("  Click FIRE      on-screen button", font_small, COLOR_TEXT_DIM),
                    ("  Clap near mic   audio tingle", font_small, COLOR_TEXT_DIM),
                    ("  [CAPTURE]       serial terminal", font_small, COLOR_TEXT_DIM),
                ]
                for i, (txt, fnt, col) in enumerate(lines):
                    if fnt is None:
                        continue
                    s = fnt.render(txt, True, col)
                    screen.blit(s, (CAM_RECT.centerx - s.get_width()//2,
                                   CAM_RECT.centery - 62 + i * 22))

            # ══════════════════════════════════════════════════════════════
            # SIDE PANELS — shared right column
            # ══════════════════════════════════════════════════════════════
            # Vertical separator between camera and side panels
            pygame.draw.line(screen, (30, 30, 30),
                             (SIDE_X - 1, BODY_TOP), (SIDE_X - 1, BODY_BOT))

            # ── THREAT PANEL ─────────────────────────────────────────────
            draw_panel(screen, THREAT_RECT, bg=COLOR_BG_PANEL, border=COLOR_BORDER, radius=0)
            draw_label(screen, font_tiny, "THREAT ASSESSMENT",
                       THREAT_RECT.x + 12, THREAT_RECT.y + 10, COLOR_TEXT_DIM)
            pygame.draw.line(screen, COLOR_BORDER,
                             (THREAT_RECT.x + 8, THREAT_RECT.y + 24),
                             (THREAT_RECT.right - 8, THREAT_RECT.y + 24))

            tlvl = data["threat_level"]
            tcol = threat_color(tlvl)
            tstr = data["threat"]

            # Large level number
            lvl_s = font_huge.render(str(max(tlvl, 0)), True, tcol)
            screen.blit(lvl_s, (THREAT_RECT.x + 14, THREAT_RECT.y + 30))

            # Threat label to the right of number
            tlab_s = font_large.render(tstr, True, tcol)
            screen.blit(tlab_s, (THREAT_RECT.x + lvl_s.get_width() + 20,
                                  THREAT_RECT.y + 48))

            # Progress bar
            bar_rect = pygame.Rect(THREAT_RECT.x + 12, THREAT_RECT.y + 100,
                                   THREAT_RECT.width - 24, 8)
            pygame.draw.rect(screen, (28, 28, 28), bar_rect)
            if tlvl > 0:
                fw = int((tlvl / 5) * bar_rect.width)
                pygame.draw.rect(screen, tcol,
                                 pygame.Rect(bar_rect.x, bar_rect.y, fw, bar_rect.height))
            # Bar end caps
            pygame.draw.rect(screen, COLOR_BORDER, bar_rect, 1)

            # Faces count
            face_s = font_med.render(f"TARGETS DETECTED  {data['faces']:02d}",
                                     True, COLOR_TEXT)
            screen.blit(face_s, (THREAT_RECT.x + 12, THREAT_RECT.y + 120))

            # Bar tick labels
            for i in range(6):
                px = bar_rect.x + int((i / 5) * bar_rect.width)
                ls = font_tiny.render(str(i), True, COLOR_TEXT_DIM)
                screen.blit(ls, (px - ls.get_width()//2, bar_rect.bottom + 3))

            # ── STATUS PANEL ─────────────────────────────────────────────
            draw_panel(screen, STATUS_RECT, bg=COLOR_BG_PANEL, border=COLOR_BORDER, radius=0)
            draw_label(screen, font_tiny, "SYSTEM STATUS",
                       STATUS_RECT.x + 12, STATUS_RECT.y + 10, COLOR_TEXT_DIM)
            pygame.draw.line(screen, COLOR_BORDER,
                             (STATUS_RECT.x + 8, STATUS_RECT.y + 24),
                             (STATUS_RECT.right - 8, STATUS_RECT.y + 24))

            cap_ago = time.time() - data["last_cap"] if data["last_cap"] else None
            cap_str = f"{cap_ago:.1f}s ago" if cap_ago and cap_ago < 3600 else "never"

            status_rows = [
                ("CAPTURES SENT",  f"{data['cap_count']:04d}",         COLOR_TEXT),
                ("LAST TRIGGER",   cap_str,                             COLOR_TEXT_DIM),
                ("TETHER",         "ONLINE" if data["conn"] else "OFFLINE",
                                   COLOR_THREAT_SECURE if data["conn"] else COLOR_THREAT_CRIT),
            ]
            for i, (key, val, col) in enumerate(status_rows):
                ky = STATUS_RECT.y + 32 + i * 17
                ks = font_tiny.render(key, True, COLOR_TEXT_DIM)
                vs = font_small.render(val, True, col)
                screen.blit(ks, (STATUS_RECT.x + 12, ky))
                screen.blit(vs, (STATUS_RECT.right - vs.get_width() - 12, ky))

            # ── FIRE BUTTON ──────────────────────────────────────────────
            pressing = self.btn_press_frames > 0

            # Determine button color
            if pressing:
                btn_bg = COLOR_TRIGGER_PRESS
                btn_border = COLOR_TEXT_BRIGHT
                lbl_col = COLOR_TEXT_BRIGHT
            elif self.btn_hover:
                btn_bg = COLOR_TRIGGER_HOV
                btn_border = (100, 100, 100)
                lbl_col = COLOR_TEXT_BRIGHT
            else:
                btn_bg = COLOR_TRIGGER_BTN
                btn_border = (45, 45, 45)
                lbl_col = COLOR_TEXT

            pygame.draw.rect(screen, btn_bg, BTN_RECT)
            pygame.draw.rect(screen, btn_border, BTN_RECT, 1)

            btn_lbl = font_large.render("FIRE CAPTURE", True, lbl_col)
            bx = BTN_RECT.centerx - btn_lbl.get_width() // 2
            by = BTN_RECT.centery - btn_lbl.get_height() // 2 + (1 if pressing else 0)
            screen.blit(btn_lbl, (bx, by))

            hint_s = font_tiny.render("SPC  /  ENTER  /  CLICK", True, COLOR_TEXT_DIM)
            screen.blit(hint_s, (BTN_RECT.centerx - hint_s.get_width()//2,
                                  BTN_RECT.bottom - 13))

            # ── EVENT LOG ────────────────────────────────────────────────
            draw_panel(screen, LOG_RECT, bg=COLOR_BG_PANEL_DARK, border=COLOR_BORDER, radius=0)
            draw_label(screen, font_tiny, "EVENT LOG",
                       LOG_RECT.x + 12, LOG_RECT.y + 8, COLOR_TEXT_DIM)
            pygame.draw.line(screen, COLOR_BORDER,
                             (LOG_RECT.x + 8, LOG_RECT.y + 22),
                             (LOG_RECT.right - 8, LOG_RECT.y + 22))

            log_y = LOG_RECT.y + 28
            for ts_str, msg, col in data["log"]:
                if log_y + 13 > LOG_RECT.bottom - 4:
                    break
                ts_s  = font_tiny.render(ts_str, True, (55, 55, 55))
                msg_s = font_tiny.render(msg,    True, col)
                # Right-align timestamp
                screen.blit(ts_s,  (LOG_RECT.x + 12, log_y))
                screen.blit(msg_s, (LOG_RECT.x + 68, log_y))
                log_y += 14

            # ══════════════════════════════════════════════════════════════
            # FFT FOOTER
            # ══════════════════════════════════════════════════════════════
            pygame.draw.line(screen, (30, 30, 30), (0, BODY_BOT), (WINDOW_WIDTH, BODY_BOT))
            draw_panel(screen, FFT_RECT, bg=COLOR_BG_PANEL_DARK, border=COLOR_BORDER, radius=0)
            draw_label(screen, font_tiny, "AUDIO SPECTRUM  —  clap to trigger",
                       FFT_RECT.x + 10, FFT_RECT.y + 5, COLOR_TEXT_DIM)

            bar_area_h = FFT_RECT.height - 22
            bar_w = max(1, (FFT_RECT.width - 8) // FFT_BAR_COUNT - 1)

            for i in range(FFT_BAR_COUNT):
                target = min(data["fft"][i], FFT_MAX_VALUE)
                self.smooth_fft[i] += (target - self.smooth_fft[i]) * 0.22
                ratio = self.smooth_fft[i] / FFT_MAX_VALUE
                h = max(2, int(ratio * bar_area_h))
                bx = FFT_RECT.x + 4 + i * (bar_w + 1)
                by = FFT_RECT.bottom - h - 2

                # White bars, dimmer at low energy, full white at peak
                brightness = int(40 + ratio * 215)
                pygame.draw.rect(screen, (brightness, brightness, brightness),
                                 (bx, by, bar_w, h))

            # Tingle threshold line — subtle dashed appearance via segments
            thresh_y = FFT_RECT.bottom - int(0.35 * bar_area_h) - 2
            for dx in range(0, FFT_RECT.width - 8, 8):
                if (dx // 8) % 2 == 0:
                    pygame.draw.line(screen, (70, 70, 70),
                                     (FFT_RECT.x + 4 + dx, thresh_y),
                                     (FFT_RECT.x + 4 + dx + 4, thresh_y))
            draw_label(screen, font_tiny, "TINGLE THRESHOLD",
                       FFT_RECT.x + 10, thresh_y - 13, (70, 70, 70))

            # ══════════════════════════════════════════════════════════════
            # TINGLE FLASH OVERLAY
            # ══════════════════════════════════════════════════════════════
            if tingle_active and (self.frame_count % 8 < 4):
                overlay = pygame.Surface((WINDOW_WIDTH, WINDOW_HEIGHT), pygame.SRCALPHA)
                # Subtle white vignette flash, not heavy colour wash
                overlay.fill((255, 255, 255, 12))
                screen.blit(overlay, (0, 0))
                # Tingle banner — just inside the camera area, top-left
                msg_s = font_large.render("! SPIDER-SENSE TINGLE !", True, COLOR_TEXT_BRIGHT)
                bkg   = pygame.Rect(CAM_RECT.x + 6, BODY_TOP + 8,
                                    msg_s.get_width() + 16, msg_s.get_height() + 6)
                pygame.draw.rect(screen, (0, 0, 0), bkg)
                pygame.draw.rect(screen, COLOR_TEXT_BRIGHT, bkg, 1)
                screen.blit(msg_s, (bkg.x + 8, bkg.y + 3))

            pygame.display.flip()
            clock.tick(FPS)

        pygame.quit()


# ═══════════════════════════════════════════════════════════════════════════════
# ENTRY POINT
# ═══════════════════════════════════════════════════════════════════════════════
def list_ports():
    print("\nAvailable serial ports:")
    for p in serial.tools.list_ports.comports():
        print(f"  {p.device:15s} — {p.description}")
    print()

def main():
    parser = argparse.ArgumentParser(description="K.A.R.E.N. Spider-Sense HUD")
    parser.add_argument("--port", "-p", required=False,
                        help="Serial port (e.g. COM3 or /dev/ttyUSB0)")
    args = parser.parse_args()

    if not args.port:
        list_ports()
        args.port = input("Enter port: ").strip()

    shared   = SharedState()
    listener = SerialListener(args.port, WEB_TETHER_BAUD, shared)
    listener.start()

    display = SentryDisplay(shared)
    try:
        display.run()
    finally:
        listener.running = False

    sys.exit(0)

if __name__ == "__main__":
    main()
