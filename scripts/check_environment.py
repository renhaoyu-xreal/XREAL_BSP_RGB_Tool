#!/usr/bin/env python3
"""RecordLabC 环境诊断脚本。"""

from __future__ import annotations

import argparse
import importlib
import json
import os
import py_compile
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional


@dataclass
class CheckItem:
    category: str
    name: str
    required: bool
    available: bool
    detail: str
    path: str = ""
    version: str = ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="RecordLabC 环境诊断")
    parser.add_argument(
        "--project-root",
        help="RecordLabC 工程根目录；默认自动推断",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="输出 JSON",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="若缺少 required 项则返回非零退出码",
    )
    return parser.parse_args()


def resolve_project_root(raw_root: Optional[str]) -> Path:
    candidates: List[Path] = []

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


def path_exists(path: Path) -> bool:
    return path.exists()


def run_command(command: List[str], timeout: int = 10) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )


def probe_command(
    name: str,
    required: bool,
    version_args: Optional[List[str]] = None,
) -> CheckItem:
    executable = shutil.which(name)
    if not executable:
        return CheckItem(
            category="system_command",
            name=name,
            required=required,
            available=False,
            detail=f"未找到命令 {name}",
        )

    version = ""
    detail = f"已找到 {name}"
    if version_args:
        try:
            result = run_command([executable, *version_args], timeout=5)
            version_output = (result.stdout or result.stderr).strip().splitlines()
            if version_output:
                version = version_output[0].strip()
        except Exception as exc:  # pragma: no cover - 仅诊断兜底
            detail = f"已找到 {name}，但版本探测失败: {exc}"

    return CheckItem(
        category="system_command",
        name=name,
        required=required,
        available=True,
        detail=detail,
        path=executable,
        version=version,
    )


def probe_qmake6_qt_version() -> CheckItem:
    qmake = shutil.which("qmake6")
    if not qmake:
        return CheckItem(
            category="build_env",
            name="Qt6(qmake6)",
            required=True,
            available=False,
            detail="未找到 qmake6，无法探测 Qt6 开发环境",
        )

    result = run_command([qmake, "-query", "QT_VERSION"], timeout=5)
    if result.returncode == 0 and result.stdout.strip():
        version = result.stdout.strip()
        return CheckItem(
            category="build_env",
            name="Qt6(qmake6)",
            required=True,
            available=True,
            detail="qmake6 可解析 Qt6 版本",
            path=qmake,
            version=version,
        )

    return CheckItem(
        category="build_env",
        name="Qt6(qmake6)",
        required=True,
        available=False,
        detail=(result.stderr or result.stdout or "qmake6 未返回 Qt 版本").strip(),
        path=qmake,
    )


def probe_pkg_config_package(package_name: str, required: bool) -> CheckItem:
    pkg_config = shutil.which("pkg-config")
    if not pkg_config:
        return CheckItem(
            category="build_env",
            name=f"{package_name}(pkg-config)",
            required=required,
            available=False,
            detail="未找到 pkg-config，无法探测 pkg-config 依赖",
        )

    result = run_command([pkg_config, "--modversion", package_name], timeout=5)
    if result.returncode == 0:
        version = result.stdout.strip()
        return CheckItem(
            category="build_env",
            name=f"{package_name}(pkg-config)",
            required=required,
            available=True,
            detail=f"pkg-config 可解析 {package_name}",
            version=version,
        )

    return CheckItem(
        category="build_env",
        name=f"{package_name}(pkg-config)",
        required=required,
        available=False,
        detail=(result.stderr or result.stdout or f"pkg-config 未返回 {package_name} 版本").strip(),
    )


def probe_opengl_dev_environment() -> CheckItem:
    required_headers = [
        Path("/usr/include/GL/gl.h"),
        Path("/usr/lib/x86_64-linux-gnu/libOpenGL.so"),
    ]
    missing_paths = [str(path) for path in required_headers if not path.exists()]
    if not missing_paths:
        return CheckItem(
            category="build_env",
            name="OpenGL(dev)",
            required=True,
            available=True,
            detail="Qt6Gui/Qt6Widgets 所需的 OpenGL 开发头和库可见",
        )

    return CheckItem(
        category="build_env",
        name="OpenGL(dev)",
        required=True,
        available=False,
        detail=(
            "缺少 Qt6 构建所需的 OpenGL 开发文件。"
            " Ubuntu 22.04 上请安装: libgl-dev libopengl-dev libegl1-mesa-dev libglx-dev"
        ),
        path=", ".join(missing_paths),
    )


def probe_file(name: str, path: Path, required: bool) -> CheckItem:
    exists = path_exists(path)
    return CheckItem(
        category="project_asset",
        name=name,
        required=required,
        available=exists,
        detail="存在" if exists else "缺失",
        path=str(path),
    )


