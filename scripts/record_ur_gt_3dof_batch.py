# Batch recording script: execute multiple trajectories sequentially
# Input format: traj_id-taker_number pairs (e.g., "10-1, 11-2, 13-3")
# Each pair will be executed in order

from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps
from nviz_ur_base import (
    build_record_dataset_name,
    extract_root_path,
    get_program_config,
    safe_exit_nviz,
    wait_for_command,
)
from scripts.common.record_path_helper import (
    resolve_record_glasses_fsn,
    resolve_record_glasses_label,
)


all_agent_names = ['glasses_nviz_node', 'UR_node', 'localhost']

print(f"[batch_recording] Starting batch recording script...")
print(f"[batch_recording] Available agents: {all_agent_names}")

initial_steps = [
    WorkflowStep.NODES_CHECK,
    WorkflowStep.START_DEVICE,
    WorkflowStep.MOVE_TO_START,
    WorkflowStep.EXECUTE_TRAJECTORY,
    WorkflowStep.START_RECORD,
    WorkflowStep.PLAY_VIDEO,
    WorkflowStep.STOP_RECORD,
    WorkflowStep.STOP_DEVICE,
    WorkflowStep.COPY_UR_FILES,
]
set_steps(initial_steps, title="批量录制")
set_step(WorkflowStep.NODES_CHECK, "running", "正在检查节点连接")


def fail_if_unavailable_agents():
    unavailable = globals().get("unavailable_script_agents") or {}
    if not unavailable:
        return
    error_lines = ["当前脚本缺失以下 node:"]
    for agent_name, reason in unavailable.items():
        error_lines.append(f"- {agent_name}: {reason}")
    error_message = "\n".join(error_lines)
    set_step(WorkflowStep.NODES_CHECK, "failed", error_message)
    finish(False, error_message)
    raise SystemExit(1)


