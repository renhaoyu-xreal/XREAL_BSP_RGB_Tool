"""
单次抓取 BSP RGB RAW 数据脚本。

功能：
1. 单次执行一次 raw 抓取
2. 依赖 Tab5 已启动 RGB 预览
3. 不录制 BMP / IMU，只保存 raw 与 sidecar
"""

from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps
from scripts.common.record_path_helper import build_bsp_dataset_name, prompt_bsp_record_labels

all_agent_names = ["glasses_bsp_node"]

RAW_CAPTURE_TARGET_SUBDIR = "rgb0/raw_data"
RAW_CAPTURE_CMD_TIMEOUT_S = 150.0
DEFAULT_RAW_RESOLUTION = 8
DEFAULT_RAW_EXPOSURE_MODE = 0
DEFAULT_RAW_EXPOSURE_VALUE = 1
DEFAULT_RAW_GAIN = 1

def _unwrap_cmd_result(result: dict) -> dict:
    payload = result.get("message") if isinstance(result, dict) else None
    if isinstance(payload, dict) and "success" in payload:
        return payload
    return result if isinstance(result, dict) else {}


def _finish_once(
    success: bool,
    message: str,
    _workflow_state={"finished": False},
    _finish=finish,
):
    if _workflow_state["finished"]:
        return
    _finish(success, message)
    _workflow_state["finished"] = True


def _fail_and_exit(
    step: WorkflowStep,
    message: str,
    _set_step=set_step,
    _finish_once_fn=_finish_once,
):
    _set_step(step, "failed", message)
    _finish_once_fn(False, message)
    print(f"[BSP_RGB_RAW] ✗ {message}")
    raise SystemExit


def _parse_int_field(values: dict, field_name: str, label: str) -> int:
    raw_value = str(values.get(field_name, "")).strip()
    try:
        return int(raw_value)
    except (TypeError, ValueError):
        raise ValueError(f"{label} 必须是整数，当前输入: {raw_value or '<empty>'}")


set_steps(
    [
        WorkflowStep.NODES_CHECK,
        WorkflowStep.GET_BSP_RUNTIME_STATE,
        WorkflowStep.CHECK,
        WorkflowStep.CAPTURE_RAW_FRAME,
    ],
    title="BSP RGB RAW 单次抓取",
)

print("=" * 60)
print("[BSP_RGB_RAW] 单次抓取 BSP RGB RAW 数据")
print("=" * 60)

set_step(WorkflowStep.NODES_CHECK, "running", "正在检查 glasses_bsp_node")
glasses_agent = script_agents.get("glasses_bsp_node")
if glasses_agent is None:
    _fail_and_exit(WorkflowStep.NODES_CHECK, "缺少 node: glasses_bsp_node")
set_step(WorkflowStep.NODES_CHECK, "success", "glasses_bsp_node 已连接")

set_step(WorkflowStep.GET_BSP_RUNTIME_STATE, "running", "正在检查 BSP RGB 运行状态")
runtime_result = _unwrap_cmd_result(glasses_agent.cmd("get_bsp_runtime_state", {}))
if not runtime_result.get("success"):
    _fail_and_exit(
        WorkflowStep.GET_BSP_RUNTIME_STATE,
        f"获取 BSP 运行状态失败: {runtime_result.get('message', 'Unknown error')}",
    )

runtime_device = runtime_result.get("device") or {}
latest_frame = runtime_result.get("latest_frame") or {}
camera_mode = runtime_result.get("camera_mode")
if not runtime_device.get("started"):
    _fail_and_exit(WorkflowStep.GET_BSP_RUNTIME_STATE, "设备未启动，请先一键启动 RGB")
if camera_mode != "rgb":
    _fail_and_exit(
        WorkflowStep.GET_BSP_RUNTIME_STATE,
        f"当前相机模式不是 RGB: {camera_mode}",
    )
if not latest_frame:
    _fail_and_exit(
        WorkflowStep.GET_BSP_RUNTIME_STATE,
        "当前没有可用的 RGB 最新帧元数据，请确认 RGB 预览已经正常出现",
    )
set_step(WorkflowStep.GET_BSP_RUNTIME_STATE, "success", "BSP RGB 运行状态已确认")

set_step(WorkflowStep.CHECK, "running", "正在获取抓取参数")
record_labels = prompt_bsp_record_labels(dialog)
if record_labels is None:
    _fail_and_exit(WorkflowStep.CHECK, "已取消录制命名输入")

