# Simple Android IMU recording test.
# It records parsed data from Android TCP port 8100 through the android agent.

from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps
import time
from time import monotonic, sleep


all_agent_names = ['android']

print("[android_imu_simple_test] Starting simple Android IMU recording test...")


def _safe_int(value, default):
    try:
        return int(str(value).strip())
    except Exception:
        return default


def _extract_root_path(result):
    if not isinstance(result, dict):
        return ""
    if result.get("root_path"):
        return result.get("root_path")
    message = result.get("message")
    if isinstance(message, dict):
        return message.get("root_path", "")
    return ""


def _safe_path_part(value, default):
    text = str(value).strip()
    if not text:
        text = default
    safe_chars = []
    for ch in text:
        if ch.isalnum() or ch in ("-", "_"):
            safe_chars.append(ch)
        else:
            safe_chars.append("_")
    cleaned = "".join(safe_chars).strip("_")
    return cleaned or default


def _parse_fan_condition(value):
    text = str(value).strip().upper()
    if text in ("FAN0", "0"):
        return "FAN0", 0, None, None, None
    if text in ("FAN100", "100"):
        return "FAN100", 100, None, None, None
    if text in ("FAN_0_100", "FAN0_100", "FAN0100", "0_100"):
        return "FAN_0_100", None, None, "0_100", None
    if text in ("GPS_ON_OFF", "GPS", "GPSONOFF", "GPS_0_1"):
        return "GPS_ON_OFF", None, None, None, "on_off"
    if text in ("BIANPIN", "WAVE_LOAD", "WAVE", "变负载"):
        return "BianPin", 100, "wave", None, None
    if text in ("ZHONGFUZAI", "HIGH_LOAD", "HIGH", "高负载"):
        return "ZhongFuZai", 100, "high", None, None
    return "NONE", None, None, None, None


def _restore_fan_if_needed(android_agent, fan_configured):
    if not android_agent or not fan_configured:
        return True
    try:
        result = android_agent.cmd("restore_fan", {})
        if isinstance(result, dict):
            return bool(result.get("success"))
        return True
    except Exception:
        return False


def _stop_load_if_needed(android_agent, load_started):
    if not android_agent or not load_started:
        return
    try:
        android_agent.cmd("stop_load", {})
    except Exception:
        pass


def _stop_fan_cycle_if_needed(android_agent, fan_cycle_started):
    if not android_agent or not fan_cycle_started:
        return
    try:
        android_agent.cmd("stop_fan_cycle", {})
    except Exception:
        pass


def _stop_gps_cycle_if_needed(android_agent, gps_cycle_started):
    if not android_agent or not gps_cycle_started:
        return
    try:
        android_agent.cmd("stop_gps_cycle", {})
    except Exception:
        pass


android_agent = None
recording_started = False
device_started = False
fan_configured = False
load_started = False
fan_cycle_started = False
gps_cycle_started = False

