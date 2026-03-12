#!/usr/bin/env python3

import csv
import pathlib
import subprocess
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "kona_scan_collect.py"


def test_collect_from_console_log() -> None:
    with tempfile.TemporaryDirectory() as td:
        tdp = pathlib.Path(td)
        log_path = tdp / "console.txt"
        metadata_path = tdp / "kona_365_sensor_ready.csv"
        out_path = tdp / "kona_avg_captures.csv"

        metadata_path.write_text(
            "panel,panel_index,id,name\n"
            "yellow_orange_red,1,449,SUNNY\n",
            encoding="utf-8",
        )

        # Valid line + malformed line that should be skipped.
        log_path.write_text(
            "I (1) foo: hello\n"
            "I (2) KONA_SCAN_CSV: idx_001,SUNNY,1,5,100,0x00,0x00,3,3,0,0,1.0,2.0,3.0,0.1,0.2,0.3,50.0,1.0,2.0,123\n"
            "I (3) KONA_SCAN_CSV: bad,row\n",
            encoding="utf-8",
        )

        proc = subprocess.run(
            [
                "python3",
                str(SCRIPT),
                "--input-log",
                str(log_path),
                "--metadata",
                str(metadata_path),
                "--output",
                str(out_path),
            ],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        assert "Captured 449 SUNNY" in proc.stdout

        with out_path.open(newline="", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))

        assert len(rows) == 1
        row = rows[0]
        assert row["panel"] == "yellow_orange_red"
        assert row["panel_index"] == "1"
        assert row["kona_id"] == "449"
        assert row["kona_name"] == "SUNNY"
        assert row["mean_lab_l"] == "50.0"


if __name__ == "__main__":
    test_collect_from_console_log()
    print("kona_scan_collect tests passed")
