#!/usr/bin/env python3
"""Kona Swatch Scanner GUI Application.

A Tkinter-based GUI for managing Kona Cotton swatch scanning sessions.
Communicates with the shade_sayer device via serial port to capture
Lab color values for each swatch.

Features:
- Display all 365 Kona swatches in a sortable list
- Show color sample and Lab/RGB info for selected swatches
- Support single or multi-swatch scanning via EXTENDED selection
- Maintain scanned values in kona_captures.json
- Export to C++ header file for firmware use

Usage:
    python3 kona_scanner_gui.py [--port /dev/ttyACM0] [--data kona_captures.json] [--debug]
"""

import argparse
import colorsys
import dataclasses
import datetime as dt
import json
import math
import os
import pathlib
import platform
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from typing import Dict, List, Optional, Tuple
try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False
    print("Warning: pyserial not installed. Serial communication disabled.", file=sys.stderr)

# Import shared Kona table generation logic from generate_kona_table.py
_SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(_SCRIPTS_DIR))
from generate_kona_table import KonaEntry, MAX_ENTRIES, render_cpp  # noqa: E402

# Serial communication constants
DEVICE_READY_DELAY_S = 0.5  # Time to wait for device initialization after connecting


@dataclasses.dataclass
class SwatchData:
    """Data for a single Kona swatch."""
    panel: str
    panel_index: int
    id: int
    name: str
    L: Optional[float] = None
    a: Optional[float] = None
    b: Optional[float] = None
    R: Optional[int] = None
    G: Optional[int] = None
    B: Optional[int] = None
    measured: bool = False
    notes: str = ""
    # Raw sensor data for pipeline replay (populated when device protocol provides it)
    raw_x: Optional[int] = None
    raw_y: Optional[int] = None
    raw_z: Optional[int] = None
    raw_ir: Optional[int] = None
    raw_clear: Optional[int] = None
    raw_gain: Optional[int] = None
    raw_integration_ms: Optional[int] = None
    # Synthetic tint/shade/tone fields — populated from kona_synthetic_tints.json
    synthetic: bool = False
    source_id: Optional[int] = None     # Parent real swatch ID
    variant: Optional[str] = None       # "deep"|"dark"|"light"|"pale"|"muted"|"dusty"
    nearest_name: Optional[str] = None  # Nearest real-world colour name (meodai/resene/xkcd)
    description: str = ""                # One-sentence natural-language colour description


# Display gamma adjustment for monitor compensation.
# Values > 1.0 make colors appear darker, < 1.0 makes them lighter.
# Default 1.1 provides slight darkening which often looks better on typical LCD monitors.
# Set to 1.0 for mathematically exact sRGB output.
DISPLAY_GAMMA = 1.1


def lab_to_rgb(L: float, a: float, b: float) -> Tuple[int, int, int]:
    """Convert CIE Lab to sRGB for display.
    
    Uses D65 illuminant reference white and applies sRGB gamma encoding.
    An additional display gamma adjustment (DISPLAY_GAMMA) is applied to
    compensate for typical LCD monitor viewing conditions.
    """
    # Lab to XYZ
    fy = (L + 16.0) / 116.0
    fx = a / 500.0 + fy
    fz = fy - b / 200.0

    # Reference white D65
    Xn, Yn, Zn = 95.047, 100.0, 108.883

    def f_inv(t: float) -> float:
        delta = 6.0 / 29.0
        if t > delta:
            return t ** 3
        return 3 * delta ** 2 * (t - 4.0 / 29.0)

    X = Xn * f_inv(fx)
    Y = Yn * f_inv(fy)
    Z = Zn * f_inv(fz)

    # XYZ to linear RGB (sRGB D65 matrix)
    r_lin = ( 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z) / 100.0
    g_lin = (-0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z) / 100.0
    b_lin = ( 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z) / 100.0

    # sRGB gamma correction with additional display gamma adjustment
    def gamma(u: float) -> float:
        if u <= 0.0031308:
            v = 12.92 * u
        else:
            v = 1.055 * (u ** (1.0 / 2.4)) - 0.055
        # Apply display gamma to compensate for monitor characteristics
        # (value clamped to avoid negative numbers from out-of-gamut colors)
        return max(0.0, v) ** DISPLAY_GAMMA

    r = int(max(0, min(255, round(gamma(r_lin) * 255))))
    g = int(max(0, min(255, round(gamma(g_lin) * 255))))
    b = int(max(0, min(255, round(gamma(b_lin) * 255))))

    return r, g, b


def rgb_to_hex(r: int, g: int, b: int) -> str:
    """Convert RGB to hex color string."""
    return f"#{r:02x}{g:02x}{b:02x}"


def ciede2000(lab1: Tuple[float, float, float], lab2: Tuple[float, float, float]) -> float:
    """Calculate CIEDE2000 color difference between two Lab colors.
    
    Returns the perceptually uniform color difference (ΔE00).
    A value of ~1.0 is considered the just-noticeable difference.
    Values under 2.0 are generally considered a close match.
    """
    L1, a1, b1 = lab1
    L2, a2, b2 = lab2
    
    # Weighting factors (standard values)
    kL = 1.0
    kC = 1.0
    kH = 1.0
    
    # Step 1: Calculate C'i and h'i
    C1 = math.sqrt(a1 * a1 + b1 * b1)
    C2 = math.sqrt(a2 * a2 + b2 * b2)
    C_bar = (C1 + C2) / 2.0
    
    C_bar_7 = C_bar ** 7
    G = 0.5 * (1.0 - math.sqrt(C_bar_7 / (C_bar_7 + 25.0 ** 7)))
    
    a1_prime = a1 * (1.0 + G)
    a2_prime = a2 * (1.0 + G)
    
    C1_prime = math.sqrt(a1_prime * a1_prime + b1 * b1)
    C2_prime = math.sqrt(a2_prime * a2_prime + b2 * b2)
    
    def calc_h_prime(a_prime: float, b: float) -> float:
        if abs(a_prime) < 1e-10 and abs(b) < 1e-10:
            return 0.0
        h = math.degrees(math.atan2(b, a_prime))
        if h < 0:
            h += 360.0
        return h
    
    h1_prime = calc_h_prime(a1_prime, b1)
    h2_prime = calc_h_prime(a2_prime, b2)
    
    # Step 2: Calculate ΔL', ΔC', ΔH'
    delta_L_prime = L2 - L1
    delta_C_prime = C2_prime - C1_prime
    
    if C1_prime * C2_prime < 1e-10:
        delta_h_prime = 0.0
    else:
        dh = h2_prime - h1_prime
        if dh > 180.0:
            dh -= 360.0
        elif dh < -180.0:
            dh += 360.0
        delta_h_prime = dh
    
    delta_H_prime = 2.0 * math.sqrt(C1_prime * C2_prime) * math.sin(math.radians(delta_h_prime / 2.0))
    
    # Step 3: Calculate CIEDE2000 color difference
    L_bar_prime = (L1 + L2) / 2.0
    C_bar_prime = (C1_prime + C2_prime) / 2.0
    
    if C1_prime * C2_prime < 1e-10:
        h_bar_prime = h1_prime + h2_prime
    else:
        h_sum = h1_prime + h2_prime
        if abs(h1_prime - h2_prime) > 180.0:
            if h_sum < 360.0:
                h_sum += 360.0
            else:
                h_sum -= 360.0
        h_bar_prime = h_sum / 2.0
    
    T = (1.0 
         - 0.17 * math.cos(math.radians(h_bar_prime - 30.0))
         + 0.24 * math.cos(math.radians(2.0 * h_bar_prime))
         + 0.32 * math.cos(math.radians(3.0 * h_bar_prime + 6.0))
         - 0.20 * math.cos(math.radians(4.0 * h_bar_prime - 63.0)))
    
    delta_theta = 30.0 * math.exp(-((h_bar_prime - 275.0) / 25.0) ** 2)
    
    C_bar_prime_7 = C_bar_prime ** 7
    RC = 2.0 * math.sqrt(C_bar_prime_7 / (C_bar_prime_7 + 25.0 ** 7))
    
    L_bar_prime_minus_50_sq = (L_bar_prime - 50.0) ** 2
    SL = 1.0 + (0.015 * L_bar_prime_minus_50_sq) / math.sqrt(20.0 + L_bar_prime_minus_50_sq)
    SC = 1.0 + 0.045 * C_bar_prime
    SH = 1.0 + 0.015 * C_bar_prime * T
    
    RT = -math.sin(math.radians(2.0 * delta_theta)) * RC
    
    delta_E = math.sqrt(
        (delta_L_prime / (kL * SL)) ** 2 +
        (delta_C_prime / (kC * SC)) ** 2 +
        (delta_H_prime / (kH * SH)) ** 2 +
        RT * (delta_C_prime / (kC * SC)) * (delta_H_prime / (kH * SH))
    )
    
    return delta_E


