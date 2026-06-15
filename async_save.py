# 用于局部验证的测试文件，模拟了SDK FSN获取流程和NVIZ 3DOF流程，并捕获它们的网络行为。日志以JSONL格式保存，便于后续分析。

import argparse
import json
import socket
import subprocess
import sys
import threading
import time
from contextlib import closing
from datetime import datetime
from importlib.metadata import PackageNotFoundError, version
from pathlib import Path
from typing import Any

from PySide6.QtCore import QCoreApplication
from xrglasses import XrGlasses as Xr


PROJECT_ROOT = Path(__file__).resolve().parent
SHELL_DIR = PROJECT_ROOT / "subnodes" / "nviz_node" / "shell"
OUTPUT_ROOT = PROJECT_ROOT / "output" / "nviz_fsn_probe"


class JsonlLogger:
    def __init__(self, output_dir: Path) -> None:
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.path = self.output_dir / "probe.jsonl"
        self._lock = threading.Lock()
        self._started = time.perf_counter()

    def event(self, name: str, **fields: Any) -> None:
        payload = {
            "ts": datetime.now().isoformat(timespec="milliseconds"),
            "elapsed_s": round(time.perf_counter() - self._started, 3),
            "event": name,
            **fields,
        }
        line = json.dumps(payload, ensure_ascii=False, default=str)
        with self._lock:
            with self.path.open("a", encoding="utf-8") as handle:
                handle.write(line + "\n")
        print(line, flush=True)


def package_version(name: str) -> str:
    try:
        return version(name)
    except PackageNotFoundError:
        return "not-installed"


def stringify(value: Any) -> str:
    try:
        return str(value)
    except Exception as exc:
        return f"<str failed: {exc}>"


def enum_name(value: Any) -> str:
    return getattr(value, "name", stringify(value))


def run_timed(logger: JsonlLogger, name: str, func) -> dict[str, Any]:
    started = time.perf_counter()
    logger.event(f"{name}.start")
    try:
        result = func()
        duration = round(time.perf_counter() - started, 3)
        logger.event(f"{name}.ok", duration_s=duration, result=result)
        return {
            "completed": True,
            "success": bool(result.get("success")) if isinstance(result, dict) else True,
            "duration_s": duration,
            "result": result,
        }
    except Exception as exc:
        duration = round(time.perf_counter() - started, 3)
        logger.event(
            f"{name}.error",
            duration_s=duration,
            error_type=type(exc).__name__,
            error=str(exc),
        )
        return {
            "completed": False,
            "success": False,
            "duration_s": duration,
            "error_type": type(exc).__name__,
            "error": str(exc),
        }


def run_script(logger: JsonlLogger, script_name: str, timeout_s: float) -> dict[str, Any]:
    script_path = SHELL_DIR / script_name
    started = time.perf_counter()
    logger.event("script.start", script=script_name, path=str(script_path))
    if not script_path.exists():
        result = {
            "success": False,
            "returncode": None,
            "stdout": "",
            "stderr": f"missing script: {script_path}",
            "duration_s": 0.0,
        }
        logger.event("script.done", script=script_name, **result)
        return result

    try:
        proc = subprocess.run(
            ["bash", str(script_path)],
            cwd=str(PROJECT_ROOT),
            text=True,
            capture_output=True,
            timeout=timeout_s,
            check=False,
        )
        result = {
            "success": proc.returncode == 0,
            "returncode": proc.returncode,
            "stdout": proc.stdout[-4000:],
            "stderr": proc.stderr[-4000:],
            "duration_s": round(time.perf_counter() - started, 3),
        }
    except subprocess.TimeoutExpired as exc:
        result = {
            "success": False,
            "returncode": None,
            "stdout": (exc.stdout or "")[-4000:],
            "stderr": (exc.stderr or "")[-4000:],
            "duration_s": round(time.perf_counter() - started, 3),
            "timeout": True,
        }

    logger.event("script.done", script=script_name, **result)
    return result


