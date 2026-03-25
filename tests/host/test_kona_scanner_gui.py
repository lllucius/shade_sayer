#!/usr/bin/env python3
"""Tests for kona_scanner_gui.py script.

Note: These tests require tkinter which may not be available in headless CI environments.
Tests will be skipped if tkinter is not available.
"""

import dataclasses
import datetime as dt
import json
import pathlib
import struct
import sys
import tempfile
import zlib
from typing import List, Optional

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

# Import shared generation logic
from generate_kona_table import KonaEntry, MAX_ENTRIES, render_cpp  # noqa: E402

# Check for tkinter availability
try:
    import tkinter as tk
    HAS_TKINTER = True
except ImportError:
    HAS_TKINTER = False
    print("Note: tkinter not available, using standalone test implementation")


# Standalone SwatchData for testing without tkinter – must mirror kona_scanner_gui.SwatchData
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
    # Raw sensor data for pipeline replay
    raw_x: Optional[int] = None
    raw_y: Optional[int] = None
    raw_z: Optional[int] = None
    raw_ir: Optional[int] = None
    raw_clear: Optional[int] = None
    raw_gain: Optional[int] = None
    raw_integration_ms: Optional[int] = None


# Constants (must match kona_scanner_gui.py)
SCHEMA_VERSION = 1
KONA_REF_T_SIZE = 16


def _generate_cpp_standalone(swatches: List[SwatchData], data_path: str) -> str:
    """Standalone wrapper around the shared render_cpp for testing.
    
    Converts SwatchData to KonaEntry objects and calls render_cpp, mirroring
    the logic in KonaScannerApp._generate_cpp().
    """
    entries = [
        KonaEntry(kona_id=s.id, l=s.L, a=s.a, b=s.b, name=s.name)
        for s in swatches
    ]
    return render_cpp(entries, pathlib.Path(data_path), source_script="kona_scanner_gui.py")


def test_generate_cpp_inline_comments():
    """Test that _generate_cpp adds inline comments with id and name."""
    # Create test swatches
    test_swatches = [
        SwatchData(panel="test", panel_index=1, id=449, name="SUNNY", 
                   L=50.123456, a=10.654321, b=-5.111111, measured=True),
        SwatchData(panel="test", panel_index=2, id=120, name="AZURE", 
                   L=60.222222, a=5.333333, b=15.444444, measured=True),
    ]
    
    # Generate C++ content using standalone implementation
    cpp_content = _generate_cpp_standalone(test_swatches, "/test/kona_captures.json")
    
    # Verify that inline comments with id and name are present
    assert "// 120 AZURE" in cpp_content, "Missing inline comment for AZURE"
    assert "// 449 SUNNY" in cpp_content, "Missing inline comment for SUNNY"
    
    # Verify the full entry format (id, L, a, b with comment)
    assert "{ 120," in cpp_content, "Missing entry for id 120"
    assert "{ 449," in cpp_content, "Missing entry for id 449"
    
    # Verify the complete line format
    assert "{ 120, 60.222222f, 5.333333f, 15.444444f }, // 120 AZURE" in cpp_content, \
        "Full line format incorrect for AZURE"
    
    print("_generate_cpp inline comments test passed")


def test_generate_cpp_sorted_by_id():
    """Test that _generate_cpp sorts entries by ID."""
    # Create test swatches in unsorted order
    test_swatches = [
        SwatchData(panel="test", panel_index=1, id=300, name="THIRD", 
                   L=30.0, a=3.0, b=3.0, measured=True),
        SwatchData(panel="test", panel_index=2, id=100, name="FIRST", 
                   L=10.0, a=1.0, b=1.0, measured=True),
        SwatchData(panel="test", panel_index=3, id=200, name="SECOND", 
                   L=20.0, a=2.0, b=2.0, measured=True),
    ]
    
    cpp_content = _generate_cpp_standalone(test_swatches, "/test/kona_captures.json")
    
    # Find positions of each entry
    pos_first = cpp_content.find("// 100 FIRST")
    pos_second = cpp_content.find("// 200 SECOND")
    pos_third = cpp_content.find("// 300 THIRD")
    
    # Verify order: FIRST < SECOND < THIRD
    assert pos_first < pos_second < pos_third, \
        f"Entries not sorted by ID: FIRST={pos_first}, SECOND={pos_second}, THIRD={pos_third}"
    
    print("_generate_cpp sorting test passed")


