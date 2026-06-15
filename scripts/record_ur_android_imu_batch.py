# Batch recording script: execute UR trajectories sequentially while recording Android IMU.
# Input format: traj_id-taker_number pairs (e.g., "10-1, 11-2, 13-3")

import time
from time import sleep

from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps
from nviz_ur_base import build_record_dataset_name, get_program_config, wait_for_command


all_agent_names = ['UR_node', 'android']

print("[batch_recording_android] Starting UR + Android IMU batch recording script...")
print(f"[batch_recording_android] Available agents: {all_agent_names}")


def parse_trajectory_list(traj_list_str):
    """解析轨迹列表字符串。

    输入格式: "10-1, 11-2, 13-3" 或 "10-1,11-2,13-3"
    返回: [(10, 1), (11, 2), (13, 3)]
    """
    result = []
    items = traj_list_str.split(',')

    for item in items:
        item = item.strip()
        if '-' in item:
            parts = item.split('-')
            if len(parts) == 2:
                try:
                    traj_id = int(parts[0].strip())
                    taker_number = int(parts[1].strip())
                    result.append((traj_id, taker_number))
                except Exception:
                    print(f"[batch_recording_android] Warning: 无法解析 '{item}'，跳过")
            else:
                print(f"[batch_recording_android] Warning: 格式错误 '{item}'，跳过")
        else:
            print(f"[batch_recording_android] Warning: 格式错误 '{item}'，需要使用 'traj_id-taker_number' 格式")

    return result


def safe_int(value, default):
    try:
        return int(str(value).strip())
    except Exception:
        return default


def safe_path_part(value, default):
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


def extract_root_path(result):
    if not isinstance(result, dict):
        return ""
    if result.get("root_path"):
        return result.get("root_path")
    message = result.get("message")
    if isinstance(message, dict):
        return message.get("root_path", "")
    return ""


def stop_android_record_if_needed(android_agent, recording_started):
    if not android_agent or not recording_started:
        return
    try:
        android_agent.cmd("stop_record", {})
    except Exception:
        pass


def stop_android_device_if_needed(android_agent, device_started):
    if not android_agent or not device_started:
        return
    try:
        android_agent.cmd("stop_device", {})
    except Exception:
        pass


def parse_fan_condition(value):
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


def restore_fan_if_needed(android_agent, fan_configured):
    if not android_agent or not fan_configured:
        return True
    try:
        result = android_agent.cmd("restore_fan", {})
        if isinstance(result, dict):
            return bool(result.get("success"))
        return True
    except Exception:
        return False


def stop_load_if_needed(android_agent, load_started):
    if not android_agent or not load_started:
        return
    try:
        android_agent.cmd("stop_load", {})
    except Exception:
        pass


def stop_fan_cycle_if_needed(android_agent, fan_cycle_started):
    if not android_agent or not fan_cycle_started:
        return
    try:
        android_agent.cmd("stop_fan_cycle", {})
    except Exception:
        pass


def stop_gps_cycle_if_needed(android_agent, gps_cycle_started):
    if not android_agent or not gps_cycle_started:
        return
    try:
        android_agent.cmd("stop_gps_cycle", {})
    except Exception:
        pass


android_agent = None
ur_agent = None
android_device_started = False
android_recording_started = False
fan_configured = False
load_started = False
fan_cycle_started = False
gps_cycle_started = False

