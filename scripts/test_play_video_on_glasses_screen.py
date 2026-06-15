#!/usr/bin/env python3
"""Test that RecordLab video playback targets the glasses display."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import play_video_on_secondary_screen as player


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "video",
        nargs="?",
        default=str(script_dir / "old_video.mp4"),
        help="Video file path. Defaults to scripts/localhost/old_video.mp4",
    )
    parser.add_argument("--screen", help="Target xrandr screen name. Defaults to non-primary screen.")
    parser.add_argument("--duration", type=float, default=15.0, help="Test playback duration in seconds.")
    parser.add_argument(
        "--mode",
        choices=("borderless", "fullscreen"),
        default="borderless",
        help="Playback placement mode to test.",
    )
    args = parser.parse_args()

    screens = player.find_screens()
    print("[test_play_video] Connected screens:")
    for index, screen in enumerate(screens):
        primary = " primary" if screen["primary"] else ""
        print(
            f"  {index}: {screen['name']}{primary} "
            f"{screen['width']}x{screen['height']}+{screen['x']}+{screen['y']}"
        )

    target = player.choose_screen(screens, args.screen)
    print(
        "[test_play_video] Target screen: "
        f"{target['name']} {target['width']}x{target['height']}+{target['x']}+{target['y']}"
    )
    print("[test_play_video] 请确认视频是否出现在眼镜显示区域，而不是当前主显示器。")

    play_script = script_dir / "play_video_on_secondary_screen.py"
    cmd = [
        sys.executable,
        str(play_script),
        str(Path(args.video).expanduser()),
        "--screen",
        target["name"],
        "--duration",
        str(args.duration),
        "--mode",
        args.mode,
        "--wait",
    ]
    result = subprocess.run(cmd, text=True)
    if result.returncode == 0:
        print("[test_play_video] mpv 正常退出。若刚才视频显示在眼镜上，则定位已修复。")
    else:
        print(f"[test_play_video] mpv returned {result.returncode}", file=sys.stderr)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
