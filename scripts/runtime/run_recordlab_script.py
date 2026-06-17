#!/usr/bin/env python3
"""RecordLabC compatibility runtime for Python orchestration scripts."""

from __future__ import annotations

import argparse
import ast
import builtins
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid
from pathlib import Path
from typing import Any


EVENT_PREFIX = "__RECORDLAB_EVENT__ "
DEFAULT_AGENT_TIMEOUT_S = 30.0
COPY_PROGRESS_INTERVAL_S = 20.0


class EventChannel:
    def emit(self, payload: dict[str, Any]) -> None:
        print(EVENT_PREFIX + json.dumps(payload, ensure_ascii=False), flush=True)

    def request(self, payload: dict[str, Any]) -> dict[str, Any]:
        request_id = payload.setdefault("id", uuid.uuid4().hex)
        self.emit(payload)
        while True:
            line = sys.stdin.readline()
            if line == "":
                return {"id": request_id, "success": False, "cancelled": True}
            try:
                response = json.loads(line)
            except json.JSONDecodeError:
                continue
            if response.get("id") == request_id:
                return response


class WorkflowQueue:
    def __init__(self, channel: EventChannel):
        self._channel = channel

    def put_nowait(self, payload: dict[str, Any]) -> None:
        self._channel.emit(payload)


class DialogAPI:
    def __init__(self, channel: EventChannel):
        self._channel = channel

    def _show(self, kind: str, title: str, message: str, **extra: Any) -> dict[str, Any]:
        return self._channel.request(
            {
                "type": "dialog",
                "kind": kind,
                "title": title,
                "message": message,
                **extra,
            }
        )

    def info(self, title: str, message: str, timeout_ms: int | None = None):
        return self._show("info", title, message, timeout_ms=timeout_ms)

    def warning(self, title: str, message: str, timeout_ms: int | None = None):
        return self._show("warning", title, message, timeout_ms=timeout_ms)

    def error(self, title: str, message: str, timeout_ms: int | None = None):
        return self._show("error", title, message, timeout_ms=timeout_ms)

    def question(self, title: str, message: str, timeout_ms: int | None = None) -> bool:
        result = self._show("question", title, message, timeout_ms=timeout_ms)
        return bool(result.get("success") and not result.get("cancelled") and result.get("response"))

    def input(self, title: str, message: str, default: str = "") -> str | None:
        result = self._show("input", title, message, default=default)
        if result.get("success") and not result.get("cancelled"):
            return result.get("response", "")
        return None

    def choice(self, title: str, message: str, items: list[str]) -> str | None:
        result = self._show("choice", title, message, items=items, multi_select=False)
        if result.get("success") and not result.get("cancelled"):
            return result.get("response")
        return None

    def multi_choice(
        self,
        title: str,
        message: str,
        items: list[str],
        default_selected: list[str] | None = None,
    ) -> list[str] | None:
        result = self._show(
            "choice",
            title,
            message,
            items=items,
            multi_select=True,
            default_selected=default_selected or [],
        )
        if result.get("success") and not result.get("cancelled"):
            return result.get("response")
        return None

    def multi_field_input(self, title: str, message: str, fields: list[dict[str, Any]]) -> dict[str, str] | None:
        result = self._show("multi_field_input", title, message, fields=fields)
        if result.get("success") and not result.get("cancelled"):
            response = result.get("response")
            return response if isinstance(response, dict) else {}
        return None


