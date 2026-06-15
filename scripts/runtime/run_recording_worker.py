#!/usr/bin/env python3
"""Run RecordLabC recording helper workers with a readiness handshake."""

from __future__ import annotations

import argparse
import logging
import signal
import sys
import time
from pathlib import Path


def write_ready(path: str, state: str, detail: str = "") -> None:
    if not path:
        return
    ready_path = Path(path)
    ready_path.parent.mkdir(parents=True, exist_ok=True)
    ready_path.write_text(f"{state}\n{detail}\n", encoding="utf-8")


def build_worker(worker_name: str, save_dir: str):
    if worker_name == "screen_capture":
        from scripts.common.screen_capture_worker import ScreenCaptureWorker

        return ScreenCaptureWorker(save_dir)
    if worker_name == "mic_record":
        from scripts.common.mic_record_worker import MicRecordWorker

        return MicRecordWorker(save_dir)
    raise ValueError(f"Unknown recording worker: {worker_name}")


def main() -> int:
    parser = argparse.ArgumentParser(description="RecordLabC recording worker runner")
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--worker", required=True, choices=["screen_capture", "mic_record"])
    parser.add_argument("--save-dir", required=True)
    parser.add_argument("--ready-file", required=True)
    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()
    save_dir = Path(args.save_dir).resolve()
    save_dir.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(project_root))

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        handlers=[
            logging.StreamHandler(sys.stderr),
            logging.FileHandler(save_dir / f"{args.worker}.log", encoding="utf-8"),
        ],
    )

    stop_requested = False

    def request_stop(_signum, _frame):
        nonlocal stop_requested
        stop_requested = True

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    worker = None
    try:
        worker = build_worker(args.worker, str(save_dir))
        worker.start()
        write_ready(args.ready_file, "ready")
        while not stop_requested:
            time.sleep(0.2)
        return 0
    except Exception as exc:
        logging.exception("Recording worker failed to start")
        write_ready(args.ready_file, "error", str(exc))
        return 1
    finally:
        if worker is not None:
            try:
                worker.stop()
            except Exception:
                logging.exception("Recording worker failed during stop")


if __name__ == "__main__":
    raise SystemExit(main())
