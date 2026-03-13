#!/usr/bin/env python3
"""Kona Swatch Scanner GUI Application.

A Tkinter-based GUI for managing Kona Cotton swatch scanning sessions.
Communicates with the shade_sayer device via serial port to capture
Lab color values for each swatch.

Features:
- Display all 365 Kona swatches in a sortable list
- Show color sample and Lab/RGB info for selected swatches
- Support single or multi-swatch scanning via EXTENDED selection
- Maintain scanned values in CSV file
- Export to C++ header file for firmware use

Usage:
    python3 kona_scanner_gui.py [--port /dev/ttyACM0] [--csv kona_swatches.csv] [--debug]
"""

import argparse
import csv
import colorsys
import dataclasses
import datetime as dt
import math
import os
import pathlib
import platform
import struct
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from typing import Dict, List, Optional, Tuple
import zlib

try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False
    print("Warning: pyserial not installed. Serial communication disabled.", file=sys.stderr)

# Schema version must match firmware konaref.h
SCHEMA_VERSION = 1
MAX_ENTRIES = 365

# Expected size of kona_ref_t struct in bytes (must match firmware)
# Layout: uint16_t kona_id (2) + padding (2) + 3 floats (12) = 16 bytes
KONA_REF_T_SIZE = 16

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

    def __init__(self, port: str = "/dev/ttyACM0", baudrate: int = 115200, debug: bool = False):
        self.port = port
        self.baudrate = baudrate
        self.debug = debug
        self.serial: Optional[serial.Serial] = None
        self._lock = threading.Lock()

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
            print(f"Serial connection error: {e}", file=sys.stderr)
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
        When debug mode is enabled, prints all TX/RX communication.
        """
        if not self.is_connected():
            return None

        with self._lock:
            try:
                # Clear input buffer before sending
                self.serial.reset_input_buffer()
                
                # Send command with newline
                if self.debug:
                    print(f"TX: {cmd}")
                self.serial.write((cmd + "\n").encode("utf-8"))
                self.serial.flush()

                # Wait for response
                start_time = time.time()
                while time.time() - start_time < timeout:
                    if self.serial.in_waiting > 0:
                        line = self.serial.readline().decode("utf-8", errors="replace").strip()
                        if self.debug:
                            print(f"RX: {line}")
                        if line.startswith("OK:") or line.startswith("ERR:"):
                            return line
                        # Continue reading - device may send log lines before response
                    time.sleep(0.05)

                if self.debug:
                    print(f"RX: (timeout after {timeout}s)")
                return None  # Timeout
            except serial.SerialException as e:
                print(f"Serial error: {e}", file=sys.stderr)
                return None

    def ping(self) -> bool:
        """Test connection with PING command."""
        response = self.send_command("PING", timeout=2.0)
        return response == "OK:PONG"

    def scan(self) -> Optional[Tuple[float, float, float, int, int, int]]:
        """Request a scan and return Lab and RGB values.
        
        Returns tuple (L, a, b, R, G, B) or None on error.
        """
        response = self.send_command("SCAN", timeout=10.0)
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
                if self.debug:
                    print(f"Parsed scan result: L={L:.4f} a={a:.4f} b={b:.4f} RGB=({R},{G},{B})")
                return (L, a, b, R, G, B)
            except (IndexError, ValueError) as e:
                print(f"Parse error: {e}, response: {response}", file=sys.stderr)
                return None

        print(f"Scan error: {response}", file=sys.stderr)
        return None

    def exit_mode(self) -> bool:
        """Send EXIT command to device."""
        response = self.send_command("EXIT", timeout=2.0)
        return response is not None and response.startswith("OK:")

    def kona_status(self) -> Optional[dict]:
        """Query Kona table status from device.
        
        Returns dict with keys: temp (bool), entries (int), builtin (bool)
        or None on error.
        """
        response = self.send_command("KONA_STATUS", timeout=2.0)
        if response and response.startswith("OK:KONA_STATUS:"):
            try:
                # Parse: OK:KONA_STATUS:temp=0,entries=5,builtin=1
                parts = response.split(":")[2].split(",")
                status = {}
                for part in parts:
                    key, value = part.split("=")
                    if key == "entries":
                        status[key] = int(value)
                    else:
                        status[key] = value == "1"
                return status
            except (IndexError, ValueError) as e:
                print(f"Parse error for KONA_STATUS: {e}", file=sys.stderr)
        return None

    def kona_clear(self) -> bool:
        """Clear temporary Kona table on device."""
        response = self.send_command("KONA_CLEAR", timeout=2.0)
        return response is not None and response.startswith("OK:KONA_CLEARED")

    def kona_load(self, table_data: bytes) -> Tuple[bool, str]:
        """Upload a temporary Kona table to the device.
        
        Args:
            table_data: Binary kona_table_t struct data
            
        Returns:
            Tuple of (success, message)
        """
        if not self.is_connected():
            return (False, "Not connected")

        with self._lock:
            try:
                # Clear input buffer
                self.serial.reset_input_buffer()
                
                # Send load command with size
                cmd = f"KONA_LOAD:{len(table_data)}\n"
                if self.debug:
                    print(f"TX: KONA_LOAD:{len(table_data)}")
                self.serial.write(cmd.encode("utf-8"))
                self.serial.flush()
                
                # Wait for READY response
                start_time = time.time()
                ready_received = False
                while time.time() - start_time < 5.0:
                    if self.serial.in_waiting > 0:
                        line = self.serial.readline().decode("utf-8", errors="replace").strip()
                        if self.debug:
                            print(f"RX: {line}")
                        if line.startswith("OK:KONA_LOAD:READY:"):
                            ready_received = True
                            break
                        if line.startswith("ERR:"):
                            return (False, line)
                    time.sleep(0.05)
                
                if not ready_received:
                    return (False, "Timeout waiting for READY")
                
                # Send binary data
                if self.debug:
                    print(f"TX: <binary data {len(table_data)} bytes>")
                self.serial.write(table_data)
                self.serial.flush()
                
                # Wait for result
                start_time = time.time()
                while time.time() - start_time < 10.0:
                    if self.serial.in_waiting > 0:
                        line = self.serial.readline().decode("utf-8", errors="replace").strip()
                        if self.debug:
                            print(f"RX: {line}")
                        if line.startswith("OK:KONA_LOADED:"):
                            # Parse entry count
                            try:
                                entries = int(line.split("=")[1])
                                return (True, f"Loaded {entries} entries")
                            except (IndexError, ValueError):
                                return (True, "Loaded successfully")
                        if line.startswith("ERR:"):
                            return (False, line)
                    time.sleep(0.05)
                
                return (False, "Timeout waiting for result")
                
            except serial.SerialException as e:
                return (False, f"Serial error: {e}")


class KonaScannerApp:
    """Main application class for Kona Swatch Scanner GUI."""

    def __init__(self, root: tk.Tk, csv_path: str, serial_port: str, debug: bool = False):
        self.root = root
        self.csv_path = pathlib.Path(csv_path)
        self.serial_port = serial_port
        self.debug = debug
        self.serial = SerialConnection(serial_port, debug=debug)
        self.swatches: Dict[int, SwatchData] = {}
        self.scan_queue: List[int] = []
        self.scanning = False
        self.scan_thread: Optional[threading.Thread] = None
        self._capture_in_progress = False  # Re-entrancy guard for capture operations
        self._unsaved_changes = False  # Track whether there are unsaved scan changes
        self._last_captured_color: Optional[str] = None  # Hex color of last captured swatch

        self._setup_ui()
        self._load_csv()
        self._populate_treeview()

    def _setup_ui(self):
        """Set up the main UI components."""
        self.root.title("Kona Swatch Scanner")
        self.root.geometry("1200x800")
        # Maximize window on startup (cross-platform with fallback)
        if platform.system() == 'Windows':
            self.root.state('zoomed')
        elif platform.system() == 'Darwin':  # macOS
            self.root.attributes('-fullscreen', True)  # or use geometry
        else:  # Linux
            try:
                self.root.attributes('-zoomed', True)
            except:
                # Fallback: set geometry to screen size
                self.root.geometry(f"{self.root.winfo_screenwidth()}x{self.root.winfo_screenheight()}+0+0")

        # Main frame with paned window
        main_pane = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main_pane.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Left frame: swatch list
        left_frame = ttk.Frame(main_pane)
        main_pane.add(left_frame, weight=3)

        # Toolbar
        toolbar = ttk.Frame(left_frame)
        toolbar.pack(fill=tk.X, pady=(0, 5))

        # Buttons with keyboard shortcuts (shown in parentheses)
        # Button enablement reflects connection state: Connect enabled when disconnected,
        # Disconnect enabled when connected.
        self.connect_btn = ttk.Button(toolbar, text="Connect (Alt+C)", command=self._on_connect)
        self.connect_btn.pack(side=tk.LEFT, padx=2)
        self.disconnect_btn = ttk.Button(toolbar, text="Disconnect (Alt+D)", command=self._on_disconnect,
                                          state=tk.DISABLED)
        self.disconnect_btn.pack(side=tk.LEFT, padx=2)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)

        self.scan_selected_btn = ttk.Button(toolbar, text="Scan (Alt+S)", command=self._on_scan_selected)
        self.scan_selected_btn.pack(side=tk.LEFT, padx=2)
        self.stop_scan_btn = ttk.Button(toolbar, text="Stop (Esc)", command=self._on_stop_scan)
        self.stop_scan_btn.pack(side=tk.LEFT, padx=2)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)

        self.save_csv_btn = ttk.Button(toolbar, text="Save (Ctrl+S)", command=self._on_save_csv)
        self.save_csv_btn.pack(side=tk.LEFT, padx=2)
        self.export_cpp_btn = ttk.Button(toolbar, text="Export (Alt+E)", command=self._on_export_cpp)
        self.export_cpp_btn.pack(side=tk.LEFT, padx=2)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)

        self.clear_scanned_btn = ttk.Button(toolbar, text="Clear (Alt+L)", command=self._on_clear_scanned)
        self.clear_scanned_btn.pack(side=tk.LEFT, padx=2)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)

        # Kona table management buttons
        self.upload_kona_btn = ttk.Button(toolbar, text="Upload (Alt+U)", command=self._on_upload_kona)
        self.upload_kona_btn.pack(side=tk.LEFT, padx=2)
        self.measure_btn = ttk.Button(toolbar, text="Measure (Alt+M)", command=self._on_measure)
        self.measure_btn.pack(side=tk.LEFT, padx=2)
        
        # Filter controls
        filter_frame = ttk.Frame(left_frame)
        filter_frame.pack(fill=tk.X, pady=(0, 5))
        
        ttk.Label(filter_frame, text="Filter (Alt+F):").pack(side=tk.LEFT)
        self.filter_var = tk.StringVar()
        self.filter_var.trace_add("write", self._on_filter_change)
        self.filter_entry = ttk.Entry(filter_frame, textvariable=self.filter_var, width=20)
        self.filter_entry.pack(side=tk.LEFT, padx=5)
        
        ttk.Label(filter_frame, text="Show:").pack(side=tk.LEFT, padx=(10, 0))
        self.show_var = tk.StringVar(value="all")
        ttk.Radiobutton(filter_frame, text="All (Alt+1)", variable=self.show_var, value="all",
                       command=self._on_filter_change).pack(side=tk.LEFT)
        ttk.Radiobutton(filter_frame, text="Scanned (Alt+2)", variable=self.show_var, value="scanned",
                       command=self._on_filter_change).pack(side=tk.LEFT)
        ttk.Radiobutton(filter_frame, text="Not Scanned (Alt+3)", variable=self.show_var, value="not_scanned",
                       command=self._on_filter_change).pack(side=tk.LEFT)

        # Treeview with scrollbars
        tree_label_frame = ttk.Frame(left_frame)
        tree_label_frame.pack(fill=tk.X)
        ttk.Label(tree_label_frame, text="Swatch List (Alt+T) - Ctrl+Space, Shift+Arrows, Home/End").pack(side=tk.LEFT, pady=(2, 0))
        
        tree_frame = ttk.Frame(left_frame)
        tree_frame.pack(fill=tk.BOTH, expand=True)

        columns = ("panel", "index", "id", "name", "measured", "L", "a", "b")
        self.tree = ttk.Treeview(tree_frame, columns=columns, show="headings", selectmode="extended")

        self.tree.heading("panel", text="Panel", command=lambda: self._sort_column("panel"))
        self.tree.heading("index", text="Index", command=lambda: self._sort_column("index"))
        self.tree.heading("id", text="ID", command=lambda: self._sort_column("id"))
        self.tree.heading("name", text="Name", command=lambda: self._sort_column("name"))
        self.tree.heading("measured", text="Scanned", command=lambda: self._sort_column("measured"))
        self.tree.heading("L", text="L*", command=lambda: self._sort_column("L"))
        self.tree.heading("a", text="a*", command=lambda: self._sort_column("a"))
        self.tree.heading("b", text="b*", command=lambda: self._sort_column("b"))

        self.tree.column("panel", width=140)
        self.tree.column("index", width=50, anchor=tk.CENTER)
        self.tree.column("id", width=50, anchor=tk.CENTER)
        self.tree.column("name", width=120)
        self.tree.column("measured", width=60, anchor=tk.CENTER)
        self.tree.column("L", width=60, anchor=tk.CENTER)
        self.tree.column("a", width=60, anchor=tk.CENTER)
        self.tree.column("b", width=60, anchor=tk.CENTER)

        vsb = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self.tree.yview)
        hsb = ttk.Scrollbar(tree_frame, orient=tk.HORIZONTAL, command=self.tree.xview)
        self.tree.configure(yscrollcommand=vsb.set, xscrollcommand=hsb.set)

        self.tree.grid(row=0, column=0, sticky="nsew")
        vsb.grid(row=0, column=1, sticky="ns")
        hsb.grid(row=1, column=0, sticky="ew")
        tree_frame.grid_rowconfigure(0, weight=1)
        tree_frame.grid_columnconfigure(0, weight=1)

        self.tree.bind("<<TreeviewSelect>>", self._on_selection_change)

        # Status bar
        self.progress_var = tk.StringVar(value="Ready")
        progress_bar = ttk.Label(left_frame, textvariable=self.progress_var, relief=tk.SUNKEN)
        progress_bar.pack(fill=tk.X, pady=(5, 0))

        # Right frame: color info panel
        right_frame = ttk.Frame(main_pane)
        main_pane.add(right_frame, weight=1)

        # Color sample canvas
        self.color_canvas = tk.Canvas(right_frame, width=200, height=200, bg="#808080",
                                       highlightthickness=1, highlightbackground="black")
        self.color_canvas.pack(pady=10)

        # Color info with LAB on left, RGB on right (read-only text boxes for copying)
        # Note: This shows the SELECTED item's info (which may differ from last captured)
        info_frame = ttk.LabelFrame(right_frame, text="Selected Item Info")
        info_frame.pack(fill=tk.X, padx=5, pady=5)

        self.info_entries = {}

        # Create rows with LAB on left, RGB on right
        lab_rgb_pairs = [("L*", "R"), ("a*", "G"), ("b*", "B")]
        for lab_label, rgb_label in lab_rgb_pairs:
            row = ttk.Frame(info_frame)
            row.pack(fill=tk.X, padx=5, pady=2)

            # LAB value on the left
            ttk.Label(row, text=f"{lab_label}:", width=4).pack(side=tk.LEFT)
            lab_var = tk.StringVar(value="-")
            lab_entry = ttk.Entry(row, textvariable=lab_var, width=10, state="readonly",
                                  font=("TkDefaultFont", 10, "bold"))
            lab_entry.pack(side=tk.LEFT, padx=(0, 10))
            self.info_entries[lab_label] = lab_var

            # RGB value on the right
            ttk.Label(row, text=f"{rgb_label}:", width=4).pack(side=tk.LEFT)
            rgb_var = tk.StringVar(value="-")
            rgb_entry = ttk.Entry(row, textvariable=rgb_var, width=6, state="readonly",
                                  font=("TkDefaultFont", 10, "bold"))
            rgb_entry.pack(side=tk.LEFT)
            self.info_entries[rgb_label] = rgb_var

        # Hex row at bottom
        hex_row = ttk.Frame(info_frame)
        hex_row.pack(fill=tk.X, padx=5, pady=2)
        ttk.Label(hex_row, text="Hex:", width=4).pack(side=tk.LEFT)
        hex_var = tk.StringVar(value="-")
        hex_entry = ttk.Entry(hex_row, textvariable=hex_var, width=10, state="readonly",
                              font=("TkDefaultFont", 10, "bold"))
        hex_entry.pack(side=tk.LEFT)
        self.info_entries["Hex"] = hex_var

        # Last captured display - shows the most recently scanned swatch info
        last_captured_frame = ttk.LabelFrame(right_frame, text="Last Captured")
        last_captured_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.last_captured_label = ttk.Label(last_captured_frame, text="No captures yet", 
                                             wraplength=250, justify=tk.LEFT)
        self.last_captured_label.pack(padx=5, pady=5)

        # Scan guidance frame
        scan_frame = ttk.LabelFrame(right_frame, text="Scanning")
        scan_frame.pack(fill=tk.X, padx=5, pady=5)

        self.scan_status_label = ttk.Label(scan_frame, text="Not scanning", wraplength=250)
        self.scan_status_label.pack(padx=5, pady=5)

        self.scan_button = ttk.Button(scan_frame, text="Capture (Space)", command=self._on_capture_current,
                                       state=tk.DISABLED)
        self.scan_button.pack(pady=5)

        self.skip_button = ttk.Button(scan_frame, text="Skip (Alt+K)", command=self._on_skip_current,
                                       state=tk.DISABLED)
        self.skip_button.pack(pady=5)
        
        # Kona table status frame
        kona_frame = ttk.LabelFrame(right_frame, text="Kona Table Status")
        kona_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.kona_status_label = ttk.Label(kona_frame, text="Not connected", justify=tk.LEFT)
        self.kona_status_label.pack(padx=5, pady=5)
        
        # Statistics frame
        stats_frame = ttk.LabelFrame(right_frame, text="Statistics")
        stats_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.stats_label = ttk.Label(stats_frame, text="", justify=tk.LEFT)
        self.stats_label.pack(padx=5, pady=5)
        self._update_stats()

        # Bind keyboard shortcuts
        self._setup_keyboard_shortcuts()

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
        self.root.bind("<Alt-u>", lambda e: self._on_upload_kona())
        self.root.bind("<Alt-m>", lambda e: self._on_measure())
        
        # Ctrl+S for Save CSV
        self.root.bind("<Control-s>", lambda e: self._on_save_csv())
        
        # Ctrl+A for Select All in treeview
        self.root.bind("<Control-a>", self._on_select_all)
        
        # Escape to stop scan
        self.root.bind("<Escape>", lambda e: self._on_stop_scan())
        
        # Window close handler to warn about unsaved changes
        self.root.protocol("WM_DELETE_WINDOW", self._on_window_close)
        
        # Alt+F to focus filter entry
        self.root.bind("<Alt-f>", self._on_focus_filter)
        
        # Alt+1/2/3 for filter radio buttons
        self.root.bind("<Alt-Key-1>", lambda e: self._set_show_filter("all"))
        self.root.bind("<Alt-Key-2>", lambda e: self._set_show_filter("scanned"))
        self.root.bind("<Alt-Key-3>", lambda e: self._set_show_filter("not_scanned"))
        
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

    def _load_csv(self):
        """Load swatch data from CSV file."""
        self.swatches.clear()

        if not self.csv_path.exists():
            print(f"CSV file not found: {self.csv_path}", file=sys.stderr)
            return

        try:
            with self.csv_path.open(newline="", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    try:
                        swatch_id = int(row.get("id", 0))
                        if swatch_id == 0:
                            continue

                        L = float(row["L"]) if row.get("L") else None
                        a = float(row["a"]) if row.get("a") else None
                        b = float(row["b"]) if row.get("b") else None
                        R = int(row["R"]) if row.get("R") else None
                        G = int(row["G"]) if row.get("G") else None
                        B = int(row["B"]) if row.get("B") else None
                        measured = row.get("measured", "").lower() == "true"

                        self.swatches[swatch_id] = SwatchData(
                            panel=row.get("panel", ""),
                            panel_index=int(row.get("panel_index", 0)),
                            id=swatch_id,
                            name=row.get("name", ""),
                            L=L,
                            a=a,
                            b=b,
                            R=R,
                            G=G,
                            B=B,
                            measured=measured,
                            notes=row.get("notes", "")
                        )
                    except (ValueError, KeyError) as e:
                        print(f"Skipping invalid row: {row} ({e})", file=sys.stderr)

            print(f"Loaded {len(self.swatches)} swatches from {self.csv_path}")
        except Exception as e:
            print(f"Error loading CSV: {e}", file=sys.stderr)

    def _save_csv(self):
        """Save swatch data to CSV file."""
        try:
            # Count measured swatches for verification
            measured_count = sum(1 for s in self.swatches.values() if s.measured)
            if self.debug:
                print(f"Saving CSV: {len(self.swatches)} total swatches, {measured_count} measured")
            
            with self.csv_path.open("w", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)
                writer.writerow(["panel", "panel_index", "id", "name", "L", "a", "b",
                                "R", "G", "B", "measured", "notes"])

                # Sort by panel, then panel_index
                sorted_swatches = sorted(self.swatches.values(),
                                        key=lambda s: (s.panel, s.panel_index))

                for s in sorted_swatches:
                    if self.debug and s.measured:
                        L_str = f"{s.L:.4f}" if s.L is not None else "None"
                        a_str = f"{s.a:.4f}" if s.a is not None else "None"
                        b_str = f"{s.b:.4f}" if s.b is not None else "None"
                        print(f"  Writing swatch[{s.id}] ({s.name}): "
                              f"L={L_str} a={a_str} b={b_str}")
                    writer.writerow([
                        s.panel,
                        s.panel_index,
                        s.id,
                        s.name,
                        f"{s.L:.4f}" if s.L is not None else "",
                        f"{s.a:.4f}" if s.a is not None else "",
                        f"{s.b:.4f}" if s.b is not None else "",
                        s.R if s.R is not None else "",
                        s.G if s.G is not None else "",
                        s.B if s.B is not None else "",
                        "true" if s.measured else "false",
                        s.notes
                    ])

            print(f"Saved {len(self.swatches)} swatches to {self.csv_path}")
            self._unsaved_changes = False  # Clear unsaved changes flag
            return True
        except Exception as e:
            print(f"Error saving CSV: {e}", file=sys.stderr)
            messagebox.showerror("Error", f"Failed to save CSV: {e}")
            return False

    def _populate_treeview(self):
        """Populate the treeview with swatch data."""
        # Clear existing items
        for item in self.tree.get_children():
            self.tree.delete(item)

        filter_text = self.filter_var.get().lower()
        show_mode = self.show_var.get()

        # Sort by panel, then panel_index
        sorted_swatches = sorted(self.swatches.values(),
                                key=lambda s: (s.panel, s.panel_index))

        for s in sorted_swatches:
            # Apply filters
            if filter_text:
                if (filter_text not in s.name.lower() and
                    filter_text not in s.panel.lower() and
                    filter_text not in str(s.id)):
                    continue

            if show_mode == "scanned" and not s.measured:
                continue
            if show_mode == "not_scanned" and s.measured:
                continue

            measured_str = "✓" if s.measured else ""
            L_str = f"{s.L:.1f}" if s.L is not None else "-"
            a_str = f"{s.a:.1f}" if s.a is not None else "-"
            b_str = f"{s.b:.1f}" if s.b is not None else "-"

            # Use swatch ID as item identifier
            self.tree.insert("", tk.END, iid=str(s.id),
                            values=(s.panel, s.panel_index, s.id, s.name,
                                   measured_str, L_str, a_str, b_str))
        
        self._update_stats()

    def _update_stats(self):
        """Update statistics display."""
        total = len(self.swatches)
        scanned = sum(1 for s in self.swatches.values() if s.measured)
        self.stats_label.config(text=f"Total: {total}\nScanned: {scanned}\nRemaining: {total - scanned}")

    def _update_kona_status(self):
        """Update Kona table status display from device."""
        if not self.serial.is_connected():
            self.kona_status_label.config(text="Not connected")
            return
        
        status = self.serial.kona_status()
        if status:
            if status.get("temp", False):
                text = f"Temporary table active\nEntries: {status.get('entries', 0)}"
            else:
                builtin = status.get("builtin", False)
                entries = status.get("entries", 0)
                if builtin:
                    text = f"Built-in table\nEntries: {entries}"
                else:
                    text = "No valid table loaded"
            self.kona_status_label.config(text=text)
        else:
            self.kona_status_label.config(text="Status query failed")

    def _sort_column(self, col: str):
        """Sort treeview by column."""
        items = [(self.tree.set(k, col), k) for k in self.tree.get_children("")]
        
        # Determine sort type
        try:
            items = [(float(v) if v and v != "-" and v != "✓" else 0, k) for v, k in items]
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

        # Show info for first selected item
        swatch_id = int(selection[0])
        swatch = self.swatches.get(swatch_id)
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
                self.color_canvas.config(bg="#808080")

    def _clear_color_info(self):
        """Clear color information display."""
        for var in self.info_entries.values():
            var.set("-")
        self.color_canvas.config(bg="#808080")

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
                # Update Kona table status
                self._update_kona_status()
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
        self.kona_status_label.config(text="Not connected")
        self.progress_var.set("Disconnected")

    def _on_scan_selected(self):
        """Start scanning selected swatches."""
        if not self.serial.is_connected():
            messagebox.showwarning("Warning", "Not connected to device")
            return

        selection = self.tree.selection()
        if not selection:
            messagebox.showwarning("Warning", "No swatches selected")
            return

        # Build scan queue from selection and store original selection
        self.scan_queue = [int(item_id) for item_id in selection]
        self._scan_original_selection = list(selection)  # Store original selection
        self.scanning = True
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
                self.scan_status_label.config(
                    text=f"Ready to scan:\n{swatch.name}\nPanel: {swatch.panel}, Index: {swatch.panel_index}\n\n"
                         f"{remaining} swatch(es) remaining"
                )
                self.scan_button.config(state=tk.NORMAL)
                self.skip_button.config(state=tk.NORMAL)
                
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
                self._advance_scan()
        else:
            self.scan_status_label.config(text="Not scanning")
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
        """Capture current swatch in queue."""
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

            self.scan_status_label.config(text=f"Scanning {swatch.name}...")
            self.scan_button.config(state=tk.DISABLED)
            self.skip_button.config(state=tk.DISABLED)
            self.root.update()

            # Perform scan
            result = self.serial.scan()
            if result:
                L, a, b, R, G, B = result
                
                # Store scan results in the swatch object (canonical data store)
                swatch.L = L
                swatch.a = a
                swatch.b = b
                swatch.R = R
                swatch.G = G
                swatch.B = B
                swatch.measured = True
                
                if self.debug:
                    print(f"Stored in swatch[{current_id}] ({swatch.name}): "
                          f"L={swatch.L:.4f} a={swatch.a:.4f} b={swatch.b:.4f} "
                          f"RGB=({swatch.R},{swatch.G},{swatch.B})")

                # Update treeview if the item exists (may be filtered out)
                tree_item_id = str(current_id)
                if self.tree.exists(tree_item_id):
                    measured_str = "✓"
                    self.tree.set(tree_item_id, "measured", measured_str)
                    self.tree.set(tree_item_id, "L", f"{L:.1f}")
                    self.tree.set(tree_item_id, "a", f"{a:.1f}")
                    self.tree.set(tree_item_id, "b", f"{b:.1f}")
                    if self.debug:
                        # Verify the treeview was actually updated
                        tree_values = self.tree.item(tree_item_id, 'values')
                        print(f"Treeview item {tree_item_id} values: {tree_values}")
                elif self.debug:
                    print(f"Warning: Treeview item {tree_item_id} does not exist (filtered?)")

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
                self.last_captured_label.config(
                    text=f"{swatch.name}\n"
                         f"L*={L:.2f}  a*={a:.2f}  b*={b:.2f}\n"
                         f"RGB=({r}, {g}, {b})")

                self.progress_var.set(f"Captured {swatch.name}: L={L:.1f} a={a:.1f} b={b:.1f}")
                
                # Force immediate UI refresh so the canvas and labels are visually updated
                # before advancing to the next item. Without this, the updates may not be
                # visible until the user moves the mouse or triggers another event.
                self.root.update_idletasks()
                
                # Mark that we have unsaved changes
                self._unsaved_changes = True
                
                # Play system bell to indicate successful scan
                self.root.bell()
            else:
                messagebox.showerror("Error", f"Failed to scan {swatch.name}")
                self.progress_var.set(f"Scan failed for {swatch.name}")

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
                    "Scan session complete.\n\nYou have unsaved changes. Save CSV now?")
                if save_now:
                    self._save_csv()
            else:
                messagebox.showinfo("Info", "Scan session complete")

        self._update_scan_ui()

    def _on_stop_scan(self):
        """Stop current scan session."""
        self.scan_queue.clear()
        self.scanning = False
        self._update_scan_ui()
        self.progress_var.set("Scan stopped")

    def _on_save_csv(self):
        """Save CSV file."""
        if self._save_csv():
            messagebox.showinfo("Info", f"Saved to {self.csv_path}")

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
        """Generate C++ source file content."""
        # Sort by ID
        sorted_swatches = sorted(swatches, key=lambda s: s.id)
        
        if len(sorted_swatches) > MAX_ENTRIES:
            raise ValueError(f"Too many entries ({len(sorted_swatches)}), max is {MAX_ENTRIES}")

        # Verify struct pack format matches expected kona_ref_t size
        entry_pack_format = "<H2x3f"  # uint16_t + 2 pad + 3 floats
        actual_size = struct.calcsize(entry_pack_format)
        assert actual_size == KONA_REF_T_SIZE, \
            f"Struct pack size mismatch: got {actual_size}, expected {KONA_REF_T_SIZE}"

        # Calculate CRC32 of entry data (matching firmware struct layout)
        payload = bytearray()
        for s in sorted_swatches:
            # Pack as: uint16_t + 2 padding bytes + 3 floats (little-endian)
            payload.extend(struct.pack(entry_pack_format, s.id, s.L, s.a, s.b))
        crc = zlib.crc32(payload) & 0xFFFFFFFF

        # Generate entry lines
        rows = []
        for s in sorted_swatches:
            rows.append(f"        {{ {s.id}, {s.L:.6f}f, {s.a:.6f}f, {s.b:.6f}f }}, // {s.id} {s.name}")
        
        if len(sorted_swatches) < MAX_ENTRIES:
            rows.append("        // Remaining entries are zero-initialized.")

        generated_at = dt.datetime.now(dt.timezone.utc).isoformat()
        
        return f'''// Auto-generated by kona_scanner_gui.py
// Source CSV: {self.csv_path}
// Generated at: {generated_at}
// Entry count: {len(sorted_swatches)}

#include "konaref.h"

const kona_table_t kona_reference = {{
    .version = KONA_REF_SCHEMA_VERSION,
    .entry_count = {len(sorted_swatches)},
    .crc32 = 0x{crc:08X}u,
    .entries = {{
{chr(10).join(rows)}
    }},
}};
'''

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
        # Mark as unsaved since clearing Lab values is a change from the loaded CSV state
        # User should save to persist the cleared state to the CSV file
        self._unsaved_changes = True
        self.progress_var.set("Cleared all scanned values")

    def _on_upload_kona(self):
        """Upload the currently loaded Kona reference table to the device."""
        if not self.serial.is_connected():
            messagebox.showerror("Error", "Not connected to device.\nConnect first, then upload.")
            return

        # Collect measured swatches from currently loaded table
        measured = [s for s in self.swatches.values() 
                   if s.measured and s.L is not None and s.a is not None and s.b is not None]
        
        if not measured:
            messagebox.showwarning("Warning", "No scanned swatches to upload.\nScan some swatches first.")
            return
        
        # Convert to (id, L, a, b) tuples sorted by ID
        entries = [(s.id, s.L, s.a, s.b) for s in sorted(measured, key=lambda s: s.id)]
        
        if len(entries) > MAX_ENTRIES:
            messagebox.showerror("Error", f"Too many entries ({len(entries)}), max is {MAX_ENTRIES}")
            return
        
        try:
            # Generate binary table data
            table_data = self._generate_kona_binary(entries)
            
            self.progress_var.set(f"Uploading {len(entries)} entries to device...")
            self.root.update()
            
            # Upload to device
            success, message = self.serial.kona_load(table_data)
            
            if success:
                messagebox.showinfo("Success", f"Kona table uploaded successfully.\n{message}")
                self.progress_var.set(f"Kona table uploaded: {message}")
                self._update_kona_status()
            else:
                messagebox.showerror("Error", f"Upload failed:\n{message}")
                self.progress_var.set(f"Upload failed: {message}")
                
        except Exception as e:
            messagebox.showerror("Error", f"Failed to upload table:\n{e}")
            self.progress_var.set(f"Upload error: {e}")

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
        
        scan_L, scan_a, scan_b, scan_R, scan_G, scan_B = result
        
        # Calculate delta E using CIEDE2000
        scan_lab = (scan_L, scan_a, scan_b)
        ref_lab = (swatch.L, swatch.a, swatch.b)
        delta_e = ciede2000(scan_lab, ref_lab)
        
        # Log detailed comparison (to console)
        print("\n" + "=" * 70)
        print(f"MEASUREMENT COMPARISON: {swatch.name} (ID: {swatch.id})")
        print("=" * 70)
        print(f"Reference (stored):  L*={swatch.L:8.3f}  a*={swatch.a:8.3f}  b*={swatch.b:8.3f}")
        print(f"Scanned (measured):  L*={scan_L:8.3f}  a*={scan_a:8.3f}  b*={scan_b:8.3f}")
        print("-" * 70)
        print(f"Difference:          ΔL*={scan_L - swatch.L:+8.3f}  Δa*={scan_a - swatch.a:+8.3f}  Δb*={scan_b - swatch.b:+8.3f}")
        print(f"CIEDE2000 ΔE:        {delta_e:.4f}")
        print("-" * 70)
        
        # Interpret the result - separate quality level and description
        if delta_e < 1.0:
            quality_level = "Excellent match"
            quality_desc = "imperceptible difference"
        elif delta_e < 2.0:
            quality_level = "Good match"
            quality_desc = "within Kona threshold"
        elif delta_e < 3.0:
            quality_level = "Fair match"
            quality_desc = "perceptible difference"
        elif delta_e < 5.0:
            quality_level = "Poor match"
            quality_desc = "noticeable difference"
        else:
            quality_level = "No match"
            quality_desc = "clearly different"
        
        print(f"Match Quality:       {quality_level} ({quality_desc})")
        print(f"Scanned RGB:         ({scan_R}, {scan_G}, {scan_B})")
        print("=" * 70 + "\n")
        
        # Update the "Last Captured" display
        r, g, b = lab_to_rgb(scan_L, scan_a, scan_b)
        self._last_captured_color = rgb_to_hex(r, g, b)
        self.color_canvas.config(bg=self._last_captured_color)
        
        self.last_captured_label.config(
            text=f"Measured: {swatch.name}\n"
                 f"L*={scan_L:.2f}  a*={scan_a:.2f}  b*={scan_b:.2f}\n"
                 f"ΔE00={delta_e:.2f} ({quality_level})")
        
        # Update status bar
        self.progress_var.set(f"{swatch.name}: ΔE={delta_e:.2f} - {quality_level}")
        
        self.root.update_idletasks()
        self.root.bell()

    def _generate_kona_binary(self, entries: List[Tuple[int, float, float, float]]) -> bytes:
        """Generate binary kona_table_t data from list of (id, L, a, b) tuples.
        
        Returns bytes that can be sent directly to the device.
        """
        # Pack entries into binary format matching firmware kona_ref_t struct
        def pack_entry(kona_id: int, L: float, a: float, b: float) -> bytes:
            # Pack as: uint16_t + 2 padding bytes + 3 floats (little-endian)
            # This must match sizeof(kona_ref_t) = 16 bytes
            return struct.pack("<H2x3f", kona_id, L, a, b)
        
        # Build entry payload - used for both CRC and final output
        entry_data = bytearray()
        for kona_id, L, a, b in entries:
            entry_data.extend(pack_entry(kona_id, L, a, b))
        
        # Calculate CRC32 over actual entries only (not padding)
        crc = zlib.crc32(entry_data) & 0xFFFFFFFF
        
        # Pad to full 365 entries (all zeros)
        remaining = MAX_ENTRIES - len(entries)
        if remaining > 0:
            entry_data.extend(b'\x00' * (remaining * KONA_REF_T_SIZE))
        
        # Build complete table: header + entries
        # Header: uint16_t version + uint16_t entry_count + uint32_t crc32
        header = struct.pack("<HHI", SCHEMA_VERSION, len(entries), crc)
        
        return header + bytes(entry_data)

    def _on_window_close(self):
        """Handle window close event with unsaved changes check."""
        if self._unsaved_changes:
            result = messagebox.askyesnocancel(
                "Unsaved Changes",
                "You have unsaved scan changes.\n\nSave before closing?")
            if result is None:  # Cancel - don't close
                return
            if result:  # Yes - save first
                if not self._save_csv():
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
    parser.add_argument("--csv", default="kona_365_sensor_ready.csv",
                       help="CSV file for swatch data")
    parser.add_argument("--debug", action="store_true",
                       help="Print all serial TX/RX communications for debugging")
    return parser.parse_args()


def main() -> int:
    """Main entry point."""
    args = parse_args()

    # Make CSV path relative to repository root (one level up from scripts directory)
    csv_path = args.csv
    if not os.path.isabs(csv_path):
        script_dir = pathlib.Path(__file__).parent  # scripts directory
        repo_root = script_dir.parent               # repository root
        csv_path = repo_root / csv_path

    if args.debug:
        print("Debug mode enabled - all serial TX/RX will be printed")

    root = tk.Tk()
    app = KonaScannerApp(root, str(csv_path), args.port, debug=args.debug)
    root.mainloop()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
