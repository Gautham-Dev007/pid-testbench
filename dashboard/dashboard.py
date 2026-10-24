#!/usr/bin/env python3
"""
Desktop dashboard for the PID temperature testbench (NUCLEO-G070RB).

Plots the 10 Hz telemetry from the board, records runs to CSV, and sends the
serial commands. Each control mode (MANUAL, ZN, ADAPTIVE) gets its own trace
and its own step metrics so runs can be compared side by side.

    pip install -r requirements.txt
    python dashboard.py
"""

import sys, csv, re, time, platform
from collections import deque
from datetime import datetime
from pathlib import Path

import serial
import serial.tools.list_ports

import matplotlib
matplotlib.use("Qt5Agg")
import matplotlib.gridspec as gridspec
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QComboBox, QLineEdit, QGroupBox, QTabWidget,
    QFileDialog, QFrame, QDoubleSpinBox, QCheckBox, QTextEdit, QSpinBox,
    QSizePolicy, QProgressBar, QScrollArea, QAbstractSpinBox, QGridLayout,
)
from PyQt5.QtCore import Qt, QTimer, QThread, pyqtSignal, QObject, pyqtSlot, QEvent
from PyQt5.QtGui import QColor, QPalette, QFont


# ─────────────────────────────────────────────────────────────────
#  Wheel guard — blocks scroll wheel on unfocused input widgets
# ─────────────────────────────────────────────────────────────────
class _WheelGuard(QObject):
    def eventFilter(self, obj, event):
        if event.type() == QEvent.Wheel and not obj.hasFocus():
            event.ignore()
            return True
        return super().eventFilter(obj, event)

WheelGuard = _WheelGuard()

def _guard(w):
    w.setFocusPolicy(Qt.StrongFocus)
    w.installEventFilter(WheelGuard)
    return w


# ─────────────────────────────────────────────────────────────────
#  Audible alarm beep (cross-platform)
# ─────────────────────────────────────────────────────────────────
def _beep():
    try:
        if platform.system() == "Windows":
            import winsound; winsound.Beep(880, 250)
        else:
            sys.stdout.write("\a"); sys.stdout.flush()
    except Exception:
        pass


# ─────────────────────────────────────────────────────────────────
#  DPI scaling
# ─────────────────────────────────────────────────────────────────
_SCALE = 1.0

def _compute_scale():
    try:
        app = QApplication.instance()
        if app is None: return 1.0
        return max(0.75, min(2.5, app.primaryScreen().logicalDotsPerInch() / 96.0))
    except Exception:
        return 1.0

def px(n):  return max(1, int(round(n * _SCALE)))
def pt(n):  return max(6, int(round(n * _SCALE)))


# ─────────────────────────────────────────────────────────────────
#  Palette (dark instrument theme)
# ─────────────────────────────────────────────────────────────────
P = dict(
    bg       = "#1E1E1E",   # window / canvas background
    surface  = "#252526",   # panel / group background
    surface2 = "#2D2D2D",   # input field background
    border   = "#3C3C3C",   # dividers, input borders
    border2  = "#505050",   # slightly brighter borders
    text     = "#D4D4D4",   # primary text
    muted    = "#858585",   # secondary / label text
    accent   = "#4E9FD9",   # single blue accent (connects, highlights)

    ok       = "#4CAF7D",   # online / success / start
    warn     = "#C9A84C",   # autotune / caution
    danger   = "#CC4444",   # stop / disconnect / alarm
    info     = "#4E9FD9",   # record / neutral action (same as accent)
    subtle   = "#5A5A5A",   # disabled / inactive

    m0       = "#5B8FBF",   # MANUAL — steel blue
    m1       = "#5BA05B",   # ZN     — muted green
    m2       = "#B8873A",   # ADAPT  — muted amber

    t0       = "#5B8FBF",
    t1       = "#5BA05B",
    t2       = "#B8873A",
    dtdt     = "#9B71B8",   # dT/dt — muted purple
)

MODE_COLORS = {0: P["t0"], 1: P["t1"], 2: P["t2"]}

# ─────────────────────────────────────────────────────────────────
#  Constants
# ─────────────────────────────────────────────────────────────────
MODES       = {0: "MANUAL", 1: "ZN", 2: "ADAPTIVE"}
MAX_POINTS  = 1200     # per channel, about 2 minutes at 10 Hz
AUTO_STOP_S = 1500     # the "25 min" recording

# Same step-metric definitions as the firmware's METRICS command.
SETTLE_BAND_FRAC  = 0.02
SETTLE_BAND_MIN_C = 0.2
SETTLE_CONFIRM_S  = 30.0
MIN_STEP_C        = 1.0

SP_MIN, SP_MAX = 20.0, 150.0   # firmware limits

# Telemetry M: field -> channel. None means "not a control mode": the row is
# filed under whichever mode is selected in the GUI. AUTOTUNE ends in ZN.
MODE_MAP = {
    "MANUAL": 0, "ZN": 1, "ADAPT": 2, "ADAPTIVE": 2, "AUTOTUNE": 1,
    "STOPPED": None, "FANMODE": None, "FAULT": None,
}

_NUM = r"(-?[\d.]+|nan)"
TELEMETRY_RE = re.compile(
    rf"SP:{_NUM}\s+T:{_NUM}C\s+D:(\d+)%\s+M:(\S+)"
    rf"\s+Kp:{_NUM}\s+Ki:{_NUM}"
    r"(?:\s+F1:(\d+)%\s+F2:(\d+)%)?"
    rf"(?:\s+dT/dt:{_NUM})?"
    rf"(?:\s+Kd:{_NUM})?"
)


def parse_telemetry(line):
    """Returns a row dict, or None if the line isn't usable telemetry."""
    m = TELEMETRY_RE.search(line)
    if not m:
        return None
    try:
        temp = float(m.group(2))
    except ValueError:
        return None
    if temp != temp:  # nan: the board flagged a bad sample
        return None
    return {
        "sp":   float(m.group(1)),
        "temp": temp,
        "duty": int(m.group(3)),
        "mode": m.group(4),
        "kp":   float(m.group(5)),
        "ki":   float(m.group(6)),
        "f1":   int(m.group(7)) if m.group(7) else 0,
        "f2":   int(m.group(8)) if m.group(8) else 0,
        "dtdt": float(m.group(9)) if m.group(9) else 0.0,
        "kd":   float(m.group(10)) if m.group(10) else 0.0,
    }