def parse_trajectory_list(traj_list_str):
    """解析轨迹列表字符串

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
                    print(f"[batch_recording] Warning: 无法解析 '{item}'，跳过")
        else:
            print(f"[batch_recording] Warning: 格式错误 '{item}'，需要使用 'traj_id-taker_number' 格式")

    return result


try:
    fail_if_unavailable_agents()

    fields = [
        {"name": "traj_list", "label": "轨迹列表 (格式: traj_id-taker, 逗号分隔)", "default": "10-1, 11-1, 13-1"},
        {"name": "glasses_sn", "label": "眼镜标识（空=自动识别）", "default": ""},
        {"name": "recorder_name", "label": "记录人名称", "default": "xlz"},
        {"name": "play_video", "label": "是否播放视频 (1=是, 0=否)", "default": "1"},
        {"name": "video_path", "label": "视频路径（空=默认 old_video.mp4）", "default": ""},
    ]

    dialog_result = dialog.multi_field_input("批量录制参数设置", "请输入批量录制参数", fields)

    if not dialog_result:
        print("[batch_recording] 用户取消输入")
    else:
        traj_list_str = dialog_result["traj_list"]
        glasses_sn = dialog_result["glasses_sn"]
        recorder_name = dialog_result["recorder_name"]
        enable_video = int(dialog_result["play_video"]) == 1
        video_path = dialog_result.get("video_path", "").strip()

        print(f"[batch_recording] Video playback: {'Enabled' if enable_video else 'Disabled'}")

        trajectory_pairs = parse_trajectory_list(traj_list_str)

        if not trajectory_pairs:
            print("[batch_recording] 错误：没有有效的轨迹对，脚本结束")
        else:
            print(f"[batch_recording] 将要执行 {len(trajectory_pairs)} 个轨迹录制")
            for traj_id, taker_number in trajectory_pairs:
                print(f"[batch_recording]   - 轨迹ID={traj_id}, 录制次数={taker_number}")

            success_count = 0
            failed_count = 0

            # 小写agent名称获取
            glasses_agent = script_agents.get("glasses_nviz_node")
            ur_agent = script_agents.get("ur_node")
            localhost_agent = script_agents.get("localhost")

            for index, (traj_id, taker_number) in enumerate(trajectory_pairs, 1):
                print(f"\n[batch_recording] ========================================")
                print(f"[batch_recording] 开始执行第 {index}/{len(trajectory_pairs)} 个轨迹")
                print(f"[batch_recording] 轨迹ID={traj_id}, 录制次数={taker_number}")
                print(f"[batch_recording] ========================================\n")

                steps = [
                    WorkflowStep.NODES_CHECK,
                    WorkflowStep.START_DEVICE,
                    WorkflowStep.MOVE_TO_START,
                    WorkflowStep.EXECUTE_TRAJECTORY,
                    WorkflowStep.START_RECORD,
                    WorkflowStep.PLAY_VIDEO,
                    WorkflowStep.STOP_RECORD,
                    WorkflowStep.STOP_DEVICE,
                    WorkflowStep.COPY_UR_FILES,
                ]
                set_steps(steps, title=f"第 {index}/{len(trajectory_pairs)} 条轨迹")

                set_step(WorkflowStep.NODES_CHECK, "running", "正在检查节点连接")
                missing_agents = []
                if glasses_agent is None:
                    missing_agents.append("glasses_nviz_node")
                if ur_agent is None:
                    missing_agents.append("UR_node")
                if localhost_agent is None:
                    missing_agents.append("localhost")

                if missing_agents:
                    agent_target_info = {
                        "glasses_nviz_node": "目标IP=localhost，goal_port=5692，feedback_port=5693",
                        "UR_node": "目标IP=192.168.10.213，goal_port=5557，feedback_port=5558",
                        "localhost": "目标IP=localhost，无独立端口",
                    }
                    error_lines = ["当前脚本缺失以下 node:"]
                    for agent_name in missing_agents:
                        target_info = agent_target_info.get(agent_name, "目标IP=未知，端口=未知")
                        error_lines.append(f"- {agent_name}: {target_info}")
                    error_message = "\n".join(error_lines)
                    set_step(WorkflowStep.NODES_CHECK, "failed", error_message)
                    finish(False, error_message)
                    failed_count += 1
                    continue

                set_step(WorkflowStep.NODES_CHECK, "success", "所有节点已连接")

                try:
                    program_name, program_id_str = get_program_config(traj_id)
                    run_success = True
                    error_message = ""
                    video_started = False

                    if enable_video and localhost_agent:
                        print("[batch_recording] Stopping any previous video playback...")
                        localhost_agent.cmd("stop_video.sh")

                    set_step(WorkflowStep.START_DEVICE, "running", "正在启动 NViz 设备")
                    start_device_result = glasses_agent.cmd("start_device", {"data_type": "3dof"})
                    if start_device_result.get("success"):
                        set_step(WorkflowStep.START_DEVICE, "success", "start_device 成功")
                    else:
                        error_message = f"start_device 失败: {start_device_result}"
                        set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                        finish(False, error_message)
                        failed_count += 1
                        safe_exit_nviz(glasses_agent, "设备启动失败，执行清理")
                        continue

                    glasses_label = resolve_record_glasses_label(
                        agent=glasses_agent,
                        agent_names=("glasses_nviz_node",),
                        preferred_label=glasses_sn,
                        allow_ssh=False,
                    )
                    glasses_fsn = resolve_record_glasses_fsn(
                        agent=glasses_agent,
                        allow_ssh=False,
                    )
                    print(f"[batch_recording] 当前眼镜标识: {glasses_label}")
                    print(f"[batch_recording] 当前眼镜 FSN: {glasses_fsn}")

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
                        safe_exit_nviz(glasses_agent, "移动到起始位失败，执行清理")
                        continue

                    date_string, file_name, dataset_name = build_record_dataset_name(
                        program_id_str,
                        taker_number,
                        glasses_label,
                        recorder_name,
                        glasses_fsn=glasses_fsn,
                    )
                    print(f"[batch_recording] 生成录制目录: {file_name}")

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
                        safe_exit_nviz(glasses_agent, "轨迹启动失败，执行清理")
                        continue

                    set_step(WorkflowStep.START_RECORD, "running", "正在开始录制")
                    start_record_result = glasses_agent.cmd("start_record", {"dataset_name": dataset_name})
                    if start_record_result.get("success"):
                        set_step(WorkflowStep.START_RECORD, "running", "录制中")
                    else:
                        run_success = False
                        error_message = f"start_record 失败: {start_record_result}"
                        set_step(WorkflowStep.START_RECORD, "failed", error_message)

                    if enable_video and localhost_agent and start_record_result.get("success"):
                        set_step(WorkflowStep.PLAY_VIDEO, "running", "正在播放视频")
                        print("[batch_recording] Starting video playback on secondary screen...")
                        play_args = [video_path] if video_path else []
                        video_result = localhost_agent.cmd("play_video_on_secondary_screen.py", {"args": play_args})
                        print(f"[batch_recording] play_video result: {video_result}")
                        if video_result.get("success"):
                            video_started = True
                            set_step(WorkflowStep.PLAY_VIDEO, "running", "视频播放中")
                        else:
                            run_success = False
                            if not error_message:
                                error_message = f"播放视频失败: {video_result}"
                            set_step(WorkflowStep.PLAY_VIDEO, "failed", f"播放视频失败: {video_result}")
                    elif enable_video:
                        run_success = False
                        if not error_message:
                            if localhost_agent:
                                error_message = "start_record 失败，跳过视频播放"
                            else:
                                error_message = "localhost node 不可用，无法播放视频"
                        set_step(WorkflowStep.PLAY_VIDEO, "failed", error_message)
                    else:
                        set_step(WorkflowStep.PLAY_VIDEO, "success", "未启用视频播放")

                    if wait_for_command(ur_agent, traj_goal_id, "execute_trajectory", progress_interval=100):
                        set_step(WorkflowStep.EXECUTE_TRAJECTORY, "success", "execute_trajectory 成功")
                    else:
                        run_success = False
                        if not error_message:
                            error_message = "execute_trajectory 失败"
                        set_step(WorkflowStep.EXECUTE_TRAJECTORY, "failed", "execute_trajectory 失败")

                    if start_record_result.get("success"):
                        delay = time_delay()
                        last_record_time = record_timer()
                        while delay is None:
                            delay = time_delay()
                            sleep(1)

                        if delay is not None:
                            delay_seconds = delay / 1000.0
                            wait_time = min(int(delay_seconds * 2 + 0.999), int(delay_seconds) + 10)
                            set_step(WorkflowStep.START_RECORD, "running", f"录制中，等待尾段 {wait_time}s")
                            deadline = time.time() + wait_time
                            while time.time() < deadline:
                                current_record_time = record_timer()
                                if current_record_time is not None and last_record_time is not None:
                                    if current_record_time >= last_record_time + wait_time:
                                        break
                                sleep(1)
                        else:
                            sleep(2)

                    final_record_time = record_timer()

                    set_step(WorkflowStep.STOP_RECORD, "running", "正在停止录制")
                    stop_record_result = glasses_agent.cmd("stop_record", {})
                    if stop_record_result.get("success"):
                        set_step(WorkflowStep.START_RECORD, "success", "录制已结束")
                        set_step(WorkflowStep.STOP_RECORD, "success", "stop_record 成功")
                    else:
                        run_success = False
                        if not error_message:
                            error_message = f"stop_record 失败: {stop_record_result}"
                        set_step(WorkflowStep.START_RECORD, "failed", f"录制停止失败: {stop_record_result}")
                        set_step(WorkflowStep.STOP_RECORD, "failed", f"stop_record 失败: {stop_record_result}")

                    if video_started and localhost_agent:
                        set_step(WorkflowStep.PLAY_VIDEO, "running", "正在停止视频")
                        stop_video_result = localhost_agent.cmd("stop_video.sh", {})
                        if stop_video_result.get("success"):
                            set_step(WorkflowStep.PLAY_VIDEO, "success", "视频已停止")
                        else:
                            run_success = False
                            if not error_message:
                                error_message = f"停止视频失败: {stop_video_result}"
                            set_step(WorkflowStep.PLAY_VIDEO, "failed", error_message)

                    set_step(WorkflowStep.STOP_DEVICE, "running", "正在停止设备")
                    stop_device_result = glasses_agent.cmd("stop_device")
                    if stop_device_result.get("success"):
                        set_step(WorkflowStep.STOP_DEVICE, "success", "stop_device 成功")
                    else:
                        run_success = False
                        if not error_message:
                            error_message = f"stop_device 失败: {stop_device_result}"
                        set_step(WorkflowStep.STOP_DEVICE, "failed", f"stop_device 失败: {stop_device_result}")

                    if final_record_time is not None and final_record_time > 0:
                        wait_for_flush = max(1.0, min(final_record_time * 0.1, 5.0))
                        sleep(wait_for_flush)
                    else:
                        sleep(1)

                    set_step(WorkflowStep.COPY_UR_FILES, "running", "正在复制 UR 文件")
                    ur_root_path_result = ur_agent.cmd("get_root_path", {})
                    nviz_root_path_result = glasses_agent.cmd("get_root_path", {})
                    ur_root_path = extract_root_path(ur_root_path_result)
                    nviz_root_path = extract_root_path(nviz_root_path_result)
                    if not ur_root_path or not nviz_root_path:
                        raise RuntimeError(
                            f"get_root_path failed: UR={ur_root_path_result}, NViz={nviz_root_path_result}"
                        )
                    ur_file_path = ur_root_path + "/" + date_string + "/" + file_name + "/*"
                    nviz_dir_path = nviz_root_path + "/" + date_string + "/" + file_name + "/"

                    def update_copy_progress(progress_message):
                        set_step(WorkflowStep.COPY_UR_FILES, "running", progress_message)

                    copy_result = ur_agent.copy_folder_from_remote(
                        remote_path=ur_file_path,
                        local_path=nviz_dir_path,
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
                        print(f"\n[batch_recording] ✓ 第 {index} 个轨迹完成 (traj_id={traj_id}, taker={taker_number})")
                    else:
                        failed_count += 1
                        finish(False, error_message or "轨迹录制失败")
                        print(f"\n[batch_recording] ✗ 第 {index} 个轨迹失败 (traj_id={traj_id}, taker={taker_number})")

                except Exception as e:
                    failed_count += 1
                    set_step(WorkflowStep.EXECUTE_TRAJECTORY, "failed", f"执行异常: {e}")
                    finish(False, f"执行异常: {e}")
                    print(f"\n[batch_recording] ✗ 第 {index} 个轨迹异常: {e}")

                if index < len(trajectory_pairs):
                    cooldown_message = (
                        "请等待2分钟让眼镜降温。\n"
                        "如已手动降温，可点击‘确定’继续。\n"
                        "如无操作，2分钟后将自动继续下一条。"
                    )
                    try:
                        dialog.info(
                            "眼镜降温提示",
                            cooldown_message,
                            timeout_ms=2 * 60 * 1000,
                        )
                    except Exception:
                        print("[batch_recording] 无法显示弹窗，直接等待2分钟降温...")
                        time.sleep(2 * 60)

            print(f"\n[batch_recording] ========================================")
            print(f"[batch_recording] 批量录制完成")
            print(f"[batch_recording] 总计: {len(trajectory_pairs)} 个轨迹")
            print(f"[batch_recording] 成功: {success_count} 个")
            print(f"[batch_recording] 失败: {failed_count} 个")
            print(f"[batch_recording] ========================================")

except Exception as e:
    print(f"[batch_recording] 脚本执行出错: {e}")
    try:
        safe_exit_nviz(glasses_agent, "脚本异常退出")
    except Exception:
        pass
finally:
    try:
        safe_exit_nviz(glasses_agent, "批量录制脚本结束清理")
    except Exception:
        pass
