#!/usr/bin/env python3
"""Tests for generate_kona_table.py script."""

import csv
import pathlib
import struct
import tempfile
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from generate_kona_table import MAX_ENTRIES, crc32_entries, parse_sensor_ready_csv, render_cpp, parse_json_captures, parse_captures


def _write_csv(path: pathlib.Path, rows):
    """Helper to write test CSV files in kona_cotton_solids_k001.csv format."""
    header = [
        "panel",
        "panel_index",
        "id",
        "name",
        "L",
        "a",
        "b",
        "R",
        "G",
        "B",
        "measured",
        "notes",
    ]
    with path.open("w", newline="") as f:
        # extrasaction="ignore" allows test rows to omit optional fields safely
        w = csv.DictWriter(f, fieldnames=header, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


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


def test_parse_and_render():
    """Test parsing CSV and rendering C++ output."""
    with tempfile.TemporaryDirectory() as tmp:
        csv_path = pathlib.Path(tmp) / "kona_cotton_solids_k001.csv"
        _write_csv(csv_path, [
            {
                "panel": "test", "panel_index": "1",
                "id": "249", "name": "CRIMSON",
                "L": "55.0", "a": "12.0", "b": "-18.0",
                "measured": "true",
            },
            {
                "panel": "test", "panel_index": "2",
                "id": "120", "name": "AZURE",
                "L": "44.0", "a": "2.0", "b": "3.0",
                "measured": "true",
            },
            # duplicate id should keep last row
            {
                "panel": "test", "panel_index": "3",
                "id": "249", "name": "CRIMSON",
                "L": "65.0", "a": "22.0", "b": "-28.0",
                "measured": "true",
            },
            # unmeasured row should be skipped
            {
                "panel": "test", "panel_index": "4",
                "id": "300", "name": "IGNORE",
                "L": "70.0", "a": "5.0", "b": "5.0",
                "measured": "false",
            },
        ])

        entries = parse_sensor_ready_csv(csv_path)
        assert len(entries) == 2
        assert entries[0].kona_id == 120
        assert entries[1].kona_id == 249
        assert abs(entries[1].l - 65.0) < 1e-6
        assert entries[0].name == "AZURE"

        crc = crc32_entries(entries)
        cpp = render_cpp(entries, csv_path)
        assert f".crc32 = 0x{crc:08X}u" in cpp
        assert ".entry_count = 2" in cpp
        assert "// 120 AZURE" in cpp
        assert "// 249 CRIMSON" in cpp


def test_skip_incomplete_lab():
    """Test that rows with missing L/a/b values are skipped even if measured=true."""
    with tempfile.TemporaryDirectory() as tmp:
        csv_path = pathlib.Path(tmp) / "kona_cotton_solids_k001.csv"
        _write_csv(csv_path, [
            {
                "panel": "test", "panel_index": "1",
                "id": "100", "name": "COMPLETE",
                "L": "50.0", "a": "5.0", "b": "5.0",
                "measured": "true",
            },
            {
                "panel": "test", "panel_index": "2",
                "id": "200", "name": "MISSING_LAB",
                "L": "", "a": "", "b": "",
                "measured": "true",
            },
        ])

        entries = parse_sensor_ready_csv(csv_path)
        assert len(entries) == 1
        assert entries[0].kona_id == 100


def test_entry_limit():
    """Test that MAX_ENTRIES limit is enforced."""
    with tempfile.TemporaryDirectory() as tmp:
        csv_path = pathlib.Path(tmp) / "kona_cotton_solids_k001.csv"
        rows = []
        for i in range(MAX_ENTRIES + 1):
            rows.append(
                {
                    "panel": "test", "panel_index": str(i + 1),
                    "id": str(i + 1), "name": f"SWATCH_{i + 1}",
                    "L": "50", "a": "0", "b": "0",
                    "measured": "true",
                }
            )
        _write_csv(csv_path, rows)

        try:
            parse_sensor_ready_csv(csv_path)
        except ValueError:
            return

        raise AssertionError("Expected ValueError for too many entries")


def test_name_comment_in_output():
    """Test that rendered C++ includes name comments per entry."""
    with tempfile.TemporaryDirectory() as tmp:
        csv_path = pathlib.Path(tmp) / "kona_cotton_solids_k001.csv"
        _write_csv(csv_path, [
            {
                "panel": "test", "panel_index": "1",
                "id": "59", "name": "CANTALOUPE",
                "L": "98.5", "a": "66.3088", "b": "90.0752",
                "measured": "true",
            },
        ])

        entries = parse_sensor_ready_csv(csv_path)
        cpp = render_cpp(entries, csv_path)
        assert "// 59 CANTALOUPE" in cpp, "Missing name comment in output"


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
    """Test that parse_captures() dispatches based on file extension."""
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

        # CSV path should still dispatch to CSV parser
        csv_path = pathlib.Path(tmp) / "swatches.csv"
        header = ["panel", "panel_index", "id", "name", "L", "a", "b",
                  "R", "G", "B", "measured", "notes"]
        import csv as csv_mod
        with csv_path.open("w", newline="") as f:
            w = csv_mod.DictWriter(f, fieldnames=header, extrasaction="ignore")
            w.writeheader()
            w.writerow({"id": "59", "name": "CANTALOUPE", "panel": "t",
                        "panel_index": "1", "L": "98.5", "a": "66.3088",
                        "b": "90.0752", "measured": "true"})
        entries_csv = parse_captures(csv_path)
        assert len(entries_csv) == 1
        assert entries_csv[0].kona_id == 59


if __name__ == "__main__":
    test_struct_size()
    test_parse_and_render()
    test_skip_incomplete_lab()
    test_entry_limit()
    test_name_comment_in_output()
    test_parse_json_basic()
    test_parse_json_missing_lab()
    test_parse_captures_autodetect()
    print("generate_kona_table tests passed")