# ─────────────────────────────────────────────────────────────────
#  QSS stylesheet
# ─────────────────────────────────────────────────────────────────
def _qss():
    # All sizing is computed once here; the stylesheet is a plain string
    # with no f-string interpolation of Python variables except palette + sizes.
    btn_pad_v = px(4)
    btn_pad_h = px(10)
    r         = px(3)    # border radius — small and sharp
    bw        = px(1)    # border width
    inp_pad   = px(3)
    grp_top   = px(12)
    grp_pad   = px(6)
    sb_w      = px(7)    # scrollbar width

    f_base  = pt(10)
    f_small = pt(9)
    f_mono  = pt(9)
    f_lbl   = pt(9)
    f_val   = pt(15)

    return f"""
/* ── Global ── */
QMainWindow, QWidget {{
    background-color: {P['bg']};
    color: {P['text']};
    font-family: 'Segoe UI', 'Inter', 'Liberation Sans', Arial, sans-serif;
    font-size: {f_base}px;
}}

/* ── Group boxes — thin border, no rounded corners ── */
QGroupBox {{
    border: {bw}px solid {P['border']};
    border-radius: {r}px;
    margin-top: {grp_top}px;
    padding: {grp_pad}px;
    font-size: {f_small}px;
    font-weight: 600;
    color: {P['muted']};
    background-color: {P['surface']};
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    left: {px(8)}px;
    padding: 0 {px(4)}px;
    background-color: {P['surface']};
    text-transform: uppercase;
    letter-spacing: 1px;
}}

/* ── Base button ── */
QPushButton {{
    background-color: {P['surface2']};
    border: {bw}px solid {P['border2']};
    border-radius: {r}px;
    padding: {btn_pad_v}px {btn_pad_h}px;
    color: {P['text']};
    font-size: {f_base}px;
    font-weight: 500;
}}
QPushButton:hover   {{ background-color: #333333; border-color: {P['accent']}; }}
QPushButton:pressed {{ background-color: {P['bg']}; }}
QPushButton:disabled {{
    color: {P['subtle']};
    border-color: {P['border']};
    background-color: {P['surface']};
}}

/* ── Semantic: connect / start / ok ── */
QPushButton#btn_connect, QPushButton#btn_start {{
    border-left: {px(3)}px solid {P['ok']};
    color: {P['ok']};
}}
QPushButton#btn_connect:hover, QPushButton#btn_start:hover {{
    background-color: #253325;
}}

/* ── Semantic: stop / disconnect ── */
QPushButton#btn_stop, QPushButton#btn_disconnect, QPushButton#btn_stop_rec {{
    border-left: {px(3)}px solid {P['danger']};
    color: {P['danger']};
}}
QPushButton#btn_stop:hover, QPushButton#btn_disconnect:hover,
QPushButton#btn_stop_rec:hover {{ background-color: #312020; }}
QPushButton#btn_stop:disabled, QPushButton#btn_stop_rec:disabled {{
    color: {P['subtle']}; border-left-color: {P['border']};
    background-color: {P['surface']};
}}

/* ── Semantic: autotune / caution ── */
QPushButton#btn_autotune {{
    border-left: {px(3)}px solid {P['warn']};
    color: {P['warn']};
}}
QPushButton#btn_autotune:hover {{ background-color: #2E2810; }}

/* ── Semantic: record / info / neutral ── */
QPushButton#btn_record, QPushButton#btn_rec_25,
QPushButton#btn_reset, QPushButton#btn_apply_gains {{
    border-left: {px(3)}px solid {P['info']};
    color: {P['info']};
}}
QPushButton#btn_record:hover, QPushButton#btn_rec_25:hover,
QPushButton#btn_reset:hover, QPushButton#btn_apply_gains:hover {{
    background-color: #1E2830;
}}
QPushButton#btn_record:disabled, QPushButton#btn_rec_25:disabled {{
    color: {P['subtle']}; border-left-color: {P['border']};
    background-color: {P['surface']};
}}

/* ── Semantic: clear (danger utility) ── */
QPushButton#btn_clear_plots {{
    border-left: {px(3)}px solid {P['danger']};
    color: {P['muted']};
}}
QPushButton#btn_clear_plots:hover {{
    color: {P['danger']}; background-color: #312020;
}}

/* ── Mode buttons: checkable ── */
QPushButton#mode_0, QPushButton#mode_1, QPushButton#mode_2 {{
    border: {bw}px solid {P['border2']};
    color: {P['muted']};
    font-weight: 500;
}}
QPushButton#mode_0:checked {{
    background-color: #1E2A38; border-color: {P['m0']};
    color: {P['m0']}; font-weight: 700;
    border-bottom: {px(2)}px solid {P['m0']};
}}
QPushButton#mode_1:checked {{
    background-color: #1E2E1E; border-color: {P['m1']};
    color: {P['m1']}; font-weight: 700;
    border-bottom: {px(2)}px solid {P['m1']};
}}
QPushButton#mode_2:checked {{
    background-color: #2E2610; border-color: {P['m2']};
    color: {P['m2']}; font-weight: 700;
    border-bottom: {px(2)}px solid {P['m2']};
}}

/* ── Fan buttons ── */
QPushButton#btn_fan_on  {{ color: {P['accent']}; border-left: {px(3)}px solid {P['accent']}; }}
QPushButton#btn_fan_off {{ color: {P['warn']};   border-left: {px(3)}px solid {P['warn']};   }}
QPushButton#btn_fan_on:hover  {{ background-color: #1A2530; }}
QPushButton#btn_fan_off:hover {{ background-color: #2E2810; }}

/* ── Input fields ── */
QComboBox, QLineEdit, QDoubleSpinBox, QSpinBox {{
    background-color: {P['surface2']};
    border: {bw}px solid {P['border']};
    border-radius: {r}px;
    padding: {inp_pad}px {px(6)}px;
    color: {P['text']};
    font-size: {f_base}px;
    selection-background-color: {P['accent']};
}}
QDoubleSpinBox:focus, QSpinBox:focus, QLineEdit:focus, QComboBox:focus {{
    border-color: {P['accent']};
}}
QComboBox::drop-down {{ border: none; width: {px(18)}px; }}
QComboBox QAbstractItemView {{
    background-color: {P['surface2']}; color: {P['text']};
    selection-background-color: {P['accent']};
    border: {bw}px solid {P['border2']};
}}

/* ── Tabs ── */
QTabWidget::pane {{
    border: {bw}px solid {P['border']};
    background-color: {P['surface']};
}}
QTabBar::tab {{
    background: {P['surface2']}; border: none;
    padding: {px(4)}px {px(10)}px;
    color: {P['muted']}; font-size: {f_small}px;
    border-right: {bw}px solid {P['border']};
}}
QTabBar::tab:selected {{
    color: {P['text']}; background: {P['surface']};
    border-bottom: {px(2)}px solid {P['accent']};
}}

/* ── Serial monitor ── */
QTextEdit {{
    background-color: {P['bg']};
    border: {bw}px solid {P['border']};
    color: {P['text']};
    font-family: 'Consolas', 'Courier New', monospace;
    font-size: {f_mono}px;
}}

/* ── Status bar ── */
QStatusBar {{
    background-color: {P['surface']};
    color: {P['muted']};
    border-top: {bw}px solid {P['border']};
    font-size: {f_small}px;
}}

/* ── Labels ── */
QLabel#val_large {{
    font-size: {f_val}px; font-weight: 700; color: {P['text']};
}}
QLabel#lbl_small {{
    font-size: {f_small}px; color: {P['muted']};
    text-transform: uppercase; letter-spacing: 1px;
}}

/* ── Checkbox ── */
QCheckBox {{ color: {P['text']}; spacing: {px(5)}px; font-size: {f_base}px; }}
QCheckBox::indicator {{
    width: {px(13)}px; height: {px(13)}px;
    border: {bw}px solid {P['border2']};
    border-radius: {px(2)}px;
    background: {P['surface2']};
}}
QCheckBox::indicator:checked {{ background: {P['accent']}; border-color: {P['accent']}; }}

/* ── Scrollbar — thin and unobtrusive ── */
QScrollBar:vertical {{
    background: {P['surface']}; width: {sb_w}px; margin: 0;
}}
QScrollBar::handle:vertical {{
    background: {P['border2']}; border-radius: {px(3)}px; min-height: {px(20)}px;
}}
QScrollBar::handle:vertical:hover {{ background: {P['muted']}; }}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}

/* ── Progress bar ── */
QProgressBar {{
    font-size: {f_small}px; border: {bw}px solid {P['border']};
    border-radius: {r}px; background-color: {P['surface2']};
    color: {P['text']}; text-align: center;
}}
QProgressBar::chunk {{ background-color: {P['info']}; border-radius: {r}px; }}
QProgressBar#rec_bar::chunk {{ background-color: {P['ok']}; }}
"""


# ─────────────────────────────────────────────────────────────────
#  DATA MODEL
# ─────────────────────────────────────────────────────────────────
class ChannelData:
    def __init__(self, mode_id):
        self.mode_id   = mode_id
        self.mode_name = MODES.get(mode_id, "?")
        self.t = deque(maxlen=MAX_POINTS); self.temp = deque(maxlen=MAX_POINTS)
        self.sp = deque(maxlen=MAX_POINTS); self.duty = deque(maxlen=MAX_POINTS)
        self.dtdt = deque(maxlen=MAX_POINTS)
        self.kp = deque(maxlen=MAX_POINTS); self.ki = deque(maxlen=MAX_POINTS)
        self.kd = deque(maxlen=MAX_POINTS)
        self.t0 = None
        self._start_step(None, None, 0.0)

    def _start_step(self, sp, temp, elapsed):
        # Mirrors StepMetrics in the firmware: rise is 10-90 % of the step,
        # settle is the last entry into a +/-2 % band held for 30 s.
        self._step_sp = sp; self._step_t0 = elapsed; self._start_temp = temp
        self._peak = temp
        self._t10 = self._t90 = None
        self._band_since = None
        self.rise_time_s = None; self.overshoot_c = 0.0; self.settle_time_s = None

    def add(self, row):
        now = time.time()
        if self.t0 is None:
            self.t0 = now
        e = now - self.t0
        self.t.append(e); self.temp.append(row["temp"]); self.sp.append(row["sp"])
        self.duty.append(row["duty"]); self.dtdt.append(row.get("dtdt", 0.0))
        self.kp.append(row["kp"]); self.ki.append(row["ki"]); self.kd.append(row.get("kd", 0.0))
        self._metrics(row, e)

    def _metrics(self, row, elapsed):
        sp, t = row["sp"], row["temp"]
        if self._step_sp is None or sp != self._step_sp:
            self._start_step(sp, t, elapsed)

        step = sp - self._start_temp
        rel = elapsed - self._step_t0
        if abs(step) >= MIN_STEP_C:
            progress = (t - self._start_temp) / step
            if self._t10 is None and progress >= 0.1: self._t10 = rel
            if self._t90 is None and progress >= 0.9: self._t90 = rel
            if self._t10 is not None and self._t90 is not None:
                self.rise_time_s = self._t90 - self._t10
            if (step > 0 and t > self._peak) or (step < 0 and t < self._peak):
                self._peak = t
            over = (self._peak - sp) if step > 0 else (sp - self._peak)
            self.overshoot_c = max(0.0, over)

        band = max(SETTLE_BAND_FRAC * abs(step), SETTLE_BAND_MIN_C)
        if abs(t - sp) <= band:
            if self._band_since is None:
                self._band_since = rel
            if rel - self._band_since >= SETTLE_CONFIRM_S:
                self.settle_time_s = self._band_since
        else:
            self._band_since = None
            self.settle_time_s = None

    def to_csv_rows(self):
        cols = zip(self.t, self.temp, self.sp, self.duty, self.dtdt, self.kp, self.ki, self.kd)
        return [{"time_s": round(t, 3), "temp_c": tm, "setpoint": sp, "duty_pct": d,
                 "dtdt": dt, "kp": kp, "ki": ki, "kd": kd, "mode": self.mode_name}
                for t, tm, sp, d, dt, kp, ki, kd in cols]