def probe_module(
    module_name: str,
    package_name: str,
    required: bool,
) -> CheckItem:
    try:
        module = importlib.import_module(module_name)
        version = getattr(module, "__version__", "")
        if not version:
            version = getattr(module, "VERSION", "")
        detail = f"可导入 {module_name}"
        if version:
            detail += f" ({version})"
        return CheckItem(
            category="python_module",
            name=package_name,
            required=required,
            available=True,
            detail=detail,
            version=str(version),
        )
    except Exception as exc:
        return CheckItem(
            category="python_module",
            name=package_name,
            required=required,
            available=False,
            detail=f"导入 {module_name} 失败: {exc}",
        )


def probe_message_system(project_root: Path) -> CheckItem:
    vendored_python = project_root / "third_party/echo_message_system/python"
    original_path = list(sys.path)
    try:
        sys.path.insert(0, str(vendored_python))
        module = importlib.import_module("message_system")
        version = getattr(module, "__version__", "")
        detail = "vendored message_system 可导入"
        return CheckItem(
            category="python_module",
            name="message_system(vendored)",
            required=True,
            available=True,
            detail=detail,
            path=str(vendored_python),
            version=str(version),
        )
    except Exception as exc:
        return CheckItem(
            category="python_module",
            name="message_system(vendored)",
            required=True,
            available=False,
            detail=f"导入 vendored message_system 失败: {exc}",
            path=str(vendored_python),
        )
    finally:
        sys.path[:] = original_path


def syntax_check(paths: List[Path]) -> CheckItem:
    failed: List[str] = []
    for path in paths:
        try:
            py_compile.compile(str(path), doraise=True)
        except Exception as exc:
            failed.append(f"{path}: {exc}")

    if failed:
        return CheckItem(
            category="python_runtime",
            name="python_syntax",
            required=True,
            available=False,
            detail="; ".join(failed),
        )

    return CheckItem(
        category="python_runtime",
        name="python_syntax",
        required=True,
        available=True,
        detail=f"已通过 {len(paths)} 个脚本的语法检查",
    )


def probe_xreal_runtime(project_root: Path) -> tuple[CheckItem, Dict[str, Any]]:
    runtime_script = project_root / "scripts/bootstrap_xreal_runtime.py"
    if not runtime_script.is_file():
        return (
            CheckItem(
                category="project_runtime",
                name="xreal_runtime",
                required=True,
                available=False,
                detail=f"缺少 runtime bootstrap 脚本: {runtime_script}",
                path=str(runtime_script),
            ),
            {},
        )

    result = run_command(
        [
            sys.executable,
            str(runtime_script),
            "--project-root",
            str(project_root),
            "--check",
            "--json",
        ],
        timeout=20,
    )
    if result.returncode not in (0, 2):
        return (
            CheckItem(
                category="project_runtime",
                name="xreal_runtime",
                required=True,
                available=False,
                detail=(result.stderr or result.stdout or "runtime 检查失败").strip(),
                path=str(project_root / "runtime/xreal_runtime"),
            ),
            {},
        )

    try:
        status = json.loads(result.stdout or "{}")
    except json.JSONDecodeError as exc:
        return (
            CheckItem(
                category="project_runtime",
                name="xreal_runtime",
                required=True,
                available=False,
                detail=f"runtime JSON 解析失败: {exc}",
                path=str(project_root / "runtime/xreal_runtime"),
            ),
            {},
        )

    blockers = status.get("blockers") or []
    detail = (
        f"项目内 XREAL runtime 已就绪，Qt={status.get('qt_version') or 'unknown'}"
        if status.get("ready")
        else " | ".join(blockers[:2]) or "项目内 XREAL runtime 未就绪"
    )
    return (
        CheckItem(
            category="project_runtime",
            name="xreal_runtime",
            required=True,
            available=bool(status.get("ready")),
            detail=detail,
            path=str(status.get("paths", {}).get("runtime_root") or project_root / "runtime/xreal_runtime"),
            version=str(status.get("qt_version") or ""),
        ),
        status,
    )


