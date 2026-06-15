#!/usr/bin/env python3
"""为 RecordLabC 准备项目内独立的 XREAL Qt 6.8 runtime。"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Any, Dict, Optional


DEFAULT_PINNED_PYSIDE6_VERSION = "6.8.3"
DEFAULT_REQUIRES_PYTHON = ">=3.10"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="准备项目内 XREAL runtime")
    parser.add_argument("--project-root", help="RecordLabC 工程根目录")
    parser.add_argument("--check", action="store_true", help="只检查当前 runtime 是否就绪")
    parser.add_argument("--force", action="store_true", help="删除旧 runtime 后重装")
    parser.add_argument("--json", action="store_true", help="输出 JSON")
    return parser.parse_args()


def resolve_project_root(raw_root: Optional[str]) -> Path:
    candidates = []
    if raw_root:
        candidates.append(Path(raw_root).expanduser())

    env_root = os.environ.get("RECORDLABC_ROOT", "").strip()
    if env_root:
        candidates.append(Path(env_root).expanduser())

    candidates.append(Path(__file__).resolve().parents[1])

    for candidate in candidates:
        root = candidate.resolve()
        if (root / "config/agents_config.json").is_file():
            return root

    return candidates[0].resolve()


def runtime_paths(project_root: Path) -> Dict[str, Path]:
    runtime_root = project_root / "runtime" / "xreal_runtime"
    site_packages = runtime_root / "site-packages"
    return {
        "runtime_root": runtime_root,
        "site_packages": site_packages,
        "manifest": runtime_root / "manifest.json",
        "qt_lib_dir": site_packages / "PySide6" / "Qt" / "lib",
        "qt_plugins_dir": site_packages / "PySide6" / "Qt" / "plugins",
        "shiboken_dir": site_packages / "shiboken6",
        "xreal_package_dir": site_packages / "xrglasses",
        "native_lib_dir": site_packages / "xrglasses" / "lib",
        "glasses_server": site_packages / "xrglasses" / "bin" / "glasses_server",
        "wheel_path": project_root / "third_party" / "xreal_glasses" / "xreal_glasses-0.4.3-py3-none-any.whl",
    }


def parse_version_tuple(version: str) -> tuple[int, ...]:
    numbers = re.findall(r"\d+", version or "")
    return tuple(int(part) for part in numbers[:3])


def version_at_least(version: str, minimum: str) -> bool:
    parsed = parse_version_tuple(version)
    required = parse_version_tuple(minimum)
    if not parsed or not required:
        return False
    while len(parsed) < len(required):
        parsed += (0,)
    while len(required) < len(parsed):
        required += (0,)
    return parsed >= required


def read_wheel_metadata(wheel_path: Path) -> Dict[str, str]:
    metadata = {
        "requires_python": DEFAULT_REQUIRES_PYTHON,
        "pyside6_version": DEFAULT_PINNED_PYSIDE6_VERSION,
    }
    if not wheel_path.is_file():
        return metadata

    with zipfile.ZipFile(wheel_path) as wheel_archive:
        metadata_entry = next(
            (name for name in wheel_archive.namelist() if name.endswith(".dist-info/METADATA")),
            "",
        )
        if not metadata_entry:
            return metadata

        content = wheel_archive.read(metadata_entry).decode("utf-8", "replace")
        for line in content.splitlines():
            if line.startswith("Requires-Python:"):
                metadata["requires_python"] = line.split(":", 1)[1].strip() or DEFAULT_REQUIRES_PYTHON
                continue
            if line.lower().startswith("requires-dist: pyside6=="):
                metadata["pyside6_version"] = line.split("==", 1)[1].strip() or DEFAULT_PINNED_PYSIDE6_VERSION
                continue
    return metadata


def extract_python_floor(spec: str) -> str:
    match = re.search(r">=\s*([0-9]+(?:\.[0-9]+)+)", spec or "")
    return match.group(1) if match else "3.10"


def python_version(executable: str) -> str:
    result = subprocess.run(
        [executable, "-c", "import sys; print('.'.join(map(str, sys.version_info[:3])))"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError((result.stderr or result.stdout or "无法探测 Python 版本").strip())
    return result.stdout.strip()


def choose_bootstrap_python(required_python_spec: str) -> Dict[str, str]:
    python_floor = extract_python_floor(required_python_spec)
    candidates = []

    env_python = os.environ.get("RECORDLABC_XREAL_PYTHON", "").strip()
    if env_python:
        candidates.append(env_python)

    candidates.extend(
        [
            "/usr/bin/python3.10",
            "/usr/bin/python3",
            sys.executable,
            shutil.which("python3.10") or "",
            shutil.which("python3") or "",
        ]
    )

    seen = set()
    for candidate in candidates:
        candidate = candidate.strip()
        if not candidate or candidate in seen:
            continue
        seen.add(candidate)
        if not Path(candidate).exists() and os.path.sep in candidate:
            continue
        try:
            detected_version = python_version(candidate)
        except Exception:
            continue
        if version_at_least(detected_version, python_floor):
            return {
                "executable": candidate,
                "version": detected_version,
            }

    raise RuntimeError(
        f"未找到满足 XREAL wheel 要求的 Python 解释器。需要 {required_python_spec}，"
        "请确认系统已安装 python3.10+，或设置 RECORDLABC_XREAL_PYTHON。"
    )


def has_shiboken_runtime(shiboken_dir: Path) -> bool:
    return any(shiboken_dir.glob("libshiboken6.abi3.so*"))


def build_status(project_root: Path) -> Dict[str, Any]:
    paths = runtime_paths(project_root)
    wheel_metadata = read_wheel_metadata(paths["wheel_path"])
    manifest_data: Dict[str, Any] = {}
    if paths["manifest"].is_file():
        manifest_data = json.loads(paths["manifest"].read_text(encoding="utf-8"))

    qt_version = str(manifest_data.get("qt_version") or manifest_data.get("pyside6_version") or "")
    required_qt_version = str(
        wheel_metadata.get("pyside6_version")
        or manifest_data.get("required_pyside6_version")
        or DEFAULT_PINNED_PYSIDE6_VERSION
    )
    ready = (
        paths["runtime_root"].is_dir()
        and paths["site_packages"].is_dir()
        and paths["manifest"].is_file()
        and (paths["qt_lib_dir"] / "libQt6Core.so.6").is_file()
        and paths["qt_plugins_dir"].is_dir()
        and has_shiboken_runtime(paths["shiboken_dir"])
        and (paths["native_lib_dir"] / "libxreal_glasses.so").is_file()
        and paths["glasses_server"].is_file()
        and version_at_least(qt_version, required_qt_version)
    )

    blockers = []
    if not paths["runtime_root"].is_dir():
        blockers.append(f"缺少 runtime 根目录: {paths['runtime_root']}")
    if not paths["site_packages"].is_dir():
        blockers.append(f"缺少 site-packages: {paths['site_packages']}")
    if not paths["manifest"].is_file():
        blockers.append(f"缺少 manifest.json: {paths['manifest']}")
    if not (paths["qt_lib_dir"] / "libQt6Core.so.6").is_file():
        blockers.append(f"缺少 Qt6Core 运行库: {paths['qt_lib_dir']}")
    if not paths["qt_plugins_dir"].is_dir():
        blockers.append(f"缺少 Qt 插件目录: {paths['qt_plugins_dir']}")
    if not has_shiboken_runtime(paths["shiboken_dir"]):
        blockers.append(f"缺少 shiboken6 运行库: {paths['shiboken_dir']}")
    if not (paths["native_lib_dir"] / "libxreal_glasses.so").is_file():
        blockers.append(f"缺少 libxreal_glasses.so: {paths['native_lib_dir']}")
    if not paths["glasses_server"].is_file():
        blockers.append(f"缺少 glasses_server: {paths['glasses_server']}")
    if qt_version and not version_at_least(qt_version, required_qt_version):
        blockers.append(f"Qt 版本过低: {qt_version} < {required_qt_version}")
    if not qt_version:
        blockers.append("manifest.json 中缺少 Qt 版本")

    return {
        "project_root": str(project_root),
        "ready": ready,
        "qt_version": qt_version,
        "required_qt_version": required_qt_version,
        "required_python": wheel_metadata.get("requires_python", DEFAULT_REQUIRES_PYTHON),
        "paths": {key: str(value) for key, value in paths.items()},
        "blockers": blockers,
        "manifest": manifest_data,
        "wheel_metadata": wheel_metadata,
    }


def run_pip_install(target: Path,
                    wheel_path: Path,
                    python_executable: str,
                    pyside6_version: str) -> None:
    pyside_command = [
        python_executable,
        "-m",
        "pip",
        "install",
        "--upgrade",
        "--target",
        str(target),
        f"PySide6=={pyside6_version}",
        f"shiboken6=={pyside6_version}",
    ]
    xreal_command = [
        python_executable,
        "-m",
        "pip",
        "install",
        "--upgrade",
        "--no-deps",
        "--target",
        str(target),
        str(wheel_path),
    ]
    subprocess.run([python_executable, "-m", "pip", "--version"], check=True)
    subprocess.run(pyside_command, check=True)
    subprocess.run(xreal_command, check=True)


def write_manifest(paths: Dict[str, Path],
                   python_info: Dict[str, str],
                   wheel_metadata: Dict[str, str]) -> Dict[str, Any]:
    command = [
        python_info["executable"],
        "-c",
        (
            "import json, sys; "
            f"sys.path.insert(0, {str(paths['site_packages'])!r}); "
            "import PySide6, shiboken6; "
            "print(json.dumps({"
            "'qt_version': getattr(PySide6, '__version__', ''), "
            "'pyside6_version': getattr(PySide6, '__version__', ''), "
            "'shiboken6_version': getattr(shiboken6, '__version__', '')"
            "}, ensure_ascii=False))"
        ),
    ]
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=True,
    )
    detected_versions = json.loads(result.stdout.strip())

    manifest = {
        **detected_versions,
        "required_pyside6_version": wheel_metadata.get("pyside6_version", DEFAULT_PINNED_PYSIDE6_VERSION),
        "requires_python": wheel_metadata.get("requires_python", DEFAULT_REQUIRES_PYTHON),
        "bootstrap_python_executable": python_info["executable"],
        "bootstrap_python_version": python_info["version"],
        "site_packages": str(paths["site_packages"]),
        "qt_lib_dir": str(paths["qt_lib_dir"]),
        "qt_plugins_dir": str(paths["qt_plugins_dir"]),
        "shiboken_dir": str(paths["shiboken_dir"]),
        "xreal_package_dir": str(paths["xreal_package_dir"]),
        "native_lib_dir": str(paths["native_lib_dir"]),
        "glasses_server": str(paths["glasses_server"]),
    }
    paths["manifest"].parent.mkdir(parents=True, exist_ok=True)
    paths["manifest"].write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return manifest


def ensure_runtime(project_root: Path, force: bool) -> Dict[str, Any]:
    paths = runtime_paths(project_root)
    wheel_metadata = read_wheel_metadata(paths["wheel_path"])
    python_info = choose_bootstrap_python(wheel_metadata.get("requires_python", DEFAULT_REQUIRES_PYTHON))
    if force and paths["runtime_root"].exists():
        shutil.rmtree(paths["runtime_root"])

    paths["site_packages"].mkdir(parents=True, exist_ok=True)
    if not paths["wheel_path"].is_file():
        raise FileNotFoundError(f"缺少 vendored wheel: {paths['wheel_path']}")

    run_pip_install(
        paths["site_packages"],
        paths["wheel_path"],
        python_info["executable"],
        wheel_metadata.get("pyside6_version", DEFAULT_PINNED_PYSIDE6_VERSION),
    )
    manifest = write_manifest(paths, python_info, wheel_metadata)
    status = build_status(project_root)
    status["manifest"] = manifest
    return status


def print_human(status: Dict[str, Any]) -> None:
    print("=" * 60)
    print("RecordLabC XREAL Runtime")
    print("=" * 60)
    print(f"project_root:        {status['project_root']}")
    print(f"required_python:     {status.get('required_python') or 'unknown'}")
    print(f"required_qt_version: {status['required_qt_version']}")
    print(f"qt_version:          {status.get('qt_version') or 'unknown'}")
    print(f"ready:               {status['ready']}")
    print("paths:")
    for key, value in status["paths"].items():
        print(f"  - {key}: {value}")
    if status["blockers"]:
        print("blockers:")
        for blocker in status["blockers"]:
            print(f"  - {blocker}")


def main() -> int:
    args = parse_args()
    project_root = resolve_project_root(args.project_root)

    if args.check:
        status = build_status(project_root)
    else:
        status = ensure_runtime(project_root, force=args.force)

    if args.json:
        print(json.dumps(status, ensure_ascii=False, indent=2))
    else:
        print_human(status)

    return 0 if status["ready"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