# ─────────────────────────────────────────────────────────────────
#  SERIAL WORKER (own thread; the GUI talks to it only through signals)
# ─────────────────────────────────────────────────────────────────
class SerialWorker(QObject):
    row_received = pyqtSignal(dict); raw_received = pyqtSignal(str)
    error_signal = pyqtSignal(str);  connected_sig = pyqtSignal(bool)
    connect_signal = pyqtSignal(str, int); disconnect_signal = pyqtSignal()
    send_signal = pyqtSignal(str)

    def __init__(self):
        super().__init__(); self._ser = None; self._active = False

    @pyqtSlot(str, int)
    def do_connect(self, port, baud):
        try:
            self._ser = serial.Serial(port, baud, timeout=0.1)
            self._active = True; self.connected_sig.emit(True)
            QTimer.singleShot(0, self._read_once)
        except serial.SerialException as e:
            self.error_signal.emit(str(e)); self.connected_sig.emit(False)

    def _read_once(self):
        if not self._active or not (self._ser and self._ser.is_open): return
        try:
            raw = self._ser.readline().decode("utf-8", errors="ignore").strip()
            if raw:
                try: self.raw_received.emit(raw)
                except RuntimeError: return
                row = parse_telemetry(raw)
                if row:
                    try: self.row_received.emit(row)
                    except RuntimeError: return
        except (serial.SerialException, OSError, ValueError) as e:
            if self._active:
                try: self.error_signal.emit(str(e)); self.connected_sig.emit(False)
                except RuntimeError: pass
            self._active = False; return
        QTimer.singleShot(10, self._read_once)

    @pyqtSlot()
    def do_disconnect(self):
        self._active = False
        if self._ser and self._ser.is_open:
            try: self._ser.close()
            except Exception: pass
        self._ser = None
        try: self.connected_sig.emit(False)
        except RuntimeError: pass

    @pyqtSlot(str)
    def send(self, cmd):
        if self._ser and self._ser.is_open:
            try: self._ser.write((cmd.strip() + "\n").encode())
            except (serial.SerialException, OSError): pass