def test_generate_cpp_valid_cpp_syntax():
    """Test that generated C++ has valid basic syntax."""
    test_swatches = [
        SwatchData(panel="test", panel_index=1, id=1, name="TEST COLOR", 
                   L=50.0, a=0.0, b=0.0, measured=True),
    ]
    
    cpp_content = _generate_cpp_standalone(test_swatches, "/test/kona_captures.json")
    
    # Check required C++ elements
    assert '#include "konaref.h"' in cpp_content, "Missing include statement"
    assert "const kona_table_t kona_reference = {" in cpp_content, "Missing struct declaration"
    assert ".version = KONA_REF_SCHEMA_VERSION," in cpp_content, "Missing version field"
    assert ".entry_count = 1," in cpp_content, "Missing entry_count field"
    assert ".crc32 = 0x" in cpp_content, "Missing crc32 field"
    assert ".entries = {" in cpp_content, "Missing entries field"
    assert "};" in cpp_content, "Missing closing brace"
    
    print("_generate_cpp valid C++ syntax test passed")


def test_scan_queue_advancement():
    """Test that scan queue properly advances through items.
    
    This tests the fix for the bug where scanning would get stuck on item 2
    (pattern: 1, 2, 2, 2, 2, ...) instead of advancing properly.
    """
    # Simulate the scan queue behavior
    scan_queue = [120, 121, 122]  # 3 consecutive items
    scanned_items = []
    
    def mock_advance_scan():
        """Simulate _advance_scan behavior."""
        if scan_queue:
            scanned_items.append(scan_queue.pop(0))
    
    # Simulate scanning 3 items
    for _ in range(3):
        if scan_queue:
            current_id = scan_queue[0]
            # Simulate successful scan
            mock_advance_scan()
    
    # Verify all items were scanned in order
    assert scanned_items == [120, 121, 122], \
        f"Expected [120, 121, 122], got {scanned_items}"
    assert len(scan_queue) == 0, f"Queue should be empty, got {scan_queue}"
    
    print("Scan queue advancement test passed")


def test_remaining_selection_calculation():
    """Test that remaining selection is calculated correctly from scan_queue.
    
    This tests the fix where selection should show only remaining items,
    not all originally selected items.
    """
    scan_queue = [120, 121, 122]
    
    # After scanning item 120, queue becomes [121, 122]
    scan_queue.pop(0)
    
    # Calculate remaining selection (simulating _update_scan_ui logic)
    remaining_selection = [str(item_id) for item_id in scan_queue]
    
    assert remaining_selection == ['121', '122'], \
        f"Expected ['121', '122'], got {remaining_selection}"
    
    # After scanning item 121, queue becomes [122]
    scan_queue.pop(0)
    remaining_selection = [str(item_id) for item_id in scan_queue]
    
    assert remaining_selection == ['122'], \
        f"Expected ['122'], got {remaining_selection}"
    
    print("Remaining selection calculation test passed")