def ping_host(host: str, timeout_s: float = 2.0) -> bool:
    proc = subprocess.run(
        ["ping", "-c", "1", "-W", str(max(1, int(timeout_s))), host],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return proc.returncode == 0


def wait_for_connectivity(
    logger: JsonlLogger,
    hosts: list[str],
    *,
    want_reachable: bool,
    timeout_s: float,
    interval_s: float = 0.5,
) -> dict[str, Any]:
    started = time.perf_counter()
    state_name = "reachable" if want_reachable else "unreachable"
    logger.event("connectivity.wait.start", hosts=hosts, want=state_name)
    last_status: dict[str, bool] = {}

    while time.perf_counter() - started < timeout_s:
        last_status = {host: ping_host(host) for host in hosts}
        any_reachable = any(last_status.values())
        matched = any_reachable if want_reachable else not any_reachable
        if matched:
            result = {
                "success": True,
                "duration_s": round(time.perf_counter() - started, 3),
                "status": last_status,
            }
            logger.event("connectivity.wait.ok", want=state_name, **result)
            return result
        time.sleep(interval_s)

    result = {
        "success": False,
        "duration_s": round(time.perf_counter() - started, 3),
        "status": last_status,
    }
    logger.event("connectivity.wait.timeout", want=state_name, **result)
    return result


class NvizCapture:
    def __init__(
        self,
        logger: JsonlLogger,
        listen_host: str,
        udp_port: int,
        tcp_port: int,
    ) -> None:
        self.logger = logger
        self.listen_host = listen_host
        self.udp_port = udp_port
        self.tcp_port = tcp_port
        self.stop_event = threading.Event()
        self.threads: list[threading.Thread] = []
        self.lock = threading.Lock()
        self.udp_packets = 0
        self.udp_bytes = 0
        self.tcp_connections = 0
        self.tcp_packets = 0
        self.tcp_bytes = 0
        self.errors: list[str] = []

    def start(self) -> None:
        self.threads = [
            threading.Thread(target=self._udp_loop, name="nviz-udp", daemon=True),
            threading.Thread(target=self._tcp_loop, name="nviz-tcp", daemon=True),
        ]
        for thread in self.threads:
            thread.start()
        self.logger.event(
            "capture.start",
            listen_host=self.listen_host,
            udp_port=self.udp_port,
            tcp_port=self.tcp_port,
        )

    def stop(self) -> dict[str, Any]:
        self.stop_event.set()
        self._poke_udp()
        self._poke_tcp()
        for thread in self.threads:
            thread.join(timeout=2.0)
        summary = self.summary()
        self.logger.event("capture.stop", **summary)
        return summary

    def summary(self) -> dict[str, Any]:
        with self.lock:
            return {
                "udp_packets": self.udp_packets,
                "udp_bytes": self.udp_bytes,
                "tcp_connections": self.tcp_connections,
                "tcp_packets": self.tcp_packets,
                "tcp_bytes": self.tcp_bytes,
                "errors": list(self.errors),
            }

    def _record_error(self, where: str, exc: Exception) -> None:
        message = f"{where}: {type(exc).__name__}: {exc}"
        with self.lock:
            self.errors.append(message)
        self.logger.event("capture.error", where=where, error=message)

    def _udp_loop(self) -> None:
        try:
            with closing(socket.socket(socket.AF_INET, socket.SOCK_DGRAM)) as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock.bind((self.listen_host, self.udp_port))
                sock.settimeout(0.5)
                self.logger.event("capture.udp.bound", address=self.listen_host, port=self.udp_port)
                while not self.stop_event.is_set():
                    try:
                        data, peer = sock.recvfrom(65535)
                    except socket.timeout:
                        continue
                    if self.stop_event.is_set():
                        break
                    with self.lock:
                        self.udp_packets += 1
                        self.udp_bytes += len(data)
                        count = self.udp_packets
                    if count <= 5 or count % 100 == 0:
                        self.logger.event(
                            "capture.udp.packet",
                            count=count,
                            bytes=len(data),
                            peer=f"{peer[0]}:{peer[1]}",
                            prefix_hex=data[:24].hex(),
                        )
        except Exception as exc:
            self._record_error("udp", exc)

    def _tcp_loop(self) -> None:
        try:
            with closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind((self.listen_host, self.tcp_port))
                server.listen(8)
                server.settimeout(0.5)
                self.logger.event("capture.tcp.bound", address=self.listen_host, port=self.tcp_port)
                while not self.stop_event.is_set():
                    try:
                        client, peer = server.accept()
                    except socket.timeout:
                        continue
                    if self.stop_event.is_set():
                        client.close()
                        break
                    with self.lock:
                        self.tcp_connections += 1
                        connection_index = self.tcp_connections
                    self.logger.event(
                        "capture.tcp.connection",
                        count=connection_index,
                        peer=f"{peer[0]}:{peer[1]}",
                    )
                    thread = threading.Thread(
                        target=self._tcp_client_loop,
                        args=(client, peer),
                        name=f"nviz-tcp-client-{connection_index}",
                        daemon=True,
                    )
                    thread.start()
                    self.threads.append(thread)
        except Exception as exc:
            self._record_error("tcp", exc)

    def _tcp_client_loop(self, client: socket.socket, peer: tuple[str, int]) -> None:
        try:
            with closing(client):
                client.settimeout(0.5)
                while not self.stop_event.is_set():
                    try:
                        data = client.recv(65535)
                    except socket.timeout:
                        continue
                    if not data:
                        return
                    with self.lock:
                        self.tcp_packets += 1
                        self.tcp_bytes += len(data)
                        count = self.tcp_packets
                    if count <= 5 or count % 100 == 0:
                        self.logger.event(
                            "capture.tcp.packet",
                            count=count,
                            bytes=len(data),
                            peer=f"{peer[0]}:{peer[1]}",
                            prefix_hex=data[:24].hex(),
                        )
        except Exception as exc:
            self._record_error("tcp_client", exc)

    def _poke_udp(self) -> None:
        try:
            with closing(socket.socket(socket.AF_INET, socket.SOCK_DGRAM)) as sock:
                sock.sendto(b"", ("127.0.0.1", self.udp_port))
        except Exception:
            pass

    def _poke_tcp(self) -> None:
        try:
            with closing(socket.create_connection(("127.0.0.1", self.tcp_port), timeout=0.2)):
                pass
        except Exception:
            pass


def sdk_fsn_probe(logger: JsonlLogger) -> dict[str, Any]:
    app = QCoreApplication.instance() or QCoreApplication(sys.argv)
    result: dict[str, Any] = {
        "xreal_glasses_version": package_version("xreal_glasses"),
        "product_ids": [],
        "product_id": None,
        "opened": False,
        "type": "",
        "firmware_version": "",
        "fsn": "",
        "active_sensors_before_close": [],
        "close_result": None,
    }
    glasses = None

    try:
        factory = Xr.GlassesFactory.instance()
        product_ids = [int(pid) for pid in factory.enumerateDevices()]
        result["product_ids"] = product_ids
        logger.event("sdk.enumerate", product_ids=product_ids, device_count=len(product_ids))
        if not product_ids:
            result["success"] = False
            result["message"] = "No glasses found"
            return result

        product_id = product_ids[0]
        result["product_id"] = product_id
        glasses = factory.createGlasses(product_id)
        logger.event("sdk.create", product_id=product_id, glasses=stringify(glasses))

        opened = bool(glasses.open())
        result["opened"] = opened
        logger.event("sdk.open", opened=opened)
        if not opened:
            result["success"] = False
            result["message"] = "open() returned false"
            return result

        result["type"] = enum_name(glasses.type())
        result["firmware_version"] = stringify(glasses.mcuFirmwareVersion())
        result["fsn"] = stringify(glasses.fsn())
        logger.event(
            "sdk.identity",
            type=result["type"],
            firmware_version=result["firmware_version"],
            fsn=result["fsn"],
        )
        result["success"] = True
        result["message"] = ""
        return result
    finally:
        if glasses is not None:
            try:
                active = set(glasses.activeSensors())
                result["active_sensors_before_close"] = [enum_name(sensor) for sensor in active]
                if active:
                    logger.event("sdk.stop_sensors.start", active=result["active_sensors_before_close"])
                    stop_result = bool(glasses.stopSensors(active))
                    logger.event("sdk.stop_sensors.done", success=stop_result)
            except Exception as exc:
                logger.event("sdk.stop_sensors.error", error=str(exc))
            try:
                close_result = bool(glasses.close())
                result["close_result"] = close_result
                logger.event("sdk.close", success=close_result)
            except Exception as exc:
                logger.event("sdk.close.error", error=str(exc))
            try:
                glasses.deleteLater()
                for _ in range(5):
                    app.processEvents()
                    time.sleep(0.05)
                logger.event("sdk.delete_later.done")
            except Exception as exc:
                logger.event("sdk.delete_later.error", error=str(exc))


def nviz_probe(logger: JsonlLogger, args: argparse.Namespace) -> dict[str, Any]:
    result: dict[str, Any] = {}
    hosts = args.glasses_hosts

    result["initial_ping"] = {host: ping_host(host) for host in hosts}
    logger.event("nviz.initial_ping", status=result["initial_ping"])

    close_result = run_script(logger, "close_pilot_gf.sh", args.script_timeout_s)
    result["close_pilot_gf"] = close_result

    result["disconnect_wait"] = wait_for_connectivity(
        logger,
        hosts,
        want_reachable=False,
        timeout_s=args.disconnect_timeout_s,
    )
    result["ready_wait"] = wait_for_connectivity(
        logger,
        hosts,
        want_reachable=True,
        timeout_s=args.ready_timeout_s,
    )

    capture = NvizCapture(logger, args.listen_host, args.udp_port, args.tcp_port)
    capture.start()
    start_result: dict[str, Any] | None = None
    end_result: dict[str, Any] | None = None
    try:
        time.sleep(args.pre_start_listen_s)
        start_result = run_script(logger, "gf_3dof_start.sh", args.script_timeout_s)
        result["gf_3dof_start"] = start_result

        listen_started = time.perf_counter()
        while time.perf_counter() - listen_started < args.capture_seconds:
            time.sleep(1.0)
            summary = capture.summary()
            logger.event(
                "capture.progress",
                remaining_s=max(0, round(args.capture_seconds - (time.perf_counter() - listen_started), 1)),
                **summary,
            )
    finally:
        end_result = run_script(logger, "gf_3dof_end.sh", args.script_timeout_s)
        result["gf_3dof_end"] = end_result
        result["capture"] = capture.stop()

    capture_summary = result["capture"]
    result["success"] = (
        bool(start_result and start_result.get("success"))
        and (capture_summary["udp_packets"] > 0 or capture_summary["tcp_packets"] > 0)
    )
    result["message"] = (
        "NVIZ data received"
        if result["success"]
        else "No NVIZ UDP/TCP data received or start script failed"
    )
    logger.event("nviz.result", **result)
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Probe whether XREAL SDK FSN acquisition interferes with the NVIZ 3dof flow."
    )
    parser.add_argument(
        "--mode",
        choices=("nviz-only", "sdk-fsn-only", "full"),
        default="full",
        help="Probe mode. full runs SDK FSN first, releases it, then runs NVIZ.",
    )
    parser.add_argument("--capture-seconds", type=float, default=15.0)
    parser.add_argument("--script-timeout-s", type=float, default=90.0)
    parser.add_argument("--disconnect-timeout-s", type=float, default=20.0)
    parser.add_argument("--ready-timeout-s", type=float, default=30.0)
    parser.add_argument("--pre-start-listen-s", type=float, default=0.5)
    parser.add_argument("--listen-host", default="0.0.0.0")
    parser.add_argument("--udp-port", type=int, default=7099)
    parser.add_argument("--tcp-port", type=int, default=8099)
    parser.add_argument(
        "--glasses-hosts",
        nargs="+",
        default=["169.254.1.1", "169.254.2.1"],
        help="Hosts used only for ping readiness around the original NVIZ shell flow.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Defaults to output/nviz_fsn_probe/<timestamp>.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = args.output_dir or (OUTPUT_ROOT / timestamp)
    logger = JsonlLogger(output_dir)
    logger.event(
        "probe.start",
        mode=args.mode,
        output_dir=str(output_dir),
        python=sys.version,
        xreal_glasses_version=package_version("xreal_glasses"),
        note="SSH FSN acquisition is intentionally excluded from this probe.",
    )

    final: dict[str, Any] = {"mode": args.mode, "output_dir": str(output_dir)}
    try:
        if args.mode in {"sdk-fsn-only", "full"}:
            final["sdk_fsn"] = run_timed(logger, "sdk_fsn_probe", lambda: sdk_fsn_probe(logger))
            if args.mode == "full":
                logger.event("probe.release_pause.start", seconds=2.0)
                time.sleep(2.0)
                logger.event("probe.release_pause.done")

        if args.mode in {"nviz-only", "full"}:
            final["nviz"] = run_timed(logger, "nviz_probe", lambda: nviz_probe(logger, args))

        logger.event("probe.done", final=final)
    except KeyboardInterrupt:
        logger.event("probe.interrupted")
        return 130

    print(f"\nprobe log: {logger.path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