# ─────────────────────────────────────────────────────────────────
#  LIVE CANVAS  — 3 subplots; professional instrument appearance
# ─────────────────────────────────────────────────────────────────
class LiveCanvas(FigureCanvas):
    def __init__(self, parent=None):
        dpi = 96.0
        try:
            app = QApplication.instance()
            if app: dpi = app.primaryScreen().logicalDotsPerInch()
        except Exception: pass
        dpi = min(max(dpi, 72), 200)

        self.fig = Figure(figsize=(10, 8), dpi=dpi, facecolor=P["bg"])
        super().__init__(self.fig)
        self.setParent(parent)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

        gs = gridspec.GridSpec(3, 1, figure=self.fig,
                               hspace=0.06, height_ratios=[3, 1.1, 1.1],
                               left=0.08, right=0.97, top=0.94, bottom=0.08)
        self.ax_temp = self.fig.add_subplot(gs[0])
        self.ax_duty = self.fig.add_subplot(gs[1], sharex=self.ax_temp)
        self.ax_dtdt = self.fig.add_subplot(gs[2], sharex=self.ax_temp)

        tf = max(7, int(8 * _SCALE)); lf = max(8, int(9 * _SCALE))
        for ax in (self.ax_temp, self.ax_duty, self.ax_dtdt):
            ax.set_facecolor(P["surface"])
            ax.tick_params(colors=P["muted"], labelsize=tf, length=3, width=0.5)
            ax.yaxis.label.set_color(P["muted"])
            ax.xaxis.label.set_color(P["muted"])
            for spine in ax.spines.values():
                spine.set_color(P["border"]); spine.set_linewidth(0.5)
            ax.grid(True, color=P["border"], linewidth=0.4, linestyle="-", alpha=0.6)

        import matplotlib.pyplot as plt
        plt.setp(self.ax_temp.get_xticklabels(), visible=False)
        plt.setp(self.ax_duty.get_xticklabels(), visible=False)
        self.ax_temp.set_ylabel("Temp (°C)",  color=P["muted"], fontsize=lf)
        self.ax_duty.set_ylabel("Duty (%)",   color=P["muted"], fontsize=lf)
        self.ax_dtdt.set_ylabel("dT/dt (°/s)",color=P["muted"], fontsize=lf)
        self.ax_dtdt.set_xlabel("Time (s)",   color=P["muted"], fontsize=lf)
        self._set_empty_limits()
        self.ax_dtdt.axhline(0, color=P["border"], lw=0.7)

        title_f = max(9, int(10 * _SCALE))
        self.fig.suptitle("PID Temperature Testbench", color=P["text"],
                          fontsize=title_f, fontweight="semibold", x=0.52)
        self.fig.text(0.97, 0.004, "IEEE RIT-B · SPS × WIE",
                      ha="right", va="bottom", color=P["muted"],
                      fontsize=max(6, int(7 * _SCALE)), style="italic", alpha=0.6)

        self._lines = {}; self._fills = {}; self._dtlines = {}; self._csv_lines = {}
        self._init_lines()

        # Crosshair
        lw_xh = max(0.7, 0.9 * _SCALE)
        self._xh_vl = [ax.axvline(x=0, color=P["text"], lw=lw_xh, ls=":", alpha=0.0, zorder=10)
                       for ax in (self.ax_temp, self.ax_duty, self.ax_dtdt)]
        self._xh_dots_t = {}; self._xh_dots_d = {}
        for mid in range(3):
            c = MODE_COLORS[mid]
            ms = max(4, 6 * _SCALE)
            dt, = self.ax_temp.plot([], [], "o", color=c, ms=ms, zorder=11, alpha=0.0,
                                    markeredgecolor=P["bg"], markeredgewidth=max(0.8, 1.2*_SCALE))
            dd, = self.ax_duty.plot([], [], "o", color=c, ms=ms, zorder=11, alpha=0.0,
                                    markeredgecolor=P["bg"], markeredgewidth=max(0.8, 1.2*_SCALE))
            self._xh_dots_t[mid] = dt; self._xh_dots_d[mid] = dd
        ann_f = max(7, int(8 * _SCALE))
        self._xh_ann = self.ax_temp.annotate(
            "", xy=(0, 0), xycoords="data", xytext=(12, -12),
            textcoords="offset points", fontsize=ann_f, fontfamily="monospace",
            color=P["text"],
            bbox=dict(boxstyle="round,pad=0.4", facecolor=P["surface2"],
                      edgecolor=P["border2"], alpha=0.92, linewidth=max(0.8, _SCALE)),
            zorder=20, visible=False)
        self.mpl_connect("motion_notify_event", self._on_move)
        self.mpl_connect("axes_leave_event",    self._on_leave)
        self.draw()

    def _set_empty_limits(self):
        # Without data matplotlib picks a meaningless 1e-17 scale.
        self.ax_temp.set_xlim(0, 60)
        self.ax_temp.set_ylim(SP_MIN, 100)
        self.ax_duty.set_ylim(-5, 105)
        self.ax_dtdt.set_ylim(-1, 1)

    def _init_lines(self):
        lw_t = max(1.3, 1.8 * _SCALE); lw_sp = max(0.8, 1.1 * _SCALE)
        lw_d = max(1.1, 1.5 * _SCALE); lw_dt = max(1.0, 1.4 * _SCALE)
        leg_f = max(7, int(8 * _SCALE))
        for mid in range(3):
            c = MODE_COLORS[mid]
            self._lines[mid] = {
                "temp": self.ax_temp.plot([], [], color=c, lw=lw_t, ls="-",
                                          alpha=0.9, solid_capstyle="round",
                                          label=MODES[mid])[0],
                "sp":   self.ax_temp.plot([], [], color=c, lw=lw_sp,
                                          ls="--", alpha=0.45, dashes=(5, 4))[0],
                "duty": self.ax_duty.plot([], [], color=c, lw=lw_d, ls="-",
                                          alpha=0.9, solid_capstyle="round")[0],
            }
            self._dtlines[mid] = self.ax_dtdt.plot([], [], color=P["dtdt"],
                                                    lw=lw_dt, ls="-", alpha=0.8)[0]
            self._fills[mid] = {"temp": None, "duty": None}
        self.ax_temp.axhline(0, color=P["border"], lw=0.5)
        self.ax_temp.legend(loc="upper left", facecolor=P["surface2"],
                            edgecolor=P["border"], labelcolor=P["text"],
                            fontsize=leg_f, framealpha=0.85)

    def _on_leave(self, event):
        for vl in self._xh_vl: vl.set_alpha(0.0)
        for mid in range(3):
            self._xh_dots_t[mid].set_alpha(0.0); self._xh_dots_d[mid].set_alpha(0.0)
        self._xh_ann.set_visible(False); self.draw_idle()

    def _on_move(self, event):
        if event.inaxes not in (self.ax_temp, self.ax_duty, self.ax_dtdt):
            self._on_leave(event); return
        xc = event.xdata
        if xc is None: return
        import bisect
        best_d = float("inf"); best_idx = best_mid = best_xa = None
        for mid in range(3):
            xd = self._lines[mid]["temp"].get_xdata()
            if len(xd) < 2: continue
            i = min(bisect.bisect_left(list(xd), xc), len(xd)-1)
            for c in (max(0, i-1), i):
                d = abs(xd[c] - xc)
                if d < best_d: best_d = d; best_idx = c; best_mid = mid; best_xa = xd
        if best_idx is None: self._on_leave(event); return
        sx = best_xa[best_idx]
        for vl in self._xh_vl: vl.set_xdata([sx, sx]); vl.set_alpha(0.5)
        ann = [f"t = {sx:.1f} s"]
        for mid in range(3):
            xd = self._lines[mid]["temp"].get_xdata()
            if len(xd) < 2: self._xh_dots_t[mid].set_alpha(0.0); self._xh_dots_d[mid].set_alpha(0.0); continue
            import bisect
            i = min(bisect.bisect_left(list(xd), sx), len(xd)-1)
            tx = float(xd[i])
            tv = float(self._lines[mid]["temp"].get_ydata()[i])
            dv = float(self._lines[mid]["duty"].get_ydata()[i]) if len(self._lines[mid]["duty"].get_ydata()) > i else float("nan")
            self._xh_dots_t[mid].set_data([tx], [tv]); self._xh_dots_t[mid].set_alpha(0.9)
            self._xh_dots_d[mid].set_data([tx], [dv]); self._xh_dots_d[mid].set_alpha(0.9)
            ann.append(f"{MODES[mid]:8s}  T={tv:.2f}°C  D={dv:.0f}%")
        self._xh_ann.xy = (sx, float(self._lines[best_mid]["temp"].get_ydata()[best_idx])
                           if len(self._lines[best_mid]["temp"].get_ydata()) > best_idx else 0)
        self._xh_ann.set_text("\n".join(ann))
        xl = self.ax_temp.get_xlim()
        xf = (sx - xl[0]) / max(xl[1] - xl[0], 1e-9)
        self._xh_ann.xyann = (-14, -12) if xf >= 0.70 else (14, -12)
        self._xh_ann.set_ha("right" if xf >= 0.70 else "left")
        self._xh_ann.set_visible(True); self.draw_idle()

    def update_mode_data(self, mode_id, ch):
        if not ch.t: return
        import matplotlib.colors as mc
        t = list(ch.t); tm = list(ch.temp); sp = list(ch.sp)
        dy = list(ch.duty); dt = list(ch.dtdt)
        self._lines[mode_id]["temp"].set_data(t, tm)
        self._lines[mode_id]["sp"].set_data(t, sp)
        self._lines[mode_id]["duty"].set_data(t, dy)
        self._dtlines[mode_id].set_data(t, dt)

        c = MODE_COLORS[mode_id]; rgb = mc.to_rgb(c); n = len(t)
        if getattr(self, f"_fl_{mode_id}", -1) < n - 50:
            for key, ax, ydata, base in (
                ("temp", self.ax_temp, tm, min(tm) if tm else 0),
                ("duty", self.ax_duty, dy, 0)):
                old = self._fills[mode_id][key]
                if old:
                    try: old.remove()
                    except ValueError: pass
                self._fills[mode_id][key] = ax.fill_between(
                    t, base, ydata, color=(*rgb, 0.07), linewidth=0, zorder=0)
            setattr(self, f"_fl_{mode_id}", n)

        all_x = max((self._lines[m]["temp"].get_xdata()[-1]
                     for m in range(3) if len(self._lines[m]["temp"].get_xdata())), default=0.0)
        self.ax_temp.set_xlim(0, max(all_x + 2, 10))
        all_y = [v for m in range(3) for k in ("temp", "sp")
                 for v in self._lines[m][k].get_ydata()]
        if all_y:
            yl, yh = min(all_y) - 3, max(all_y) + 3
            if yh - yl < 10: mid_y = (yl+yh)/2; yl, yh = mid_y-5, mid_y+5
            self.ax_temp.set_ylim(yl, yh)
        all_dt = [v for m in range(3) for v in self._dtlines[m].get_ydata()]
        if all_dt:
            dl, dh = min(all_dt) - 0.3, max(all_dt) + 0.3
            if dh - dl < 0.8: mdv = (dl+dh)/2; dl, dh = mdv-0.4, mdv+0.4
            self.ax_dtdt.set_ylim(dl, dh)
        self.draw_idle()

    def load_csv_overlay(self, filepath, mode_id):
        if filepath in self._csv_lines:
            for ln in self._csv_lines[filepath].values(): ln.remove()
        c = MODE_COLORS.get(mode_id, P["text"])
        t, tm, sp, dy = [], [], [], []
        try:
            with open(filepath, newline="") as f:
                for row in csv.DictReader(f):
                    t.append(float(row["time_s"])); tm.append(float(row["temp_c"]))
                    sp.append(float(row["setpoint"])); dy.append(float(row["duty_pct"]))
        except (OSError, KeyError, ValueError): return
        lw = max(1.2, 1.7 * _SCALE)
        self._csv_lines[filepath] = {
            "temp": self.ax_temp.plot(t, tm, color=c, lw=lw, ls="-", alpha=0.4,
                                      label=f"CSV:{MODES.get(mode_id,'?')}")[0],
            "sp":   self.ax_temp.plot(t, sp, color=c, lw=max(0.7, lw*0.5),
                                      ls=":", alpha=0.3)[0],
            "duty": self.ax_duty.plot(t, dy, color=c, lw=lw*0.75, ls="-", alpha=0.4)[0],
        }
        self.ax_temp.legend(loc="upper left", facecolor=P["surface2"],
                            edgecolor=P["border"], labelcolor=P["text"],
                            fontsize=max(7, int(8*_SCALE)), framealpha=0.85)
        for ax in (self.ax_temp, self.ax_duty, self.ax_dtdt):
            ax.relim(); ax.autoscale_view()
        self.draw_idle()

    def clear_plots(self):
        for mid in range(3):
            for l in self._lines[mid].values(): l.set_data([], [])
            self._dtlines[mid].set_data([], [])
            for key in ("temp", "duty"):
                if self._fills[mid][key]:
                    try: self._fills[mid][key].remove()
                    except ValueError: pass
                    self._fills[mid][key] = None
        for ls in self._csv_lines.values():
            for l in ls.values(): l.remove()
        self._csv_lines.clear()
        self._set_empty_limits()
        self.draw_idle()

    def export_png(self, filepath):
        self.fig.savefig(filepath, dpi=300, facecolor=P["bg"], bbox_inches="tight")

    def draw_alarm_line(self, temp, show):
        if not hasattr(self, "_alarm_hl"):
            self._alarm_hl = self.ax_temp.axhline(
                y=temp, color=P["danger"], lw=max(0.9, 1.2*_SCALE),
                ls="--", alpha=0.0, zorder=8)
        if show:
            self._alarm_hl.set_ydata([temp, temp]); self._alarm_hl.set_alpha(0.75)
        else:
            self._alarm_hl.set_alpha(0.0)
        self.draw_idle()