try:
    fields = [
        {"name": "duration_seconds", "label": "录制时长(秒)", "default": "10"},
        {"name": "experiment_keyword", "label": "实验关键字", "default": "test"},
        {"name": "operator_name", "label": "实验人员", "default": "xjh"},
        {
            "name": "fan_condition",
            "label": "实验场景",
            "default": "NONE",
            "choices": ["NONE", "FAN0", "FAN100", "FAN_0_100", "BianPin", "ZhongFuZai"], #, "GPS_ON_OFF"
        },
        {"name": "android_tcp_port", "label": "Android TCP端口", "default": "8100"},
    ]

    dialog_result = dialog.multi_field_input(
        "Android IMU简单录制测试",
        "只保存8100端口解析后的Android IMU CSV",
        fields,
    )

    if not dialog_result:
        print("[android_imu_simple_test] 用户取消输入")
    else:
        duration_seconds = _safe_int(dialog_result["duration_seconds"], 10)
        android_tcp_port = _safe_int(dialog_result["android_tcp_port"], 8100)
        experiment_keyword = _safe_path_part(dialog_result["experiment_keyword"], "test")
        operator_name = _safe_path_part(dialog_result["operator_name"], "unknown")
        fan_condition, fan_speed, load_mode, fan_cycle_mode, gps_cycle_mode = _parse_fan_condition(dialog_result["fan_condition"])
        if fan_condition != "NONE" and experiment_keyword == "test":
            experiment_keyword = fan_condition
        script_name = "record_android_imu_simple_test"
        experiment_time = time.strftime("%Y%m%d_%H%M%S")
        dataset_name = "android_" + experiment_keyword + "_" + operator_name + "_" + script_name + "_" + experiment_time

        if duration_seconds <= 0:
            duration_seconds = 10

        steps = [
            WorkflowStep.NODES_CHECK,
            WorkflowStep.START_DEVICE,
            WorkflowStep.START_RECORD,
            WorkflowStep.STOP_RECORD,
            WorkflowStep.STOP_DEVICE,
            WorkflowStep.GET_ROOT_PATH,
        ]
        set_steps(steps, title="Android IMU简单录制测试")

        set_step(WorkflowStep.NODES_CHECK, "running", "正在检查 android 节点")
        android_agent = script_agents.get("android")
        if android_agent is None:
            error_message = "当前脚本缺失 android node: 目标IP=localhost，TCP端口=8100"
            set_step(WorkflowStep.NODES_CHECK, "failed", error_message)
            finish(False, error_message)
        else:
            set_step(WorkflowStep.NODES_CHECK, "success", "android 节点已连接")

            print(f"[android_imu_simple_test] TCP port: {android_tcp_port}")
            print(f"[android_imu_simple_test] Dataset: {dataset_name}")
            print(f"[android_imu_simple_test] Duration: {duration_seconds}s")
            print(f"[android_imu_simple_test] Fan condition: {fan_condition}")
            if load_mode:
                print(f"[android_imu_simple_test] Load mode: {load_mode}")

            set_step(WorkflowStep.START_DEVICE, "running", f"正在重启 Android TCP:{android_tcp_port}")
            start_device_result = android_agent.cmd("restart_device", {"tcp_port": android_tcp_port})
            if not start_device_result.get("success"):
                error_message = f"restart_device 失败: {start_device_result}"
                set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                finish(False, error_message)
            else:
                device_started = True
                set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启")

                if fan_speed is not None:
                    print(f"[android_imu_simple_test] Setting fan speed: {fan_speed}")
                    fan_result = android_agent.cmd("set_fan", {"speed": fan_speed})
                    if not fan_result.get("success"):
                        error_message = f"set_fan 失败: {fan_result}"
                        set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                        finish(False, error_message)
                        raise RuntimeError(error_message)
                    else:
                        fan_configured = True
                        set_step(WorkflowStep.START_DEVICE, "success", f"Android 采集链路已重启，风扇={fan_speed}")

                if load_mode:
                    print(f"[android_imu_simple_test] Starting Android load: {load_mode}")
                    load_result = android_agent.cmd("start_load", {"mode": load_mode})
                    if not load_result.get("success"):
                        error_message = f"start_load 失败: {load_result}"
                        set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                        finish(False, error_message)
                        raise RuntimeError(error_message)
                    load_started = True
                    if load_mode == "wave":
                        set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启，风扇=100，变负载运行中")
                    else:
                        set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启，风扇=100，高负载运行中")

                if fan_cycle_mode:
                    print(f"[android_imu_simple_test] Starting fan cycle: {fan_cycle_mode}")
                    fan_cycle_result = android_agent.cmd("start_fan_cycle", {"mode": fan_cycle_mode})
                    if not fan_cycle_result.get("success"):
                        error_message = f"start_fan_cycle 失败: {fan_cycle_result}"
                        set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                        finish(False, error_message)
                        raise RuntimeError(error_message)
                    fan_cycle_started = True
                    set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启，风扇0/100循环运行中")

                if gps_cycle_mode:
                    print(f"[android_imu_simple_test] Starting GPS cycle: {gps_cycle_mode}")
                    gps_cycle_result = android_agent.cmd("start_gps_cycle", {"mode": gps_cycle_mode})
                    if not gps_cycle_result.get("success"):
                        error_message = f"start_gps_cycle 失败: {gps_cycle_result}"
                        set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                        finish(False, error_message)
                        raise RuntimeError(error_message)
                    gps_cycle_started = True
                    set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启，GPS开关循环运行中")

                set_step(WorkflowStep.START_RECORD, "running", "正在开始写入 mobile_data.csv")
                start_record_result = android_agent.cmd("start_record", {"dataset_name": dataset_name})
                if not start_record_result.get("success"):
                    error_message = f"start_record 失败: {start_record_result}"
                    set_step(WorkflowStep.START_RECORD, "failed", error_message)
                    finish(False, error_message)
                else:
                    recording_started = True
                    set_step(WorkflowStep.START_RECORD, "success", f"录制中: {duration_seconds}s")

                    deadline = monotonic() + duration_seconds
                    next_log_time = 0.0
                    while True:
                        remaining_float = deadline - monotonic()
                        if remaining_float <= 0:
                            break
                        remaining = int(remaining_float + 0.999)
                        if load_mode == "wave":
                            elapsed = int(duration_seconds - remaining)
                            phase = "high" if (elapsed // 60) % 2 == 0 else "normal"
                            if monotonic() >= next_log_time:
                                print(f"[android_imu_simple_test] Recording... {remaining}s remaining, wave phase={phase}")
                                next_log_time = monotonic() + 1.0
                        else:
                            if monotonic() >= next_log_time:
                                print(f"[android_imu_simple_test] Recording... {remaining}s remaining")
                                next_log_time = monotonic() + 1.0
                        sleep(min(0.1, remaining_float))

                    set_step(WorkflowStep.STOP_RECORD, "running", "正在停止录制")
                    stop_record_result = android_agent.cmd("stop_record", {})
                    recording_started = False
                    if not stop_record_result.get("success"):
                        error_message = f"stop_record 失败: {stop_record_result}"
                        set_step(WorkflowStep.STOP_RECORD, "failed", error_message)
                        finish(False, error_message)
                    else:
                        set_step(WorkflowStep.STOP_RECORD, "success", "录制已停止")
                        _stop_fan_cycle_if_needed(android_agent, fan_cycle_started)
                        fan_cycle_started = False
                        _stop_gps_cycle_if_needed(android_agent, gps_cycle_started)
                        gps_cycle_started = False
                        _stop_load_if_needed(android_agent, load_started)
                        load_started = False
                        if _restore_fan_if_needed(android_agent, fan_configured):
                            fan_configured = False

                        set_step(WorkflowStep.STOP_DEVICE, "running", "正在停止 TCP server")
                        stop_device_result = android_agent.cmd("stop_device", {})
                        device_started = False
                        if not stop_device_result.get("success"):
                            error_message = f"stop_device 失败: {stop_device_result}"
                            set_step(WorkflowStep.STOP_DEVICE, "failed", error_message)
                            finish(False, error_message)
                        else:
                            set_step(WorkflowStep.STOP_DEVICE, "success", "TCP server 已停止")

                            set_step(WorkflowStep.GET_ROOT_PATH, "running", "正在获取保存路径")
                            root_path_result = android_agent.cmd("get_root_path", {})
                            root_path = _extract_root_path(root_path_result)
                            if root_path:
                                output_dir = root_path + "/" + dataset_name
                                output_file = stop_record_result.get("csv_path") or output_dir
                                print(f"[android_imu_simple_test] Saved CSV: {output_file}")
                                set_step(WorkflowStep.GET_ROOT_PATH, "success", output_file)
                                finish(True, f"录制完成: {output_file}")
                            else:
                                print("[android_imu_simple_test] Recording finished, but root_path is unknown")
                                set_step(WorkflowStep.GET_ROOT_PATH, "success", "录制完成，未获取到root_path")
                                finish(True, "录制完成")

except Exception as e:
    print(f"[android_imu_simple_test] 脚本执行出错: {e}")
    try:
        if android_agent and recording_started:
            android_agent.cmd("stop_record", {})
    except Exception:
        pass
    _stop_load_if_needed(android_agent, load_started)
    load_started = False
    _stop_fan_cycle_if_needed(android_agent, fan_cycle_started)
    fan_cycle_started = False
    _stop_gps_cycle_if_needed(android_agent, gps_cycle_started)
    gps_cycle_started = False
    if _restore_fan_if_needed(android_agent, fan_configured):
        fan_configured = False
    try:
        if android_agent and device_started:
            android_agent.cmd("stop_device", {})
    except Exception:
        pass
    finish(False, f"脚本执行出错: {e}")
finally:
    try:
        if android_agent and recording_started:
            android_agent.cmd("stop_record", {})
        _stop_fan_cycle_if_needed(android_agent, fan_cycle_started)
        _stop_gps_cycle_if_needed(android_agent, gps_cycle_started)
        _stop_load_if_needed(android_agent, load_started)
        _restore_fan_if_needed(android_agent, fan_configured)
        if android_agent and device_started:
            android_agent.cmd("stop_device", {})
    except Exception:
        pass
