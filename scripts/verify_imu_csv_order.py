#!/usr/bin/env python3
"""Verify RecordLabC IMU CSV column order."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Iterable


EXPECTED_HEADER = [
    "timestamp_ns",
    "type",
    "data0",
    "data1",
    "data2",
    "data3",
    "data4",
    "data5",
]


def iter_csv_files(paths: Iterable[Path]) -> Iterable[Path]:
    for path in paths:
        if path.is_dir():
            yield from sorted(path.glob("imu_*.csv"))
        else:
            yield path


def verify_file(path: Path, max_rows: int) -> bool:
    if not path.exists():
        print(f"[FAIL] {path}: file does not exist")
        return False
    if path.stat().st_size == 0:
        print(f"[SKIP] {path}: empty file")
        return True

    with path.open(newline="") as handle:
        reader = csv.reader(handle)
        try:
            header = next(reader)
        except StopIteration:
            print(f"[SKIP] {path}: empty file")
            return True

        if header != EXPECTED_HEADER:
            print(f"[FAIL] {path}: header={header}")
            return False

        checked_rows = 0
        for row_number, row in enumerate(reader, start=2):
            if len(row) != len(EXPECTED_HEADER):
                print(f"[FAIL] {path}:{row_number}: expected 8 columns, got {len(row)}")
                return False
            try:
                int(row[0])
                int(row[1])
                for value in row[2:]:
                    float(value)
            except ValueError as exc:
                print(f"[FAIL] {path}:{row_number}: invalid numeric value: {exc}")
                return False

            checked_rows += 1
            if max_rows > 0 and checked_rows >= max_rows:
                break

    print(f"[OK] {path}: header/order valid, checked_rows={checked_rows}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify IMU CSV files use timestamp_ns,type,data0..data5 order."
    )
    parser.add_argument("paths", nargs="+", type=Path, help="CSV files or record dirs")
    parser.add_argument(
        "--max-rows",
        type=int,
        default=1000,
        help="Rows to validate per file; use 0 to scan all rows.",
    )
    args = parser.parse_args()

    files = list(iter_csv_files(args.paths))
    if not files:
        print("[FAIL] no imu_*.csv files found")
        return 1

    ok = True
    for path in files:
        ok = verify_file(path, args.max_rows) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
