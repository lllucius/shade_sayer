#!/usr/bin/env python3

import csv
import pathlib
import tempfile
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from generate_kona_table import MAX_ENTRIES, crc32_entries, parse_capture_csv, render_cpp


def _write_csv(path: pathlib.Path, rows):
    header = [
        "kona_id",
        "stddev_xyz_x",
        "stddev_xyz_y",
        "stddev_xyz_z",
        "mean_lab_l",
        "mean_lab_a",
        "mean_lab_b",
    ]
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=header)
        w.writeheader()
        w.writerows(rows)


def test_parse_and_render():
    with tempfile.TemporaryDirectory() as tmp:
        csv_path = pathlib.Path(tmp) / "kona_avg_captures.csv"
        _write_csv(csv_path, [
            {
                "kona_id": "249",
                "stddev_xyz_x": "0.10",
                "stddev_xyz_y": "0.20",
                "stddev_xyz_z": "0.30",
                "mean_lab_l": "55.0",
                "mean_lab_a": "12.0",
                "mean_lab_b": "-18.0",
            },
            {
                "kona_id": "120",
                "stddev_xyz_x": "0.40",
                "stddev_xyz_y": "0.50",
                "stddev_xyz_z": "0.60",
                "mean_lab_l": "44.0",
                "mean_lab_a": "2.0",
                "mean_lab_b": "3.0",
            },
            # duplicate id should keep last row
            {
                "kona_id": "249",
                "stddev_xyz_x": "1.10",
                "stddev_xyz_y": "1.20",
                "stddev_xyz_z": "1.30",
                "mean_lab_l": "65.0",
                "mean_lab_a": "22.0",
                "mean_lab_b": "-28.0",
            },
        ])

        entries = parse_capture_csv(csv_path)
        assert len(entries) == 2
        assert entries[0].kona_id == 120
        assert entries[1].kona_id == 249
        assert abs(entries[1].l - 65.0) < 1e-6

        crc = crc32_entries(entries)
        cpp = render_cpp(entries, csv_path)
        assert f".crc32 = 0x{crc:08X}u" in cpp
        assert ".entry_count = 2" in cpp


def test_entry_limit():
    with tempfile.TemporaryDirectory() as tmp:
        csv_path = pathlib.Path(tmp) / "kona_avg_captures.csv"
        rows = []
        for i in range(MAX_ENTRIES + 1):
            rows.append(
                {
                    "kona_id": str(i + 1),
                    "stddev_xyz_x": "0",
                    "stddev_xyz_y": "0",
                    "stddev_xyz_z": "0",
                    "mean_lab_l": "0",
                    "mean_lab_a": "0",
                    "mean_lab_b": "0",
                }
            )
        _write_csv(csv_path, rows)

        try:
            parse_capture_csv(csv_path)
        except ValueError:
            return

        raise AssertionError("Expected ValueError for too many entries")


if __name__ == "__main__":
    test_parse_and_render()
    test_entry_limit()
    print("generate_kona_table tests passed")