try:
    fields = [
        {"name": "traj_list", "label": "轨迹列表 (格式: traj_id-taker, 逗号分隔)", "default": "10-1, 11-1, 13-1"},
        {"name": "experiment_keyword", "label": "实验关键字", "default": "test"},
        {"name": "recorder_name", "label": "实验人员", "default": "xjh"},
        {
            "name": "fan_condition",
            "label": "实验场景",
            "default": "NONE",
            "choices": ["NONE", "FAN0", "FAN100", "FAN_0_100", "BianPin", "ZhongFuZai"],
        },
        {"name": "android_tcp_port", "label": "Android TCP端口", "default": "8100"},
        {"name": "cooldown_minutes", "label": "轨迹间冷却时间(分钟)", "default": "10"},
    ]

    dialog_result = dialog.multi_field_input(
        "批量录制参数设置 (UR + Android IMU)",
        "请输入批量录制参数",
        fields,
    )

    if not dialog_result:
        print("[batch_recording_android] 用户取消输入")
    else:
        traj_list_str = dialog_result["traj_list"]
        experiment_keyword = safe_path_part(dialog_result["experiment_keyword"], "test")
        recorder_name = safe_path_part(dialog_result["recorder_name"], "unknown")
        fan_condition, fan_speed, load_mode, fan_cycle_mode, gps_cycle_mode = parse_fan_condition(dialog_result["fan_condition"])
        if fan_condition != "NONE" and experiment_keyword == "test":
            experiment_keyword = fan_condition
        android_tcp_port = safe_int(dialog_result["android_tcp_port"], 8100)
        cooldown_minutes = max(0, safe_int(dialog_result.get("cooldown_minutes", 10), 10))

        print("[batch_recording_android] Video playback: Disabled")
        print(f"[batch_recording_android] Android TCP port: {android_tcp_port}")
        print(f"[batch_recording_android] Experiment keyword: {experiment_keyword}")
        print(f"[batch_recording_android] Recorder: {recorder_name}")
        print(f"[batch_recording_android] Fan condition: {fan_condition}")
        print(f"[batch_recording_android] Cooldown: {cooldown_minutes}min")
        if load_mode:
            print(f"[batch_recording_android] Load mode: {load_mode}")

        trajectory_pairs = parse_trajectory_list(traj_list_str)

        if not trajectory_pairs:
            print("[batch_recording_android] 错误：没有有效的轨迹对，脚本结束")
        else:
            print(f"[batch_recording_android] 将要执行 {len(trajectory_pairs)} 个轨迹录制")
            for traj_id, taker_number in trajectory_pairs:
                print(f"[batch_recording_android]   - 轨迹ID={traj_id}, 录制次数={taker_number}")

            success_count = 0
            failed_count = 0

            ur_agent = script_agents.get("ur_node")
            android_agent = script_agents.get("android")

            initial_steps = [
                WorkflowStep.NODES_CHECK,
                WorkflowStep.START_DEVICE,
                WorkflowStep.MOVE_TO_START,
                WorkflowStep.START_RECORD,
                WorkflowStep.EXECUTE_TRAJECTORY,
                WorkflowStep.STOP_RECORD,
                WorkflowStep.COPY_UR_FILES,
            ]
            set_steps(initial_steps, title="UR + Android IMU 批量录制准备")
            set_step(WorkflowStep.NODES_CHECK, "running", "正在检查节点连接")

            missing_agents = []
            if ur_agent is None:
                missing_agents.append("UR_node")
            if android_agent is None:
                missing_agents.append("android")

            if missing_agents:
                agent_target_info = {
                    "UR_node": "目标IP=192.168.10.213，goal_port=5557，feedback_port=5558",
                    "android": f"目标IP=localhost，TCP端口={android_tcp_port}",
                }
                error_lines = ["当前脚本缺失以下 node:"]
                for agent_name in missing_agents:
                    target_info = agent_target_info.get(agent_name, "目标IP=未知，端口=未知")
                    error_lines.append(f"- {agent_name}: {target_info}")
                error_message = "\n".join(error_lines)
                set_step(WorkflowStep.NODES_CHECK, "failed", error_message)
                finish(False, error_message)
            else:
                set_step(WorkflowStep.NODES_CHECK, "success", "所需节点已连接")

                set_step(WorkflowStep.START_DEVICE, "running", f"正在重启 Android TCP:{android_tcp_port}")
                start_device_result = android_agent.cmd("restart_device", {"tcp_port": android_tcp_port})
                if not start_device_result.get("success"):
                    error_message = f"Android restart_device 失败: {start_device_result}"
                    set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                    finish(False, error_message)
                else:
                    android_device_started = True
                    set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启")

                    if fan_speed is not None:
                        print(f"[batch_recording_android] Setting fan speed: {fan_speed}")
                        fan_result = android_agent.cmd("set_fan", {"speed": fan_speed})
                        if not fan_result.get("success"):
                            error_message = f"Android set_fan 失败: {fan_result}"
                            set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                            finish(False, error_message)
                            raise RuntimeError(error_message)
                        fan_configured = True
                        set_step(WorkflowStep.START_DEVICE, "success", f"Android 采集链路已重启，风扇={fan_speed}")

                    if load_mode:
                        print(f"[batch_recording_android] Starting Android load: {load_mode}")
                        load_result = android_agent.cmd("start_load", {"mode": load_mode})
                        if not load_result.get("success"):
                            error_message = f"Android start_load 失败: {load_result}"
                            set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                            finish(False, error_message)
                            raise RuntimeError(error_message)
                        load_started = True
                        if load_mode == "wave":
                            set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启，风扇=100，变负载运行中")
                        else:
                            set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启，风扇=100，高负载运行中")

                    if fan_cycle_mode:
                        print(f"[batch_recording_android] Starting fan cycle: {fan_cycle_mode}")
                        fan_cycle_result = android_agent.cmd("start_fan_cycle", {"mode": fan_cycle_mode})
                        if not fan_cycle_result.get("success"):
                            error_message = f"Android start_fan_cycle 失败: {fan_cycle_result}"
                            set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                            finish(False, error_message)
                            raise RuntimeError(error_message)
                        fan_cycle_started = True
                        set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启，风扇0/100循环运行中")

                    if gps_cycle_mode:
                        print(f"[batch_recording_android] Starting GPS cycle: {gps_cycle_mode}")
                        gps_cycle_result = android_agent.cmd("start_gps_cycle", {"mode": gps_cycle_mode})
                        if not gps_cycle_result.get("success"):
                            error_message = f"Android start_gps_cycle 失败: {gps_cycle_result}"
                            set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                            finish(False, error_message)
                            raise RuntimeError(error_message)
                        gps_cycle_started = True
                        set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启，GPS开关循环运行中")

                    for index, (traj_id, taker_number) in enumerate(trajectory_pairs, 1):
                        print(f"\n[batch_recording_android] ========================================")
                        print(f"[batch_recording_android] 开始执行第 {index}/{len(trajectory_pairs)} 个轨迹")
                        print(f"[batch_recording_android] 轨迹ID={traj_id}, 录制次数={taker_number}")
                        print(f"[batch_recording_android] ========================================\n")

                        steps = [
                            WorkflowStep.NODES_CHECK,
                            WorkflowStep.START_DEVICE,
                            WorkflowStep.MOVE_TO_START,
                            WorkflowStep.START_RECORD,
                            WorkflowStep.EXECUTE_TRAJECTORY,
                            WorkflowStep.STOP_RECORD,
                            WorkflowStep.COPY_UR_FILES,
                        ]
                        set_steps(steps, title=f"第 {index}/{len(trajectory_pairs)} 条轨迹")
                        set_step(WorkflowStep.NODES_CHECK, "success", "所需节点已连接，Android 采集链路已重启")
                        set_step(WorkflowStep.START_DEVICE, "success", "Android 采集链路已重启")

                        run_success = True
                        error_message = ""
                        final_record_time = None
                        android_recording_started = False

                        try:
                            program_name, program_id_str = get_program_config(traj_id)

                            set_step(WorkflowStep.MOVE_TO_START, "running", "正在移动到起始位")
                            move_result = ur_agent.cmd(
                                "move_to_start",
                                {"program_name": program_name},
                                wait_for_result=False,
                            )
                            move_goal_id = move_result.get("goal_id")
                            if wait_for_command(ur_agent, move_goal_id, "move_to_start", progress_interval=10):
                                set_step(WorkflowStep.MOVE_TO_START, "success", "move_to_start 成功")
                            else:
                                error_message = "move_to_start 失败"
                                set_step(WorkflowStep.MOVE_TO_START, "failed", error_message)
                                finish(False, error_message)
                                failed_count += 1
                                continue

                            date_string, file_name, dataset_name = build_record_dataset_name(
                                program_id_str,
                                taker_number,
                                experiment_keyword,
                                recorder_name,
                            )
                            print(f"[batch_recording_android] 生成录制目录: {file_name}")

                            set_step(WorkflowStep.START_RECORD, "running", "正在开始录制 Android IMU")
                            start_record_result = android_agent.cmd("start_record", {"dataset_name": dataset_name})
                            if start_record_result.get("success"):
                                android_recording_started = True
                                set_step(WorkflowStep.START_RECORD, "success", "Android start_record 成功")
                            else:
                                error_message = f"Android start_record 失败: {start_record_result}"
                                set_step(WorkflowStep.START_RECORD, "failed", error_message)
                                finish(False, error_message)
                                failed_count += 1
                                continue

                            set_step(WorkflowStep.EXECUTE_TRAJECTORY, "running", "正在执行轨迹")
                            traj_result = ur_agent.cmd(
                                "execute_trajectory",
                                {
                                    "program_name": program_name,
                                    "record_data": 1,
                                    "save_subpath": file_name,
                                },
                                wait_for_result=False,
                            )
                            traj_goal_id = traj_result.get("goal_id")
                            if not traj_goal_id:
                                error_message = f"execute_trajectory 启动失败: {traj_result}"
                                set_step(WorkflowStep.EXECUTE_TRAJECTORY, "failed", error_message)
                                finish(False, error_message)
                                failed_count += 1
                                stop_android_record_if_needed(android_agent, android_recording_started)
                                android_recording_started = False
                                continue

                            if wait_for_command(ur_agent, traj_goal_id, "execute_trajectory", progress_interval=100):
                                set_step(WorkflowStep.EXECUTE_TRAJECTORY, "success", "execute_trajectory 成功")
                            else:
                                run_success = False
                                if not error_message:
                                    error_message = "execute_trajectory 失败"
                                set_step(WorkflowStep.EXECUTE_TRAJECTORY, "failed", "execute_trajectory 失败")

                            if android_recording_started:
                                delay = time_delay()
                                last_record_time = record_timer()
                                wait_delay_start = time.time()
                                while delay is None and time.time() - wait_delay_start < 30:
                                    delay = time_delay()
                                    sleep(1)

                                if delay is not None:
                                    delay_seconds = delay / 1000.0
                                    wait_time = min(int(delay_seconds * 2 + 0.999), int(delay_seconds) + 10)
                                    set_step(WorkflowStep.START_RECORD, "success", f"录制中，等待尾段 {wait_time}s")
                                    deadline = time.time() + wait_time
                                    while time.time() < deadline:
                                        current_record_time = record_timer()
                                        if current_record_time is not None and last_record_time is not None:
                                            if current_record_time >= last_record_time + wait_time:
                                                break
                                        sleep(1)
                                else:
                                    print("[batch_recording_android] No time_delay available, waiting default 2 seconds...")
                                    sleep(2)

                            final_record_time = record_timer()

                            set_step(WorkflowStep.STOP_RECORD, "running", "正在停止 Android IMU 录制")
                            if android_recording_started:
                                stop_record_result = android_agent.cmd("stop_record", {})
                                android_recording_started = False
                                if stop_record_result.get("success"):
                                    set_step(WorkflowStep.STOP_RECORD, "success", "Android stop_record 成功")
                                else:
                                    run_success = False
                                    if not error_message:
                                        error_message = f"Android stop_record 失败: {stop_record_result}"
                                    set_step(WorkflowStep.STOP_RECORD, "failed", f"Android stop_record 失败: {stop_record_result}")
                            else:
                                set_step(WorkflowStep.STOP_RECORD, "failed", "Android 未开始录制")

                            if final_record_time is not None and final_record_time > 0:
                                wait_for_flush = max(1.0, min(final_record_time * 0.1, 5.0))
                                sleep(wait_for_flush)
                            else:
                                sleep(1)

                            set_step(WorkflowStep.COPY_UR_FILES, "running", "正在复制 UR 文件到 Android 录制目录")
                            ur_root_path_result = ur_agent.cmd("get_root_path", {})
                            android_root_path_result = android_agent.cmd("get_root_path", {})
                            ur_root_path = extract_root_path(ur_root_path_result)
                            android_root_path = extract_root_path(android_root_path_result)
                            ur_file_path = ur_root_path + "/" + date_string + "/" + file_name + "/*"
                            android_dir_path = android_root_path + "/" + date_string + "/" + file_name + "/"

                            def update_copy_progress(progress_message):
                                set_step(WorkflowStep.COPY_UR_FILES, "running", progress_message)

                            copy_result = ur_agent.copy_folder_from_remote(
                                remote_path=ur_file_path,
                                local_path=android_dir_path,
                                progress_callback=update_copy_progress,
                            )
                            if copy_result.get("success"):
                                set_step(WorkflowStep.COPY_UR_FILES, "success", "UR 文件复制成功")
                            else:
                                run_success = False
                                if not error_message:
                                    error_message = f"copy_ur_files 失败: {copy_result}"
                                set_step(WorkflowStep.COPY_UR_FILES, "failed", f"copy_ur_files 失败: {copy_result}")

                            if run_success:
                                success_count += 1
                                finish(True, "本条轨迹录制完成")
                                print(f"\n[batch_recording_android] ✓ 第 {index} 个轨迹完成 (traj_id={traj_id}, taker={taker_number})")
                            else:
                                failed_count += 1
                                finish(False, error_message or "轨迹录制失败")
                                print(f"\n[batch_recording_android] ✗ 第 {index} 个轨迹失败 (traj_id={traj_id}, taker={taker_number})")

                        except Exception as e:
                            failed_count += 1
                            stop_android_record_if_needed(android_agent, android_recording_started)
                            android_recording_started = False
                            set_step(WorkflowStep.EXECUTE_TRAJECTORY, "failed", f"执行异常: {e}")
                            finish(False, f"执行异常: {e}")
                            print(f"\n[batch_recording_android] ✗ 第 {index} 个轨迹异常: {e}")

                        if index < len(trajectory_pairs) and cooldown_minutes > 0:
                            cooldown_message = (
                                f"请等待{cooldown_minutes}分钟让设备降温。\n"
                                "如已手动降温，可点击'确定'继续。\n"
                                f"如无操作，{cooldown_minutes}分钟后将自动继续下一条。"
                            )
                            try:
                                dialog.info(
                                    "设备降温提示",
                                    cooldown_message,
                                    timeout_ms=cooldown_minutes * 60 * 1000,
                                )
                            except Exception:
                                print(f"[batch_recording_android] 无法显示弹窗，直接等待{cooldown_minutes}分钟降温...")
                                time.sleep(cooldown_minutes * 60)

                    print(f"\n[batch_recording_android] ========================================")
                    print("[batch_recording_android] 批量录制完成")
                    print(f"[batch_recording_android] 总计: {len(trajectory_pairs)} 个轨迹")
                    print(f"[batch_recording_android] 成功: {success_count} 个")
                    print(f"[batch_recording_android] 失败: {failed_count} 个")
                    print(f"[batch_recording_android] ========================================")
                    stop_fan_cycle_if_needed(android_agent, fan_cycle_started)
                    fan_cycle_started = False
                    stop_gps_cycle_if_needed(android_agent, gps_cycle_started)
                    gps_cycle_started = False
                    stop_load_if_needed(android_agent, load_started)
                    load_started = False
                    if restore_fan_if_needed(android_agent, fan_configured):
                        fan_configured = False

except Exception as e:
    print(f"[batch_recording_android] 脚本执行出错: {e}")
    try:
        stop_android_record_if_needed(android_agent, android_recording_started)
        android_recording_started = False
    except Exception:
        pass
    stop_load_if_needed(android_agent, load_started)
    load_started = False
    stop_fan_cycle_if_needed(android_agent, fan_cycle_started)
    fan_cycle_started = False
    stop_gps_cycle_if_needed(android_agent, gps_cycle_started)
    gps_cycle_started = False
    if restore_fan_if_needed(android_agent, fan_configured):
        fan_configured = False
finally:
    try:
        stop_android_record_if_needed(android_agent, android_recording_started)
    except Exception:
        pass
    try:
        stop_fan_cycle_if_needed(android_agent, fan_cycle_started)
    except Exception:
        pass
    try:
        stop_gps_cycle_if_needed(android_agent, gps_cycle_started)
    except Exception:
        pass
    try:
        stop_load_if_needed(android_agent, load_started)
    except Exception:
        pass
    try:
        restore_fan_if_needed(android_agent, fan_configured)
    except Exception:
        pass
    try:
        stop_android_device_if_needed(android_agent, android_device_started)
    except Exception:
        pass