# ─────────────────────────────────────────────────────────────────
#  METRICS PANEL  — tabbed, compact grid
# ─────────────────────────────────────────────────────────────────
class MetricsPanel(QGroupBox):
    def __init__(self, title, mode_id, parent=None):
        super().__init__(title, parent)
        self.mode_id = mode_id
        c = MODE_COLORS.get(mode_id, P["text"])
        self.setStyleSheet(f"QGroupBox {{ border-color: {c}; color: {c}; }}")
        g = QGridLayout(self); g.setSpacing(px(3)); g.setContentsMargins(px(4), px(10), px(4), px(4))

        def cell(label, row, col):
            lbl = QLabel(label.upper()); lbl.setObjectName("lbl_small")
            lbl.setAlignment(Qt.AlignCenter)
            val = QLabel("—"); val.setObjectName("val_large")
            val.setStyleSheet(f"font-size: {pt(13)}px; font-weight: 700; color: {c};")
            val.setAlignment(Qt.AlignCenter)
            g.addWidget(lbl, row*2, col); g.addWidget(val, row*2+1, col)
            return val

        self.v_rise = cell("Rise", 0, 0); self.v_over = cell("Overshoot", 0, 1)
        self.v_settle = cell("Settle", 0, 2); self.v_kp = cell("Kp", 1, 0)
        self.v_ki = cell("Ki", 1, 1); self.v_kd = cell("Kd", 1, 2)

    def update_from_channel(self, ch):
        def ft(v): return "—" if v is None else f"{v:.1f} s"
        self.v_rise.setText(ft(ch.rise_time_s))
        self.v_over.setText(f"{ch.overshoot_c:.2f} °C")
        self.v_settle.setText(ft(ch.settle_time_s))
        if ch.kp:
            self.v_kp.setText(f"{ch.kp[-1]:.3f}")
            self.v_ki.setText(f"{ch.ki[-1]:.4f}")
            self.v_kd.setText(f"{ch.kd[-1]:.3f}")