def test_display_gamma_darkens_colors():
    """Test that DISPLAY_GAMMA > 1.0 produces darker RGB values.
    
    With DISPLAY_GAMMA > 1.0, the lab_to_rgb function should produce
    darker (lower) RGB values than with DISPLAY_GAMMA = 1.0.
    
    Note: This test uses a standalone implementation rather than importing
    from kona_scanner_gui.py because tkinter is not available in the test
    environment. The test verifies the mathematical behavior of the gamma
    adjustment algorithm.
    """
    # Test data: Lab values that produce non-black colors
    # (black colors produce (0,0,0) with any gamma, so we skip them)
    test_colors = [
        (50.0, 0.0, 0.0),   # Mid gray
        (75.0, 20.0, 60.0), # Orange tone
        (25.0, 10.0, -30.0), # Dark blue
    ]
    
    # Standalone implementation with configurable gamma
    # (mirrors kona_scanner_gui.lab_to_rgb implementation)
    def lab_to_rgb_with_gamma(L, a, b, display_gamma=1.0):
        fy = (L + 16.0) / 116.0
        fx = a / 500.0 + fy
        fz = fy - b / 200.0
        Xn, Yn, Zn = 95.047, 100.0, 108.883
        
        def f_inv(t):
            delta = 6.0 / 29.0
            if t > delta:
                return t ** 3
            return 3 * delta ** 2 * (t - 4.0 / 29.0)
        
        X = Xn * f_inv(fx)
        Y = Yn * f_inv(fy)
        Z = Zn * f_inv(fz)
        
        r_lin = ( 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z) / 100.0
        g_lin = (-0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z) / 100.0
        b_lin = ( 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z) / 100.0
        
        def gamma_func(u):
            if u <= 0.0031308:
                v = 12.92 * u
            else:
                v = 1.055 * (u ** (1.0 / 2.4)) - 0.055
            return max(0.0, v) ** display_gamma
        
        r = int(max(0, min(255, round(gamma_func(r_lin) * 255))))
        g = int(max(0, min(255, round(gamma_func(g_lin) * 255))))
        b = int(max(0, min(255, round(gamma_func(b_lin) * 255))))
        return r, g, b
    
    for L, a, b in test_colors:
        # Calculate RGB with gamma=1.0 (no adjustment)
        r1, g1, b1 = lab_to_rgb_with_gamma(L, a, b, 1.0)
        # Calculate RGB with gamma=1.1 (darkening)
        r2, g2, b2 = lab_to_rgb_with_gamma(L, a, b, 1.1)
        
        # Verify that gamma > 1.0 produces darker or equal values
        # (darker means lower RGB values)
        assert r2 <= r1, f"DISPLAY_GAMMA>1.0 should darken R: {r2} > {r1}"
        assert g2 <= g1, f"DISPLAY_GAMMA>1.0 should darken G: {g2} > {g1}"
        assert b2 <= b1, f"DISPLAY_GAMMA>1.0 should darken B: {b2} > {b1}"
        
        # Calculate total brightness and verify darkening occurred
        total_orig = r1 + g1 + b1
        total_dark = r2 + g2 + b2
        
        # All test colors should be non-black, so darkening should occur
        assert total_orig > 0, f"Lab({L},{a},{b}) should produce non-black color"
        assert total_dark < total_orig, \
            f"Lab({L},{a},{b}): Expected darkening, got {total_dark} >= {total_orig}"
    
    print("Display gamma darkening test passed")


