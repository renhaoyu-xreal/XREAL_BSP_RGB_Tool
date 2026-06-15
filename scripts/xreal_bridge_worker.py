#!/usr/bin/env python3

"""
XREAL Python bridge worker.

这里承担的是 XREAL Qt 回调到 C++ 主进程之间的桥接工作。

当前这层维持一个“事件最新值合并 + 普通响应队列”的发送模型：
- 响应帧直接入普通队列；
- 高频 event 帧允许只保留最新值，降低 stdout 管道积压；
- writer 线程从普通队列和最新事件缓存里择机取帧输出。
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import queue
import shutil
import struct
import subprocess
import sys
import threading
import time
import traceback
from pathlib import Path
from typing import Any


MAGIC = b"RLCB"
FRAME_HEADER_SIZE = 12
BRIDGE_OUT_QUEUE_SIZE = 2048
CAMERA_EVENT_INTERVAL_NS = 33_333_333
IMU_BATCH_MAX_ITEMS = 256
IMU_BATCH_MAX_DELAY_NS = 1_000_000
WRITER_IDLE_TIMEOUT_S = 0.001


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="RecordLabC XREAL bridge worker")
    parser.add_argument("--project-root", required=True, help="RecordLabC project root")
    return parser.parse_args()


def configure_python_path(project_root: Path) -> Path:
    runtime_root = Path(
        os.environ.get("RECORDLABC_XREAL_RUNTIME_ROOT", "").strip()
        or project_root / "runtime" / "xreal_runtime"
    )
    site_packages = Path(
        os.environ.get("RECORDLABC_XREAL_SITE_PACKAGES", "").strip()
        or runtime_root / "site-packages"
    )

    sys.path.insert(0, str(project_root))
    sys.path.insert(0, str(site_packages))
    return site_packages


def runtime_root_for(project_root: Path) -> Path:
    return Path(
        os.environ.get("RECORDLABC_XREAL_RUNTIME_ROOT", "").strip()
        or project_root / "runtime" / "xreal_runtime"
    )


def libva_has_va_mapbuffer2() -> bool:
    try:
        libva = ctypes.CDLL("libva.so.2")
        getattr(libva, "vaMapBuffer2")
        return True
    except Exception:
        return False


def choose_c_compiler() -> str:
    candidates = [
        os.environ.get("CC", "").strip(),
        shutil.which("cc") or "",
        shutil.which("gcc") or "",
        shutil.which("clang") or "",
    ]
    seen: set[str] = set()
    for candidate in candidates:
        if not candidate or candidate in seen:
            continue
        seen.add(candidate)
        return candidate
    return ""


def ensure_va_compat_shim(project_root: Path) -> Path:
    source_path = project_root / "scripts" / "common" / "xreal_va_compat.c"
    if not source_path.is_file():
        raise FileNotFoundError(f"缺少 VA 兼容 shim 源文件: {source_path}")

    output_path = runtime_root_for(project_root) / "compat" / "librecordlabc_va_compat.so"
    output_path.parent.mkdir(parents=True, exist_ok=True)

    if output_path.is_file() and output_path.stat().st_mtime >= source_path.stat().st_mtime:
        return output_path

    compiler = choose_c_compiler()
    if not compiler:
        raise RuntimeError("未找到可用的 C 编译器（cc/gcc/clang），无法构建 libva 兼容 shim")

    temp_output = output_path.with_suffix(output_path.suffix + ".tmp")
    command = [
        compiler,
        "-shared",
        "-fPIC",
        "-O2",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-o",
        str(temp_output),
        str(source_path),
        "-ldl",
    ]
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0 or not temp_output.is_file():
        details = (result.stderr or result.stdout or "未知错误").strip()
        raise RuntimeError(f"构建 libva 兼容 shim 失败: {details}")

    temp_output.replace(output_path)
    return output_path


def prepend_env_path(existing: str, entry: str) -> str:
    parts = [part for part in existing.split(":") if part]
    if entry not in parts:
        parts.insert(0, entry)
    return ":".join(parts)


def maybe_enable_va_compat(project_root: Path) -> None:
    if sys.platform != "linux":
        return
    if os.environ.get("RECORDLABC_DISABLE_VA_COMPAT", "").strip() == "1":
        return
    if os.environ.get("RECORDLABC_VA_COMPAT_ACTIVE", "").strip() == "1":
        return
    if libva_has_va_mapbuffer2():
        return

    shim_path = ensure_va_compat_shim(project_root)
    python_executable = (sys.executable or "").strip()
    if not python_executable or os.path.sep not in python_executable:
        python_executable = shutil.which(python_executable or "python3") or "/usr/bin/python3"
    env = os.environ.copy()
    env["LD_PRELOAD"] = prepend_env_path(env.get("LD_PRELOAD", ""), str(shim_path))
    env["RECORDLABC_VA_COMPAT_ACTIVE"] = "1"

    print(
        "[xreal_bridge_worker] system libva lacks vaMapBuffer2, "
        f"re-exec with compat shim: {shim_path}",
        file=sys.stderr,
    )
    os.execve(python_executable, [python_executable, *sys.argv], env)


def read_exact(stream: Any, size: int) -> bytes | None:
    buffer = bytearray()
    while len(buffer) < size:
        chunk = stream.read(size - len(buffer))
        if not chunk:
            return None
        buffer.extend(chunk)
    return bytes(buffer)


def encode_frame(header: dict[str, Any], payload: bytes = b"") -> bytes:
    header_bytes = json.dumps(header, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return MAGIC + struct.pack("<II", len(header_bytes), len(payload)) + header_bytes + payload


def signal_bool(obj: Any, name: str) -> bool:
    value = getattr(obj, name, False)
    if callable(value):
        return bool(value())
    return bool(value)


def signal_int(obj: Any, name: str, default: int = 0) -> int:
    value = getattr(obj, name, default)
    if callable(value):
        value = value()
    try:
        return int(value)
    except Exception:
        return default


def signal_float(obj: Any, name: str, default: float = 0.0) -> float:
    value = getattr(obj, name, default)
    if callable(value):
        value = value()
    try:
        return float(value)
    except Exception:
        return default


def vector3(value: Any) -> list[float]:
    try:
        return [float(value[0]), float(value[1]), float(value[2])]
    except Exception:
        return [0.0, 0.0, 0.0]


def enum_int(value: Any, default: int = 0) -> int:
    enum_value = getattr(value, "value", value)
    try:
        return int(enum_value)
    except Exception:
        return default


def unwrap_imu_data(value: Any) -> Any:
    data_func = getattr(value, "data", None)
    if callable(data_func):
        try:
            return data_func()
        except Exception:
            return value
    return value


def qimage_bytes(image: Any) -> bytes:
    size = int(image.sizeInBytes())
    bits = image.constBits()
    if hasattr(bits, "tobytes"):
        raw = bits.tobytes()
    else:
        if hasattr(bits, "setsize"):
            bits.setsize(size)
        raw = bytes(bits)
    return raw[:size]


def main() -> int:
    args = parse_args()
    project_root = Path(args.project_root).resolve()
    maybe_enable_va_compat(project_root)
    configure_python_path(project_root)

    from PySide6.QtCore import QObject, QBuffer, QByteArray, QCoreApplication, QIODevice, Qt, QTimer, Signal
    from PySide6.QtGui import QImage
    from xrglasses import XrGlasses as Xr

    grayscale_format = QImage.Format.Format_Grayscale8
    rgb_format = QImage.Format.Format_RGB888

    class BridgeWorker(QObject):
        command_received = Signal(object)

        def __init__(self) -> None:
            super().__init__()
            self.factory = None
            self.glasses = None
            self.out_queue: queue.Queue[bytes | None] = queue.Queue(maxsize=BRIDGE_OUT_QUEUE_SIZE)
            self._dropped_frame_count = 0
            self._coalesced_frame_count = 0
            self._last_camera_event_ns = 0
            self._latest_event_frames: dict[str, bytes] = {}
            self._latest_event_lock = threading.Lock()
            self._pending_imu_payloads: list[dict[str, Any]] = []
            self._pending_imu_lock = threading.Lock()
            self._last_imu_batch_flush_ns = time.monotonic_ns()
            self._writer_shutdown = False
            self.writer_thread = threading.Thread(target=self._writer_loop, daemon=True)
            self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
            self.writer_thread.start()
            self.reader_thread.start()
            self.command_received.connect(self._handle_command, Qt.QueuedConnection)

        def _writer_loop(self) -> None:
            stdout_fd = sys.stdout.fileno()
            while True:
                frame = None
                queued_shutdown = False

                # 控制响应必须优先于实时数据流。start_sensors 一返回后
                # IMU 会立刻高频回调；如果先写 IMU/camera，response 可能被
                # 长时间压在队列后面，C++ 侧就会误判 start_device 超时。
                try:
                    queued_frame = self.out_queue.get_nowait()
                    if queued_frame is None:
                        queued_shutdown = True
                    else:
                        frame = queued_frame
                except queue.Empty:
                    pass

                if not queued_shutdown:
                    with self._latest_event_lock:
                        # 相机预览必须优先于普通 IMU 发送，
                        # 否则高频 IMU 会长期把 camera 最新帧饿死，UI 就一直显示“等待图像流”。
                        if frame is not None:
                            pass
                        elif "camera" in self._latest_event_frames:
                            frame = self._latest_event_frames.pop("camera")
                        elif self._latest_event_frames:
                            _, frame = self._latest_event_frames.popitem()

                if frame is None and not queued_shutdown:
                    imu_batch = self._take_imu_batch()
                    if imu_batch:
                        frame = encode_frame(
                            {"type": "event", "event": "imu_batch", "payload": {"items": imu_batch}}
                        )

                if frame is None and not queued_shutdown:
                    try:
                        frame = self.out_queue.get(timeout=WRITER_IDLE_TIMEOUT_S)
                    except queue.Empty:
                        pass

                if frame is None:
                    if self._writer_shutdown:
                        with self._latest_event_lock:
                            if not self._latest_event_frames and self.out_queue.empty():
                                break
                    continue
                try:
                    view = memoryview(frame)
                    while view:
                        written = os.write(stdout_fd, view)
                        view = view[written:]
                except Exception:
                    break

        def _take_imu_batch(self) -> list[dict[str, Any]]:
            now_ns = time.monotonic_ns()
            with self._pending_imu_lock:
                pending_count = len(self._pending_imu_payloads)
                if pending_count == 0:
                    self._last_imu_batch_flush_ns = now_ns
                    return []
                if (
                    pending_count < IMU_BATCH_MAX_ITEMS
                    and now_ns - self._last_imu_batch_flush_ns < IMU_BATCH_MAX_DELAY_NS
                ):
                    return []

                take_count = min(pending_count, IMU_BATCH_MAX_ITEMS)
                batch = self._pending_imu_payloads[:take_count]
                del self._pending_imu_payloads[:take_count]
                self._last_imu_batch_flush_ns = now_ns
                return batch

        def _reader_loop(self) -> None:
            stream = sys.stdin.buffer
            try:
                while True:
                    prefix = read_exact(stream, FRAME_HEADER_SIZE)
                    if prefix is None:
                        break
                    if prefix[:4] != MAGIC:
                        continue
                    header_size, payload_size = struct.unpack("<II", prefix[4:12])
                    header_bytes = read_exact(stream, header_size)
                    payload_bytes = read_exact(stream, payload_size)
                    if header_bytes is None or payload_bytes is None:
                        break
                    header = json.loads(header_bytes.decode("utf-8"))
                    header["_binary_payload"] = payload_bytes
                    self.command_received.emit(header)
            except Exception:
                traceback.print_exc(file=sys.stderr)
            self.command_received.emit({"type": "request", "id": "__shutdown__", "action": "shutdown", "payload": {}})

        def _enqueue_frame(self, header: dict[str, Any], payload: bytes = b"", drop_ok: bool = False) -> None:
            frame = encode_frame(header, payload)
            try:
                if drop_ok:
                    event_key = ""
                    if header.get("type") == "event":
                        event_key = str(header.get("event", "")).strip()
                    if not event_key:
                        self.out_queue.put_nowait(frame)
                        return

                    with self._latest_event_lock:
                        replaced = event_key in self._latest_event_frames
                        self._latest_event_frames[event_key] = frame
                    if replaced:
                        self._coalesced_frame_count += 1
                        if self._coalesced_frame_count == 1 or self._coalesced_frame_count % 200 == 0:
                            print(
                                f"coalesced stale {event_key} frame(s): {self._coalesced_frame_count}",
                                file=sys.stderr,
                            )
                else:
                    self.out_queue.put(frame)
            except queue.Full:
                if drop_ok:
                    self._dropped_frame_count += 1
                    if self._dropped_frame_count == 1 or self._dropped_frame_count % 100 == 0:
                        print(
                            f"queue Full! (drop_ok={drop_ok}, dropped={self._dropped_frame_count})",
                            file=sys.stderr,
                        )
                else:
                    print(f"queue Full! (drop_ok={drop_ok})", file=sys.stderr)

        def _send_response(self, request_id: str, result: dict[str, Any]) -> None:
            self._enqueue_frame({"type": "response", "id": request_id, "result": result})

        def _send_event(self, event_name: str, payload: dict[str, Any], binary_payload: bytes = b"",
                        drop_ok: bool = False) -> None:
            self._enqueue_frame(
                {"type": "event", "event": event_name, "payload": payload},
                payload=binary_payload,
                drop_ok=drop_ok,
            )

        def _close_glasses_internal(self) -> None:
            if not self.glasses:
                return
            try:
                self.glasses.imuUpdated.disconnect(self._on_imu_updated)
            except Exception:
                pass
            try:
                self.glasses.camUpdated.disconnect(self._on_cam_updated)
            except Exception:
                pass
            try:
                active = self.glasses.activeSensors()
                if active:
                    self.glasses.stopSensors(active)
            except Exception:
                pass
            try:
                self.glasses.close()
            except Exception:
                pass
            try:
                self.glasses.deleteLater()
            except Exception:
                pass
            self.glasses = None

        def _sensor_set_from_mask(self, sensor_mask: int) -> set[Any]:
            sensors: set[Any] = set()
            if sensor_mask & 0x01:
                sensors.add(Xr.SensorType.Imu)
            if sensor_mask & 0x02:
                sensors.add(Xr.SensorType.Slam)
            if sensor_mask & 0x04:
                sensors.add(Xr.SensorType.Rgb)
            if sensor_mask & 0x08:
                sensors.add(Xr.SensorType.Display)
            return sensors

        def _active_sensor_names(self) -> list[str]:
            if not self.glasses:
                return []
            try:
                return sorted(sensor.name for sensor in self.glasses.activeSensors())
            except Exception:
                return []

        def _handle_command(self, request: dict[str, Any]) -> None:
            request_id = str(request.get("id", ""))
            action = str(request.get("action", ""))
            payload = request.get("payload", {}) or {}

            try:
                if action == "shutdown":
                    self._close_glasses_internal()
                    self._send_response(request_id, {"success": True, "message": ""})
                    self._writer_shutdown = True
                    self.out_queue.put(None)
                    QTimer.singleShot(0, QCoreApplication.quit)
                    return

                if action == "enumerate_devices":
                    try:
                        factory = Xr.GlassesFactory.instance()
                        product_ids = [int(pid) for pid in factory.enumerateDevices()]
                    except Exception as enum_exc:
                        self._send_response(request_id, {
                            "success": False,
                            "message": f"enumerate failed: {enum_exc}",
                            "product_ids": [],
                            "device_count": 0,
                        })
                        return
                    self._send_response(request_id, {
                        "success": True,
                        "message": "",
                        "product_ids": product_ids,
                        "device_count": len(product_ids),
                    })
                    return

                if action == "create_glasses":
                    self._close_glasses_internal()
                    self.factory = Xr.GlassesFactory.instance()
                    product_ids = list(self.factory.enumerateDevices())
                    if not product_ids:
                        self._send_response(request_id, {"success": False, "message": "No glasses found"})
                        return
                    product_id = int(product_ids[0])
                    self.glasses = self.factory.createGlasses(product_id)
                    # IMU is a 1000Hz-class stream. Routing it through the
                    # Python/Qt queued event loop throttles delivery heavily
                    # on some machines, so handle it in the emitter thread and
                    # immediately hand the compact event to the bridge writer.
                    self.glasses.imuUpdated.connect(self._on_imu_updated, Qt.DirectConnection)
                    # ImagePair/BspImage objects are owned by the SDK callback.
                    # Some RGB backends hand out frames whose Python wrapper can
                    # become empty by the time a queued Qt slot runs, so consume
                    # the image synchronously and only enqueue our compact bridge
                    # frame afterward.
                    self.glasses.camUpdated.connect(self._on_cam_updated, Qt.DirectConnection)
                    self._send_response(
                        request_id,
                        {
                            "success": True,
                            "message": "",
                            "product_id": product_id,
                            "device_ids": [int(item) for item in product_ids],
                        },
                    )
                    return

                if action == "open_glasses":
                    if not self.glasses:
                        self._send_response(request_id, {"success": False, "message": "Glasses not created"})
                        return
                    opened = bool(self.glasses.open())
                    self._send_response(
                        request_id,
                        {
                            "success": opened,
                            "message": "" if opened else "Failed to open glasses",
                            "opened": bool(self.glasses.isOpened()),
                        },
                    )
                    return

                if action == "start_sensors":
                    if not self.glasses:
                        self._send_response(request_id, {"success": False, "message": "Glasses not created"})
                        return
                    sensor_mask = int(payload.get("sensor_mask", 0))
                    sensors = self._sensor_set_from_mask(sensor_mask)
                    if not sensors:
                        self._send_response(request_id, {"success": False, "message": "No sensors selected"})
                        return
                    active_before = set()
                    try:
                        active_before = set(self.glasses.activeSensors())
                    except Exception:
                        active_before = set()
                    sensors_to_start = sensors - active_before
                    started = True
                    if sensors_to_start:
                        started = bool(self.glasses.startSensors(sensors_to_start))
                    if started and Xr.SensorType.Slam in sensors_to_start:
                        try:
                            self.glasses.setFrameRate(Xr.SensorType.Slam, 30.0)
                        except Exception:
                            pass
                    self._send_response(
                        request_id,
                        {
                            "success": started,
                            "message": "" if started else "Failed to start sensors",
                            "active_sensors": self._active_sensor_names(),
                        },
                    )
                    return

                if action == "stop_sensors":
                    if not self.glasses:
                        self._send_response(request_id, {"success": False, "message": "Glasses not created"})
                        return
                    sensor_mask = int(payload.get("sensor_mask", 0))
                    sensors = self._sensor_set_from_mask(sensor_mask)
                    if not sensors:
                        self._send_response(request_id, {"success": False, "message": "No sensors selected"})
                        return
                    stopped = bool(self.glasses.stopSensors(sensors))
                    self._send_response(
                        request_id,
                        {
                            "success": stopped,
                            "message": "" if stopped else "Failed to stop sensors",
                            "active_sensors": self._active_sensor_names(),
                        },
                    )
                    return

                if action == "configure_glasses":
                    if not self.glasses:
                        self._send_response(request_id, {"success": False, "message": "Glasses not created"})
                        return

                    def _to_bool(value: Any) -> bool:
                        if isinstance(value, bool):
                            return value
                        if isinstance(value, str):
                            return value.strip().lower() in {"1", "true", "yes", "on"}
                        return bool(value)

                    if "slam_fps" in payload:
                        self.glasses.setFrameRate(Xr.SensorType.Slam, float(payload["slam_fps"]))
                    if "exposure" in payload:
                        self.glasses.setExposure(Xr.SensorType.Slam, int(float(payload["exposure"])))
                    if "auto_exposure" in payload:
                        self.glasses.setAutoExposure(Xr.SensorType.Slam, _to_bool(payload["auto_exposure"]))
                    if "rgb_fps" in payload:
                        self.glasses.setFrameRate(Xr.SensorType.Rgb, float(payload["rgb_fps"]))
                    if "rgb_exposure" in payload:
                        self.glasses.setExposure(Xr.SensorType.Rgb, int(float(payload["rgb_exposure"])))
                    if "rgb_auto_exposure" in payload:
                        self.glasses.setAutoExposure(Xr.SensorType.Rgb, _to_bool(payload["rgb_auto_exposure"]))
                    if "rgb_gain" in payload:
                        self.glasses.setGain(Xr.SensorType.Rgb, float(payload["rgb_gain"]))
                    if "enable_display" in payload:
                        if _to_bool(payload["enable_display"]):
                            display_started = bool(self.glasses.startSensors({Xr.SensorType.Display}))
                            if not display_started:
                                self._send_response(
                                    request_id,
                                    {
                                        "success": False,
                                        "message": "Failed to start display",
                                        "active_sensors": self._active_sensor_names(),
                                    },
                                )
                                return
                        else:
                            active_sensors = set()
                            try:
                                active_sensors = set(self.glasses.activeSensors())
                            except Exception:
                                active_sensors = set()
                            if Xr.SensorType.Display in active_sensors:
                                display_stopped = bool(self.glasses.stopSensors({Xr.SensorType.Display}))
                                if not display_stopped:
                                    self._send_response(
                                        request_id,
                                        {
                                            "success": False,
                                            "message": "Failed to stop display",
                                            "active_sensors": self._active_sensor_names(),
                                        },
                                    )
                                    return
                    self._send_response(request_id, {"success": True, "message": "", "active_sensors": self._active_sensor_names()})
                    return

                if action == "get_glasses_state":
                    if not self.glasses:
                        self._send_response(request_id, {"success": False, "message": "Glasses not created"})
                        return
                    has_rgb_sensor = False
                    rgb_cam_sn = ""
                    try:
                        has_rgb_sensor = bool(self.glasses.hasSensor(Xr.SensorType.Rgb))
                    except Exception:
                        pass
                    if has_rgb_sensor:
                        try:
                            rgb_cam_sn = str(self.glasses.rgbCamSn())
                        except Exception:
                            rgb_cam_sn = ""
                    self._send_response(
                        request_id,
                        {
                            "success": True,
                            "message": "",
                            "is_opened": bool(self.glasses.isOpened()),
                            "active_sensors": self._active_sensor_names(),
                            "fsn": str(self.glasses.fsn()),
                            "mcu_firmware_version": str(self.glasses.mcuFirmwareVersion()),
                            "has_rgb_sensor": has_rgb_sensor,
                            "rgb_cam_sn": rgb_cam_sn,
                        },
                    )
                    return

                if action == "close_glasses":
                    self._close_glasses_internal()
                    self._send_response(request_id, {"success": True, "message": ""})
                    return

                self._send_response(request_id, {"success": False, "message": f"Unknown action: {action}"})
            except Exception as exc:
                traceback.print_exc(file=sys.stderr)
                self._send_response(request_id, {"success": False, "message": str(exc)})

        def _on_imu_updated(self, imu_data: Any) -> None:
            imu_data = unwrap_imu_data(imu_data)
            payload = {
                "imu_idx": signal_int(imu_data, "imu_idx", 0),
                "hmd_time_ns": signal_int(imu_data, "hmd_time_ns", 0),
                "hmd_hw_time_ns": signal_int(imu_data, "hmd_hw_time_ns", 0),
                "hmd_sensor_time_ns": signal_int(imu_data, "hmd_sensor_time_ns", 0),
                "hasGyro": signal_bool(imu_data, "hasGyro"),
                "hasAcc": signal_bool(imu_data, "hasAcc"),
                "hasMag": signal_bool(imu_data, "hasMag"),
                "gyro": vector3(getattr(imu_data, "gyro", [])),
                "acc": vector3(getattr(imu_data, "acc", [])),
                "mag": vector3(getattr(imu_data, "mag", [])),
                "temperature": signal_float(imu_data, "temperature", 0.0),
            }
            # IMU 频率必须与原版一致。单条 SDK 回调一帧 stdout 在 C++ bridge
            # 中会把总吞吐压到约 1000 frame/s；批量发送后 C++ 再展开为旧格式。
            with self._pending_imu_lock:
                self._pending_imu_payloads.append(payload)

        def _on_cam_updated(self, sensor_type: Any, image_pair: Any) -> None:
            try:
                sensor_value = enum_int(sensor_type, -1)
                slam_value = enum_int(Xr.SensorType.Slam, -2)
                rgb_value = enum_int(Xr.SensorType.Rgb, -3)
                if sensor_value not in (slam_value, rgb_value):
                    return
                if image_pair is None:
                    self._log_camera_debug_once("null ImagePair from SDK")
                    return
                now_ns = time.monotonic_ns()
                if now_ns - self._last_camera_event_ns < CAMERA_EVENT_INTERVAL_NS:
                    return
                self._last_camera_event_ns = now_ns

                payload: dict[str, Any] = {
                    "timestamp": int(image_pair.timestamp()),
                    "cams": [],
                }
                raw_parts: list[bytes] = []
                offset = 0
                is_rgb = sensor_value == rgb_value
                if is_rgb:
                    # Python 版 BSP 节点会在 0/1 两路里选择第一路非空 RGB
                    # BspImage。部分设备/SDK 组合的 RGB 帧并不固定出现在
                    # index 0；只读 index 0 会导致 C++ 端永远收不到 camera
                    # event，进而无法创建共享内存预览。
                    camera_indices = [0, 1]
                else:
                    camera_indices = [0, 1]

                for source_index in camera_indices:
                    bsp_img = self._bsp_image_at(image_pair, source_index)
                    source_image = None
                    if bsp_img is not None:
                        source_image = getattr(bsp_img, "image", None)
                    if source_image is None or source_image.isNull():
                        source_image = self._plain_image_at(image_pair, source_index)
                    if source_image is None or source_image.isNull():
                        continue
                    image = source_image.convertToFormat(rgb_format if is_rgb else grayscale_format)
                    encoded = QByteArray()
                    buffer = QBuffer(encoded)
                    if not buffer.open(QIODevice.OpenModeFlag.WriteOnly):
                        continue
                    if not image.save(buffer, "JPEG", 72):
                        continue
                    raw = bytes(encoded)
                    if not raw:
                        continue
                    output_index = 0 if is_rgb else source_index
                    payload["cams"].append(
                        {
                            "index": output_index,
                            "source_image_idx": source_index,
                            "width": int(image.width()),
                            "height": int(image.height()),
                            "bytes_per_line": int(image.bytesPerLine()),
                            "qt_format": enum_int(image.format(), enum_int(grayscale_format)),
                            "source_format": enum_int(bsp_img.image.format(), 0),
                            "format": "rgb888" if is_rgb else "grayscale8",
                            "encoded_format": "JPEG",
                            "data_offset": offset,
                            "data_size": len(raw),
                            "exposure_start_time_device": self._bsp_attr_int(bsp_img, "exposure_start_time_device"),
                            "exposure_start_time_system": self._bsp_attr_int(bsp_img, "exposure_start_time_system"),
                            "exposure_duration": self._bsp_attr_int(bsp_img, "exposure_duration"),
                            "rolling_shutter_time": self._bsp_attr_int(bsp_img, "rolling_shutter_time"),
                            "stride": self._bsp_attr_int(bsp_img, "stride", int(image.bytesPerLine())),
                            "gain": self._bsp_attr_int(bsp_img, "gain"),
                            "temperature": signal_float(bsp_img, "temperature", 0.0) if bsp_img is not None else 0.0,
                        }
                    )
                    raw_parts.append(raw)
                    offset += len(raw)
                    if is_rgb:
                        break

                if payload["cams"]:
                    self._send_event("camera", payload, b"".join(raw_parts), drop_ok=True)
                else:
                    self._log_camera_debug_once(
                        f"no usable {'RGB' if is_rgb else 'SLAM'} images in ImagePair; "
                        f"pair_null={self._safe_image_pair_is_null(image_pair)}"
                    )
            except Exception:
                traceback.print_exc(file=sys.stderr)

        def _bsp_image_at(self, image_pair: Any, index: int) -> Any:
            try:
                image = image_pair.bspImageAt(index)
                if image is not None and not image.isNull():
                    return image
            except Exception:
                pass
            return None

        def _plain_image_at(self, image_pair: Any, index: int) -> Any:
            try:
                image = image_pair.imageAt(index)
                if image is not None and not image.isNull():
                    return image
            except Exception:
                pass
            if index == 0:
                try:
                    image = image_pair.left()
                    if image is not None and not image.isNull():
                        return image
                except Exception:
                    pass
            if index == 1:
                try:
                    image = image_pair.right()
                    if image is not None and not image.isNull():
                        return image
                except Exception:
                    pass
            return None

        def _bsp_attr_int(self, bsp_img: Any, name: str, default: int = 0) -> int:
            if bsp_img is None:
                return default
            try:
                return int(getattr(bsp_img, name, default))
            except Exception:
                return default

        def _safe_image_pair_is_null(self, image_pair: Any) -> str:
            try:
                return str(bool(image_pair.isNull()))
            except Exception as exc:
                return f"unknown:{exc}"

        def _log_camera_debug_once(self, message: str) -> None:
            if getattr(self, "_camera_debug_logged", False):
                return
            self._camera_debug_logged = True
            print(f"[xreal_bridge_worker] camera callback produced no frame: {message}", file=sys.stderr)

    app = QCoreApplication(sys.argv)
    keepalive = QTimer()
    keepalive.timeout.connect(lambda: None)
    keepalive.start(500)
    worker = BridgeWorker()
    setattr(app, "_bridge_worker", worker)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