# ─────────────────────────────────────────────────────────────────
#  MAIN WINDOW
# ─────────────────────────────────────────────────────────────────
class MainWindow(QMainWindow):
    send_to_serial = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        self.setWindowTitle("PID Testbench  —  IEEE RIT-B  |  SPS × WIE")
        self.resize(px(1400), px(860))
        self.setStyleSheet(_qss())

        self.channels = {i: ChannelData(i) for i in range(3)}
        self.active_mode = 2; self.recording = False
        self.record_mode = 2; self.record_start = None
        self.csv_path = None; self.csv_writer = None; self.csv_file_obj = None
        self.connected = False; self.last_row = {}
        self._fan_active = False; self._rec_25_mode = False
        self._multi_rec_active = False; self._multi_rec_files = {}
        self._multi_rec_start = None
        self._alarm_enabled = False; self._alarm_temp = 90.0; self._alarm_firing = False
        self._last_skip_mode = None

        self._setup_serial()
        self._build_ui()

        self.plot_timer = QTimer(); self.plot_timer.timeout.connect(self._refresh_plots)
        self.plot_timer.start(250)
        self.clock_timer = QTimer(); self.clock_timer.timeout.connect(self._tick_clock)
        self.clock_timer.start(1000)
        self._rec_blink_state = False
        self.rec_blink_timer = QTimer(); self.rec_blink_timer.timeout.connect(self._blink_rec)

        self.status_label = QLabel("Disconnected")
        self.statusBar().addPermanentWidget(self.status_label)

    def _setup_serial(self):
        self.serial_thread = QThread(); self.serial_worker = SerialWorker()
        self.serial_worker.moveToThread(self.serial_thread)
        self.serial_worker.connect_signal.connect(self.serial_worker.do_connect)
        self.serial_worker.disconnect_signal.connect(self.serial_worker.do_disconnect)
        self.serial_worker.row_received.connect(self._on_row)
        self.serial_worker.raw_received.connect(self._on_raw)
        self.serial_worker.error_signal.connect(self._on_serial_error)
        self.serial_worker.connected_sig.connect(self._on_connection)
        self.serial_worker.send_signal.connect(self.serial_worker.send)
        self.send_to_serial.connect(self.serial_worker.send_signal)
        self.serial_thread.start()

    # ── helpers ──────────────────────────────────────────────────

    def _sep(self):
        f = QFrame(); f.setFrameShape(QFrame.HLine)
        f.setStyleSheet(f"color: {P['border']}; background: {P['border']}; max-height: 1px;")
        return f

    def _lbl(self, text, small=True):
        l = QLabel(text)
        l.setStyleSheet(f"color:{P['muted']}; font-size:{pt(9 if small else 10)}px;")
        return l

    # ─────────────────────────────────────────────────────────────
    #  UI BUILD
    # ─────────────────────────────────────────────────────────────

    def _build_ui(self):
        root = QWidget(); self.setCentralWidget(root)
        rl = QHBoxLayout(root); rl.setSpacing(px(6))
        rl.setContentsMargins(px(6), px(6), px(6), px(6))

        # ── sidebar ──────────────────────────────────────────────
        inner = QWidget()
        il = QVBoxLayout(inner); il.setSpacing(px(6))
        il.setContentsMargins(0, 0, px(1), px(4))

        il.addWidget(self._build_header())
        il.addWidget(self._build_connection())
        il.addWidget(self._build_readout())
        il.addWidget(self._build_control())
        il.addWidget(self._build_tune())
        il.addWidget(self._build_alarm())
        il.addWidget(self._build_recording())
        il.addWidget(self._build_metrics())
        il.addStretch()

        # sidebar width: 300 px  +  7 px scrollbar  =  307 → give 310
        scroll = QScrollArea()
        scroll.setWidget(inner); scroll.setWidgetResizable(True)
        scroll.setFixedWidth(px(310))
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        scroll.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        scroll.setStyleSheet(f"QScrollArea {{ border: none; background: {P['bg']}; }}"
                             f"QScrollArea > QWidget > QWidget {{ background: {P['bg']}; }}")
        rl.addWidget(scroll)

        # ── centre ───────────────────────────────────────────────
        cw = QWidget(); cl = QVBoxLayout(cw); cl.setSpacing(px(4)); rl.addWidget(cw, stretch=1)
        self.canvas = LiveCanvas(); cl.addWidget(self.canvas, stretch=3)

        mon = QGroupBox("Serial Monitor"); ml = QVBoxLayout(mon); ml.setSpacing(px(3))
        self.log_text = QTextEdit(); self.log_text.setReadOnly(True)
        self.log_text.document().setMaximumBlockCount(2000)  # telemetry arrives at 10 Hz
        self.log_text.setMaximumHeight(px(110)); ml.addWidget(self.log_text)
        cr = QHBoxLayout()
        self.cmd_input = QLineEdit()
        self.cmd_input.setPlaceholderText("Command  (START  STOP  RESET  GAINS  STATUS  Sxx.x  HELP)")
        self.cmd_input.returnPressed.connect(self._send_custom_cmd); cr.addWidget(self.cmd_input)
        bs = QPushButton("Send"); bs.setFixedWidth(px(54)); bs.clicked.connect(self._send_custom_cmd)
        cr.addWidget(bs); ml.addLayout(cr); cl.addWidget(mon, stretch=1)

    # ── Header (replaces the branding panel) ─────────────────────

    def _build_header(self):
        w = QWidget()
        w.setStyleSheet(f"background:{P['surface']}; border-bottom: 1px solid {P['border']};")
        l = QHBoxLayout(w); l.setContentsMargins(px(8), px(5), px(8), px(5)); l.setSpacing(px(8))
        org = QLabel("IEEE RIT-B")
        org.setStyleSheet(f"color:{P['text']}; font-weight:700; font-size:{pt(10)}px;")
        div = QFrame(); div.setFrameShape(QFrame.VLine)
        div.setFixedWidth(1); div.setStyleSheet(f"background:{P['border']};")
        desc = QLabel("PID Testbench  ·  SPS × WIE")
        desc.setStyleSheet(f"color:{P['muted']}; font-size:{pt(9)}px;")
        self.conn_dot = QLabel("OFFLINE")
        self.conn_dot.setStyleSheet(f"color:{P['danger']}; font-weight:700; font-size:{pt(9)}px;")
        l.addWidget(org); l.addWidget(div); l.addWidget(desc); l.addStretch()
        l.addWidget(self.conn_dot)
        return w

    # ── Connection ────────────────────────────────────────────────

    def _build_connection(self):
        g = QGroupBox("Connection"); l = QVBoxLayout(g)
        l.setSpacing(px(4)); l.setContentsMargins(px(6), px(14), px(6), px(6))

        r1 = QHBoxLayout(); r1.setSpacing(px(4))
        self.port_combo = _guard(QComboBox()); self._refresh_ports()
        self.port_combo.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        r1.addWidget(self.port_combo)
        btn_scan = QPushButton("Scan"); btn_scan.setFixedWidth(px(44))
        btn_scan.clicked.connect(self._refresh_ports); r1.addWidget(btn_scan)
        l.addLayout(r1)

        r2 = QHBoxLayout(); r2.setSpacing(px(4))
        r2.addWidget(self._lbl("Baud:"))
        self.baud_combo = _guard(QComboBox())
        bauds = ["9600","19200","38400","115200"]
        for b in bauds: self.baud_combo.addItem(b)
        self.baud_combo.setCurrentIndex(bauds.index("115200"))
        r2.addWidget(self.baud_combo); l.addLayout(r2)

        r3 = QHBoxLayout(); r3.setSpacing(px(4))
        self.btn_connect = QPushButton("Connect"); self.btn_connect.setObjectName("btn_connect")
        self.btn_connect.clicked.connect(self._do_connect)
        self.btn_disconnect = QPushButton("Disconnect"); self.btn_disconnect.setObjectName("btn_disconnect")
        self.btn_disconnect.clicked.connect(self._do_disconnect); self.btn_disconnect.setEnabled(False)
        r3.addWidget(self.btn_connect); r3.addWidget(self.btn_disconnect); l.addLayout(r3)
        return g

    # ── Live readout ──────────────────────────────────────────────

    def _build_readout(self):
        g = QGroupBox("Live Readings"); l = QVBoxLayout(g)
        l.setSpacing(px(2)); l.setContentsMargins(px(6), px(14), px(6), px(4))

        row = QHBoxLayout(); row.setSpacing(px(2))

        def block(attr, label, color):
            bw = QWidget(); bv = QVBoxLayout(bw); bv.setSpacing(px(1)); bv.setContentsMargins(0,0,0,0)
            v = QLabel("—"); v.setAlignment(Qt.AlignCenter)
            v.setStyleSheet(f"font-size:{pt(16)}px; font-weight:700; color:{color};")
            lb = QLabel(label.upper()); lb.setAlignment(Qt.AlignCenter)
            lb.setStyleSheet(f"color:{P['muted']}; font-size:{pt(8)}px; letter-spacing:1px;")
            bv.addWidget(v); bv.addWidget(lb); setattr(self, attr, v); return bw

        def vdiv():
            f = QFrame(); f.setFrameShape(QFrame.VLine)
            f.setFixedWidth(1); f.setStyleSheet(f"background:{P['border']};"); return f

        row.addWidget(block("live_temp", "Temp °C",    P["accent"]), stretch=1)
        row.addWidget(vdiv())
        row.addWidget(block("live_duty", "Duty %",     P["warn"]),   stretch=1)
        row.addWidget(vdiv())
        row.addWidget(block("live_dtdt", "dT/dt °/s",  P["dtdt"]),   stretch=1)
        l.addLayout(row)

        self.live_sp_lbl = QLabel("SP: — °C  |  Mode: —")
        self.live_sp_lbl.setAlignment(Qt.AlignCenter)
        self.live_sp_lbl.setStyleSheet(f"color:{P['muted']}; font-size:{pt(8)}px;")
        l.addWidget(self.live_sp_lbl)
        return g

    # ── Algorithm Control ─────────────────────────────────────────

    def _build_control(self):
        g = QGroupBox("Algorithm Control"); l = QVBoxLayout(g)
        l.setSpacing(px(4)); l.setContentsMargins(px(6), px(14), px(6), px(6))

        # Mode buttons — single line, equal widths, no newlines
        ml = QHBoxLayout(); ml.setSpacing(px(3)); self.mode_buttons = {}
        for mid, name in MODES.items():
            b = QPushButton(name); b.setObjectName(f"mode_{mid}")
            b.setCheckable(True); b.setChecked(mid == self.active_mode)
            b.clicked.connect(lambda _, m=mid: self._switch_mode(m))
            ml.addWidget(b); self.mode_buttons[mid] = b
        l.addLayout(ml)

        # Setpoint
        sr = QHBoxLayout(); sr.setSpacing(px(4)); sr.addWidget(self._lbl("Setpoint:"))
        self.sp_spin = _guard(QDoubleSpinBox())
        self.sp_spin.setRange(SP_MIN, SP_MAX); self.sp_spin.setValue(75.0); self.sp_spin.setSingleStep(5.0)
        self.sp_spin.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        sr.addWidget(self.sp_spin)
        btn_sp = QPushButton("Set"); btn_sp.setObjectName("btn_reset")
        btn_sp.setFixedWidth(px(46)); btn_sp.clicked.connect(self._send_setpoint)
        sr.addWidget(btn_sp); l.addLayout(sr)

        # Action row — four flat buttons, colour via left-border
        ar = QHBoxLayout(); ar.setSpacing(px(3))
        actions = [("Start","START","btn_start"), ("Autotune","AUTOTUNE","btn_autotune"),
                   ("Reset","RESET","btn_reset"), ("Stop","STOP","btn_stop")]
        for label, cmd, oid in actions:
            b = QPushButton(label); b.setObjectName(oid)
            b.clicked.connect(lambda _, c=cmd: self._send_cmd(c)); ar.addWidget(b)
        l.addLayout(ar)

        l.addWidget(self._sep())

        # Fan override
        fr = QHBoxLayout(); fr.setSpacing(px(3))
        self.btn_fan_on = QPushButton("Fans On"); self.btn_fan_on.setObjectName("btn_fan_on")
        self.btn_fan_on.clicked.connect(self._toggle_fan_on)
        self.btn_fan_off = QPushButton("Fans Off"); self.btn_fan_off.setObjectName("btn_fan_off")
        self.btn_fan_off.clicked.connect(self._toggle_fan_off); self.btn_fan_off.setEnabled(False)
        fr.addWidget(self.btn_fan_on); fr.addWidget(self.btn_fan_off)
        l.addLayout(fr)
        self.fan_lbl = self._lbl("Fan override: auto")
        self.fan_lbl.setAlignment(Qt.AlignCenter); l.addWidget(self.fan_lbl)

        # Clear plots
        bc = QPushButton("Clear Plots"); bc.setObjectName("btn_clear_plots")
        bc.clicked.connect(self._clear_plots); l.addWidget(bc)
        return g

    # ── Live gain tuning ──────────────────────────────────────────

    def _build_tune(self):
        g = QGroupBox("Gain Override"); l = QVBoxLayout(g)
        l.setSpacing(px(4)); l.setContentsMargins(px(6), px(14), px(6), px(6))

        def spin_row(label, lo, hi, step, default, attr):
            r = QHBoxLayout(); r.setSpacing(px(4))
            lbl = QLabel(label); lbl.setFixedWidth(px(22))
            lbl.setStyleSheet(f"color:{P['muted']}; font-size:{pt(9)}px;")
            sp = _guard(QDoubleSpinBox()); sp.setRange(lo, hi); sp.setValue(default)
            sp.setSingleStep(step); sp.setDecimals(4)
            sp.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
            r.addWidget(lbl); r.addWidget(sp); setattr(self, attr, sp); l.addLayout(r)

        spin_row("Kp", 0, 200, 0.5,  10.0, "tune_kp")
        spin_row("Ki", 0, 100, 0.05,  2.0, "tune_ki")
        spin_row("Kd", 0,  50, 0.05,  1.5, "tune_kd")

        br = QHBoxLayout(); br.setSpacing(px(4))
        ba = QPushButton("Apply"); ba.setObjectName("btn_apply_gains"); ba.clicked.connect(self._apply_gains)
        bf = QPushButton("Fetch"); bf.setFixedWidth(px(48)); bf.clicked.connect(lambda: self._send_cmd("GAINS"))
        br.addWidget(ba); br.addWidget(bf); l.addLayout(br)
        return g

    # ── Alarm ─────────────────────────────────────────────────────

    def _build_alarm(self):
        g = QGroupBox("Threshold Alarm"); l = QVBoxLayout(g)
        l.setSpacing(px(4)); l.setContentsMargins(px(6), px(14), px(6), px(6))
        r = QHBoxLayout(); r.setSpacing(px(4))
        self.alarm_chk = QCheckBox("Enable at")
        self.alarm_chk.toggled.connect(self._on_alarm_toggle); r.addWidget(self.alarm_chk)
        self.alarm_spin = _guard(QDoubleSpinBox())
        self.alarm_spin.setRange(20, 300); self.alarm_spin.setValue(90.0)
        self.alarm_spin.setSuffix(" °C"); self.alarm_spin.setSingleStep(5.0)
        self.alarm_spin.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.alarm_spin.valueChanged.connect(self._on_alarm_change); r.addWidget(self.alarm_spin)
        l.addLayout(r)
        self.alarm_lbl = QLabel("Alarm off")
        self.alarm_lbl.setAlignment(Qt.AlignCenter)
        self.alarm_lbl.setStyleSheet(f"color:{P['muted']}; font-size:{pt(9)}px;")
        l.addWidget(self.alarm_lbl)
        return g

    # ── Recording ─────────────────────────────────────────────────

    def _build_recording(self):
        g = QGroupBox("Data Recording"); l = QVBoxLayout(g)
        l.setSpacing(px(4)); l.setContentsMargins(px(6), px(14), px(6), px(6))

        r1 = QHBoxLayout(); r1.setSpacing(px(4)); r1.addWidget(self._lbl("Channel:"))
        self.rec_mode_combo = _guard(QComboBox())
        for mid, name in MODES.items(): self.rec_mode_combo.addItem(name, mid)
        self.rec_mode_combo.setCurrentIndex(2)
        self.rec_mode_combo.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        r1.addWidget(self.rec_mode_combo); l.addLayout(r1)

        self.btn_rec_25 = QPushButton("Record 25 min  (auto-stop)")
        self.btn_rec_25.setObjectName("btn_rec_25"); self.btn_rec_25.clicked.connect(self._start_25)
        l.addWidget(self.btn_rec_25)

        r2 = QHBoxLayout(); r2.setSpacing(px(4))
        self.btn_record = QPushButton("Record"); self.btn_record.setObjectName("btn_record")
        self.btn_record.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.btn_record.clicked.connect(self._start_manual)
        self.btn_stop_rec = QPushButton("Stop"); self.btn_stop_rec.setObjectName("btn_stop_rec")
        self.btn_stop_rec.setFixedWidth(px(46)); self.btn_stop_rec.clicked.connect(self._stop_recording)
        self.btn_stop_rec.setEnabled(False)
        r2.addWidget(self.btn_record); r2.addWidget(self.btn_stop_rec); l.addLayout(r2)

        self.rec_lbl = QLabel("Not recording")
        self.rec_lbl.setAlignment(Qt.AlignCenter)
        self.rec_lbl.setStyleSheet(f"color:{P['muted']}; font-size:{pt(9)}px;")
        l.addWidget(self.rec_lbl)

        self.rec_progress = QProgressBar(); self.rec_progress.setObjectName("rec_bar")
        self.rec_progress.setRange(0, AUTO_STOP_S); self.rec_progress.setValue(0)
        self.rec_progress.setFormat("%vs / 1500s"); self.rec_progress.setMaximumHeight(px(12))
        l.addWidget(self.rec_progress)

        l.addWidget(self._sep())

        btn_load = QPushButton("Load CSV Overlay"); btn_load.clicked.connect(self._load_csv)
        btn_exp  = QPushButton("Export Plot  (300 dpi)"); btn_exp.clicked.connect(self._export_plot)
        btn_stat = QPushButton("Export Session Stats"); btn_stat.clicked.connect(self._export_stats)
        for b in (btn_load, btn_exp, btn_stat): l.addWidget(b)
        return g

    # ── Metrics ───────────────────────────────────────────────────

    def _build_metrics(self):
        g = QGroupBox("Step Metrics"); l = QVBoxLayout(g)
        l.setContentsMargins(px(4), px(14), px(4), px(4))
        tabs = QTabWidget(); self.metric_panels = {}
        for mid, name in MODES.items():
            mp = MetricsPanel(name, mid); tabs.addTab(mp, name); self.metric_panels[mid] = mp
        l.addWidget(tabs); return g

    # ─────────────────────────────────────────────────────────────
    #  Serial plumbing
    # ─────────────────────────────────────────────────────────────

    def _refresh_ports(self):
        self.port_combo.clear()
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if not ports: ports = ["(none found)"]
        for p in ports: self.port_combo.addItem(p)

    def _do_connect(self):
        port = self.port_combo.currentText()
        if not port or "none" in port.lower(): self._log("No port selected."); return
        self.serial_worker.connect_signal.emit(port, int(self.baud_combo.currentText()))

    def _do_disconnect(self): self.serial_worker.disconnect_signal.emit()

    @pyqtSlot(bool)
    def _on_connection(self, ok):
        self.connected = ok
        self.btn_connect.setEnabled(not ok); self.btn_disconnect.setEnabled(ok)
        if ok:
            self.conn_dot.setText("ONLINE")
            self.conn_dot.setStyleSheet(f"color:{P['ok']}; font-weight:700; font-size:{pt(9)}px;")
            self.status_label.setText(f"Connected  —  {self.port_combo.currentText()}")
        else:
            self.conn_dot.setText("OFFLINE")
            self.conn_dot.setStyleSheet(f"color:{P['danger']}; font-weight:700; font-size:{pt(9)}px;")
            self.status_label.setText("Disconnected")

    @pyqtSlot(dict)
    def _on_row(self, row):
        self.last_row = row
        mode_str = row.get("mode", "").upper()
        mid = MODE_MAP.get(mode_str, None)
        if mid is None: mid = self.active_mode
        self.channels[mid].add(row)

        self.live_temp.setText(f"{row['temp']:.1f} °C")
        self.live_duty.setText(f"{row['duty']} %")
        self.live_dtdt.setText(f"{row.get('dtdt', 0.0):+.2f}")
        self.live_sp_lbl.setText(
            f"SP {row['sp']:.1f} °C  |  {row['mode']}  |  Kp {row['kp']:.3f}  Ki {row['ki']:.4f}")

        if self._alarm_enabled:
            over = row["temp"] >= self._alarm_temp
            if over and not self._alarm_firing:
                self._alarm_firing = True
                self.alarm_lbl.setText(f"ALARM  {row['temp']:.1f} >= {self._alarm_temp:.1f} °C")
                self.alarm_lbl.setStyleSheet(f"color:{P['danger']}; font-size:{pt(9)}px; font-weight:700;")
                _beep()
            elif not over and self._alarm_firing:
                self._alarm_firing = False
                self.alarm_lbl.setText(f"Armed at {self._alarm_temp:.1f} °C")
                self.alarm_lbl.setStyleSheet(f"color:{P['warn']}; font-size:{pt(9)}px;")

        if self.recording:
            if mid == self.record_mode:
                self._write_row(row)
                self.rec_progress.setValue(int(time.time() - self.record_start))
            elif mode_str != self._last_skip_mode:
                # Logged once per change, not on every 100 ms line.
                self._log(f"[REC] M:{mode_str} is not the recorded channel "
                          f"({MODES[self.record_mode]}), rows not saved")
        self._last_skip_mode = mode_str if self.recording and mid != self.record_mode else None

    @pyqtSlot(str)
    def _on_raw(self, raw): self._log(raw)

    @pyqtSlot(str)
    def _on_serial_error(self, err): self._log(f"[ERR] {err}"); self._on_connection(False)

    # ─────────────────────────────────────────────────────────────
    #  Control actions
    # ─────────────────────────────────────────────────────────────

    def _switch_mode(self, m):
        self.active_mode = m
        for mid, b in self.mode_buttons.items(): b.setChecked(mid == m)
        self._send_cmd(f"MODE {m}"); self._clear_plots()

    def _clear_plots(self):
        for mid in range(3):
            self.channels[mid] = ChannelData(mid)
            mp = self.metric_panels[mid]
            for attr in ("v_rise","v_over","v_settle","v_kp","v_ki","v_kd"):
                getattr(mp, attr).setText("—")
        self.canvas.clear_plots(); self._log("Plots cleared.")

    def _send_setpoint(self):
        sp = self.sp_spin.value(); self._send_cmd(f"S{sp:.1f}")
        for mid in range(3): self.channels[mid] = ChannelData(mid)

    def _toggle_fan_on(self):
        self._send_cmd("FANMODE ON"); self._fan_active = True
        self.btn_fan_on.setEnabled(False); self.btn_fan_off.setEnabled(True)
        self.fan_lbl.setText("Fan override: on  (heater locked off)")
        self.fan_lbl.setStyleSheet(f"color:{P['warn']}; font-size:{pt(8)}px;")

    def _toggle_fan_off(self):
        self._send_cmd("FANMODE OFF"); self._fan_active = False
        self.btn_fan_on.setEnabled(True); self.btn_fan_off.setEnabled(False)
        self.fan_lbl.setText("Fan override: auto")
        self.fan_lbl.setStyleSheet(f"color:{P['muted']}; font-size:{pt(8)}px;")

    def _send_cmd(self, cmd):
        if self.connected: self.send_to_serial.emit(cmd); self._log(f"TX  {cmd}")
        else: self._log(f"[offline]  {cmd}")

    def _send_custom_cmd(self):
        cmd = self.cmd_input.text().strip()
        if cmd: self._send_cmd(cmd); self.cmd_input.clear()

    def _apply_gains(self):
        kp, ki, kd = self.tune_kp.value(), self.tune_ki.value(), self.tune_kd.value()
        # The firmware applies these as the MANUAL gains and switches to MANUAL.
        self._send_cmd(f"SETGAINS {kp:.4f} {ki:.4f} {kd:.4f}")
        self.active_mode = 0
        for mid, b in self.mode_buttons.items(): b.setChecked(mid == 0)

    def _on_alarm_toggle(self, checked):
        self._alarm_enabled = checked; self._alarm_firing = False
        thr = self.alarm_spin.value(); self.canvas.draw_alarm_line(thr, checked)
        if checked:
            self.alarm_lbl.setText(f"Armed at {thr:.1f} °C")
            self.alarm_lbl.setStyleSheet(f"color:{P['warn']}; font-size:{pt(9)}px;")
        else:
            self.alarm_lbl.setText("Alarm off")
            self.alarm_lbl.setStyleSheet(f"color:{P['muted']}; font-size:{pt(9)}px;")

    def _on_alarm_change(self, val):
        self._alarm_temp = val
        if self._alarm_enabled:
            self.canvas.draw_alarm_line(val, True)
            self.alarm_lbl.setText(f"Armed at {val:.1f} °C")

    # ─────────────────────────────────────────────────────────────
    #  Recording
    # ─────────────────────────────────────────────────────────────

    def _start_25(self): self._rec_25_mode = True;  self._start_recording()
    def _start_manual(self): self._rec_25_mode = False; self._start_recording()

    def _start_recording(self):
        self.record_mode = self.rec_mode_combo.currentData(Qt.UserRole)
        if self.record_mode is None:
            self.record_mode = self.rec_mode_combo.currentIndex()
            self._log(f"[WARN] fell back to currentIndex={self.record_mode}")
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        path, _ = QFileDialog.getSaveFileName(
            self, "Save Recording", f"pid_{MODES[self.record_mode]}_{ts}.csv", "CSV (*.csv)")
        if not path: self._rec_25_mode = False; return
        self.csv_path = path; self.csv_file_obj = open(path, "w", newline="")
        self.csv_writer = csv.DictWriter(self.csv_file_obj,
                          fieldnames=["time_s","temp_c","setpoint","duty_pct","dtdt","kp","ki","kd","mode"])
        self.csv_writer.writeheader()
        self.recording = True; self.record_start = time.time()
        self.channels[self.record_mode] = ChannelData(self.record_mode)
        self.rec_progress.setMaximum(AUTO_STOP_S); self.rec_progress.setValue(0)
        self.btn_record.setEnabled(False); self.btn_rec_25.setEnabled(False)
        self.btn_stop_rec.setEnabled(True)
        self._rec_blink_state = True; self.rec_blink_timer.start(700)
        mode_name = MODES.get(self.record_mode, "?")
        self.rec_lbl.setText(f"Recording  {mode_name}")
        self._log(f"Recording started  →  {path}")

    def _write_row(self, row):
        if not self.csv_writer: return
        elapsed = time.time() - self.record_start
        self.csv_writer.writerow({
            "time_s": round(elapsed, 3), "temp_c": row["temp"], "setpoint": row["sp"],
            "duty_pct": row["duty"], "dtdt": row.get("dtdt", 0.0),
            "kp": row["kp"], "ki": row["ki"], "kd": row.get("kd", 0.0),
            "mode": MODES.get(self.record_mode, "?")})
        self.csv_file_obj.flush()

    def _stop_recording(self):
        if not self.recording: return
        self.recording = False; self._rec_25_mode = False
        self.rec_blink_timer.stop(); self._rec_blink_state = False
        if self.csv_file_obj: self.csv_file_obj.close(); self.csv_file_obj = None
        self.btn_record.setEnabled(True); self.btn_rec_25.setEnabled(True)
        self.btn_stop_rec.setEnabled(False)
        saved = Path(self.csv_path).name if self.csv_path else "—"
        self.rec_lbl.setText(f"Saved:  {saved}")
        self.rec_lbl.setStyleSheet(f"color:{P['ok']}; font-size:{pt(9)}px;")
        self._log(f"Recording stopped  —  {self.csv_path}")
        if self.csv_path: self.canvas.load_csv_overlay(self.csv_path, self.record_mode)

    def _blink_rec(self):
        self._rec_blink_state = not self._rec_blink_state
        c = P["danger"] if self._rec_blink_state else P["muted"]
        self.rec_lbl.setStyleSheet(f"color:{c}; font-size:{pt(9)}px; font-weight:600;")

    # ─────────────────────────────────────────────────────────────
    #  CSV overlay / export
    # ─────────────────────────────────────────────────────────────

    def _load_csv(self):
        path, _ = QFileDialog.getOpenFileName(self, "Load CSV Overlay", "", "CSV (*.csv)")
        if not path: return
        mid = next((m for m, n in MODES.items() if n in path.upper()), 2)
        self.canvas.load_csv_overlay(path, mid)
        self._log(f"Overlay:  {Path(path).name}  as {MODES[mid]}")

    def _export_plot(self):
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        path, _ = QFileDialog.getSaveFileName(self, "Export Plot", f"pid_plot_{ts}.png", "PNG (*.png)")
        if path: self.canvas.export_png(path); self._log(f"Plot exported  →  {path}")

    def _export_stats(self):
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        path, _ = QFileDialog.getSaveFileName(self, "Export Session Stats",
                   f"pid_stats_{ts}.csv", "CSV (*.csv)")
        if not path: return
        try:
            with open(path, "w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=["time_s","temp_c","setpoint","duty_pct",
                                                   "dtdt","kp","ki","kd","mode"])
                w.writeheader()
                for ch in self.channels.values():
                    for row in ch.to_csv_rows(): w.writerow(row)
            self._log(f"Stats exported  →  {path}")
        except OSError as e: self._log(f"[ERR] {e}")

    # ─────────────────────────────────────────────────────────────
    #  Timers
    # ─────────────────────────────────────────────────────────────

    def _refresh_plots(self):
        for mid, ch in self.channels.items():
            if ch.t:
                self.canvas.update_mode_data(mid, ch)
                self.metric_panels[mid].update_from_channel(ch)

    def _log(self, msg):
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.log_text.append(f"[{ts}]  {msg}")
        sb = self.log_text.verticalScrollBar(); sb.setValue(sb.maximum())

    def _tick_clock(self):
        ts = datetime.now().strftime("%H:%M:%S")
        if self.connected and self.last_row:
            r = self.last_row
            self.statusBar().showMessage(
                f"{ts}   SP {r.get('sp','?')} °C   T {r.get('temp','?')} °C   "
                f"D {r.get('duty','?')}%   Kp {r.get('kp','?')}   Ki {r.get('ki','?')}   "
                f"dT/dt {r.get('dtdt','?')}   F1 {r.get('f1',0)}%  F2 {r.get('f2',0)}%")
        else:
            self.statusBar().showMessage(f"{ts}   Awaiting data …")

        if self.recording and self.record_start is not None:
            elapsed = time.time() - self.record_start
            self.rec_progress.setValue(min(int(elapsed), AUTO_STOP_S))
            mode_name = MODES.get(self.record_mode, "?")
            if self._rec_25_mode:
                if elapsed >= AUTO_STOP_S:
                    self._log("Auto-stop: 25-minute recording complete.")
                    self._stop_recording(); return
                rem = max(0, AUTO_STOP_S - int(elapsed))
                m, s = divmod(rem, 60)
                self.rec_lbl.setText(f"Recording  {mode_name}  {int(elapsed)}s  |  {m:02d}:{s:02d}")
            else:
                self.rec_lbl.setText(f"Recording  {mode_name}  {int(elapsed)} s")

    def closeEvent(self, event):
        self._stop_recording()
        self.serial_worker.disconnect_signal.emit()
        self.serial_thread.quit(); self.serial_thread.wait(2000)
        event.accept()