def build_report(project_root: Path) -> Dict[str, Any]:
    items: List[CheckItem] = []
    xreal_runtime_item, xreal_runtime_status = probe_xreal_runtime(project_root)

    items.extend(
        [
            probe_file("config", project_root / "config/agents_config.json", True),
            probe_file("guide", project_root / "docs/RecordLabC录数据使用指南.md", True),
            probe_file(
                "runtime_script",
                project_root / "scripts/runtime/run_recordlab_script.py",
                True,
            ),
            probe_file(
                "recording_worker",
                project_root / "scripts/runtime/run_recording_worker.py",
                True,
            ),
            probe_file(
                "xreal_wheel",
                project_root / "third_party/xreal_glasses/xreal_glasses-0.4.3-py3-none-any.whl",
                True,
            ),
            probe_file(
                "xreal_pyi",
                project_root / "third_party/xreal_glasses/XrGlasses.pyi",
                True,
            ),
            probe_file(
                "vendored_echo_python",
                project_root / "third_party/echo_message_system/python",
                True,
            ),
            xreal_runtime_item,
        ]
    )

    items.extend(
        [
            probe_module("zmq", "pyzmq", True),
            probe_module("paramiko", "paramiko", False),
            probe_module("numpy", "numpy", False),
            probe_module("PIL", "Pillow", True),
            probe_message_system(project_root),
            probe_module("PySide6", "PySide6", False),
            probe_module("shiboken6", "shiboken6", False),
        ]
    )

    items.extend(
        [
            probe_command("cmake", True, ["--version"]),
            probe_command("ninja", True, ["--version"]),
            probe_command("ssh", True, ["-V"]),
            probe_command("scp", True),
            probe_command("sshpass", True, ["-V"]),
            probe_command("ping", True, ["-V"]),
            probe_command("arecord", True, ["--version"]),
            probe_command("mpv", True, ["--version"]),
            probe_command("adb", False, ["version"]),
        ]
    )
    items.extend(
        [
            probe_qmake6_qt_version(),
            probe_opengl_dev_environment(),
            probe_pkg_config_package("libzmq", True),
            probe_pkg_config_package("gl", True),
            probe_pkg_config_package("opengl", True),
        ]
    )

    syntax_paths = [
        project_root / "scripts/runtime/run_recordlab_script.py",
        project_root / "scripts/runtime/run_recording_worker.py",
        project_root / "scripts/record_bsp_imu.py",
        project_root / "scripts/record_bsp_imu_cam.py",
        project_root / "scripts/record_bsp_imu_static.py",
        project_root / "scripts/record_bsp_imu_dynamic.py",
    ]
    items.append(syntax_check(syntax_paths))

    required_missing = [item.name for item in items if item.required and not item.available]
    optional_missing = [item.name for item in items if not item.required and not item.available]

    qt_dev_version = ""
    for item in items:
        if item.name == "Qt6(qmake6)" and item.available:
            qt_dev_version = item.version
            break

    notes = [
        "当前默认 BSP 录制路径已经使用 C++ CameraSnapshotWorker；PySide6/shiboken6 缺失不会阻塞默认 C++ 相机快照链路。",
        "setup_xreal_runtime.sh 会优先选择系统 python3.10+，避免被当前 conda 环境里的 python3 版本绑死。",
        "adb 只作为少数文件拉取场景的可选回退，不属于当前默认 BSP 录制主链路硬前提。",
        "如果 Qt6(qmake6) 不可见，通常表示 Qt6 开发包未正确安装，CMake 构建会失败。",
        "如果 OpenGL(dev)、gl(pkg-config) 或 opengl(pkg-config) 缺失，Qt6Gui/Qt6Widgets 常会在 CMake 阶段报 WrapOpenGL 错误。",
    ]

    summary = (
        f"required_missing={len(required_missing)} | "
        f"optional_missing={len(optional_missing)} | "
        f"qt6core_dev={qt_dev_version or 'unavailable'} | "
        f"xreal_runtime={xreal_runtime_status.get('qt_version') or 'missing'}"
    )

    return {
        "project_root": str(project_root),
        "python": {
            "executable": sys.executable,
            "version": sys.version,
        },
        "xreal_runtime": xreal_runtime_status,
        "ready": not required_missing,
        "summary": summary,
        "qt6core_dev_version": qt_dev_version,
        "required_missing": required_missing,
        "optional_missing": optional_missing,
        "checks": [asdict(item) for item in items],
        "notes": notes,
    }


def print_human_readable(report: Dict[str, Any]) -> None:
    print("=" * 60)
    print("RecordLabC 环境诊断")
    print("=" * 60)
    print(f"project_root: {report['project_root']}")
    print(f"python:      {report['python']['executable']}")
    print(f"summary:     {report['summary']}")

    grouped: Dict[str, List[Dict[str, Any]]] = {}
    for item in report["checks"]:
        grouped.setdefault(item["category"], []).append(item)

    ordered_categories = [
        "project_asset",
        "project_runtime",
        "python_module",
        "system_command",
        "build_env",
        "python_runtime",
    ]

    for category in ordered_categories:
        items = grouped.get(category)
        if not items:
            continue
        print(f"\n[{category}]")
        for item in items:
            marker = "OK" if item["available"] else "MISS"
            suffix = " required" if item["required"] else " optional"
            print(f"  - [{marker}] {item['name']}{suffix}: {item['detail']}")
            if item.get("path"):
                print(f"      path: {item['path']}")
            if item.get("version"):
                print(f"      version: {item['version']}")

    if report["required_missing"]:
        print("\nrequired_missing:")
        for name in report["required_missing"]:
            print(f"  - {name}")

    if report["optional_missing"]:
        print("\noptional_missing:")
        for name in report["optional_missing"]:
            print(f"  - {name}")

    if report["notes"]:
        print("\nnotes:")
        for note in report["notes"]:
            print(f"  - {note}")


def main() -> int:
    args = parse_args()
    project_root = resolve_project_root(args.project_root)
    report = build_report(project_root)

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print_human_readable(report)

    if args.strict and not report["ready"]:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
