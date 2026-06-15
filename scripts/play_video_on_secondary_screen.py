#!/usr/bin/env python3
"""Play a video full-screen on the secondary display."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


SCREEN_RE = re.compile(
    r"^(?P<name>\S+)\s+connected(?P<primary>\s+primary)?\s+"
    r"(?P<width>\d+)x(?P<height>\d+)\+(?P<x>-?\d+)\+(?P<y>-?\d+)"
)


def run_text(cmd: list[str]) -> str:
    return subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)


def find_screens() -> list[dict[str, str]]:
    output = run_text(["xrandr", "--query"])
    screens = []
    for line in output.splitlines():
        match = SCREEN_RE.search(line)
        if match:
            screen = match.groupdict()
            screen["primary"] = bool(screen.get("primary"))
            screens.append(screen)
    return screens


def choose_screen(screens: list[dict[str, str]], preferred: Optional[str]) -> dict[str, str]:
    if preferred:
        for screen in screens:
            if screen["name"] == preferred:
                return screen
        raise RuntimeError(f"Screen not found: {preferred}")

    if len(screens) >= 2:
        if any(screen["primary"] for screen in screens):
            for screen in screens:
                if not screen["primary"]:
                    return screen
        return screens[1]
    if screens:
        return screens[0]
    raise RuntimeError("No connected display found from xrandr")


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "video",
        nargs="?",
        default=str(script_dir / "old_video.mp4"),
        help="Video file path. Defaults to scripts/localhost/old_video.mp4",
    )
    parser.add_argument("--screen", help="xrandr screen name, e.g. HDMI-1")
    parser.add_argument("--volume", default="50%", help="Master volume passed to amixer")
    parser.add_argument("--duration", type=float, help="Stop playback after this many seconds")
    parser.add_argument("--no-loop", action="store_true", help="Disable looping playback")
    parser.add_argument("--list-screens", action="store_true", help="Print xrandr connected screens and exit")
    parser.add_argument("--wait", action="store_true", help="Run mpv in foreground and return its exit code")
    parser.add_argument(
        "--mode",
        choices=("borderless", "fullscreen"),
        default="borderless",
        help="Use a borderless screen-sized window by default; fullscreen may be moved by Wayland.",
    )
    args = parser.parse_args()

    screens = find_screens()
    if args.list_screens:
        for index, screen in enumerate(screens):
            primary = " primary" if screen["primary"] else ""
            print(
                f"{index}: {screen['name']}{primary} "
                f"{screen['width']}x{screen['height']}+{screen['x']}+{screen['y']}"
            )
        return 0

    video_path = Path(os.path.expanduser(args.video)).resolve()
    if not video_path.exists() and args.video == str(script_dir / "old_video.mp4"):
        legacy_video = Path("/home/hyren/RecordLab/scripts/localhost/old_video.mp4")
        if legacy_video.exists():
            video_path = legacy_video
    if not video_path.exists():
        print(f"Video file not found: {video_path}", file=sys.stderr)
        return 2

    if shutil.which("mpv") is None:
        print(
            "mpv not found. Please install mpv first: sudo apt update && sudo apt install -y mpv",
            file=sys.stderr,
        )
        return 3

    try:
        if shutil.which("amixer") is not None:
            subprocess.run(
                ["amixer", "set", "Master", args.volume],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )

        screen = choose_screen(screens, args.screen)
        geometry = f"{screen['width']}x{screen['height']}+{screen['x']}+{screen['y']}"
        cmd = [
            "mpv",
            "--no-resume-playback",
            "--force-window=immediate",
            "--force-window-position",
            f"--screen-name={screen['name']}",
            f"--fs-screen-name={screen['name']}",
            f"--geometry={geometry}",
            f"--title=RecordLab video on {screen['name']}",
            (
                "--osd-playing-msg="
                f"RecordLab target: {screen['name']} "
                f"{screen['width']}x{screen['height']}+{screen['x']}+{screen['y']}"
            ),
        ]
        if args.mode == "fullscreen":
            cmd.append("--fs")
        else:
            cmd.extend([
                "--no-border",
                "--ontop",
                "--ontop-level=system",
            ])
        if os.environ.get("XDG_SESSION_TYPE") == "wayland" and os.environ.get("DISPLAY"):
            cmd.append("--gpu-context=x11egl")
        if not args.no_loop:
            cmd.append("--loop-file=inf")
        if args.duration and args.duration > 0:
            cmd.append(f"--length={args.duration}")
        cmd.append(str(video_path))
        print(f"Target screen: {screen['name']}")
        print(f"Target geometry: {screen['width']}x{screen['height']}+{screen['x']}+{screen['y']}")
        print(f"Playing video: {video_path}")
        print(f"Display mode: {args.mode}")
        print(f"mpv geometry: {geometry}")
        print(f"mpv command: {' '.join(cmd)}")

        if args.wait:
            result = subprocess.run(cmd)
            return result.returncode

        subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("Video playback started on secondary screen")
        return 0
    except subprocess.CalledProcessError as exc:
        print(exc.output or str(exc), file=sys.stderr)
        return 4
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
