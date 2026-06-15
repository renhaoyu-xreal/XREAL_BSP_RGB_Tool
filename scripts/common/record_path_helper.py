"""
BSP 录制目录命名辅助函数。

最后一级目录命名风格：
{glasses}_{fsn}_{experiment_keyword}_{recorder_name}_{sub_experiment_path}_{YYYYMMDDHHMMSS}

例如：
Hylla_L551X00179_2_2_free_record_only_imu_20260204115110
"""

from __future__ import annotations

import json
import os
import re
import time
import getpass
from pathlib import Path
from typing import Any, Iterable, Optional, Tuple

import paramiko


DEFAULT_HOSTNAME = "169.254.2.1"
DEFAULT_PORT = 22
DEFAULT_USERNAME = "root"
DEFAULT_PASSWORD = "xreal2017"

DEFAULT_EXPERIMENT_KEYWORD = os.environ.get("RECORDLAB_BSP_EXPERIMENT_KEYWORD", "exp")
DEFAULT_RECORDER_NAME = os.environ.get("RECORDLAB_BSP_RECORDER_NAME") or os.environ.get("USER") or getpass.getuser()
DEFAULT_RGB_RAW_CAPTURE_INTERVAL_S = os.environ.get("RECORDLAB_BSP_RGB_RAW_CAPTURE_INTERVAL_S", "1")
UNKNOWN_GLASSES_ID = "UNKNOWN_GLASSES"
UNKNOWN_FSN = "UNKNOWN_FSN"
PLACEHOLDER_GLASSES_IDS = {
    "",
    "UNKNOWN",
    "UNKNOWN_GLASSES",
    "TEST_SN",
    "TEST",
    "NONE",
    "NULL",
    "AUTO",
    "自动",
    "自动识别",
}
PLACEHOLDER_FSNS = PLACEHOLDER_GLASSES_IDS | {
    "UNKNOWN_FSN",
    "FSN",
}

_LAST_EXPERIMENT_KEYWORD = ""
_LAST_RECORDER_NAME = ""
_LAST_RGB_RAW_CAPTURE_INTERVAL_S = ""


def _sanitize_token(value: Optional[str], fallback: str) -> str:
    text = str(value or "").strip()
    text = re.sub(r"[\\/\s]+", "_", text)
    text = re.sub(r"[^0-9A-Za-z._\-\u4e00-\u9fa5]+", "_", text)
    text = text.strip("._-")
    return text or fallback


def sanitize_record_token(value: Optional[str], fallback: str) -> str:
    return _sanitize_token(value, fallback)


def _is_placeholder_glasses_id(value: Optional[str]) -> bool:
    return str(value or "").strip().upper() in PLACEHOLDER_GLASSES_IDS


def _is_placeholder_fsn(value: Optional[str]) -> bool:
    return str(value or "").strip().upper() in PLACEHOLDER_FSNS


def _get_default_experiment_keyword() -> str:
    global _LAST_EXPERIMENT_KEYWORD
    if not _LAST_EXPERIMENT_KEYWORD:
        _LAST_EXPERIMENT_KEYWORD = _sanitize_token(DEFAULT_EXPERIMENT_KEYWORD, "exp")
    return _LAST_EXPERIMENT_KEYWORD


def _get_default_recorder_name() -> str:
    global _LAST_RECORDER_NAME
    if not _LAST_RECORDER_NAME:
        _LAST_RECORDER_NAME = _sanitize_token(DEFAULT_RECORDER_NAME, "user")
    return _LAST_RECORDER_NAME


def _get_default_rgb_raw_capture_interval_s() -> str:
    global _LAST_RGB_RAW_CAPTURE_INTERVAL_S
    if not _LAST_RGB_RAW_CAPTURE_INTERVAL_S:
        _LAST_RGB_RAW_CAPTURE_INTERVAL_S = str(DEFAULT_RGB_RAW_CAPTURE_INTERVAL_S or "1").strip() or "1"
    return _LAST_RGB_RAW_CAPTURE_INTERVAL_S


def _project_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _load_agents_config() -> dict:
    config_path = _project_root() / "config" / "agents_config.json"
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    agents = config.get("agents")
    return agents if isinstance(agents, dict) else {}