# ─────────────────────────────────────────────────────────────────
#  Entry point
# ─────────────────────────────────────────────────────────────────
def main():
    QApplication.setAttribute(Qt.AA_EnableHighDpiScaling, True)
    QApplication.setAttribute(Qt.AA_UseHighDpiPixmaps,    True)
    app = QApplication(sys.argv)
    app.setApplicationName("PID Testbench  —  IEEE RIT-B")
    app.setStyle("Fusion")

    global _SCALE
    _SCALE = _compute_scale()

    # Fusion palette override — keeps the dark theme coherent under Fusion
    pal = QPalette()
    pal.setColor(QPalette.Window,          QColor(P["bg"]))
    pal.setColor(QPalette.WindowText,      QColor(P["text"]))
    pal.setColor(QPalette.Base,            QColor(P["surface2"]))
    pal.setColor(QPalette.AlternateBase,   QColor(P["surface"]))
    pal.setColor(QPalette.ToolTipBase,     QColor(P["surface2"]))
    pal.setColor(QPalette.ToolTipText,     QColor(P["text"]))
    pal.setColor(QPalette.Text,            QColor(P["text"]))
    pal.setColor(QPalette.Button,          QColor(P["surface2"]))
    pal.setColor(QPalette.ButtonText,      QColor(P["text"]))
    pal.setColor(QPalette.BrightText,      QColor(P["accent"]))
    pal.setColor(QPalette.Link,            QColor(P["accent"]))
    pal.setColor(QPalette.Highlight,       QColor(P["accent"]))
    pal.setColor(QPalette.HighlightedText, QColor(P["bg"]))
    pal.setColor(QPalette.Disabled, QPalette.ButtonText, QColor(P["subtle"]))
    pal.setColor(QPalette.Disabled, QPalette.Text,       QColor(P["subtle"]))
    app.setPalette(pal)

    # Application-wide default font — sans-serif, not monospace
    app.setFont(QFont("Segoe UI", pt(10)))

    win = MainWindow(); win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