class SerialConnection:
    """Manages serial communication with the shade_sayer device."""

    def __init__(self, port: str = "/dev/ttyACM0", baudrate: int = 115200, debug: bool = False,
                 log_callback=None):
        self.port = port
        self.baudrate = baudrate
        self.debug = debug
        self.serial: Optional[serial.Serial] = None
        self._lock = threading.Lock()
        self._log_callback = log_callback

    def _log(self, msg: str):
        """Log a message via the callback (GUI console) or stdout."""
        if self._log_callback:
            self._log_callback(msg)
        else:
            print(msg)

    def connect(self) -> bool:
        """Establish serial connection."""
        if not HAS_SERIAL:
            return False
        try:
            with self._lock:
                self.serial = serial.Serial(
                    self.port,
                    self.baudrate,
                    timeout=2.0,
                    write_timeout=2.0
                )
                # Wait for device USB-serial to initialize after connection
                time.sleep(DEVICE_READY_DELAY_S)
                # Clear any pending data
                self.serial.reset_input_buffer()
                self.serial.reset_output_buffer()
            return True
        except serial.SerialException as e:
            self._log(f"Serial connection error: {e}")
            return False

    def disconnect(self):
        """Close serial connection."""
        with self._lock:
            if self.serial and self.serial.is_open:
                try:
                    self.serial.close()
                except Exception:
                    pass
            self.serial = None

    def is_connected(self) -> bool:
        """Check if serial port is connected."""
        with self._lock:
            return self.serial is not None and self.serial.is_open

    def send_command(self, cmd: str, timeout: float = 5.0) -> Optional[str]:
        """Send a command and wait for response.
        
        Returns the response line or None on error/timeout.
        All TX/RX communication is logged to the console output area.
        """
        if not self.is_connected():
            return None

        with self._lock:
            try:
                # Clear input buffer before sending
                self.serial.reset_input_buffer()
                
                # Send command with newline
                self._log(f"TX: {cmd}")
                self.serial.write((cmd + "\n").encode("utf-8"))
                self.serial.flush()

                # Wait for response
                start_time = time.time()
                while time.time() - start_time < timeout:
                    if self.serial.in_waiting > 0:
                        line = self.serial.readline().decode("utf-8", errors="replace").strip()
                        self._log(f"RX: {line}")
                        if line.startswith("OK:") or line.startswith("ERR:"):
                            return line
                        # Continue reading - device may send log lines before response
                    time.sleep(0.05)

                self._log(f"RX: (timeout after {timeout}s)")
                return None  # Timeout
            except serial.SerialException as e:
                self._log(f"Serial error: {e}")
                return None

    def ping(self) -> bool:
        """Test connection with PING command."""
        response = self.send_command("PING", timeout=2.0)
        return response == "OK:PONG"

    def scan(self) -> Optional[Tuple]:
        """Request a scan and return Lab, RGB, and raw sensor values.

        Sends SCAN to get Lab+RGB, then RAWDATA to retrieve the raw ADC counts
        from the same measurement for pipeline replay.

        Returns tuple (L, a, b, R, G, B, raw_x, raw_y, raw_z, raw_ir, raw_clear,
        raw_gain, raw_integration_ms) or None on error.
        raw_* values are None when RAWDATA is unavailable or returns an error.
        """
        response = self.send_command("SCAN", timeout=20.0)
        if response is None:
            return None

        if response.startswith("OK:LAB:"):
            try:
                # Parse: OK:LAB:L,a,b:RGB:R,G,B
                parts = response.split(":")
                lab_parts = parts[2].split(",")
                rgb_parts = parts[4].split(",")
                L = float(lab_parts[0])
                a = float(lab_parts[1])
                b = float(lab_parts[2])
                R = int(rgb_parts[0])
                G = int(rgb_parts[1])
                B = int(rgb_parts[2])
            except (IndexError, ValueError) as e:
                self._log(f"Parse error: {e}, response: {response}")
                return None

            # Retrieve raw ADC values from the just-completed scan via RAWDATA command.
            # These values are used by regenerate_kona_lab.py to replay the measurement
            # through the color pipeline after pipeline parameter changes.
            raw_x = raw_y = raw_z = raw_ir = raw_clear = raw_gain = raw_int_ms = None
            raw_resp = self.send_command("RAWDATA", timeout=2.0)
            if raw_resp and raw_resp.startswith("OK:RAWDATA:"):
                try:
                    raw_parts = raw_resp[len("OK:RAWDATA:"):].split(",")
                    raw_x     = int(raw_parts[0])
                    raw_y     = int(raw_parts[1])
                    raw_z     = int(raw_parts[2])
                    raw_ir    = int(raw_parts[3])
                    raw_clear = int(raw_parts[4])
                    raw_gain  = int(raw_parts[5])
                    raw_int_ms = int(raw_parts[6])
                except (IndexError, ValueError) as e:
                    self._log(f"RAWDATA parse error: {e}, response: {raw_resp}")

            if self.debug:
                self._log(f"Parsed scan: L={L:.4f} a={a:.4f} b={b:.4f} RGB=({R},{G},{B})"
                          f" RAW=({raw_x},{raw_y},{raw_z},{raw_ir},{raw_clear},"
                          f"gain={raw_gain},int={raw_int_ms}ms)")
            return (L, a, b, R, G, B, raw_x, raw_y, raw_z, raw_ir, raw_clear,
                    raw_gain, raw_int_ms)

        self._log(f"Scan error: {response}")
        return None

    def exit_mode(self) -> bool:
        """Send EXIT command to device."""
        response = self.send_command("EXIT", timeout=2.0)
        return response is not None and response.startswith("OK:")