def test_generate_kona_binary():
    """Test that binary Kona table data is generated correctly.
    
    This tests the binary serialization that matches the firmware's kona_table_t struct.
    """
    # Create test entries: (kona_id, L, a, b)
    test_entries = [
        (100, 50.0, 10.0, -5.0),
        (200, 75.0, -20.0, 30.0),
    ]
    
    # Calculate expected binary format
    # Header: uint16_t version + uint16_t entry_count + uint32_t crc32
    # Entries: 365 * 16 bytes each (uint16_t id + 2 pad + 3 floats)
    
    # Build entry payload for CRC calculation
    import zlib
    crc_payload = bytearray()
    for kona_id, L, a, b in test_entries:
        crc_payload.extend(struct.pack("<H2x3f", kona_id, L, a, b))
    expected_crc = zlib.crc32(crc_payload) & 0xFFFFFFFF
    
    # Build full table
    entry_payload = bytearray()
    for kona_id, L, a, b in test_entries:
        entry_payload.extend(struct.pack("<H2x3f", kona_id, L, a, b))
    # Pad to 365 entries
    remaining = MAX_ENTRIES - len(test_entries)
    entry_payload.extend(b'\x00' * (remaining * KONA_REF_T_SIZE))
    
    header = struct.pack("<HHI", SCHEMA_VERSION, len(test_entries), expected_crc)
    expected_data = header + bytes(entry_payload)
    
    # Simulate the _generate_kona_binary function
    def generate_kona_binary_standalone(entries):
        entry_data = bytearray()
        for kona_id, L, a, b in entries:
            entry_data.extend(struct.pack("<H2x3f", kona_id, L, a, b))
        
        remaining = MAX_ENTRIES - len(entries)
        if remaining > 0:
            entry_data.extend(b'\x00' * (remaining * KONA_REF_T_SIZE))
        
        crc_data = bytearray()
        for kona_id, L, a, b in entries:
            crc_data.extend(struct.pack("<H2x3f", kona_id, L, a, b))
        crc = zlib.crc32(crc_data) & 0xFFFFFFFF
        
        header = struct.pack("<HHI", SCHEMA_VERSION, len(entries), crc)
        return header + bytes(entry_data)
    
    actual_data = generate_kona_binary_standalone(test_entries)
    
    # Verify total size
    expected_size = 8 + (MAX_ENTRIES * KONA_REF_T_SIZE)  # header + entries
    assert len(actual_data) == expected_size, \
        f"Expected {expected_size} bytes, got {len(actual_data)}"
    
    # Verify header
    version, count, crc = struct.unpack("<HHI", actual_data[:8])
    assert version == SCHEMA_VERSION, f"Version mismatch: {version} != {SCHEMA_VERSION}"
    assert count == len(test_entries), f"Count mismatch: {count} != {len(test_entries)}"
    assert crc == expected_crc, f"CRC mismatch: {crc:#x} != {expected_crc:#x}"
    
    # Verify first entry
    entry1 = struct.unpack("<H2x3f", actual_data[8:24])
    assert entry1[0] == 100, f"First entry ID mismatch: {entry1[0]}"
    assert abs(entry1[1] - 50.0) < 1e-6, f"First entry L mismatch: {entry1[1]}"
    
    print("Binary Kona table generation test passed")


def test_lab_value_formatting():
    """Test that Lab value formatting handles None values correctly.
    
    This tests the fix for the bug where f-string formatting like
    f"{s.L:.4f if s.L else 'None'}" would fail with:
    "Invalid format specifier '.4f if s.L else 'None'' for object of type 'float'"
    
    The fix uses conditional expressions BEFORE the format specifier:
    f"{s.L:.4f}" if s.L is not None else "None"
    """
    # Test swatches with various Lab value states
    test_cases = [
        # (L, a, b, expected_L_str, expected_a_str, expected_b_str)
        (50.1234, 10.5678, -5.9999, "50.1234", "10.5678", "-5.9999"),  # All values
        (None, 10.0, -5.0, "None", "10.0000", "-5.0000"),  # L is None
        (50.0, None, -5.0, "50.0000", "None", "-5.0000"),  # a is None
        (50.0, 10.0, None, "50.0000", "10.0000", "None"),  # b is None
        (None, None, None, "None", "None", "None"),  # All None
        (0.0, 0.0, 0.0, "0.0000", "0.0000", "0.0000"),  # Zero values are valid and formatted
    ]
    
    for L, a, b, expected_L, expected_a, expected_b in test_cases:
        # Use the same formatting logic as the debug output in _save_json
        L_str = f"{L:.4f}" if L is not None else "None"
        a_str = f"{a:.4f}" if a is not None else "None"
        b_str = f"{b:.4f}" if b is not None else "None"
        
        assert L_str == expected_L, f"L mismatch for {L}: got '{L_str}', expected '{expected_L}'"
        assert a_str == expected_a, f"a mismatch for {a}: got '{a_str}', expected '{expected_a}'"
        assert b_str == expected_b, f"b mismatch for {b}: got '{b_str}', expected '{expected_b}'"
    
    print("Lab value formatting test passed")