class AgentWrapper:
    def __init__(self, name: str, config: dict[str, Any], root: Path):
        self._name = name
        self._config = config
        self._root = root
        self._pending: dict[str, tuple[subprocess.Popen[str], Path]] = {}
        self._remote_action_client = None
        self._remote_results: dict[str, dict[str, Any]] = {}
        self._remote_feedback: dict[str, list[Any]] = {}
        self._remote_lock = threading.Lock()

    @property
    def name(self) -> str:
        return self._name

    def cmd(
        self,
        cmd_name: str,
        args: dict[str, Any] | str | None = None,
        wait_for_result: bool = True,
        timeout: float = DEFAULT_AGENT_TIMEOUT_S,
        **kwargs: Any,
    ) -> dict[str, Any]:
        params: dict[str, Any] = {}
        if isinstance(args, str):
            try:
                decoded = json.loads(args)
                if isinstance(decoded, dict):
                    params.update(decoded)
            except json.JSONDecodeError:
                return {"success": False, "message": "Invalid JSON string"}
        elif isinstance(args, dict):
            params.update(args)
        params.update(kwargs)

        if self._name == "localhost":
            return self._run_localhost(cmd_name, params, wait_for_result, timeout)
        return self._run_agent_cmd(cmd_name, params, wait_for_result, timeout)

    def check_cmd(self, goal_id: str | int) -> dict[str, Any]:
        goal_key = str(goal_id)
        if self._uses_remote_action_client():
            with self._remote_lock:
                result = self._remote_results.get(goal_key)
                if result is None:
                    return {"done": False, "status": None, "result": None}
                feedback = self._remote_feedback.get(goal_key, [])
            response = {
                "done": True,
                "status": result["status"],
                "result": result["result"],
            }
            if feedback:
                response["feedback"] = feedback
            return response

        pending = self._pending.get(goal_key)
        if pending is None:
            return {"done": True, "status": "FAILED", "result": {"success": False, "message": "Unknown goal_id"}}

        process, result_path = pending
        if process.poll() is None:
            result_file_size = None
            try:
                result_file_size = result_path.stat().st_size
            except FileNotFoundError:
                result_file_size = None
            return {
                "done": False,
                "status": None,
                "result": None,
                "pid": process.pid,
                "result_file": str(result_path),
                "result_file_exists": result_path.exists(),
                "result_file_size": result_file_size,
            }

        result = _read_json_file(result_path)
        returncode = process.returncode
        self._cleanup_result_file(result_path)
        self._pending.pop(goal_key, None)
        success = bool(result.get("success"))
        return {
            "done": True,
            "status": "SUCCEEDED" if success else "FAILED",
            "result": result,
            "returncode": returncode,
        }

    def get_state(self) -> dict[str, Any]:
        return self.cmd("check", {})

    def copy_folder_from_remote(
        self,
        remote_path: str,
        local_path: str,
        method: str = "scp",
        remote_host: str | None = None,
        remote_user: str | None = None,
        remote_password: str | None = None,
        adb_device: str | None = None,
        progress_callback=None,
    ) -> dict[str, Any]:
        destination = Path(local_path).expanduser()
        destination.mkdir(parents=True, exist_ok=True)
        if progress_callback:
            progress_callback(f"正在回传 UR 文件: {remote_path} -> {destination}")

        if method == "adb":
            cmd = ["adb"]
            if adb_device:
                cmd += ["-s", adb_device]
            cmd += ["pull", remote_path, str(destination)]
        else:
            host = remote_host or self._config.get("subnode_host") or "localhost"
            user = remote_user or (self._config.get("custom_params") or {}).get("remote_user") or os.environ.get("USER", "")
            password = remote_password or (self._config.get("custom_params") or {}).get("remote_password")
            remote = f"{user}@{host}:{remote_path}" if user else f"{host}:{remote_path}"
            if password and shutil.which("sshpass"):
                cmd = ["sshpass", "-p", password, "scp", "-r", "-o", "StrictHostKeyChecking=no", remote, str(destination)]
            else:
                cmd = ["scp", "-r", remote, str(destination)]

        process = subprocess.Popen(
            cmd,
            cwd=str(self._root),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        last_report_time = time.monotonic()
        while process.poll() is None:
            now = time.monotonic()
            if now - last_report_time >= COPY_PROGRESS_INTERVAL_S:
                transferred_size = _path_size(destination)
                progress_message = (
                    f"UR 文件回传中，已接收 {_format_bytes(transferred_size)}"
                )
                print(f"[runtime] {progress_message}", flush=True)
                if progress_callback:
                    progress_callback(progress_message)
                last_report_time = now
            time.sleep(1.0)

        stdout, stderr = process.communicate()
        transferred_size = _path_size(destination)
        final_message = f"UR 文件回传完成，已接收 {_format_bytes(transferred_size)}"
        if process.returncode == 0:
            if progress_callback:
                progress_callback(final_message)
            return {
                "success": True,
                "message": "Copy completed",
                "transferred_size": transferred_size,
                "transferred_size_text": _format_bytes(transferred_size),
            }
        return {"success": False, "message": stderr.strip() or stdout.strip()}

    def stop_for_shutdown(self) -> None:
        try:
            if self._name in {"glasses_bsp_node", "helen_node"}:
                self.cmd("stop_record", {}, timeout=8.0)
                self.cmd("stop_device", {}, timeout=8.0)
            if self._name == "localhost":
                self.cmd("stop_video.sh", {}, timeout=5.0)
        except Exception:
            pass

    def _run_agent_cmd(
        self,
        cmd_name: str,
        params: dict[str, Any],
        wait_for_result: bool,
        timeout: float,
    ) -> dict[str, Any]:
        tool = _agent_cmd_path(self._root)
        result_path = _make_result_path()
        effective_timeout = timeout
        if not wait_for_result:
            # Signal agent_cmd_main to wait without a result timeout.
            effective_timeout = 0.0
        command = [
            str(tool),
            "--agent",
            self._name,
            "--cmd",
            cmd_name,
            "--params-json",
            json.dumps(params, ensure_ascii=False),
            "--timeout",
            str(effective_timeout),
            "--result-file",
            str(result_path),
        ]
        if not wait_for_result:
            # Long-running nonblocking commands are polled via their result file.
            # Do not keep stdout/stderr pipes here: if the child writes enough
            # logs during a long trajectory, an unread pipe can fill and block
            # the child before it exits and publishes the result file.
            process = subprocess.Popen(
                command,
                cwd=str(self._root),
                text=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            goal_id = uuid.uuid4().hex
            self._pending[goal_id] = (process, result_path)
            return {"success": True, "message": "Command started", "goal_id": goal_id}

        completed = subprocess.run(
            command,
            cwd=str(self._root),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout + 10.0,
        )
        result = _read_json_file(result_path)
        self._cleanup_result_file(result_path)
        if result:
            return result
        return {
            "success": completed.returncode == 0,
            "message": completed.stderr.strip() or completed.stdout.strip() or f"recordlabc_agent_cmd exited {completed.returncode}",
        }

    def _uses_remote_action_client(self) -> bool:
        # Remote RecordLabC agents are served by the legacy raw-JSON action
        # protocol. Keep routing through recordlabc_agent_cmd, which already
        # uses LegacyRemoteActionClient, instead of the Python message_system
        # ActionClient protocol used by local Python agents.
        return False

    def _ensure_remote_action_client(self):
        if self._remote_action_client is not None:
            return self._remote_action_client

        _install_echo_python_path(self._root)
        from message_system import ActionClient

        host = str(self._config.get("subnode_host") or "localhost")
        goal_port = int(self._config.get("goal_port"))
        feedback_port = int(self._config.get("feedback_port"))
        client = ActionClient(
            name=f"{self._name}_script_client",
            action_name=f"{self._name}_actions",
            goal_host=host,
            goal_port=goal_port,
            feedback_host=host,
            feedback_port=feedback_port,
            timeout=5000,
        )

        def on_feedback(goal_id, feedback):
            with self._remote_lock:
                self._remote_feedback.setdefault(str(goal_id), []).append(feedback)

        def on_result(goal_id, result, status):
            status_value = getattr(status, "value", str(status))
            with self._remote_lock:
                self._remote_results[str(goal_id)] = {
                    "status": status_value,
                    "result": result,
                }

        client.set_feedback_callback(on_feedback)
        client.set_result_callback(on_result)
        client.start_listening()
        if not client.wait_for_server(timeout=10000):
            try:
                client.close()
            except Exception:
                pass
            raise RuntimeError(
                f"ActionServer not ready for {self._name} at {host}:{goal_port}/{feedback_port}"
            )
        # Match the Python RecordLab agent: allow PUB/SUB subscriptions to
        # propagate before sending goals, otherwise early result messages can
        # be silently dropped by ZeroMQ slow joiner behavior.
        time.sleep(1.0)
        self._remote_action_client = client
        return client

    def _run_remote_agent_cmd(
        self,
        cmd_name: str,
        params: dict[str, Any],
        wait_for_result: bool,
        timeout: float,
    ) -> dict[str, Any]:
        try:
            client = self._ensure_remote_action_client()
            goal = {"cmd": cmd_name, "params": params}
            goal_id = client.send_goal(goal)
            with self._remote_lock:
                self._remote_feedback.setdefault(str(goal_id), [])

            if not wait_for_result:
                return {"success": True, "message": "Command sent", "goal_id": goal_id}

            result, status = client.wait_for_result(
                goal_id,
                timeout=int(timeout * 1000) if timeout and timeout > 0 else None,
            )
            status_value = getattr(status, "value", str(status))
            success = status_value == "SUCCEEDED"
            with self._remote_lock:
                feedback = list(self._remote_feedback.get(str(goal_id), []))
            return {
                "success": success,
                "message": result if success else (result or "Command failed"),
                "result": result,
                "status": status_value,
                "goal_id": goal_id,
                "feedback": feedback,
            }
        except Exception as exc:
            return {"success": False, "message": str(exc)}

    def _run_localhost(
        self,
        cmd_name: str,
        params: dict[str, Any],
        wait_for_result: bool,
        timeout: float,
    ) -> dict[str, Any]:
        if cmd_name == "check":
            return {"success": True, "message": "Localhost is ready"}
        if cmd_name in {"stop_all", "estop"}:
            for process_name in ("mpv", "vlc", "mplayer", "ffplay"):
                subprocess.run(["pkill", process_name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return {"success": True, "message": "Stop commands sent"}

        script_name = params.get("script") or cmd_name
        script_path = Path(str(script_name)).expanduser()
        if not script_path.is_absolute():
            scripts_dir = self._config.get("scripts_dir", "scripts")
            script_path = self._root / scripts_dir / str(script_name)
        if not script_path.exists():
            return {"success": False, "message": f"Script not found: {script_name}"}

        script_args = [str(value) for value in params.get("args", [])] if isinstance(params.get("args"), list) else []
        if script_path.suffix == ".sh":
            command = ["bash", str(script_path), *script_args]
        elif script_path.suffix == ".py":
            command = [sys.executable, str(script_path), *script_args]
        else:
            command = [str(script_path), *script_args]

        if not wait_for_result:
            process = subprocess.Popen(
                command,
                cwd=str(self._root),
                text=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            goal_id = uuid.uuid4().hex
            result_path = _make_result_path()
            self._pending[goal_id] = (process, result_path)
            return {"success": True, "message": "Script started", "goal_id": goal_id}

        completed = subprocess.run(
            command,
            cwd=str(self._root),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        if completed.returncode == 0:
            message = completed.stdout.strip() or f"Script executed successfully: {script_path.name}"
            return {"success": True, "message": message, "output": completed.stdout}
        return {
            "success": False,
            "message": "Script failed",
            "output": completed.stdout,
            "error": completed.stderr,
            "returncode": completed.returncode,
        }

    @staticmethod
    def _cleanup_result_file(path: Path) -> None:
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def _make_result_path() -> Path:
    handle = tempfile.NamedTemporaryFile(prefix="recordlabc_script_", suffix=".json", delete=False)
    handle.close()
    return Path(handle.name)


def _path_size(path: Path) -> int:
    try:
        if path.is_file():
            return path.stat().st_size
        if not path.exists():
            return 0
        total = 0
        for item in path.rglob("*"):
            try:
                if item.is_file():
                    total += item.stat().st_size
            except OSError:
                continue
        return total
    except OSError:
        return 0


def _format_bytes(size: int) -> str:
    value = float(max(0, size))
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if value < 1024.0 or unit == "TB":
            if unit == "B":
                return f"{int(value)} {unit}"
            return f"{value:.1f} {unit}"
        value /= 1024.0


def _read_json_file(path: Path) -> dict[str, Any]:
    try:
        if not path.exists() or path.stat().st_size == 0:
            return {}
        payload = json.loads(path.read_text(encoding="utf-8"))
        return payload if isinstance(payload, dict) else {"success": False, "message": str(payload)}
    except Exception as exc:
        return {"success": False, "message": f"Failed to read result file: {exc}"}


def _install_echo_python_path(root: Path) -> None:
    echo_python_path = root / "third_party" / "echo_message_system" / "python"
    echo_python_text = str(echo_python_path)
    if echo_python_text not in sys.path:
        sys.path.insert(0, echo_python_text)


class RuntimeStateCache:
    """Long-lived subscribers for script-visible global state values."""

    _TOPICS = {
        "record_timer": ("record_timer", 16520),
        "time_delay": ("time_delay", 16521),
        "motion_status": ("motion_status", 16525),
    }

    def __init__(self, root: Path, host: str = "localhost"):
        self._root = root
        self._host = host
        self._lock = threading.Lock()
        self._values: dict[str, Any] = {}
        self._raw: dict[str, Any] = {}
        self._subscribers: list[Any] = []
        self._available = False

    def start(self) -> None:
        try:
            _install_echo_python_path(self._root)
            from message_system import Subscriber
        except Exception as exc:
            print(f"[runtime] State cache disabled: {exc}", flush=True)
            return

        for state_key, (topic, port) in self._TOPICS.items():
            try:
                subscriber = Subscriber(
                    name=f"recordlab_script_{state_key}",
                    topic=topic,
                    callback=lambda _topic, data, key=state_key: self._on_data(key, data),
                    host=self._host,
                    port=port,
                    encoding="json",
                )
                subscriber.start()
                self._subscribers.append(subscriber)
            except Exception as exc:
                print(f"[runtime] Failed to subscribe {topic} on {self._host}:{port}: {exc}", flush=True)
        self._available = bool(self._subscribers)
        if self._available:
            print("[runtime] State cache subscribers started: record_timer, time_delay, motion_status", flush=True)

    def close(self) -> None:
        for subscriber in self._subscribers:
            try:
                subscriber.close()
            except Exception:
                pass
        self._subscribers.clear()

    def get(self, state_key: str):
        with self._lock:
            if state_key in self._values:
                return self._values[state_key]
        return _state_value(self._root, state_key)

    def clear(self, state_key: str) -> None:
        with self._lock:
            self._values.pop(state_key, None)
            self._raw.pop(state_key, None)

    def _on_data(self, state_key: str, data: Any) -> None:
        value = self._extract_value(state_key, data)
        with self._lock:
            self._raw[state_key] = data
            self._values[state_key] = value

    @staticmethod
    def _extract_value(state_key: str, data: Any):
        if not isinstance(data, dict):
            return None
        if state_key == "record_timer":
            return data.get("duration_ns", 0.0) / 1_000_000_000.0
        if state_key == "time_delay":
            return data.get("time_delay_ns", 0.0) / 1_000_000.0
        if state_key == "motion_status":
            return data.get("status")
        return None


def _agent_cmd_path(root: Path) -> Path:
    candidates = [
        root / "build" / "recordlabc_agent_cmd",
        Path.cwd() / "build" / "recordlabc_agent_cmd",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def _state_value(root: Path, state_key: str, timeout: float = 1.0):
    tool = _agent_cmd_path(root)
    result_path = _make_result_path()
    try:
        subprocess.run(
            [
                str(tool),
                "--state-key",
                state_key,
                "--timeout",
                str(timeout),
                "--result-file",
                str(result_path),
            ],
            cwd=str(root),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=timeout + 2.0,
        )
        payload = _read_json_file(result_path)
        return payload.get("value")
    finally:
        try:
            result_path.unlink()
        except FileNotFoundError:
            pass


def _load_agent_definitions(config_path: Path, root: Path) -> dict[str, AgentWrapper]:
    config = json.loads(config_path.read_text(encoding="utf-8"))
    raw_agents = config.get("agents") or {}
    wrappers: dict[str, AgentWrapper] = {}
    for name, agent_config in raw_agents.items():
        wrapper = AgentWrapper(name, agent_config if isinstance(agent_config, dict) else {}, root)
        wrappers[name] = wrapper
    return wrappers


def _extract_required_agent_names(script_path: Path) -> list[str]:
    try:
        tree = ast.parse(script_path.read_text(encoding="utf-8"), filename=str(script_path))
    except Exception:
        return []

    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(target, ast.Name) and target.id == "all_agent_names" for target in node.targets):
            continue
        if not isinstance(node.value, (ast.List, ast.Tuple)):
            return []
        names: list[str] = []
        for item in node.value.elts:
            if isinstance(item, ast.Constant) and isinstance(item.value, str):
                names.append(item.value)
        return names
    return []


def _resolve_agent_definition(agent_defs: dict[str, AgentWrapper], requested_name: str) -> AgentWrapper | None:
    if requested_name in agent_defs:
        return agent_defs[requested_name]
    requested_lower = requested_name.lower()
    for name, wrapper in agent_defs.items():
        if name.lower() == requested_lower:
            return wrapper
    return None


def _add_agent_aliases(agents: dict[str, AgentWrapper], wrapper: AgentWrapper) -> None:
    agents[wrapper.name] = wrapper
    agents[wrapper.name.lower()] = wrapper


def _probe_params_for_agent(agent_name: str, script_path: Path) -> dict[str, Any]:
    if agent_name == "glasses_bsp_node" and script_path.name in {
        "record_ur_gt_3dof_batch_bsp.py",
        "record_bsp_id1088_ur_gt_3dof_batch.py",
    }:
        return {"ssh_required": False, "skip_ssh": True}
    return {}


def _probe_agent(
    wrapper: AgentWrapper,
    root: Path,
    script_path: Path,
    timeout: float = 2.0,
) -> tuple[bool, str]:
    if wrapper.name == "localhost":
        result = wrapper.cmd("check", {}, timeout=timeout)
        return bool(result.get("success")), str(result.get("message") or result)

    tool = _agent_cmd_path(root)
    result_path = _make_result_path()
    try:
        completed = subprocess.run(
            [
                str(tool),
                "--agent",
                wrapper.name,
                "--cmd",
                "check",
                "--params-json",
                json.dumps(_probe_params_for_agent(wrapper.name, script_path), ensure_ascii=False),
                "--timeout",
                str(timeout),
                "--result-file",
                str(result_path),
            ],
            cwd=str(root),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout + 3.0,
        )
        result = _read_json_file(result_path)
        success = bool(result.get("success")) and completed.returncode == 0
        message = str(
            result.get("message")
            or completed.stderr.strip()
            or completed.stdout.strip()
            or ("ready" if success else "not ready")
        )
        return success, message
    except Exception as exc:
        return False, str(exc)
    finally:
        try:
            result_path.unlink()
        except FileNotFoundError:
            pass


def _prepare_script_agents(
    agent_defs: dict[str, AgentWrapper],
    required_agent_names: list[str],
    root: Path,
    script_path: Path,
) -> tuple[dict[str, AgentWrapper], dict[str, str]]:
    if not required_agent_names:
        agents: dict[str, AgentWrapper] = {}
        for wrapper in agent_defs.values():
            _add_agent_aliases(agents, wrapper)
        return agents, {}

    agents: dict[str, AgentWrapper] = {}
    unavailable: dict[str, str] = {}
    probed: dict[str, tuple[bool, str]] = {}
    for requested_name in dict.fromkeys(required_agent_names):
        wrapper = _resolve_agent_definition(agent_defs, requested_name)
        if wrapper is None:
            unavailable[requested_name] = "未在 agents_config.json 中定义"
            continue

        if wrapper.name not in probed:
            probed[wrapper.name] = _probe_agent(wrapper, root, script_path)
        ready, message = probed[wrapper.name]
        if ready:
            _add_agent_aliases(agents, wrapper)
        else:
            unavailable[requested_name] = message
    return agents, unavailable


def _install_paths(root: Path, script_path: Path) -> None:
    entries = [
        root,
        root / "scripts",
        root / "scripts" / "runtime",
        script_path.parent,
    ]
    for entry in reversed(entries):
        text = str(entry)
        if text not in sys.path:
            sys.path.insert(0, text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--script", required=True)
    parser.add_argument("script_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    root = Path(args.project_root).resolve()
    config_path = Path(args.config).resolve()
    script_path = Path(args.script).resolve()
    channel = EventChannel()
    agent_defs = _load_agent_definitions(config_path, root)
    required_agent_names = _extract_required_agent_names(script_path)
    agents, unavailable_agents = _prepare_script_agents(
        agent_defs, required_agent_names, root, script_path)

    def shutdown_handler(signum, frame):
        print(f"[runtime] 收到停止信号 {signum}，正在尝试清理录制/播放")
        try:
            if workflow is not None and not getattr(workflow, "_finished", False):
                workflow.set_step("stop_record", "running", "用户停止执行，正在停止录制")
        except Exception:
            pass
        for key in ("glasses_bsp_node", "helen_node", "localhost"):
            wrapper = agent_defs.get(key)
            if wrapper:
                wrapper.stop_for_shutdown()
        try:
            if workflow is not None and not getattr(workflow, "_finished", False):
                try:
                    workflow.set_step("stop_record", "success", "停止命令已发送")
                except Exception:
                    pass
                workflow.finish(False, "用户停止执行")
        except Exception:
            channel.emit({
                "type": "workflow",
                "action": "finish",
                "finished": True,
                "success": False,
                "message": "用户停止执行",
            })
        raise SystemExit(130)

    signal.signal(signal.SIGTERM, shutdown_handler)
    signal.signal(signal.SIGINT, shutdown_handler)

    _install_paths(root, script_path)
    state_cache = RuntimeStateCache(root)
    state_cache.start()
    sys.argv = [str(script_path), *args.script_args]
    workflow = None
    original_print = builtins.print

    try:
        from flowagent.core.script_workflow import (
            SimpleScriptWorkflow,
            bind_workflow,
            clear_workflow_binding,
            finish,
        )

        workflow = SimpleScriptWorkflow(WorkflowQueue(channel))
        bind_workflow(workflow)

        def safe_print(*print_args, **kwargs):
            kwargs.setdefault("flush", True)
            original_print(*print_args, **kwargs)

        globals_dict: dict[str, Any] = {
            "__name__": "__main__",
            "__file__": str(script_path),
            "print": safe_print,
            "time": time,
            "sleep": time.sleep,
            "dialog": DialogAPI(channel),
            "workflow": workflow,
            "script_agents": agents,
            "unavailable_script_agents": unavailable_agents,
            "record_timer": lambda: state_cache.get("record_timer"),
            "time_delay": lambda: state_cache.get("time_delay"),
            "motion_status": lambda: state_cache.get("motion_status"),
            "clear_record_timer": lambda: state_cache.clear("record_timer"),
            "clear_time_delay": lambda: state_cache.clear("time_delay"),
            "clear_motion_status": lambda: state_cache.clear("motion_status"),
            **agents,
        }

        if required_agent_names:
            print(f"[runtime] 脚本声明节点: {', '.join(required_agent_names)}")
        if unavailable_agents:
            print("[runtime] 以下节点未就绪，不会注入 script_agents:")
            for agent_name, reason in unavailable_agents.items():
                print(f"[runtime]   - {agent_name}: {reason}")

        code = script_path.read_text(encoding="utf-8")
        exec(compile(code, str(script_path), "exec"), globals_dict, globals_dict)
        if getattr(workflow, "_finished", False) and getattr(workflow, "_success", True) is False:
            return 1
        return 0
    except SystemExit as exc:
        if workflow is not None and getattr(workflow, "_finished", False) and getattr(workflow, "_success", True) is False:
            return 1
        code = exc.code if isinstance(exc.code, int) else 0
        return code
    except Exception:
        message = traceback.format_exc()
        print(message, file=sys.stderr, flush=True)
        try:
            if workflow is not None:
                workflow.finish(False, "脚本执行失败")
        except Exception:
            channel.emit({"type": "workflow", "action": "finish", "finished": True, "success": False, "message": "脚本执行失败"})
        return 1
    finally:
        try:
            clear_workflow_binding()
        except Exception:
            pass
        state_cache.close()


if __name__ == "__main__":
    raise SystemExit(main())