def _read_latest_local_glasses_id() -> Optional[str]:
    data_root = _project_root() / "data"
    if not data_root.exists():
        return None

    json_candidates = sorted(
        data_root.glob("**/glasses_config.json"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    for path in json_candidates:
        try:
            config = json.loads(path.read_text(encoding="utf-8"))
            for key in ("FSN", "glasses_id"):
                if key in config:
                    return _sanitize_token(config.get(key), UNKNOWN_GLASSES_ID)
        except Exception:
            continue

    txt_candidates = sorted(
        data_root.glob("**/record_info.txt"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    pattern = re.compile(r"\[ro\.bsp\.glasses_id\]:\s*\[(.+?)\]")
    for path in txt_candidates:
        try:
            for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
                match = pattern.search(line)
                if match:
                    return _sanitize_token(match.group(1), UNKNOWN_GLASSES_ID)
        except Exception:
            continue

    return None


def _read_latest_local_glasses_fsn() -> Optional[str]:
    data_root = _project_root() / "data"
    if not data_root.exists():
        return None

    json_candidates = sorted(
        list(data_root.glob("**/glasses_config.json"))
        + list(data_root.glob("**/glass_config.json")),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    for path in json_candidates:
        try:
            config = json.loads(path.read_text(encoding="utf-8"))
            for key in ("fsn", "FSN", "glasses_fsn"):
                value = config.get(key)
                if value and not _is_placeholder_fsn(str(value)):
                    return _sanitize_token(value, UNKNOWN_FSN)
        except Exception:
            continue

    txt_candidates = sorted(
        data_root.glob("**/record_info.txt"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    patterns = [
        re.compile(r"\[ro\.bsp\.fsn\]:\s*\[(.+?)\]", re.IGNORECASE),
        re.compile(r"\bfsn\b\s*[:=]\s*([^\s,\]]+)", re.IGNORECASE),
    ]
    for path in txt_candidates:
        try:
            for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
                for pattern in patterns:
                    match = pattern.search(line)
                    if match and not _is_placeholder_fsn(match.group(1)):
                        return _sanitize_token(match.group(1), UNKNOWN_FSN)
        except Exception:
            continue

    return None


def _read_glasses_id_via_ssh(timeout_s: float = 3.0) -> Optional[str]:
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        ssh.connect(
            DEFAULT_HOSTNAME,
            port=DEFAULT_PORT,
            username=DEFAULT_USERNAME,
            password=DEFAULT_PASSWORD,
            timeout=timeout_s,
        )

        commands = [
            "/usr/usrdata/bin/getprop ro.bsp.glasses_id",
            "cat /factory/glasses_config.json 2>/dev/null",
        ]
        for command in commands:
            stdin, stdout, stderr = ssh.exec_command(command, timeout=timeout_s)
            output = stdout.read().decode("utf-8", errors="ignore").strip()
            if not output:
                continue

            if output.startswith("{"):
                try:
                    config = json.loads(output)
                    glasses_id = config.get("FSN") or config.get("glasses_id")
                    if glasses_id:
                        return _sanitize_token(glasses_id, UNKNOWN_GLASSES_ID)
                except Exception:
                    continue

            return _sanitize_token(output, UNKNOWN_GLASSES_ID)
    except Exception:
        return None
    finally:
        try:
            ssh.close()
        except Exception:
            pass

    return None


def _read_glasses_fsn_via_ssh(timeout_s: float = 3.0) -> Optional[str]:
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        ssh.connect(
            DEFAULT_HOSTNAME,
            port=DEFAULT_PORT,
            username=DEFAULT_USERNAME,
            password=DEFAULT_PASSWORD,
            timeout=timeout_s,
        )

        commands = [
            "/usr/usrdata/bin/getprop ro.bsp.fsn",
            "cat /factory/glasses_config.json 2>/dev/null",
            "cat /data/glass_config.json 2>/dev/null",
        ]
        for command in commands:
            stdin, stdout, stderr = ssh.exec_command(command, timeout=timeout_s)
            output = stdout.read().decode("utf-8", errors="ignore").strip()
            if not output:
                continue

            if output.startswith("{"):
                try:
                    config = json.loads(output)
                    for key in ("fsn", "FSN", "glasses_fsn"):
                        value = config.get(key)
                        if value and not _is_placeholder_fsn(str(value)):
                            return _sanitize_token(value, UNKNOWN_FSN)
                except Exception:
                    continue
                continue

            if not _is_placeholder_fsn(output):
                return _sanitize_token(output, UNKNOWN_FSN)
    except Exception:
        return None
    finally:
        try:
            ssh.close()
        except Exception:
            pass

    return None


def _load_supported_device_name(product_id: int, agent_names: Iterable[str]) -> Optional[str]:
    agents = _load_agents_config()
    product_key = str(product_id)
    for agent_name in agent_names:
        agent_config = agents.get(agent_name)
        if not isinstance(agent_config, dict):
            continue
        supported_devices = agent_config.get("supported_devices")
        if not isinstance(supported_devices, dict):
            continue
        device_name = supported_devices.get(product_key)
        if device_name:
            return _sanitize_token(device_name, product_key)
    return None


def _load_configured_device_label(agent_names: Iterable[str]) -> Optional[str]:
    agents = _load_agents_config()
    for agent_name in agent_names:
        agent_config = agents.get(agent_name)
        if not isinstance(agent_config, dict):
            continue
        supported_devices = agent_config.get("supported_devices")
        if isinstance(supported_devices, dict) and len(supported_devices) == 1:
            value = next(iter(supported_devices.values()))
            if value:
                return _sanitize_token(value, UNKNOWN_GLASSES_ID)

        init_params = agent_config.get("init_device_params")
        if isinstance(init_params, dict):
            for key in ("device_name", "device_model", "model", "device_type"):
                value = init_params.get(key)
                if value and not _is_placeholder_glasses_id(str(value)):
                    return _sanitize_token(value, UNKNOWN_GLASSES_ID)
    return None


def _unwrap_agent_result(result: Any) -> dict:
    if not isinstance(result, dict):
        return {}
    payload = result.get("result")
    return payload if isinstance(payload, dict) else result


def _device_payload_from_command(command: str, payload: dict) -> dict:
    if command == "get_bsp_runtime_state":
        device_payload = payload.get("device")
        if isinstance(device_payload, dict):
            return device_payload
    return payload


def _resolve_agent_device_label(
    agent: Optional[Any],
    agent_names: Iterable[str],
) -> Optional[str]:
    if agent is None:
        return None

    for command in ("check", "get_bsp_runtime_state"):
        try:
            result = agent.cmd(command, {})
            payload = _device_payload_from_command(command, _unwrap_agent_result(result))

            product_id = int(payload.get("product_id", 0))
            if product_id > 0:
                device_name = _load_supported_device_name(product_id, agent_names)
                return device_name or _sanitize_token(str(product_id), UNKNOWN_GLASSES_ID)
        except Exception:
            continue

    return None


def _resolve_agent_device_fsn(agent: Optional[Any]) -> Optional[str]:
    if agent is None:
        return None

    for command in ("check", "get_bsp_runtime_state"):
        try:
            result = agent.cmd(command, {})
            payload = _device_payload_from_command(command, _unwrap_agent_result(result))
            for key in ("fsn", "FSN", "glasses_fsn"):
                value = payload.get(key)
                if value and not _is_placeholder_fsn(str(value)):
                    return _sanitize_token(value, UNKNOWN_FSN)
        except Exception:
            continue

    return None


def resolve_record_glasses_label(
    agent: Optional[Any] = None,
    agent_names: Iterable[str] = ("helen_node", "glasses_bsp_node"),
    preferred_label: Optional[str] = None,
    allow_ssh: bool = True,
) -> str:
    if preferred_label and not _is_placeholder_glasses_id(preferred_label):
        return _sanitize_token(preferred_label, UNKNOWN_GLASSES_ID)

    agent_label = _resolve_agent_device_label(agent, agent_names)
    if agent_label:
        return agent_label

    if allow_ssh:
        glasses_id = _read_glasses_id_via_ssh()
        if glasses_id:
            return glasses_id

    if allow_ssh:
        glasses_id = _read_latest_local_glasses_id()
        if glasses_id:
            return glasses_id

    config_label = _load_configured_device_label(agent_names)
    if config_label:
        return config_label

    return UNKNOWN_GLASSES_ID


def resolve_record_glasses_fsn(
    agent: Optional[Any] = None,
    preferred_fsn: Optional[str] = None,
    allow_ssh: bool = True,
) -> str:
    if preferred_fsn and not _is_placeholder_fsn(preferred_fsn):
        return _sanitize_token(preferred_fsn, UNKNOWN_FSN)

    agent_fsn = _resolve_agent_device_fsn(agent)
    if agent_fsn:
        return agent_fsn

    if allow_ssh:
        glasses_fsn = _read_glasses_fsn_via_ssh()
        if glasses_fsn:
            return glasses_fsn

    if allow_ssh:
        glasses_fsn = _read_latest_local_glasses_fsn()
        if glasses_fsn:
            return glasses_fsn

    return UNKNOWN_FSN


def resolve_bsp_glasses_id(
    agent: Optional[Any] = None,
    agent_names: Iterable[str] = ("helen_node", "glasses_bsp_node"),
    preferred_label: Optional[str] = None,
) -> str:
    return resolve_record_glasses_label(
        agent=agent,
        agent_names=agent_names,
        preferred_label=preferred_label,
        allow_ssh=True,
    )


def prompt_bsp_record_labels(dialog_api: Optional[Any]) -> Optional[Tuple[str, str]]:
    """提示输入实验关键字与录制人。"""

    default_experiment_keyword = _get_default_experiment_keyword()
    default_recorder_name = _get_default_recorder_name()

    if dialog_api is None:
        return default_experiment_keyword, default_recorder_name

    result = dialog_api.multi_field_input(
        "请填写实验关键词和录制人",
        "请填写实验关键词和录制人",
        [
            {
                "name": "experiment_keyword",
                "label": "实验关键字",
                "default": default_experiment_keyword,
            },
            {
                "name": "recorder_name",
                "label": "录制人",
                "default": default_recorder_name,
            },
        ],
    )
    if not result:
        return None

    experiment_keyword = _sanitize_token(
        result.get("experiment_keyword"),
        default_experiment_keyword,
    )
    recorder_name = _sanitize_token(
        result.get("recorder_name"),
        default_recorder_name,
    )

    global _LAST_EXPERIMENT_KEYWORD, _LAST_RECORDER_NAME
    _LAST_EXPERIMENT_KEYWORD = experiment_keyword
    _LAST_RECORDER_NAME = recorder_name
    return experiment_keyword, recorder_name


def prompt_bsp_rgb_record_options(dialog_api: Optional[Any]) -> Optional[Tuple[str, str, str]]:
    """提示输入实验关键字、录制人和 raw 抓取间隔。"""

    default_experiment_keyword = _get_default_experiment_keyword()
    default_recorder_name = _get_default_recorder_name()
    default_raw_capture_interval_s = _get_default_rgb_raw_capture_interval_s()

    if dialog_api is None:
        return default_experiment_keyword, default_recorder_name, default_raw_capture_interval_s

    result = dialog_api.multi_field_input(
        "请填写 RGB 录制信息",
        "请填写实验关键词、录制人和 raw 抓取间隔（秒）",
        [
            {
                "name": "experiment_keyword",
                "label": "实验关键字",
                "default": default_experiment_keyword,
            },
            {
                "name": "recorder_name",
                "label": "录制人",
                "default": default_recorder_name,
            },
            {
                "name": "raw_capture_interval_s",
                "label": "raw 抓取间隔(s)",
                "default": default_raw_capture_interval_s,
            },
        ],
    )
    if not result:
        return None

    experiment_keyword = _sanitize_token(
        result.get("experiment_keyword"),
        default_experiment_keyword,
    )
    recorder_name = _sanitize_token(
        result.get("recorder_name"),
        default_recorder_name,
    )
    raw_capture_interval_s = str(
        result.get("raw_capture_interval_s") or default_raw_capture_interval_s
    ).strip() or default_raw_capture_interval_s

    global _LAST_EXPERIMENT_KEYWORD, _LAST_RECORDER_NAME, _LAST_RGB_RAW_CAPTURE_INTERVAL_S
    _LAST_EXPERIMENT_KEYWORD = experiment_keyword
    _LAST_RECORDER_NAME = recorder_name
    _LAST_RGB_RAW_CAPTURE_INTERVAL_S = raw_capture_interval_s
    return experiment_keyword, recorder_name, raw_capture_interval_s


def build_bsp_dataset_name(
    sub_experiment_path: str,
    experiment_keyword: Optional[str] = None,
    recorder_name: Optional[str] = None,
    leaf_token_override: Optional[str] = None,
    glasses_id_override: Optional[str] = None,
    glasses_fsn_override: Optional[str] = None,
    agent: Optional[Any] = None,
    agent_names: Iterable[str] = ("helen_node", "glasses_bsp_node"),
) -> Tuple[str, str]:
    """构造 BSP 录制目录名。

    返回:
        (dataset_name, glasses_id)
        dataset_name 形如:
        free_record/only_imu/Hylla_L551X00179_2_2_free_record_only_imu_20260204115110
    """

    clean_path = str(sub_experiment_path or "").strip().strip("/\\")
    if not clean_path:
        raise ValueError("sub_experiment_path 不能为空")

    glasses_id = _sanitize_token(glasses_id_override, UNKNOWN_GLASSES_ID)
    if glasses_id == UNKNOWN_GLASSES_ID:
        glasses_id = resolve_bsp_glasses_id(agent=agent, agent_names=agent_names)
    glasses_fsn = resolve_record_glasses_fsn(
        agent=agent,
        preferred_fsn=glasses_fsn_override,
        allow_ssh=True,
    )
    experiment_token = _sanitize_token(
        experiment_keyword,
        _get_default_experiment_keyword(),
    )
    recorder_token = _sanitize_token(
        recorder_name,
        _get_default_recorder_name(),
    )
    leaf_token_source = (
        leaf_token_override
        if leaf_token_override is not None
        else clean_path.replace("/", "_").replace("\\", "_")
    )
    path_token = _sanitize_token(leaf_token_source, "record")
    timestamp = time.strftime("%Y%m%d%H%M%S")

    leaf_dir = f"{glasses_id}_{glasses_fsn}_{experiment_token}_{recorder_token}_{path_token}_{timestamp}"
    return f"{clean_path}/{leaf_dir}", glasses_id
