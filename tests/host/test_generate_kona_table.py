#!/usr/bin/env python3
"""Tests for generate_kona_table.py script."""

import pathlib
import struct
import tempfile
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from generate_kona_table import MAX_ENTRIES, crc32_entries, render_cpp, parse_json_captures, parse_captures


def test_struct_size():
    """Test that the binary format matches C++ struct layout (16 bytes with padding)."""
    # Expected layout: uint16_t + 2 padding + 3 floats = 16 bytes
    expected_size = 16
    packed_data = struct.pack("<H2x3f", 42, 1.0, 2.0, 3.0)
    assert len(packed_data) == expected_size, f"Expected {expected_size} bytes, got {len(packed_data)}"
    
    # Verify we can unpack it correctly
    kona_id, l, a, b = struct.unpack("<H2x3f", packed_data)
    assert kona_id == 42
    assert l == 1.0
    assert a == 2.0
    assert b == 3.0


def _write_json(path: pathlib.Path, swatches: list, schema_version: int = 1):
    """Helper to write test kona_captures.json files."""
    import json
    data = {
        "schema_version": schema_version,
        "capture_date": "",
        "device": {"firmware_version": "", "firmware_commit": ""},
        "pipeline_config_snapshot": {},
        "swatches": swatches,
    }
    with path.open("w") as f:
        json.dump(data, f, indent=2)


def test_parse_json_basic():
    """Test basic JSON capture parsing with measured swatches."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"
        _write_json(json_path, [
            {
                "panel": "test", "panel_index": 1,
                "id": 249, "name": "CRIMSON",
                "measured": True,
                "raw": {"x": 0, "y": 0, "z": 0, "ir": 0, "clear": 0, "gain": 5, "integration_ms": 100},
                "lab": {"l": 55.0, "a": 12.0, "b": -18.0},
                "rgb": {"r": None, "g": None, "b": None},
                "notes": "",
            },
            {
                "panel": "test", "panel_index": 2,
                "id": 120, "name": "AZURE",
                "measured": True,
                "raw": {"x": 0, "y": 0, "z": 0, "ir": 0, "clear": 0, "gain": 5, "integration_ms": 100},
                "lab": {"l": 44.0, "a": 2.0, "b": 3.0},
                "rgb": {"r": None, "g": None, "b": None},
                "notes": "",
            },
            # Unmeasured – should be skipped
            {
                "panel": "test", "panel_index": 3,
                "id": 300, "name": "IGNORE",
                "measured": False,
                "raw": {},
                "lab": {"l": None, "a": None, "b": None},
                "rgb": {},
                "notes": "",
            },
        ])

        entries = parse_json_captures(json_path)
        assert len(entries) == 2
        assert entries[0].kona_id == 120
        assert entries[1].kona_id == 249
        assert abs(entries[1].l - 55.0) < 1e-6
        assert entries[0].name == "AZURE"

        cpp = render_cpp(entries, json_path)
        assert "// 120 AZURE" in cpp
        assert "// 249 CRIMSON" in cpp


def test_parse_json_missing_lab():
    """Test that JSON entries with null Lab values are skipped."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"
        _write_json(json_path, [
            {
                "id": 100, "name": "COMPLETE", "panel": "t", "panel_index": 1,
                "measured": True,
                "raw": {},
                "lab": {"l": 50.0, "a": 5.0, "b": 5.0},
                "rgb": {}, "notes": "",
            },
            {
                "id": 200, "name": "NO_LAB", "panel": "t", "panel_index": 2,
                "measured": True,
                "raw": {},
                "lab": {"l": None, "a": None, "b": None},
                "rgb": {}, "notes": "",
            },
        ])

        entries = parse_json_captures(json_path)
        assert len(entries) == 1
        assert entries[0].kona_id == 100