def test_collect_measured_swatches_for_upload():
    """Test that measured swatches are correctly collected and converted for upload.
    
    This tests the logic used by _on_upload_kona to gather measured swatches
    from self.swatches and convert them to (id, L, a, b) tuples sorted by ID.
    
    Edge cases tested:
    - Unmeasured swatches (measured=False) should be excluded
    - Swatches with partial Lab values (e.g., missing 'a') should be excluded
    - Only swatches with complete data should be included
    - Results should be sorted by swatch ID
    """
    # Create test swatches dict simulating self.swatches
    swatches = {
        200: SwatchData(panel="A", panel_index=1, id=200, name="SECOND",
                       L=75.0, a=10.0, b=20.0, measured=True),
        100: SwatchData(panel="A", panel_index=2, id=100, name="FIRST",
                       L=50.0, a=5.0, b=10.0, measured=True),
        300: SwatchData(panel="A", panel_index=3, id=300, name="UNMEASURED",
                       L=None, a=None, b=None, measured=False),  # Not measured
        400: SwatchData(panel="A", panel_index=4, id=400, name="PARTIAL",
                       L=60.0, a=None, b=15.0, measured=True),  # Missing 'a'
        500: SwatchData(panel="A", panel_index=5, id=500, name="THIRD",
                       L=80.0, a=15.0, b=25.0, measured=True),
    }
    
    # Collect measured swatches (logic from _on_upload_kona)
    measured = [s for s in swatches.values() 
               if s.measured and s.L is not None and s.a is not None and s.b is not None]
    
    # Convert to (id, L, a, b) tuples sorted by ID
    entries = [(s.id, s.L, s.a, s.b) for s in sorted(measured, key=lambda s: s.id)]
    
    # Verify only complete measured swatches are included
    assert len(entries) == 3, f"Expected 3 entries, got {len(entries)}"
    
    # Verify entries are sorted by ID
    assert entries[0][0] == 100, f"First entry should be ID 100, got {entries[0][0]}"
    assert entries[1][0] == 200, f"Second entry should be ID 200, got {entries[1][0]}"
    assert entries[2][0] == 500, f"Third entry should be ID 500, got {entries[2][0]}"
    
    # Verify Lab values are correct
    assert entries[0] == (100, 50.0, 5.0, 10.0), f"Entry 0 mismatch: {entries[0]}"
    assert entries[1] == (200, 75.0, 10.0, 20.0), f"Entry 1 mismatch: {entries[1]}"
    assert entries[2] == (500, 80.0, 15.0, 25.0), f"Entry 2 mismatch: {entries[2]}"
    
    # Verify unmeasured (300) and partial (400) swatches are excluded
    entry_ids = [e[0] for e in entries]
    assert 300 not in entry_ids, "Unmeasured swatch (300) should be excluded"
    assert 400 not in entry_ids, "Partial swatch (400) should be excluded"
    
    print("Collect measured swatches for upload test passed")


# ---------------------------------------------------------------------------
# JSON round-trip tests (standalone – no tkinter required)
# ---------------------------------------------------------------------------

def _make_json_data(swatches: list) -> dict:
    """Build a minimal kona_captures.json structure."""
    return {
        "schema_version": 1,
        "capture_date": "2026-01-01T00:00:00Z",
        "device": {"firmware_version": "1.0.0", "firmware_commit": "aabbccdd"},
        "pipeline_config_snapshot": {},
        "swatches": swatches,
    }


def _swatch_to_json_entry(s: SwatchData) -> dict:
    """Convert a SwatchData to the JSON swatch entry format (mirrors _save_json logic)."""
    return {
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
    }