raw_params = dialog.multi_field_input(
    "RAW 抓取参数",
    (
        "请输入 cameratest raw 抓取参数\n\n"
        "提示:\n"
        "分辨率：8=4K流，详见 NRRgbCameraImageResolution\n"
        "曝光模式：0=手动曝光\n"
        "曝光值: 最大1600\n"
        "增益: 未知"
    ),
    [
        {
            "name": "resolution",
            "label": "分辨率参数",
            "default": str(DEFAULT_RAW_RESOLUTION),
        },
        {
            "name": "exposure_mode",
            "label": "曝光模式",
            "default": str(DEFAULT_RAW_EXPOSURE_MODE),
        },
        {
            "name": "exposure_value",
            "label": "曝光值",
            "default": str(DEFAULT_RAW_EXPOSURE_VALUE),
        },
        {
            "name": "gain",
            "label": "增益",
            "default": str(DEFAULT_RAW_GAIN),
        },
    ],
)
if raw_params is None:
    _fail_and_exit(WorkflowStep.CHECK, "已取消 RAW 抓取参数输入")

try:
    raw_resolution = _parse_int_field(raw_params, "resolution", "分辨率参数")
    raw_exposure_mode = _parse_int_field(raw_params, "exposure_mode", "曝光模式")
    raw_exposure_value = _parse_int_field(raw_params, "exposure_value", "曝光值")
    raw_gain = _parse_int_field(raw_params, "gain", "增益")
except ValueError as exc:
    _fail_and_exit(WorkflowStep.CHECK, str(exc))

experiment_keyword, recorder_name = record_labels
dataset_name, glasses_id = build_bsp_dataset_name(
    "slam_rgb_imu",
    experiment_keyword=experiment_keyword,
    recorder_name=recorder_name,
    leaf_token_override="free_record_rgb_raw",
    agent=glasses_agent,
    agent_names=("glasses_bsp_node",),
)
set_step(WorkflowStep.CHECK, "success", f"抓取参数已确认: {dataset_name}")

print(f"[BSP_RGB_RAW] 当前眼镜标识: {glasses_id}")
print(f"[BSP_RGB_RAW] 实验关键字: {experiment_keyword}")
print(f"[BSP_RGB_RAW] 录制人: {recorder_name}")
print(f"[BSP_RGB_RAW] 数据将保存到: data/{dataset_name}/")
print(
    "[BSP_RGB_RAW] RAW 参数: "
    f"resolution={raw_resolution}, "
    f"exposure_mode={raw_exposure_mode}, "
    f"exposure_value={raw_exposure_value}, "
    f"gain={raw_gain}"
)
print("[BSP_RGB_RAW] 当前 RGB 预览已就绪，将执行双 reboot raw 抓取并恢复 RGB")

set_step(WorkflowStep.CAPTURE_RAW_FRAME, "running", "正在单次抓取 raw")
result = _unwrap_cmd_result(glasses_agent.cmd("capture_raw_frame", {
    "dataset_name": dataset_name,
    "target_subdir": RAW_CAPTURE_TARGET_SUBDIR,
    "raw_resolution": raw_resolution,
    "raw_exposure_mode": raw_exposure_mode,
    "raw_exposure_value": raw_exposure_value,
    "raw_gain": raw_gain,
}, timeout=RAW_CAPTURE_CMD_TIMEOUT_S))

if not result.get("success"):
    _fail_and_exit(
        WorkflowStep.CAPTURE_RAW_FRAME,
        f"单次抓取失败: {result.get('message', 'Unknown error')}",
    )

raw_file = result.get("raw_file", "--")
frame_file = result.get("frame_file", "--")
glass_timestamp_file = result.get("glass_timestamp_file", "--")
metadata_file = result.get("metadata_file", "--")
pre_capture_temp = result.get("pre_capture_rgb_temperature")
restored_temp = result.get("restored_first_rgb_temperature")
average_temp = result.get("average_rgb_temperature")

set_step(
    WorkflowStep.CAPTURE_RAW_FRAME,
    "success",
    f"单次抓取成功: {raw_file}",
)
print(f"[BSP_RGB_RAW] ✓ Raw 抓取成功: {raw_file}")
print(f"[BSP_RGB_RAW] metadata: {metadata_file}")
print(f"[BSP_RGB_RAW] frame: {frame_file}")
print(f"[BSP_RGB_RAW] glass_timestamp: {glass_timestamp_file}")
print(f"[BSP_RGB_RAW] 抓取前最后一帧温度: {pre_capture_temp}")
print(f"[BSP_RGB_RAW] 恢复后首帧温度: {restored_temp}")
print(f"[BSP_RGB_RAW] 平均温度: {average_temp}")
if result.get("rgb_restore_success") is not None:
    print(
        "[BSP_RGB_RAW] RGB恢复: "
        f"{'成功' if result.get('rgb_restore_success') else '失败'} "
        f"{result.get('rgb_restore_message', '')}".rstrip()
    )

_finish_once(True, f"单次抓取成功: {raw_file}")