def test_parse_captures_autodetect():
    """Test that parse_captures() reads from a kona_captures.json file."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "captures.json"
        _write_json(json_path, [
            {
                "id": 59, "name": "CANTALOUPE", "panel": "t", "panel_index": 1,
                "measured": True,
                "raw": {},
                "lab": {"l": 98.5, "a": 66.3088, "b": 90.0752},
                "rgb": {}, "notes": "",
            },
        ])

        entries = parse_captures(json_path)
        assert len(entries) == 1
        assert entries[0].kona_id == 59
        assert abs(entries[0].l - 98.5) < 1e-4


def test_json_duplicate_id():
    """Test that duplicate swatch IDs are deduplicated (last entry wins)."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"
        _write_json(json_path, [
            {"id": 249, "name": "CRIMSON", "panel": "t", "panel_index": 1,
             "measured": True, "raw": {}, "lab": {"l": 55.0, "a": 12.0, "b": -18.0},
             "rgb": {}, "notes": ""},
            {"id": 120, "name": "AZURE", "panel": "t", "panel_index": 2,
             "measured": True, "raw": {}, "lab": {"l": 44.0, "a": 2.0, "b": 3.0},
             "rgb": {}, "notes": ""},
            # Duplicate id=249 — last entry wins
            {"id": 249, "name": "CRIMSON", "panel": "t", "panel_index": 3,
             "measured": True, "raw": {}, "lab": {"l": 65.0, "a": 22.0, "b": -28.0},
             "rgb": {}, "notes": ""},
        ])

        entries = parse_json_captures(json_path)
        assert len(entries) == 2
        assert entries[0].kona_id == 120
        assert entries[1].kona_id == 249
        assert abs(entries[1].l - 65.0) < 1e-6

        crc = crc32_entries(entries)
        cpp = render_cpp(entries, json_path)
        assert f".crc32 = 0x{crc:08X}u" in cpp
        assert ".entry_count = 2" in cpp


def test_entry_limit():
    """Test that MAX_ENTRIES limit is enforced."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"
        swatches = [
            {"id": i + 1, "name": f"SWATCH_{i + 1}", "panel": "t",
             "panel_index": i + 1, "measured": True, "raw": {},
             "lab": {"l": 50.0, "a": 0.0, "b": 0.0}, "rgb": {}, "notes": ""}
            for i in range(MAX_ENTRIES + 1)
        ]
        _write_json(json_path, swatches)

        try:
            parse_json_captures(json_path)
        except ValueError:
            return

        raise AssertionError("Expected ValueError for too many entries")


def test_name_comment_in_output():
    """Test that rendered C++ includes name comments per entry."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"
        _write_json(json_path, [
            {"id": 59, "name": "CANTALOUPE", "panel": "t", "panel_index": 1,
             "measured": True, "raw": {},
             "lab": {"l": 98.5, "a": 66.3088, "b": 90.0752},
             "rgb": {}, "notes": ""},
        ])

        entries = parse_json_captures(json_path)
        cpp = render_cpp(entries, json_path)
        assert "// 59 CANTALOUPE" in cpp, "Missing name comment in output"


def test_description_table_in_output():
    """Test that rendered C++ includes a description table when descriptions are present."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"
        _write_json(json_path, [
            {"id": 59, "name": "CANTALOUPE", "panel": "t", "panel_index": 1,
             "measured": True, "raw": {},
             "lab": {"l": 98.5, "a": 66.3088, "b": 90.0752},
             "rgb": {}, "notes": "",
             "description": "A warm orange like ripe cantaloupe with a bright, sunny look."},
            {"id": 120, "name": "AZURE", "panel": "t", "panel_index": 2,
             "measured": True, "raw": {},
             "lab": {"l": 44.0, "a": 2.0, "b": 3.0},
             "rgb": {}, "notes": "",
             "description": "A medium blue like a clear morning sky with a crisp, cool look."},
        ])

        entries = parse_json_captures(json_path)
        assert entries[0].description == "A warm orange like ripe cantaloupe with a bright, sunny look."
        assert entries[1].description == "A medium blue like a clear morning sky with a crisp, cool look."

        cpp = render_cpp(entries, json_path)
        assert "kona_description_count = 2" in cpp
        assert "kona_description_t" in cpp
        assert '"A warm orange like ripe cantaloupe with a bright, sunny look."' in cpp
        assert '"A medium blue like a clear morning sky with a crisp, cool look."' in cpp


def test_description_table_empty_when_no_descriptions():
    """Test that rendered C++ has empty description table when no descriptions are present."""
    with tempfile.TemporaryDirectory() as tmp:
        json_path = pathlib.Path(tmp) / "kona_captures.json"
        _write_json(json_path, [
            {"id": 59, "name": "CANTALOUPE", "panel": "t", "panel_index": 1,
             "measured": True, "raw": {},
             "lab": {"l": 98.5, "a": 66.3088, "b": 90.0752},
             "rgb": {}, "notes": ""},
        ])

        entries = parse_json_captures(json_path)
        assert entries[0].description == ""

        cpp = render_cpp(entries, json_path)
        assert "kona_description_count = 0" in cpp


if __name__ == "__main__":
    test_struct_size()
    test_parse_json_basic()
    test_parse_json_missing_lab()
    test_parse_captures_autodetect()
    test_json_duplicate_id()
    test_entry_limit()
    test_name_comment_in_output()
    test_description_table_in_output()
    test_description_table_empty_when_no_descriptions()
    print("generate_kona_table tests passed")