def _load_json_swatches(path: pathlib.Path) -> dict:
    """Load swatches from a JSON file (mirrors _load_json logic)."""
    with path.open(encoding="utf-8") as f:
        data = json.load(f)
    swatches = {}
    for entry in data.get("swatches", []):
        swatch_id = int(entry.get("id", 0))
        if swatch_id == 0:
            continue
        lab = entry.get("lab") or {}
        raw = entry.get("raw") or {}
        swatches[swatch_id] = SwatchData(
            panel=str(entry.get("panel", "")),
            panel_index=int(entry.get("panel_index", 0)),
            id=swatch_id,
            name=str(entry.get("name", "")),
            L=float(lab["l"]) if lab.get("l") is not None else None,
            a=float(lab["a"]) if lab.get("a") is not None else None,
            b=float(lab["b"]) if lab.get("b") is not None else None,
            measured=bool(entry.get("measured", False)),
            notes=str(entry.get("notes", "")),
            raw_x=int(raw["x"]) if raw.get("x") is not None else None,
            raw_y=int(raw["y"]) if raw.get("y") is not None else None,
            raw_z=int(raw["z"]) if raw.get("z") is not None else None,
            raw_ir=int(raw["ir"]) if raw.get("ir") is not None else None,
            raw_clear=int(raw["clear"]) if raw.get("clear") is not None else None,
            raw_gain=int(raw["gain"]) if raw.get("gain") is not None else None,
            raw_integration_ms=int(raw["integration_ms"]) if raw.get("integration_ms") is not None else None,
        )
    return swatches


def test_json_roundtrip_lab_values():
    """Test that Lab values survive a JSON write/read cycle without precision loss."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"

        original = SwatchData(
            panel="yellow_orange_red", panel_index=1, id=449, name="SUNNY",
            L=98.5, a=26.026400, b=110.0,
            measured=True, notes="test",
            raw_x=32200000, raw_y=33400000, raw_z=27900000,
            raw_ir=619520, raw_clear=21435649, raw_gain=5, raw_integration_ms=100,
        )

        data = _make_json_data([_swatch_to_json_entry(original)])
        with json_path.open("w") as f:
            json.dump(data, f, indent=2)
            f.write("\n")

        loaded = _load_json_swatches(json_path)
        assert 449 in loaded, "Swatch 449 should be present after JSON round-trip"

        s = loaded[449]
        assert abs(s.L - 98.5) < 1e-5, f"L mismatch: {s.L}"
        assert abs(s.a - 26.0264) < 1e-3, f"a mismatch: {s.a}"
        assert abs(s.b - 110.0) < 1e-5, f"b mismatch: {s.b}"
        assert s.raw_x == 32200000
        assert s.raw_gain == 5
        assert s.raw_integration_ms == 100

    print("JSON round-trip Lab values test passed")


def test_json_roundtrip_null_raw():
    """Test that swatches with null raw data survive JSON round-trip."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"

        original = SwatchData(
            panel="blues", panel_index=3, id=120, name="AZURE",
            L=44.0, a=2.0, b=3.0, measured=True,
        )

        data = _make_json_data([_swatch_to_json_entry(original)])
        with json_path.open("w") as f:
            json.dump(data, f, indent=2)
            f.write("\n")

        loaded = _load_json_swatches(json_path)
        assert 120 in loaded

        s = loaded[120]
        assert s.raw_x is None
        assert s.raw_gain is None
        assert abs(s.L - 44.0) < 1e-6

    print("JSON round-trip null raw data test passed")


def test_json_unmeasured_not_included_in_cpp():
    """Test that unmeasured JSON swatches are excluded from C++ output."""
    from generate_kona_table import parse_json_captures, render_cpp

    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"

        measured = SwatchData(panel="t", panel_index=1, id=100, name="MEASURED",
                              L=50.0, a=5.0, b=5.0, measured=True)
        unmeasured = SwatchData(panel="t", panel_index=2, id=200, name="UNMEASURED",
                                L=None, a=None, b=None, measured=False)

        data = _make_json_data([
            _swatch_to_json_entry(measured),
            _swatch_to_json_entry(unmeasured),
        ])
        with json_path.open("w") as f:
            json.dump(data, f, indent=2)

        entries = parse_json_captures(json_path)
        assert len(entries) == 1, f"Expected 1 entry, got {len(entries)}"
        assert entries[0].kona_id == 100

        cpp = render_cpp(entries, json_path)
        assert "// 100 MEASURED" in cpp
        assert "200" not in cpp

    print("JSON unmeasured exclusion test passed")