class KonaScannerApp:
    """Main application class for Kona Swatch Scanner GUI."""

    def __init__(self, root: tk.Tk, data_path: str, serial_port: str,
                 synthetic_path: Optional[str] = None, debug: bool = False):
        self.root = root
        self.data_path = pathlib.Path(data_path)
        self.synthetic_path = pathlib.Path(synthetic_path) if synthetic_path else None
        self.serial_port = serial_port
        self.debug = debug
        self.swatches: Dict[int, SwatchData] = {}
        self.synthetic_swatches: Dict[int, SwatchData] = {}  # keyed by synthetic ID
        self.scan_queue: List[int] = []
        self.scanning = False
        self.scan_thread: Optional[threading.Thread] = None
        self._capture_in_progress = False  # Re-entrancy guard for capture operations
        self._unsaved_changes = False  # Track whether there are unsaved scan changes
        self._last_captured_color: Optional[str] = None  # Hex color of last captured swatch

        self._setup_ui()
        # Create serial connection after UI setup so log callback can use console widget
        self.serial = SerialConnection(serial_port, debug=debug,
                                       log_callback=self._log_console)
        self._load_data()
        self._populate_treeview()

    def _setup_ui(self):
        """Set up the main UI components."""
        self.root.title("Kona Swatch Scanner")
        # Default geometry fits 1097x617 (4K at 350% scaling)
        self.root.geometry("1050x600")
        self.root.minsize(800, 500)  # Minimum usable size
        # Maximize window on startup (cross-platform)
        if platform.system() == 'Windows':
            self.root.state('zoomed')
        elif platform.system() == 'Darwin':
            # macOS: Use screen geometry to maximize
            self.root.update_idletasks()
            screen_width = self.root.winfo_screenwidth()
            screen_height = self.root.winfo_screenheight()
            self.root.geometry(f"{screen_width}x{screen_height}+0+0")
        else:
            # Linux: Use -zoomed attribute
            try:
                self.root.attributes('-zoomed', True)
            except tk.TclError:
                # Fallback for environments that don't support -zoomed
                self.root.update_idletasks()
                screen_width = self.root.winfo_screenwidth()
                screen_height = self.root.winfo_screenheight()
                self.root.geometry(f"{screen_width}x{screen_height}+0+0")
        # Make mouse pointer black (standard arrow cursor)
        self.root.config(cursor="left_ptr red")

        # Dark mode theme configuration
        self._setup_dark_theme()

        # Main frame with paned window
        main_pane = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main_pane.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Left frame: swatch list
        left_frame = ttk.Frame(main_pane)
        main_pane.add(left_frame, weight=3)

        # Toolbar
        toolbar = ttk.Frame(left_frame)
        toolbar.pack(fill=tk.X, pady=(0, 3))

        # Buttons - keyboard shortcuts in tooltips (hover) to save space
        # Button enablement reflects connection state
        self.connect_btn = ttk.Button(toolbar, text="Connect", command=self._on_connect)
        self.connect_btn.pack(side=tk.LEFT, padx=1)
        self.disconnect_btn = ttk.Button(toolbar, text="Disconnect", command=self._on_disconnect,
                                          state=tk.DISABLED)
        self.disconnect_btn.pack(side=tk.LEFT, padx=1)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=3)

        self.scan_selected_btn = ttk.Button(toolbar, text="Scan", command=self._on_scan_selected)
        self.scan_selected_btn.pack(side=tk.LEFT, padx=1)
        self.stop_scan_btn = ttk.Button(toolbar, text="Stop", command=self._on_stop_scan)
        self.stop_scan_btn.pack(side=tk.LEFT, padx=1)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=3)

        self.save_btn = ttk.Button(toolbar, text="Save", command=self._on_save)
        self.save_btn.pack(side=tk.LEFT, padx=1)
        self.export_cpp_btn = ttk.Button(toolbar, text="Export", command=self._on_export_cpp)
        self.export_cpp_btn.pack(side=tk.LEFT, padx=1)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=3)

        self.clear_scanned_btn = ttk.Button(toolbar, text="Clear", command=self._on_clear_scanned)
        self.clear_scanned_btn.pack(side=tk.LEFT, padx=1)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=3)

        self.measure_btn = ttk.Button(toolbar, text="Measure", command=self._on_measure)
        self.measure_btn.pack(side=tk.LEFT, padx=1)
        
        # Filter controls - compact layout
        filter_frame = ttk.Frame(left_frame)
        filter_frame.pack(fill=tk.X, pady=(0, 3))
        
        ttk.Label(filter_frame, text="Filter:").pack(side=tk.LEFT)
        self.filter_var = tk.StringVar()
        self.filter_var.trace_add("write", self._on_filter_change)
        self.filter_entry = ttk.Entry(filter_frame, textvariable=self.filter_var, width=15)
        self.filter_entry.pack(side=tk.LEFT, padx=3)
        
        ttk.Label(filter_frame, text="Show:").pack(side=tk.LEFT, padx=(5, 0))
        self.show_var = tk.StringVar(value="all")
        ttk.Radiobutton(filter_frame, text="All", variable=self.show_var, value="all",
                       command=self._on_filter_change).pack(side=tk.LEFT)
        ttk.Radiobutton(filter_frame, text="Scanned", variable=self.show_var, value="scanned",
                       command=self._on_filter_change).pack(side=tk.LEFT)
        ttk.Radiobutton(filter_frame, text="Not Scanned", variable=self.show_var, value="not_scanned",
                       command=self._on_filter_change).pack(side=tk.LEFT)

        # Synthetic tints checkbox — only visible when synthetic data is loaded
        self.show_synthetic_var = tk.BooleanVar(value=False)
        self.synthetic_cb = ttk.Checkbutton(
            filter_frame, text="Synthetics",
            variable=self.show_synthetic_var,
            command=self._on_filter_change)
        # Packed later by _load_synthetic_json() when data is available

        # Vertical PanedWindow so the treeview and console are resizable
        left_pane = ttk.PanedWindow(left_frame, orient=tk.VERTICAL)
        left_pane.pack(fill=tk.BOTH, expand=True)

        # Treeview with scrollbars
        tree_frame = ttk.Frame(left_pane)

        columns = ("panel", "index", "id", "name", "measured", "L", "a", "b")
        self.tree = ttk.Treeview(tree_frame, columns=columns, show="headings", selectmode="extended")

        self.tree.heading("panel", text="Panel", command=lambda: self._sort_column("panel"))
        self.tree.heading("index", text="Idx", command=lambda: self._sort_column("index"))
        self.tree.heading("id", text="ID", command=lambda: self._sort_column("id"))
        self.tree.heading("name", text="Name", command=lambda: self._sort_column("name"))
        self.tree.heading("measured", text="OK", command=lambda: self._sort_column("measured"))
        self.tree.heading("L", text="L*", command=lambda: self._sort_column("L"))
        self.tree.heading("a", text="a*", command=lambda: self._sort_column("a"))
        self.tree.heading("b", text="b*", command=lambda: self._sort_column("b"))

        # Compact column widths for small screens
        self.tree.column("panel", width=100)
        self.tree.column("index", width=35, anchor=tk.CENTER)
        self.tree.column("id", width=35, anchor=tk.CENTER)
        self.tree.column("name", width=90)
        self.tree.column("measured", width=30, anchor=tk.CENTER)
        self.tree.column("L", width=45, anchor=tk.CENTER)
        self.tree.column("a", width=45, anchor=tk.CENTER)
        self.tree.column("b", width=45, anchor=tk.CENTER)

        vsb = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self.tree.yview)
        hsb = ttk.Scrollbar(tree_frame, orient=tk.HORIZONTAL, command=self.tree.xview)
        self.tree.configure(yscrollcommand=vsb.set, xscrollcommand=hsb.set)

        self.tree.grid(row=0, column=0, sticky="nsew")
        vsb.grid(row=0, column=1, sticky="ns")
        hsb.grid(row=1, column=0, sticky="ew")
        tree_frame.grid_rowconfigure(0, weight=1)
        tree_frame.grid_columnconfigure(0, weight=1)

        self.tree.bind("<<TreeviewSelect>>", self._on_selection_change)

        left_pane.add(tree_frame, weight=3)

        # Console output area — shows serial TX/RX and measurement details
        console_frame = ttk.LabelFrame(left_pane, text="Console")
        # Use wrap=NONE with horizontal scrollbar so lines don't wrap
        self.console_text = tk.Text(console_frame, height=8, wrap=tk.NONE, state=tk.DISABLED,
                                    font=("Courier", 14), bg="#1e1e1e", fg="#d4d4d4",
                                    insertbackground="#d4d4d4")
        console_vsb = ttk.Scrollbar(console_frame, orient=tk.VERTICAL,
                                    command=self.console_text.yview)
        console_hsb = ttk.Scrollbar(console_frame, orient=tk.HORIZONTAL,
                                    command=self.console_text.xview)
        self.console_text.configure(yscrollcommand=console_vsb.set,
                                    xscrollcommand=console_hsb.set)
        self.console_text.grid(row=0, column=0, sticky="nsew")
        console_vsb.grid(row=0, column=1, sticky="ns")
        console_hsb.grid(row=1, column=0, sticky="ew")
        console_frame.grid_rowconfigure(0, weight=1)
        console_frame.grid_columnconfigure(0, weight=1)

        # Allow mouse text selection even though the widget is DISABLED.
        # Temporarily switch to NORMAL during mouse drag operations.
        self.console_text.bind("<ButtonPress-1>", self._console_enable_select)
        self.console_text.bind("<ButtonRelease-1>", self._console_disable_select)
        self.console_text.bind("<B1-Motion>", self._console_enable_select)

        # Prevent keyboard edits while still allowing Ctrl+A / Ctrl+C
        self.console_text.bind("<Key>", lambda e: "break"
                               if e.keysym not in ("c", "a")
                               or not (e.state & 0x4)  # 0x4 = Control modifier
                               else None)

        # Right-click context menu for console (Select All + Copy)
        self._console_menu = tk.Menu(self.console_text, tearoff=0,
                                     bg="#2d2d2d", fg="#d4d4d4",
                                     activebackground="#0078d4", activeforeground="#ffffff")
        self._console_menu.add_command(label="Select All    Ctrl+A",
                                       command=self._console_select_all)
        self._console_menu.add_command(label="Copy            Ctrl+C",
                                       command=self._console_copy)
        self.console_text.bind("<Button-3>", self._console_context_menu)
        self.console_text.bind("<Control-a>", lambda e: self._console_select_all())
        self.console_text.bind("<Control-c>", lambda e: self._console_copy())

        left_pane.add(console_frame, weight=1)

        # Status bar
        self.progress_var = tk.StringVar(value="Ready")
        progress_bar = ttk.Label(left_frame, textvariable=self.progress_var, relief=tk.SUNKEN)
        progress_bar.pack(fill=tk.X, pady=(3, 0))

        # Right frame: color info panel with scrollbar for small screens
        right_frame = ttk.Frame(main_pane)
        main_pane.add(right_frame, weight=1)
        
        # Create a canvas with scrollbar for the right panel content
        right_canvas = tk.Canvas(right_frame, highlightthickness=0, bg="#1e1e1e")
        right_scrollbar = ttk.Scrollbar(right_frame, orient=tk.VERTICAL, command=right_canvas.yview)
        right_content = ttk.Frame(right_canvas)
        
        right_canvas.configure(yscrollcommand=right_scrollbar.set)
        right_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        right_canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        
        right_canvas_frame = right_canvas.create_window((0, 0), window=right_content, anchor="nw")
        
        def configure_scroll_region(event):
            right_canvas.configure(scrollregion=right_canvas.bbox("all"))
        right_content.bind("<Configure>", configure_scroll_region)
        
        def configure_canvas_width(event):
            right_canvas.itemconfig(right_canvas_frame, width=event.width)
        right_canvas.bind("<Configure>", configure_canvas_width)

        # Color sample canvas - smaller for compact display
        self.color_canvas = tk.Canvas(right_content, width=120, height=120, bg="#404040",
                                       highlightthickness=1, highlightbackground="#555555")
        self.color_canvas.pack(pady=5)

        # Monospaced font for aligned values (size 11 for better readability)
        mono_font = ("Courier", 11)
        label_font = ("TkDefaultFont", 10)

        # Color info display (read-only text boxes for copying)
        info_frame = ttk.LabelFrame(right_content, text="Selected Info")
        info_frame.pack(fill=tk.X, padx=3, pady=3)

        self.info_entries = {}

        # Use grid layout for aligned labels and values
        # Row 0: L*, a*, and b*
        # Row 1: R, G, and B
        # Row 2: Hex (spanning)
        for col_idx, lab_label in enumerate(["L*", "a*", "b*"]):
            ttk.Label(info_frame, text=f"{lab_label}:", font=label_font, anchor=tk.W).grid(
                row=0, column=col_idx * 2, sticky=tk.W, padx=(5, 2), pady=1)
            lab_var = tk.StringVar(value="-")
            lab_entry = ttk.Entry(info_frame, textvariable=lab_var, width=8, state="readonly",
                                  font=mono_font, justify=tk.RIGHT)
            lab_entry.grid(row=0, column=col_idx * 2 + 1, sticky=tk.W, padx=(0, 5), pady=1)
            self.info_entries[lab_label] = lab_var

        for col_idx, rgb_label in enumerate(["R", "G", "B"]):
            ttk.Label(info_frame, text=f"{rgb_label}:", font=label_font, anchor=tk.W).grid(
                row=1, column=col_idx * 2, sticky=tk.W, padx=(5, 2), pady=1)
            rgb_var = tk.StringVar(value="-")
            rgb_entry = ttk.Entry(info_frame, textvariable=rgb_var, width=5, state="readonly",
                                  font=mono_font, justify=tk.RIGHT)
            rgb_entry.grid(row=1, column=col_idx * 2 + 1, sticky=tk.W, padx=(0, 5), pady=1)
            self.info_entries[rgb_label] = rgb_var

        # Hex row at bottom - spans columns
        ttk.Label(info_frame, text="Hex:", font=label_font, anchor=tk.W).grid(
            row=2, column=0, sticky=tk.W, padx=(5, 2), pady=1)
        hex_var = tk.StringVar(value="-")
        hex_entry = ttk.Entry(info_frame, textvariable=hex_var, width=10, state="readonly",
                              font=mono_font, justify=tk.LEFT)
        hex_entry.grid(row=2, column=1, columnspan=5, sticky=tk.W, pady=1)
        self.info_entries["Hex"] = hex_var

        # Nearest name row for synthetic swatches (row 3)
        ttk.Label(info_frame, text="Near:", font=label_font, anchor=tk.W).grid(
            row=3, column=0, sticky=tk.W, padx=(5, 2), pady=1)
        nearest_var = tk.StringVar(value="-")
        nearest_entry = ttk.Entry(info_frame, textvariable=nearest_var, width=20, state="readonly",
                                  font=mono_font, justify=tk.LEFT)
        nearest_entry.grid(row=3, column=1, columnspan=5, sticky=tk.W, pady=1)
        self.info_entries["Near"] = nearest_var

        # Description row (row 4)
        ttk.Label(info_frame, text="Desc:", font=label_font, anchor=tk.W).grid(
            row=4, column=0, sticky=tk.NW, padx=(5, 2), pady=1)
        desc_var = tk.StringVar(value="-")
        desc_entry = ttk.Entry(info_frame, textvariable=desc_var, width=40, state="readonly",
                               font=mono_font, justify=tk.LEFT)
        desc_entry.grid(row=4, column=1, columnspan=5, sticky=tk.W, pady=1)
        self.info_entries["Desc"] = desc_var

        # Configure column weights for proper resizing
        for c in (1, 3, 5):
            info_frame.columnconfigure(c, weight=1)

        # Last captured display - shows the most recently scanned swatch info
        last_captured_frame = ttk.LabelFrame(right_content, text="Last Captured")
        last_captured_frame.pack(fill=tk.X, padx=3, pady=3)

        self.lc_vars = {}
        # Row 0: Name (spanning)
        # Row 1: L* value, RGB value
        # Row 2: a* value, ΔE00 value
        # Row 3: b* value
        for field in ["Name", "L*", "a*", "b*", "RGB", "ΔE00"]:
            self.lc_vars[field] = tk.StringVar(value="-")

        # Row 0: Name
        ttk.Label(last_captured_frame, text="Name:", font=label_font, anchor=tk.W).grid(
            row=0, column=0, sticky=tk.W, padx=(5, 2), pady=1)
        ttk.Label(last_captured_frame, textvariable=self.lc_vars["Name"], font=mono_font, anchor=tk.W).grid(
            row=0, column=1, columnspan=3, sticky=tk.W, padx=(2, 5), pady=1)
        # Row 1: L* and RGB
        ttk.Label(last_captured_frame, text="L*:", font=label_font, anchor=tk.W).grid(
            row=1, column=0, sticky=tk.W, padx=(5, 2), pady=1)
        ttk.Label(last_captured_frame, textvariable=self.lc_vars["L*"], font=mono_font, anchor=tk.W).grid(
            row=1, column=1, sticky=tk.W, padx=(2, 5), pady=1)
        ttk.Label(last_captured_frame, text="RGB:", font=label_font, anchor=tk.W).grid(
            row=1, column=2, sticky=tk.W, padx=(5, 2), pady=1)
        ttk.Label(last_captured_frame, textvariable=self.lc_vars["RGB"], font=mono_font, anchor=tk.W).grid(
            row=1, column=3, sticky=tk.W, padx=(2, 5), pady=1)
        # Row 2: a* and ΔE00
        ttk.Label(last_captured_frame, text="a*:", font=label_font, anchor=tk.W).grid(
            row=2, column=0, sticky=tk.W, padx=(5, 2), pady=1)
        ttk.Label(last_captured_frame, textvariable=self.lc_vars["a*"], font=mono_font, anchor=tk.W).grid(
            row=2, column=1, sticky=tk.W, padx=(2, 5), pady=1)
        ttk.Label(last_captured_frame, text="ΔE00:", font=label_font, anchor=tk.W).grid(
            row=2, column=2, sticky=tk.W, padx=(5, 2), pady=1)
        ttk.Label(last_captured_frame, textvariable=self.lc_vars["ΔE00"], font=mono_font, anchor=tk.W).grid(
            row=2, column=3, sticky=tk.W, padx=(2, 5), pady=1)
        # Row 3: b*
        ttk.Label(last_captured_frame, text="b*:", font=label_font, anchor=tk.W).grid(
            row=3, column=0, sticky=tk.W, padx=(5, 2), pady=1)
        ttk.Label(last_captured_frame, textvariable=self.lc_vars["b*"], font=mono_font, anchor=tk.W).grid(
            row=3, column=1, sticky=tk.W, padx=(2, 5), pady=1)
        last_captured_frame.columnconfigure(1, weight=1)
        last_captured_frame.columnconfigure(3, weight=1)

        # Scan guidance frame
        scan_frame = ttk.LabelFrame(right_content, text="Scanning")
        scan_frame.pack(fill=tk.X, padx=3, pady=3)

        self.scan_vars = {}
        scan_fields = ["Status", "Swatch", "Panel", "Remaining"]
        for row_idx, field in enumerate(scan_fields):
            ttk.Label(scan_frame, text=f"{field}:", font=label_font, anchor=tk.W).grid(
                row=row_idx, column=0, sticky=tk.W, padx=(5, 2), pady=1)
            var = tk.StringVar(value="-" if field != "Status" else "Not scanning")
            ttk.Label(scan_frame, textvariable=var, font=mono_font, anchor=tk.W).grid(
                row=row_idx, column=1, sticky=tk.W, padx=(2, 5), pady=1)
            self.scan_vars[field] = var

        # Button row - Capture left, Skip right
        btn_row = ttk.Frame(scan_frame)
        btn_row.grid(row=len(scan_fields), column=0, columnspan=2, sticky=tk.EW, padx=3, pady=2)
        scan_frame.columnconfigure(1, weight=1)

        self.scan_button = ttk.Button(btn_row, text="Capture", command=self._on_capture_current,
                                       state=tk.DISABLED)
        self.scan_button.pack(side=tk.LEFT)

        self.skip_button = ttk.Button(btn_row, text="Skip", command=self._on_skip_current,
                                       state=tk.DISABLED)
        self.skip_button.pack(side=tk.RIGHT)

        # Statistics frame
        stats_frame = ttk.LabelFrame(right_content, text="Stats")
        stats_frame.pack(fill=tk.X, padx=3, pady=3)

        self.stats_vars = {}
        stats_fields = ["Total", "Scanned", "Remaining"]
        for col_idx, field in enumerate(stats_fields):
            ttk.Label(stats_frame, text=f"{field}:", font=label_font, anchor=tk.W).grid(
                row=0, column=col_idx * 2, sticky=tk.W, padx=(5, 2), pady=1)
            var = tk.StringVar(value="0")
            ttk.Label(stats_frame, textvariable=var, font=mono_font, anchor=tk.W).grid(
                row=0, column=col_idx * 2 + 1, sticky=tk.W, padx=(2, 5), pady=1)
            self.stats_vars[field] = var
        for c in (1, 3, 5):
            stats_frame.columnconfigure(c, weight=1)
        self._update_stats()

        # Bind keyboard shortcuts
        self._setup_keyboard_shortcuts()

    def _setup_dark_theme(self):
        """Configure the dark mode theme for the application."""
        # Dark mode color palette
        bg_dark = "#1e1e1e"       # Main background
        bg_medium = "#2d2d2d"     # Slightly lighter background
        bg_light = "#3c3c3c"      # Lighter elements
        fg_main = "#d4d4d4"       # Main text color
        fg_dim = "#808080"        # Dimmed/disabled text
        accent = "#0078d4"        # Selection/accent color
        accent_dark = "#005a9e"   # Pressed state accent
        border = "#555555"        # Border color

        # Configure root window background
        self.root.configure(bg=bg_dark)

        # Create and configure ttk style
        style = ttk.Style()

        # Use 'clam' theme as a base - it provides more customization options
        style.theme_use("clam")

        # Configure main frame and widget backgrounds
        style.configure(".", background=bg_dark, foreground=fg_main,
                        fieldbackground=bg_light, troughcolor=bg_medium,
                        bordercolor=border, lightcolor=bg_light, darkcolor=bg_dark)

        # Frame style
        style.configure("TFrame", background=bg_dark)

        # LabelFrame style
        style.configure("TLabelframe", background=bg_dark, foreground=fg_main,
                        bordercolor=border)
        style.configure("TLabelframe.Label", background=bg_dark, foreground=fg_main)

        # Label style
        style.configure("TLabel", background=bg_dark, foreground=fg_main)

        # Button style
        style.configure("TButton", background=bg_medium, foreground=fg_main,
                        bordercolor=border, lightcolor=bg_light, darkcolor=bg_dark,
                        focuscolor=accent)
        style.map("TButton",
                  background=[("active", bg_light), ("pressed", accent_dark),
                              ("disabled", bg_dark)],
                  foreground=[("disabled", fg_dim)])

        # Entry style
        style.configure("TEntry", fieldbackground=bg_light, foreground=fg_main,
                        insertcolor=fg_main, bordercolor=border,
                        lightcolor=border, darkcolor=border)
        style.map("TEntry",
                  fieldbackground=[("readonly", bg_medium), ("disabled", bg_dark)],
                  foreground=[("readonly", fg_main), ("disabled", fg_dim)])

        # Combobox style
        style.configure("TCombobox", fieldbackground=bg_light, foreground=fg_main,
                        background=bg_medium, arrowcolor=fg_main,
                        bordercolor=border)
        style.map("TCombobox",
                  fieldbackground=[("readonly", bg_light)],
                  background=[("active", bg_light)])

        # Radiobutton style
        style.configure("TRadiobutton", background=bg_dark, foreground=fg_main,
                        indicatorcolor=bg_light)
        style.map("TRadiobutton",
                  background=[("active", bg_medium)],
                  indicatorcolor=[("selected", accent)])

        # Checkbutton style
        style.configure("TCheckbutton", background=bg_dark, foreground=fg_main,
                        indicatorcolor=bg_light)
        style.map("TCheckbutton",
                  background=[("active", bg_medium)],
                  indicatorcolor=[("selected", accent)])

        # Scrollbar style
        style.configure("TScrollbar", background=bg_medium, troughcolor=bg_dark,
                        bordercolor=bg_dark, arrowcolor=fg_main,
                        lightcolor=bg_medium, darkcolor=bg_medium)
        style.map("TScrollbar",
                  background=[("active", bg_light), ("pressed", bg_light)])

        # PanedWindow style
        style.configure("TPanedwindow", background=bg_dark)
        style.configure("Sash", background=border, sashthickness=4)

        # Separator style
        style.configure("TSeparator", background=border)

        # Treeview style - dark theme for the swatch list
        style.configure("Treeview",
                        background=bg_medium,
                        foreground=fg_main,
                        fieldbackground=bg_medium,
                        bordercolor=border,
                        lightcolor=bg_medium,
                        darkcolor=bg_medium)
        style.configure("Treeview.Heading",
                        background=bg_light,
                        foreground=fg_main,
                        bordercolor=border,
                        lightcolor=bg_light,
                        darkcolor=bg_light)
        style.map("Treeview",
                  background=[("selected", accent)],
                  foreground=[("selected", "#ffffff")])
        style.map("Treeview.Heading",
                  background=[("active", "#4a4a4a")])

    def _setup_keyboard_shortcuts(self):
        """Set up keyboard shortcuts for the application."""
        # Spacebar for Capture
        self.root.bind("<space>", self._on_spacebar_capture)
        
        # Alt key shortcuts for toolbar buttons
        self.root.bind("<Alt-c>", lambda e: self._on_connect())
        self.root.bind("<Alt-d>", lambda e: self._on_disconnect())
        self.root.bind("<Alt-s>", lambda e: self._on_scan_selected())
        self.root.bind("<Alt-e>", lambda e: self._on_export_cpp())
        self.root.bind("<Alt-l>", lambda e: self._on_clear_scanned())
        self.root.bind("<Alt-k>", lambda e: self._on_skip_current())
        self.root.bind("<Alt-m>", lambda e: self._on_measure())
        
        # Ctrl+S for Save
        self.root.bind("<Control-s>", lambda e: self._on_save())
        
        # Ctrl+A for Select All in treeview
        self.root.bind("<Control-a>", self._on_select_all)
        
        # Escape to stop scan
        self.root.bind("<Escape>", lambda e: self._on_stop_scan())
        
        # Window close handler to warn about unsaved changes
        self.root.protocol("WM_DELETE_WINDOW", self._on_window_close)
        
        # Alt+F to focus filter entry
        self.root.bind("<Alt-f>", self._on_focus_filter)
        
        # Alt+1/2/3 for filter radio buttons, Alt+4 toggles synthetics
        self.root.bind("<Alt-Key-1>", lambda e: self._set_show_filter("all"))
        self.root.bind("<Alt-Key-2>", lambda e: self._set_show_filter("scanned"))
        self.root.bind("<Alt-Key-3>", lambda e: self._set_show_filter("not_scanned"))
        self.root.bind("<Alt-Key-4>", lambda e: self._toggle_synthetic())
        
        # Alt+T to focus treeview (swatch list)
        self.root.bind("<Alt-t>", self._on_focus_treeview)
        
        # Windows-style keyboard navigation for treeview
        # Ctrl+Arrow: Move focus without changing selection
        self.tree.bind("<Control-Up>", self._on_ctrl_up)
        self.tree.bind("<Control-Down>", self._on_ctrl_down)
        
        # Ctrl+Space: Toggle selection of focused item
        self.tree.bind("<Control-space>", self._on_ctrl_space)
        
        # Shift+Arrow: Extend selection (override default for consistent behavior)
        self.tree.bind("<Shift-Up>", self._on_shift_up)
        self.tree.bind("<Shift-Down>", self._on_shift_down)
        
        # Home/End: Move to first/last item
        self.tree.bind("<Home>", self._on_home)
        self.tree.bind("<End>", self._on_end)
        
        # Ctrl+Home/End: Move focus to first/last without changing selection
        self.tree.bind("<Control-Home>", self._on_ctrl_home)
        self.tree.bind("<Control-End>", self._on_ctrl_end)
        
        # Shift+Home/End: Extend selection to first/last item
        self.tree.bind("<Shift-Home>", self._on_shift_home)
        self.tree.bind("<Shift-End>", self._on_shift_end)

    def _log_console(self, msg: str):
        """Append a message to the console output text area.

        Thread-safe: schedules the update on the Tk main thread when called
        from a background thread (e.g. serial I/O).  Automatically scrolls
        to the latest line and caps the buffer at 2000 lines.
        """
        def _append():
            self.console_text.configure(state=tk.NORMAL)
            self.console_text.insert(tk.END, msg + "\n")
            # Cap buffer at 2000 lines to avoid unbounded memory growth
            line_count = int(self.console_text.index("end-1c").split(".")[0])
            if line_count > 2000:
                self.console_text.delete("1.0", f"{line_count - 2000}.0")
            self.console_text.see(tk.END)
            self.console_text.configure(state=tk.DISABLED)

        # Schedule on the main thread if called from a background thread
        try:
            self.root.after_idle(_append)
        except RuntimeError:
            # Fallback if Tk mainloop has already exited
            pass

    def _console_select_all(self):
        """Select all text in the console output area."""
        self.console_text.configure(state=tk.NORMAL)
        self.console_text.tag_add(tk.SEL, "1.0", tk.END)
        self.console_text.configure(state=tk.DISABLED)
        return "break"

    def _console_copy(self):
        """Copy selected text from the console to the clipboard."""
        try:
            sel = self.console_text.get(tk.SEL_FIRST, tk.SEL_LAST)
            self.root.clipboard_clear()
            self.root.clipboard_append(sel)
        except tk.TclError:
            pass  # No selection
        return "break"

    def _console_context_menu(self, event):
        """Show right-click context menu on the console output area."""
        self._console_menu.tk_popup(event.x_root, event.y_root)

    def _console_enable_select(self, event=None):
        """Temporarily enable the console text widget for mouse selection."""
        self.console_text.configure(state=tk.NORMAL)

    def _console_disable_select(self, event=None):
        """Re-disable the console text widget after mouse selection."""
        self.console_text.configure(state=tk.DISABLED)

    def _on_focus_filter(self, event=None):
        """Focus the filter entry field and select all text.
        
        Returns 'break' to prevent the event from propagating further
        in the Tkinter event system.
        """
        self.filter_entry.focus_set()
        self.filter_entry.select_range(0, tk.END)
        return "break"

    def _on_focus_treeview(self, event=None):
        """Focus the swatch list treeview.
        
        Sets widget focus to the treeview. If no item is currently selected,
        automatically selects and focuses the first item to enable immediate
        keyboard navigation. Uses focus_set() for widget focus and focus()
        for item focus within the tree.
        
        Returns 'break' to prevent the event from propagating further
        in the Tkinter event system.
        """
        self.tree.focus_set()
        # Select first item if nothing is selected
        if not self.tree.selection():
            children = self.tree.get_children()
            if children:
                self.tree.selection_set(children[0])
                self.tree.focus(children[0])
        return "break"

    def _on_select_all(self, event=None):
        """Select all visible items in the treeview."""
        # Only select all if treeview is focused, otherwise let default Ctrl+A behavior work
        if self.root.focus_get() == self.tree:
            children = self.tree.get_children()
            if children:
                self.tree.selection_set(children)
            return "break"
        return None  # Allow default behavior for other widgets

    def _get_adjacent_item(self, item: str, direction: int) -> Optional[str]:
        """Get the item adjacent to the given item in the specified direction.
        
        Args:
            item: The current item ID
            direction: -1 for previous (up), 1 for next (down)
            
        Returns:
            The adjacent item ID, or None if at boundary
        """
        children = self.tree.get_children()
        if not children:
            return None
        try:
            idx = children.index(item)
            new_idx = idx + direction
            if 0 <= new_idx < len(children):
                return children[new_idx]
        except ValueError:
            pass
        return None

    def _on_ctrl_up(self, event=None):
        """Move focus up without changing selection (Ctrl+Up)."""
        focused = self.tree.focus()
        if focused:
            prev_item = self._get_adjacent_item(focused, -1)
            if prev_item:
                self.tree.focus(prev_item)
                self.tree.see(prev_item)
        return "break"

    def _on_ctrl_down(self, event=None):
        """Move focus down without changing selection (Ctrl+Down)."""
        focused = self.tree.focus()
        if focused:
            next_item = self._get_adjacent_item(focused, 1)
            if next_item:
                self.tree.focus(next_item)
                self.tree.see(next_item)
        return "break"

    def _on_ctrl_space(self, event=None):
        """Toggle selection of the focused item (Ctrl+Space)."""
        focused = self.tree.focus()
        if focused:
            if focused in self.tree.selection():
                self.tree.selection_remove(focused)
            else:
                self.tree.selection_add(focused)
        return "break"

    def _on_shift_up(self, event=None):
        """Extend selection upward (Shift+Up)."""
        focused = self.tree.focus()
        if focused:
            prev_item = self._get_adjacent_item(focused, -1)
            if prev_item:
                self.tree.selection_add(prev_item)
                self.tree.focus(prev_item)
                self.tree.see(prev_item)
        return "break"

    def _on_shift_down(self, event=None):
        """Extend selection downward (Shift+Down)."""
        focused = self.tree.focus()
        if focused:
            next_item = self._get_adjacent_item(focused, 1)
            if next_item:
                self.tree.selection_add(next_item)
                self.tree.focus(next_item)
                self.tree.see(next_item)
        return "break"

    def _on_home(self, event=None):
        """Move focus and selection to the first item (Home)."""
        children = self.tree.get_children()
        if children:
            first_item = children[0]
            self.tree.selection_set(first_item)
            self.tree.focus(first_item)
            self.tree.see(first_item)
        return "break"

    def _on_end(self, event=None):
        """Move focus and selection to the last item (End)."""
        children = self.tree.get_children()
        if children:
            last_item = children[-1]
            self.tree.selection_set(last_item)
            self.tree.focus(last_item)
            self.tree.see(last_item)
        return "break"

    def _on_ctrl_home(self, event=None):
        """Move focus to first item without changing selection (Ctrl+Home)."""
        children = self.tree.get_children()
        if children:
            first_item = children[0]
            self.tree.focus(first_item)
            self.tree.see(first_item)
        return "break"

    def _on_ctrl_end(self, event=None):
        """Move focus to last item without changing selection (Ctrl+End)."""
        children = self.tree.get_children()
        if children:
            last_item = children[-1]
            self.tree.focus(last_item)
            self.tree.see(last_item)
        return "break"

    def _on_shift_home(self, event=None):
        """Extend selection from focused item to first item (Shift+Home)."""
        children = self.tree.get_children()
        focused = self.tree.focus()
        if children and focused:
            try:
                focused_idx = children.index(focused)
                # Select all items from first to focused (inclusive)
                for item_idx in range(focused_idx + 1):
                    self.tree.selection_add(children[item_idx])
                first_item = children[0]
                self.tree.focus(first_item)
                self.tree.see(first_item)
            except ValueError:
                pass
        return "break"

    def _on_shift_end(self, event=None):
        """Extend selection from focused item to last item (Shift+End)."""
        children = self.tree.get_children()
        focused = self.tree.focus()
        if children and focused:
            try:
                focused_idx = children.index(focused)
                # Select all items from focused to last (inclusive)
                for item_idx in range(focused_idx, len(children)):
                    self.tree.selection_add(children[item_idx])
                last_item = children[-1]
                self.tree.focus(last_item)
                self.tree.see(last_item)
            except ValueError:
                pass
        return "break"

    def _set_show_filter(self, value: str):
        """Set the show filter radio button value."""
        self.show_var.set(value)
        self._on_filter_change()

    def _toggle_synthetic(self):
        """Toggle the Synthetics checkbox and refresh the treeview."""
        self.show_synthetic_var.set(not self.show_synthetic_var.get())
        self._on_filter_change()

    def _load_data(self):
        """Load swatch data from kona_captures.json and optional synthetic tints."""
        self._load_json()
        self._load_synthetic_json()

    def _save_data(self) -> bool:
        """Save swatch data to kona_captures.json."""
        return self._save_json()

    def _load_json(self):
        """Load swatch data from a kona_captures.json file."""
        self.swatches.clear()

        if not self.data_path.exists():
            self._log_console(f"Data file not found: {self.data_path}")
            return

        try:
            with self.data_path.open(encoding="utf-8") as f:
                data = json.load(f)

            for entry in data.get("swatches", []):
                try:
                    swatch_id = int(entry.get("id", 0))
                    if swatch_id == 0:
                        continue

                    lab = entry.get("lab") or {}
                    L = float(lab["l"]) if lab.get("l") is not None else None
                    a = float(lab["a"]) if lab.get("a") is not None else None
                    b = float(lab["b"]) if lab.get("b") is not None else None

                    raw = entry.get("raw") or {}
                    raw_x = int(raw["x"]) if raw.get("x") is not None else None
                    raw_y = int(raw["y"]) if raw.get("y") is not None else None
                    raw_z = int(raw["z"]) if raw.get("z") is not None else None
                    raw_ir = int(raw["ir"]) if raw.get("ir") is not None else None
                    raw_clear = int(raw["clear"]) if raw.get("clear") is not None else None
                    raw_gain = int(raw["gain"]) if raw.get("gain") is not None else None
                    raw_integration_ms = int(raw["integration_ms"]) if raw.get("integration_ms") is not None else None

                    measured = bool(entry.get("measured", False))

                    self.swatches[swatch_id] = SwatchData(
                        panel=str(entry.get("panel", "")),
                        panel_index=int(entry.get("panel_index", 0)),
                        id=swatch_id,
                        name=str(entry.get("name", "")),
                        L=L,
                        a=a,
                        b=b,
                        measured=measured,
                        notes=str(entry.get("notes", "")),
                        raw_x=raw_x,
                        raw_y=raw_y,
                        raw_z=raw_z,
                        raw_ir=raw_ir,
                        raw_clear=raw_clear,
                        raw_gain=raw_gain,
                        raw_integration_ms=raw_integration_ms,
                        description=str(entry.get("description", "")),
                    )
                except (ValueError, KeyError, TypeError, AttributeError) as e:
                    self._log_console(f"Skipping invalid JSON swatch entry: {entry} ({e})")

            self._log_console(f"Loaded {len(self.swatches)} swatches from {self.data_path}")
        except Exception as e:
            self._log_console(f"Error loading JSON: {e}")

    def _load_synthetic_json(self):
        """Load synthetic tint/shade entries from kona_synthetic_tints.json.

        Synthetic entries are stored separately from real swatches and only
        displayed when the "Synthetics" checkbox is enabled. Each entry is
        linked to its parent real swatch via source_id.
        """
        self.synthetic_swatches.clear()

        if self.synthetic_path is None or not self.synthetic_path.exists():
            return

        try:
            with self.synthetic_path.open(encoding="utf-8") as f:
                data = json.load(f)

            for entry in data.get("swatches", []):
                try:
                    synth_id = int(entry.get("id", 0))
                    if synth_id == 0:
                        continue

                    lab = entry.get("lab") or {}
                    L = float(lab["l"]) if lab.get("l") is not None else None
                    a = float(lab["a"]) if lab.get("a") is not None else None
                    b = float(lab["b"]) if lab.get("b") is not None else None

                    source_id = int(entry.get("source_id", 0))
                    variant = str(entry.get("variant", ""))

                    # Inherit panel/panel_index from parent real swatch if available
                    parent = self.swatches.get(source_id)
                    panel = parent.panel if parent else ""
                    panel_index = parent.panel_index if parent else 0

                    self.synthetic_swatches[synth_id] = SwatchData(
                        panel=panel,
                        panel_index=panel_index,
                        id=synth_id,
                        name=str(entry.get("name", "")),
                        L=L,
                        a=a,
                        b=b,
                        measured=False,
                        notes=str(entry.get("notes", "")),
                        synthetic=True,
                        source_id=source_id,
                        variant=variant,
                        nearest_name=entry.get("nearest_name") or None,
                        description=str(entry.get("description", "")),
                    )
                except (ValueError, KeyError, TypeError, AttributeError) as e:
                    if self.debug:
                        self._log_console(f"Skipping invalid synthetic entry: {entry} ({e})")

            self._log_console(f"Loaded {len(self.synthetic_swatches)} synthetic tints "
                              f"from {self.synthetic_path}")

            # Show the Synthetics checkbox now that data is available
            self.synthetic_cb.pack(side=tk.LEFT, padx=(5, 0))

        except Exception as e:
            self._log_console(f"Error loading synthetic JSON: {e}")

    def _save_json(self) -> bool:
        """Save swatch data to a kona_captures.json file.

        Preserves existing top-level metadata (schema_version, capture_date,
        device, pipeline_config_snapshot) from the loaded JSON when possible.
        Raw sensor fields are written for swatches that have them so they can
        be used for pipeline replay via regenerate_kona_lab.py.
        """
        try:
            # Preserve existing top-level metadata if the file exists
            existing: dict = {}
            if self.data_path.exists():
                try:
                    with self.data_path.open(encoding="utf-8") as f:
                        existing = json.load(f)
                except Exception:
                    pass

            measured_count = sum(1 for s in self.swatches.values() if s.measured)
            if self.debug:
                self._log_console(f"Saving JSON: {len(self.swatches)} total swatches, {measured_count} measured")

            # Build swatch list sorted by panel / panel_index
            sorted_swatches = sorted(self.swatches.values(),
                                     key=lambda s: (s.panel, s.panel_index))

            swatch_entries = []
            for s in sorted_swatches:
                if self.debug and s.measured:
                    L_str = f"{s.L:.4f}" if s.L is not None else "None"
                    a_str = f"{s.a:.4f}" if s.a is not None else "None"
                    b_str = f"{s.b:.4f}" if s.b is not None else "None"
                    self._log_console(f"  Writing swatch[{s.id}] ({s.name}): "
                                      f"L={L_str} a={a_str} b={b_str}")

                entry: dict = {
                    "panel": s.panel,
                    "panel_index": s.panel_index,
                    "id": s.id,
                    "name": s.name,
                    "measured": s.measured,
                    "raw": {
                        "x": s.raw_x,
                        "y": s.raw_y,
                        "z": s.raw_z,
                        "ir": s.raw_ir,
                        "clear": s.raw_clear,
                        "gain": s.raw_gain,
                        "integration_ms": s.raw_integration_ms,
                    },
                    "lab": {
                        "l": round(s.L, 6) if s.L is not None else None,
                        "a": round(s.a, 6) if s.a is not None else None,
                        "b": round(s.b, 6) if s.b is not None else None,
                    },
                    "notes": s.notes,
                    "description": s.description,
                }
                swatch_entries.append(entry)

            output = {
                "schema_version": existing.get("schema_version", 1),
                "capture_date": existing.get("capture_date", ""),
                "device": existing.get("device", {"firmware_version": "", "firmware_commit": ""}),
                "pipeline_config_snapshot": existing.get("pipeline_config_snapshot", {}),
                "swatches": swatch_entries,
            }

            with self.data_path.open("w", encoding="utf-8") as f:
                json.dump(output, f, indent=2)
                f.write("\n")  # Trailing newline for POSIX compliance

            self._log_console(f"Saved {len(self.swatches)} swatches to {self.data_path}")
            self._unsaved_changes = False
            return True
        except Exception as e:
            self._log_console(f"Error saving JSON: {e}")
            messagebox.showerror("Error", f"Failed to save JSON: {e}")
            return False

    def _populate_treeview(self):
        """Populate the treeview with swatch data.

        When the Synthetics checkbox is enabled, synthetic tint/shade/tone
        entries are inserted immediately after their parent real swatch,
        sorted by variant order
        (deep → dark → light → pale → muted → dusty).
        """
        # Clear existing items
        for item in self.tree.get_children():
            self.tree.delete(item)

        filter_text = self.filter_var.get().lower()
        show_mode = self.show_var.get()
        show_synthetic = self.show_synthetic_var.get()

        # Build a lookup of synthetic entries grouped by source_id
        synthetics_by_source: Dict[int, List[SwatchData]] = {}
        if show_synthetic:
            variant_order = {
                "deep": 0, "dark": 1, "light": 2, "pale": 3,
                "muted": 4, "dusty": 5,
            }
            for s in self.synthetic_swatches.values():
                if s.source_id is not None:
                    synthetics_by_source.setdefault(s.source_id, []).append(s)
            # Sort each group by variant order
            for group in synthetics_by_source.values():
                group.sort(key=lambda x: variant_order.get(x.variant or "", 99))

        # Sort by panel, then panel_index
        sorted_swatches = sorted(self.swatches.values(),
                                key=lambda s: (s.panel, s.panel_index))

        for s in sorted_swatches:
            # Apply text filter to real swatch
            matches_filter = True
            if filter_text:
                if (filter_text not in s.name.lower() and
                    filter_text not in s.panel.lower() and
                    filter_text not in str(s.id)):
                    matches_filter = False

            if show_mode == "scanned" and not s.measured:
                if not show_synthetic or s.id not in synthetics_by_source:
                    continue
            if show_mode == "not_scanned" and s.measured:
                if not show_synthetic or s.id not in synthetics_by_source:
                    continue

            if matches_filter:
                measured_str = "✓" if s.measured else ""
                L_str = f"{s.L:.1f}" if s.L is not None else "-"
                a_str = f"{s.a:.1f}" if s.a is not None else "-"
                b_str = f"{s.b:.1f}" if s.b is not None else "-"

                self.tree.insert("", tk.END, iid=str(s.id),
                                values=(s.panel, s.panel_index, s.id, s.name,
                                       measured_str, L_str, a_str, b_str))

            # Insert synthetic children right after the parent
            if show_synthetic and s.id in synthetics_by_source:
                for syn in synthetics_by_source[s.id]:
                    # Apply text filter to synthetic entries too
                    if filter_text:
                        if (filter_text not in syn.name.lower() and
                            filter_text not in s.panel.lower() and
                            filter_text not in str(syn.id) and
                            filter_text not in str(s.id)):
                            continue

                    L_str = f"{syn.L:.1f}" if syn.L is not None else "-"
                    a_str = f"{syn.a:.1f}" if syn.a is not None else "-"
                    b_str = f"{syn.b:.1f}" if syn.b is not None else "-"

                    # Use "syn_" prefix for unique iid to avoid collision
                    display_name = syn.name
                    if syn.nearest_name:
                        display_name = f"{syn.name} ({syn.nearest_name})"
                    self.tree.insert("", tk.END, iid=f"syn_{syn.id}",
                                    values=(s.panel, s.panel_index, syn.id,
                                           f"  ↳ {display_name}",
                                           "~", L_str, a_str, b_str),
                                    tags=("synthetic",))

        # Style synthetic rows with a distinct foreground color
        self.tree.tag_configure("synthetic", foreground="#888888")

        self._update_stats()

    def _update_stats(self):
        """Update statistics display."""
        total = len(self.swatches)
        scanned = sum(1 for s in self.swatches.values() if s.measured)
        self.stats_vars["Total"].set(str(total))
        self.stats_vars["Scanned"].set(str(scanned))
        self.stats_vars["Remaining"].set(str(total - scanned))

    def _sort_column(self, col: str):
        """Sort treeview by column."""
        items = [(self.tree.set(k, col), k) for k in self.tree.get_children("")]
        
        # Determine sort type
        try:
            items = [(float(v) if v and v not in ("-", "✓", "~") else 0, k) for v, k in items]
        except ValueError:
            pass
        
        items.sort()
        for index, (_, k) in enumerate(items):
            self.tree.move(k, "", index)

    def _on_filter_change(self, *args):
        """Handle filter changes."""
        self._populate_treeview()

    def _on_selection_change(self, event):
        """Handle treeview selection change."""
        selection = self.tree.selection()
        if not selection:
            self._clear_color_info()
            return

        # Show info for first selected item — resolve synthetic IDs
        item_id = selection[0]
        if item_id.startswith("syn_"):
            synth_id = int(item_id[4:])
            swatch = self.synthetic_swatches.get(synth_id)
        else:
            swatch = self.swatches.get(int(item_id))
        if swatch:
            # During scanning, preserve the last captured color in the canvas
            # so the user can verify the captured color matches the swatch.
            # The info panel will still show the next item's values.
            update_canvas = not self.scanning or self._last_captured_color is None
            self._show_color_info(swatch, update_canvas=update_canvas)

    def _show_color_info(self, swatch: SwatchData, update_canvas: bool = True):
        """Display color information for a swatch.
        
        Args:
            swatch: The swatch data to display
            update_canvas: Whether to update the color canvas. Set to False during
                          scanning to preserve the last captured color for verification.
        """
        if swatch.L is not None and swatch.a is not None and swatch.b is not None:
            self.info_entries["L*"].set(f"{swatch.L:.2f}")
            self.info_entries["a*"].set(f"{swatch.a:.2f}")
            self.info_entries["b*"].set(f"{swatch.b:.2f}")

            # Always compute RGB from Lab for accurate color display.
            # The device-reported RGB values have saturation/lightness corrections
            # applied which can make colors appear different from the physical swatch.
            r, g, b = lab_to_rgb(swatch.L, swatch.a, swatch.b)

            self.info_entries["R"].set(str(r))
            self.info_entries["G"].set(str(g))
            self.info_entries["B"].set(str(b))

            hex_color = rgb_to_hex(r, g, b)
            self.info_entries["Hex"].set(hex_color)

            # Update color sample (unless preserving last captured color during scanning)
            if update_canvas:
                self.color_canvas.config(bg=hex_color)
        else:
            for key in ["L*", "a*", "b*", "R", "G", "B", "Hex"]:
                self.info_entries[key].set("-")
            if update_canvas:
                self.color_canvas.config(bg="#404040")

        # Show nearest real-world colour name for synthetic swatches
        self.info_entries["Near"].set(swatch.nearest_name if swatch.nearest_name else "-")

        # Show description
        self.info_entries["Desc"].set(swatch.description if swatch.description else "-")

    def _clear_color_info(self):
        """Clear color information display."""
        for var in self.info_entries.values():
            var.set("-")
        self.color_canvas.config(bg="#404040")

    def _on_connect(self):
        """Handle connect button click."""
        if not HAS_SERIAL:
            messagebox.showerror("Error", "pyserial library not installed.\nInstall with: pip install pyserial")
            return

        if self.serial.is_connected():
            messagebox.showinfo("Info", "Already connected")
            return

        self.progress_var.set("Connecting...")
        self.root.update()

        if self.serial.connect():
            # Test connection
            if self.serial.ping():
                # Update button states to reflect connection
                self.connect_btn.config(state=tk.DISABLED)
                self.disconnect_btn.config(state=tk.NORMAL)
                self.progress_var.set("Connected to device")
            else:
                self.serial.disconnect()
                self.progress_var.set("Device connected but not in serial mode")
                messagebox.showwarning("Warning", 
                    "Serial port opened but device not responding.\n\n"
                    "Make sure the device is in serial scan mode:\n"
                    "1. Connect device via USB\n"
                    "2. Press button 5 times quickly\n"
                    "3. Wait for 'Serial scan mode active' announcement")
        else:
            self.progress_var.set("Failed to connect")
            messagebox.showerror("Error", f"Failed to connect to {self.serial_port}")

    def _on_disconnect(self):
        """Handle disconnect button click."""
        if not self.serial.is_connected():
            return

        # Send exit command if connected
        self.serial.exit_mode()
        self.serial.disconnect()
        # Update button states to reflect disconnection
        self.connect_btn.config(state=tk.NORMAL)
        self.disconnect_btn.config(state=tk.DISABLED)
        self.progress_var.set("Disconnected")

    def _on_scan_selected(self):
        """Start scanning selected swatches."""
        self._log_console("[SCAN] Button clicked")
        
        if not self.serial.is_connected():
            self._log_console("[SCAN] Not connected - showing warning")
            messagebox.showwarning("Warning", "Not connected to device")
            return

        selection = self.tree.selection()
        self._log_console(f"[SCAN] Selection: {len(selection)} item(s)")
        
        if not selection:
            self._log_console("[SCAN] No selection - showing warning")
            messagebox.showwarning("Warning", "No swatches selected")
            return

        # Build scan queue from selection — exclude synthetic entries (not scannable)
        real_selection = [item_id for item_id in selection
                          if not item_id.startswith("syn_")]
        self._log_console(f"[SCAN] Real selection (non-synthetic): {len(real_selection)} item(s)")
        
        if not real_selection:
            self._log_console("[SCAN] Only synthetic items selected - showing warning")
            messagebox.showwarning("Warning",
                "Synthetic color variations cannot be scanned.\n"
                "Please select a real swatch.")
            return

        self.scan_queue = [int(item_id) for item_id in real_selection]
        self._scan_original_selection = list(real_selection)  # Store original selection
        self.scanning = True
        
        # Update progress bar to indicate scan session started
        count = len(self.scan_queue)
        swatch_word = "swatch" if count == 1 else "swatches"
        self.progress_var.set(f"Scan session started: {count} {swatch_word}")
        self._log_console(f"[SCAN] Session started with {count} {swatch_word}")
        
        self._update_scan_ui()

    def _update_scan_ui(self):
        """Update scan UI state.
        
        Forces an immediate UI refresh via update_idletasks() to ensure the
        status label and button states are visually updated without waiting
        for event loop processing.
        """
        if self.scanning and self.scan_queue:
            current_id = self.scan_queue[0]
            swatch = self.swatches.get(current_id)
            if swatch:
                remaining = len(self.scan_queue)
                self.scan_vars["Status"].set("Ready to scan")
                self.scan_vars["Swatch"].set(swatch.name)
                self.scan_vars["Panel"].set(f"{swatch.panel}, Idx {swatch.panel_index}")
                self.scan_vars["Remaining"].set(str(remaining))
                self.scan_button.config(state=tk.NORMAL)
                self.skip_button.config(state=tk.NORMAL)
                
                self._log_console(f"[SCAN] Ready to scan: {swatch.name} ({remaining} remaining)")
                
                # Update selection to show only remaining items to scan
                # This deselects items as they are scanned
                remaining_selection = [str(item_id) for item_id in self.scan_queue
                                       if self.tree.exists(str(item_id))]
                if remaining_selection:
                    self.tree.selection_set(remaining_selection)
                
                # Focus and scroll to current item
                self.tree.focus(str(current_id))
                self.tree.see(str(current_id))
            else:
                self._log_console(f"[SCAN] Swatch ID {current_id} not found in swatches dict - skipping")
                self._advance_scan()
        else:
            self.scan_vars["Status"].set("Not scanning")
            self.scan_vars["Swatch"].set("-")
            self.scan_vars["Panel"].set("-")
            self.scan_vars["Remaining"].set("-")
            self.scan_button.config(state=tk.DISABLED)
            self.skip_button.config(state=tk.DISABLED)
            self.scanning = False
            # Clear last captured color when scanning ends so selection changes update canvas normally
            self._last_captured_color = None
            # Clean up stored selection
            if hasattr(self, '_scan_original_selection'):
                del self._scan_original_selection
        
        # Force immediate UI refresh so status changes are visible right away
        # Using update_idletasks() is safer than update() as it doesn't process
        # user input events which could cause re-entrancy issues.
        self.root.update_idletasks()


    def _on_capture_current(self):
        """Capture current swatch in queue.

        Performs a scan, then a verification measurement.  If the verification
        CIEDE2000 ΔE is not < 1.0 ("excellent match"), the scan+verify cycle
        is retried up to 3 total attempts.  If all attempts fail, the scan
        session is terminated with an error message.
        """
        if not self.scanning or not self.scan_queue:
            return

        # Re-entrancy guard - prevent concurrent captures from root.update() processing events
        if self._capture_in_progress:
            return
        self._capture_in_progress = True

        try:
            if not self.serial.is_connected():
                messagebox.showerror("Error", "Device disconnected")
                self.scanning = False
                self._update_scan_ui()
                return

            current_id = self.scan_queue[0]
            swatch = self.swatches.get(current_id)
            if not swatch:
                self._advance_scan()
                return

            MAX_VERIFY_ATTEMPTS = 3
            verified = False

            for attempt in range(1, MAX_VERIFY_ATTEMPTS + 1):
                if attempt == 1:
                    self.scan_vars["Status"].set(f"Scanning...")
                else:
                    self.scan_vars["Status"].set(f"Attempt {attempt}/{MAX_VERIFY_ATTEMPTS}")
                self.scan_vars["Swatch"].set(swatch.name)
                self.scan_button.config(state=tk.DISABLED)
                self.skip_button.config(state=tk.DISABLED)
                self.root.update()

                # --- Capture scan ---
                result = self.serial.scan()
                if not result:
                    self._log_console(f"Scan failed for {swatch.name} (attempt {attempt})")
                    time.sleep(0.5)  # Brief delay before retry
                    continue

                L, a, b, R, G, B, raw_x, raw_y, raw_z, raw_ir, raw_clear, raw_gain, raw_int_ms = result

                # Store scan results in the swatch object (canonical data store)
                swatch.L = L
                swatch.a = a
                swatch.b = b
                swatch.R = R
                swatch.G = G
                swatch.B = B
                swatch.raw_x = raw_x
                swatch.raw_y = raw_y
                swatch.raw_z = raw_z
                swatch.raw_ir = raw_ir
                swatch.raw_clear = raw_clear
                swatch.raw_gain = raw_gain
                swatch.raw_integration_ms = raw_int_ms
                swatch.measured = True

                if self.debug:
                    self._log_console(f"Stored in swatch[{current_id}] ({swatch.name}): "
                                      f"L={swatch.L:.4f} a={swatch.a:.4f} b={swatch.b:.4f} "
                                      f"RGB=({swatch.R},{swatch.G},{swatch.B})")

                # --- Verification measurement ---
                self.scan_vars["Status"].set(f"Verifying...")
                self.root.update()

                verify_result = self.serial.scan()
                if not verify_result:
                    self._log_console(f"Verification scan failed for {swatch.name} (attempt {attempt})")
                    time.sleep(0.5)  # Brief delay before retry
                    continue

                v_L, v_a, v_b, *_ = verify_result
                delta_e = ciede2000((L, a, b), (v_L, v_a, v_b))

                self._log_console(
                    f"Verify {swatch.name} attempt {attempt}: "
                    f"ref L={L:.2f} a={a:.2f} b={b:.2f}  "
                    f"meas L={v_L:.2f} a={v_a:.2f} b={v_b:.2f}  "
                    f"ΔE={delta_e:.4f}")

                if delta_e < 1.0:
                    verified = True
                    break
                else:
                    self._log_console(
                        f"  ΔE={delta_e:.4f} ≥ 1.0 — not excellent, retrying..."
                        if attempt < MAX_VERIFY_ATTEMPTS
                        else f"  ΔE={delta_e:.4f} ≥ 1.0 — not excellent after {MAX_VERIFY_ATTEMPTS} attempts")
                    if attempt < MAX_VERIFY_ATTEMPTS:
                        time.sleep(0.5)  # Brief delay before retry

            if not verified:
                msg = (f"Could not obtain an excellent match for {swatch.name} "
                       f"after {MAX_VERIFY_ATTEMPTS} attempts.\n\n"
                       f"Scan session terminated.")
                self._log_console(f"SCAN ABORTED: {msg}")
                messagebox.showerror("Verification Failed", msg)
                self.scanning = False
                self._update_scan_ui()
                return

            # --- Update UI with the verified capture ---
            tree_item_id = str(current_id)
            if self.tree.exists(tree_item_id):
                measured_str = "✓"
                self.tree.set(tree_item_id, "measured", measured_str)
                self.tree.set(tree_item_id, "L", f"{swatch.L:.1f}")
                self.tree.set(tree_item_id, "a", f"{swatch.a:.1f}")
                self.tree.set(tree_item_id, "b", f"{swatch.b:.1f}")
                if self.debug:
                    tree_values = self.tree.item(tree_item_id, 'values')
                    self._log_console(f"Treeview item {tree_item_id} values: {tree_values}")
            elif self.debug:
                self._log_console(f"Warning: Treeview item {tree_item_id} does not exist (filtered?)")

            # Update display panel - read back from swatch to ensure consistency
            self._show_color_info(swatch)
            self._update_stats()

            # Store the last captured color for the canvas display during scanning.
            # This allows visual verification that the captured color matches the swatch.
            r, g, b = lab_to_rgb(swatch.L, swatch.a, swatch.b)
            self._last_captured_color = rgb_to_hex(r, g, b)
            self.color_canvas.config(bg=self._last_captured_color)

            # Update "Last Captured" display to show what was just scanned
            # This persists even when selection changes to next item
            self.lc_vars["Name"].set(swatch.name)
            self.lc_vars["L*"].set(f"{swatch.L:.2f}")
            self.lc_vars["a*"].set(f"{swatch.a:.2f}")
            self.lc_vars["b*"].set(f"{swatch.b:.2f}")
            self.lc_vars["RGB"].set(f"({r}, {g}, {b})")
            self.lc_vars["ΔE00"].set("-")

            self.progress_var.set(f"Captured {swatch.name}: L={swatch.L:.1f} a={swatch.a:.1f} b={swatch.b:.1f}")

            # Force immediate UI refresh so the canvas and labels are visually updated
            # before advancing to the next item. Without this, the updates may not be
            # visible until the user moves the mouse or triggers another event.
            self.root.update_idletasks()

            # Mark that we have unsaved changes
            self._unsaved_changes = True

            # Play system bell to indicate successful scan
            self.root.bell()

            self._advance_scan()
        finally:
            self._capture_in_progress = False

    def _on_spacebar_capture(self, event):
        """Handle spacebar keypress as shortcut for Capture button."""
        # Only capture if we're in scanning mode, have items in queue, not already capturing,
        # and the Capture button is enabled.
        # Use instate(['!disabled']) to check if button is NOT disabled (ttk idiomatic approach),
        # and _capture_in_progress to prevent re-entrancy during root.update() calls.
        if (self.scanning and self.scan_queue and 
            not self._capture_in_progress and
            self.scan_button.instate(['!disabled'])):
            self._on_capture_current()
        return "break"  # Prevent event propagation

    def _on_skip_current(self):
        """Skip current swatch in queue."""
        if not self.scanning or not self.scan_queue:
            return
        self._advance_scan()

    def _advance_scan(self):
        """Move to next swatch in queue."""
        if self.scan_queue:
            self.scan_queue.pop(0)
        
        if not self.scan_queue:
            self.scanning = False
            self.progress_var.set("Scan complete")
            # Prompt to save if there are unsaved changes
            if self._unsaved_changes:
                save_now = messagebox.askyesno(
                    "Scan Complete",
                    "Scan session complete.\n\nYou have unsaved changes. Save data now?")
                if save_now:
                    self._save_data()
            else:
                messagebox.showinfo("Info", "Scan session complete")

        self._update_scan_ui()

    def _on_stop_scan(self):
        """Stop current scan session."""
        self.scan_queue.clear()
        self.scanning = False
        self._update_scan_ui()
        self.progress_var.set("Scan stopped")

    def _on_save(self):
        """Save data to kona_captures.json."""
        if self._save_data():
            messagebox.showinfo("Info", f"Saved to {self.data_path}")

    def _on_export_cpp(self):
        """Export to C++ header file."""
        # Only export measured swatches
        measured = [s for s in self.swatches.values() 
                   if s.measured and s.L is not None and s.a is not None and s.b is not None]
        
        if not measured:
            messagebox.showwarning("Warning", "No scanned swatches to export")
            return

        # Ask for output file
        output_path = filedialog.asksaveasfilename(
            title="Export C++ File",
            defaultextension=".cpp",
            filetypes=[("C++ Source", "*.cpp"), ("All Files", "*.*")],
            initialfile="konaref_generated.cpp"
        )

        if not output_path:
            return

        try:
            cpp_content = self._generate_cpp(measured)
            with open(output_path, "w", encoding="utf-8") as f:
                f.write(cpp_content)
            
            messagebox.showinfo("Info", f"Exported {len(measured)} entries to {output_path}")
            self.progress_var.set(f"Exported {len(measured)} entries")
        except Exception as e:
            messagebox.showerror("Error", f"Export failed: {e}")

    def _generate_cpp(self, swatches: List[SwatchData]) -> str:
        """Generate C++ source file content using shared render_cpp logic."""
        entries = [
            KonaEntry(kona_id=s.id, l=s.L, a=s.a, b=s.b, name=s.name)
            for s in swatches
        ]
        return render_cpp(entries, self.data_path, source_script="kona_scanner_gui.py")

    def _on_clear_scanned(self):
        """Clear all scanned values."""
        result = messagebox.askyesno("Confirm", "Clear all scanned Lab values?")
        if not result:
            return
        
        for swatch in self.swatches.values():
            swatch.L = None
            swatch.a = None
            swatch.b = None
            swatch.R = None
            swatch.G = None
            swatch.B = None
            swatch.measured = False
        
        self._populate_treeview()
        self._clear_color_info()
        # Mark as unsaved since clearing Lab values is a change from the loaded state
        self._unsaved_changes = True
        self.progress_var.set("Cleared all scanned values")

    def _on_measure(self):
        """Take a measurement and compare to the selected swatch.
        
        Triggers a scan on the device and logs detailed comparison information
        between the scanned values and the selected swatch's reference values
        to help debug Kona color matching issues.
        """
        if not self.serial.is_connected():
            messagebox.showerror("Error", "Not connected to device.")
            return
        
        # Get selected swatch
        selection = self.tree.selection()
        if not selection:
            messagebox.showwarning("Warning", "Please select a swatch first.")
            return
        
        swatch_id = int(selection[0])
        swatch = self.swatches.get(swatch_id)
        if not swatch:
            messagebox.showerror("Error", "Selected swatch not found.")
            return
        
        # Check if swatch has reference values
        if swatch.L is None or swatch.a is None or swatch.b is None:
            messagebox.showwarning(
                "Warning", 
                f"Swatch '{swatch.name}' has no reference Lab values.\n"
                "Scan the swatch first to capture reference values.")
            return
        
        self.progress_var.set(f"Measuring {swatch.name}...")
        self.root.update_idletasks()
        
        # Perform the scan
        result = self.serial.scan()
        
        if not result:
            messagebox.showerror("Error", f"Measurement failed for {swatch.name}")
            self.progress_var.set("Measurement failed")
            return
        
        scan_L, scan_a, scan_b, scan_R, scan_G, scan_B, *_ = result
        
        # Calculate delta E using CIEDE2000
        scan_lab = (scan_L, scan_a, scan_b)
        ref_lab = (swatch.L, swatch.a, swatch.b)
        delta_e = ciede2000(scan_lab, ref_lab)
        
        # Log detailed comparison to console output area
        self._log_console("=" * 70)
        self._log_console(f"MEASUREMENT COMPARISON: {swatch.name} (ID: {swatch.id})")
        self._log_console("=" * 70)
        self._log_console(f"Reference (stored):  L*={swatch.L:8.3f}  a*={swatch.a:8.3f}  b*={swatch.b:8.3f}")
        self._log_console(f"Scanned (measured):  L*={scan_L:8.3f}  a*={scan_a:8.3f}  b*={scan_b:8.3f}")
        self._log_console("-" * 70)
        self._log_console(f"Difference:          ΔL*={scan_L - swatch.L:+8.3f}  Δa*={scan_a - swatch.a:+8.3f}  Δb*={scan_b - swatch.b:+8.3f}")
        self._log_console(f"CIEDE2000 ΔE:        {delta_e:.4f}")
        self._log_console("-" * 70)
        
        # Interpret the result - separate quality level and description
        if delta_e < 1.0:
            quality_level = "Excellent"
            quality_desc = "imperceptible difference"
        elif delta_e < 2.0:
            quality_level = "Good"
            quality_desc = "within Kona threshold"
        elif delta_e < 3.0:
            quality_level = "Fair"
            quality_desc = "perceptible difference"
        elif delta_e < 5.0:
            quality_level = "Poor"
            quality_desc = "noticeable difference"
        else:
            quality_level = "No match"
            quality_desc = "clearly different"
        
        self._log_console(f"Match Quality:       {quality_level} ({quality_desc})")
        self._log_console(f"Scanned RGB:         ({scan_R}, {scan_G}, {scan_B})")
        self._log_console("=" * 70)
        
        # Update the "Last Captured" display
        r, g, b = lab_to_rgb(scan_L, scan_a, scan_b)
        self._last_captured_color = rgb_to_hex(r, g, b)
        self.color_canvas.config(bg=self._last_captured_color)
        
        self.lc_vars["Name"].set(swatch.name)
        self.lc_vars["L*"].set(f"{scan_L:.2f}")
        self.lc_vars["a*"].set(f"{scan_a:.2f}")
        self.lc_vars["b*"].set(f"{scan_b:.2f}")
        self.lc_vars["RGB"].set(f"({r}, {g}, {b})")
        self.lc_vars["ΔE00"].set(f"{delta_e:.2f} ({quality_level})")
        
        # Update status bar
        self.progress_var.set(f"{swatch.name}: ΔE={delta_e:.2f} - {quality_level}")
        
        self.root.update_idletasks()
        self.root.bell()

    def _on_window_close(self):
        """Handle window close event with unsaved changes check."""
        if self._unsaved_changes:
            result = messagebox.askyesnocancel(
                "Unsaved Changes",
                "You have unsaved scan changes.\n\nSave before closing?")
            if result is None:  # Cancel - don't close
                return
            if result:  # Yes - save first
                if not self._save_data():
                    return  # Save failed, don't close
            # If No (result is False), proceed to close without saving
        
        # Clean up serial connection (ignore errors to ensure window closes)
        try:
            self.serial.disconnect()
        except Exception:
            pass
        self.root.destroy()


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0",
                       help="Serial port for device communication")
    parser.add_argument("--data", default="kona_captures.json",
                       help="kona_captures.json data file path")
    parser.add_argument("--synthetic", default=None,
                       help="kona_synthetic_tints.json file path (auto-detected if omitted)")
    parser.add_argument("--debug", action="store_true",
                       help="Enable extra verbose debug logging in the console")
    return parser.parse_args()


def main() -> int:
    """Main entry point."""
    args = parse_args()

    # Make data path relative to repository root (one level up from scripts directory)
    script_dir = pathlib.Path(__file__).parent  # scripts directory
    repo_root = script_dir.parent               # repository root

    data_path = args.data
    if not os.path.isabs(data_path):
        data_path = repo_root / data_path

    # Resolve synthetic tints path: explicit arg > auto-detect beside data file > repo root
    synthetic_path = None
    if args.synthetic is not None:
        sp = pathlib.Path(args.synthetic)
        synthetic_path = sp if sp.is_absolute() else repo_root / sp
    else:
        # Auto-detect: look beside the data file, then at repo root
        for candidate in [pathlib.Path(data_path).parent / "kona_synthetic_tints.json",
                          repo_root / "kona_synthetic_tints.json"]:
            if candidate.exists():
                synthetic_path = candidate
                break

    root = tk.Tk()
    app = KonaScannerApp(root, str(data_path), args.port,
                         synthetic_path=str(synthetic_path) if synthetic_path else None,
                         debug=args.debug)
    root.mainloop()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
