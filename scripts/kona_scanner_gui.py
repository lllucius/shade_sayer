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
import os
import pathlib
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


def lab_to_rgb(L: float, a: float, b: float) -> Tuple[int, int, int]:
    """Convert CIE Lab to sRGB (approximate for display).
    
    Uses D65 illuminant reference white.
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

    # Gamma correction
    def gamma(u: float) -> float:
        if u <= 0.0031308:
            return 12.92 * u
        return 1.055 * (u ** (1.0 / 2.4)) - 0.055

    r = int(max(0, min(255, round(gamma(r_lin) * 255))))
    g = int(max(0, min(255, round(gamma(g_lin) * 255))))
    b = int(max(0, min(255, round(gamma(b_lin) * 255))))

    return r, g, b


def rgb_to_hex(r: int, g: int, b: int) -> str:
    """Convert RGB to hex color string."""
    return f"#{r:02x}{g:02x}{b:02x}"


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

        self._setup_ui()
        self._load_csv()
        self._populate_treeview()

    def _setup_ui(self):
        """Set up the main UI components."""
        self.root.title("Kona Swatch Scanner")
        self.root.geometry("1200x800")

        # Main frame with paned window
        main_pane = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        main_pane.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Left frame: swatch list
        left_frame = ttk.Frame(main_pane)
        main_pane.add(left_frame, weight=3)

        # Toolbar
        toolbar = ttk.Frame(left_frame)
        toolbar.pack(fill=tk.X, pady=(0, 5))

        ttk.Button(toolbar, text="Connect", command=self._on_connect).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Disconnect", command=self._on_disconnect).pack(side=tk.LEFT, padx=2)
        
        self.status_label = ttk.Label(toolbar, text="Disconnected", foreground="red")
        self.status_label.pack(side=tk.LEFT, padx=10)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)

        ttk.Button(toolbar, text="Scan Selected", command=self._on_scan_selected).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Stop Scan", command=self._on_stop_scan).pack(side=tk.LEFT, padx=2)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)

        ttk.Button(toolbar, text="Save CSV", command=self._on_save_csv).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Export C++", command=self._on_export_cpp).pack(side=tk.LEFT, padx=2)

        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)

        ttk.Button(toolbar, text="Clear Scanned", command=self._on_clear_scanned).pack(side=tk.LEFT, padx=2)
        
        # Filter controls
        filter_frame = ttk.Frame(left_frame)
        filter_frame.pack(fill=tk.X, pady=(0, 5))
        
        ttk.Label(filter_frame, text="Filter:").pack(side=tk.LEFT)
        self.filter_var = tk.StringVar()
        self.filter_var.trace_add("write", self._on_filter_change)
        filter_entry = ttk.Entry(filter_frame, textvariable=self.filter_var, width=20)
        filter_entry.pack(side=tk.LEFT, padx=5)
        
        ttk.Label(filter_frame, text="Show:").pack(side=tk.LEFT, padx=(10, 0))
        self.show_var = tk.StringVar(value="all")
        ttk.Radiobutton(filter_frame, text="All", variable=self.show_var, value="all",
                       command=self._on_filter_change).pack(side=tk.LEFT)
        ttk.Radiobutton(filter_frame, text="Scanned", variable=self.show_var, value="scanned",
                       command=self._on_filter_change).pack(side=tk.LEFT)
        ttk.Radiobutton(filter_frame, text="Not Scanned", variable=self.show_var, value="not_scanned",
                       command=self._on_filter_change).pack(side=tk.LEFT)

        # Treeview with scrollbars
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
        info_frame = ttk.LabelFrame(right_frame, text="Color Information")
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

        # Scan guidance frame
        scan_frame = ttk.LabelFrame(right_frame, text="Scanning")
        scan_frame.pack(fill=tk.X, padx=5, pady=5)

        self.scan_status_label = ttk.Label(scan_frame, text="Not scanning", wraplength=250)
        self.scan_status_label.pack(padx=5, pady=5)

        self.scan_button = ttk.Button(scan_frame, text="Capture", command=self._on_capture_current,
                                       state=tk.DISABLED)
        self.scan_button.pack(pady=5)

        self.skip_button = ttk.Button(scan_frame, text="Skip", command=self._on_skip_current,
                                       state=tk.DISABLED)
        self.skip_button.pack(pady=5)
        
        # Statistics frame
        stats_frame = ttk.LabelFrame(right_frame, text="Statistics")
        stats_frame.pack(fill=tk.X, padx=5, pady=5)
        
        self.stats_label = ttk.Label(stats_frame, text="", justify=tk.LEFT)
        self.stats_label.pack(padx=5, pady=5)
        self._update_stats()

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
            with self.csv_path.open("w", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)
                writer.writerow(["panel", "panel_index", "id", "name", "L", "a", "b",
                                "R", "G", "B", "measured", "notes"])

                # Sort by panel, then panel_index
                sorted_swatches = sorted(self.swatches.values(),
                                        key=lambda s: (s.panel, s.panel_index))

                for s in sorted_swatches:
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
            self._show_color_info(swatch)

    def _show_color_info(self, swatch: SwatchData):
        """Display color information for a swatch."""
        if swatch.L is not None and swatch.a is not None and swatch.b is not None:
            self.info_entries["L*"].set(f"{swatch.L:.2f}")
            self.info_entries["a*"].set(f"{swatch.a:.2f}")
            self.info_entries["b*"].set(f"{swatch.b:.2f}")

            # Convert Lab to RGB for display
            if swatch.R is not None and swatch.G is not None and swatch.B is not None:
                r, g, b = swatch.R, swatch.G, swatch.B
            else:
                r, g, b = lab_to_rgb(swatch.L, swatch.a, swatch.b)

            self.info_entries["R"].set(str(r))
            self.info_entries["G"].set(str(g))
            self.info_entries["B"].set(str(b))

            hex_color = rgb_to_hex(r, g, b)
            self.info_entries["Hex"].set(hex_color)

            # Update color sample
            self.color_canvas.config(bg=hex_color)
        else:
            for key in ["L*", "a*", "b*", "R", "G", "B", "Hex"]:
                self.info_entries[key].set("-")
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
                self.status_label.config(text="Connected", foreground="green")
                self.progress_var.set("Connected to device")
            else:
                self.serial.disconnect()
                self.status_label.config(text="Device not responding", foreground="orange")
                self.progress_var.set("Device connected but not in serial mode")
                messagebox.showwarning("Warning", 
                    "Serial port opened but device not responding.\n\n"
                    "Make sure the device is in serial scan mode:\n"
                    "1. Connect device via USB\n"
                    "2. Press button 5 times quickly\n"
                    "3. Wait for 'Serial scan mode active' announcement")
        else:
            self.status_label.config(text="Connection failed", foreground="red")
            self.progress_var.set("Failed to connect")
            messagebox.showerror("Error", f"Failed to connect to {self.serial_port}")

    def _on_disconnect(self):
        """Handle disconnect button click."""
        if not self.serial.is_connected():
            return

        # Send exit command if connected
        self.serial.exit_mode()
        self.serial.disconnect()
        self.status_label.config(text="Disconnected", foreground="red")
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

        # Build scan queue from selection
        self.scan_queue = [int(item_id) for item_id in selection]
        self.scanning = True
        self._update_scan_ui()

    def _update_scan_ui(self):
        """Update scan UI state."""
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
                
                # Select and scroll to current item in tree
                self.tree.selection_set(str(current_id))
                self.tree.see(str(current_id))
            else:
                self._advance_scan()
        else:
            self.scan_status_label.config(text="Not scanning")
            self.scan_button.config(state=tk.DISABLED)
            self.skip_button.config(state=tk.DISABLED)
            self.scanning = False

    def _on_capture_current(self):
        """Capture current swatch in queue."""
        if not self.scanning or not self.scan_queue:
            return

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
            swatch.L = L
            swatch.a = a
            swatch.b = b
            swatch.R = R
            swatch.G = G
            swatch.B = B
            swatch.measured = True

            # Update treeview
            measured_str = "✓"
            self.tree.set(str(current_id), "measured", measured_str)
            self.tree.set(str(current_id), "L", f"{L:.1f}")
            self.tree.set(str(current_id), "a", f"{a:.1f}")
            self.tree.set(str(current_id), "b", f"{b:.1f}")

            # Update display
            self._show_color_info(swatch)
            self._update_stats()

            self.progress_var.set(f"Captured {swatch.name}: L={L:.1f} a={a:.1f} b={b:.1f}")
        else:
            messagebox.showerror("Error", f"Failed to scan {swatch.name}")
            self.progress_var.set(f"Scan failed for {swatch.name}")

        self._advance_scan()

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
            rows.append(f"        {{ {s.id}, {s.L:.6f}f, {s.a:.6f}f, {s.b:.6f}f }},")
        
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
        self.progress_var.set("Cleared all scanned values")


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