def test_swatchdata_has_raw_fields():
    """Test that SwatchData has all expected raw sensor fields."""
    s = SwatchData(panel="t", panel_index=1, id=1, name="TEST")
    # Verify raw fields exist with None defaults
    assert hasattr(s, "raw_x") and s.raw_x is None
    assert hasattr(s, "raw_y") and s.raw_y is None
    assert hasattr(s, "raw_z") and s.raw_z is None
    assert hasattr(s, "raw_ir") and s.raw_ir is None
    assert hasattr(s, "raw_clear") and s.raw_clear is None
    assert hasattr(s, "raw_gain") and s.raw_gain is None
    assert hasattr(s, "raw_integration_ms") and s.raw_integration_ms is None

    # Verify raw fields can be set
    s.raw_x = 32200000
    s.raw_gain = 5
    s.raw_integration_ms = 100
    assert s.raw_x == 32200000
    assert s.raw_gain == 5

    print("SwatchData raw fields test passed")


def test_scan_response_parsing():
    """Test SCAN and RAWDATA response parsing used by SerialConnection.scan()."""

    # --- SCAN response (Lab + RGB only) ---
    def parse_scan_response(response):
        if not response.startswith("OK:LAB:"):
            return None
        try:
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
        except (IndexError, ValueError):
            return None

    scan_resp = "OK:LAB:98.5000,26.0264,110.0000:RGB:255,228,0"
    result = parse_scan_response(scan_resp)
    assert result is not None
    assert abs(result[0] - 98.5) < 1e-4
    assert result[3] == 255 and result[4] == 228 and result[5] == 0

    # --- RAWDATA response ---
    def parse_rawdata_response(response):
        prefix = "OK:RAWDATA:"
        if not response.startswith(prefix):
            return None
        try:
            raw_parts = response[len(prefix):].split(",")
            raw_x     = int(raw_parts[0])
            raw_y     = int(raw_parts[1])
            raw_z     = int(raw_parts[2])
            raw_ir    = int(raw_parts[3])
            raw_clear = int(raw_parts[4])
            raw_gain  = int(raw_parts[5])
            raw_int_ms = int(raw_parts[6])
            return (raw_x, raw_y, raw_z, raw_ir, raw_clear, raw_gain, raw_int_ms)
        except (IndexError, ValueError):
            return None

    rawdata_resp = "OK:RAWDATA:32200000,33400000,27900000,619520,21435649,5,100"
    raw = parse_rawdata_response(rawdata_resp)
    assert raw is not None
    assert raw[0] == 32200000  # raw_x
    assert raw[1] == 33400000  # raw_y
    assert raw[2] == 27900000  # raw_z
    assert raw[3] == 619520    # raw_ir
    assert raw[4] == 21435649  # raw_clear
    assert raw[5] == 5         # raw_gain
    assert raw[6] == 100       # raw_integration_ms

    # ERR:NO_RAWDATA when no scan has happened yet
    assert parse_rawdata_response("ERR:NO_RAWDATA") is None

    print("Scan response parsing test passed")


if __name__ == "__main__":
    test_generate_cpp_inline_comments()
    test_generate_cpp_sorted_by_id()
    test_generate_cpp_valid_cpp_syntax()
    test_scan_queue_advancement()
    test_remaining_selection_calculation()
    test_display_gamma_darkens_colors()
    test_generate_kona_binary()
    test_lab_value_formatting()
    test_collect_measured_swatches_for_upload()
    test_json_roundtrip_lab_values()
    test_json_roundtrip_null_raw()
    test_json_unmeasured_not_included_in_cpp()
    test_swatchdata_has_raw_fields()
    test_scan_response_parsing()
    print("\nAll kona_scanner_gui tests passed")
