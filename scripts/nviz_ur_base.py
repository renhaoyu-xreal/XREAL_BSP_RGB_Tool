"""
NViz & UR 录制公共函数库

提供给脚本使用的通用工具函数，包括：
- 日期时间工具
- 设备控制函数
- 轨迹录制函数
"""


import time

from scripts.common.record_path_helper import (
    resolve_record_glasses_fsn,
    resolve_record_glasses_label,
    sanitize_record_token,
)


def notify_progress(progress_callback, event, status, message):
    """向脚本回调底层事实事件。"""
    if not progress_callback:
        return
    try:
        progress_callback(event, status, message)
    except Exception as e:
        print(f"[nviz_ur_base] Warning: progress_callback failed: {e}")

def get_date_string():
    """获取当天日期字符串，格式: 20260109
    
    注意：time 模块由 ScriptExecutor 提供，无需导入
    """
    return time.strftime("%Y%m%d")


def build_record_dataset_name(
    program_id_str,
    taker_number,
    glasses_sn,
    recorder_name,
    record_start_time=None,
    glasses_fsn=None,
):
    """构造带秒级时间戳的录制目录名和数据集路径。"""
    if record_start_time is None:
        record_start_time = time.localtime()

    date_string = time.strftime("%Y%m%d", record_start_time)
    timestamp_string = time.strftime("%Y%m%d%H%M%S", record_start_time)
    glasses_token = sanitize_record_token(glasses_sn, "UNKNOWN_GLASSES")
    fsn_token = sanitize_record_token(glasses_fsn, "UNKNOWN_FSN")
    recorder_token = sanitize_record_token(recorder_name, "user")
    record_dir_name = (
        f"{program_id_str}-{taker_number}-{glasses_token}-{fsn_token}-"
        f"{recorder_token}-{timestamp_string}"
    )
    dataset_name = date_string + "/" + record_dir_name
    return date_string, record_dir_name, dataset_name


def extract_root_path(command_result):
    """兼容 C/Python agent 的 get_root_path 返回格式。"""
    if not isinstance(command_result, dict):
        return ""
    root_path = command_result.get("root_path")
    if root_path:
        return str(root_path)
    message = command_result.get("message")
    if isinstance(message, dict):
        root_path = message.get("root_path")
        if root_path:
            return str(root_path)
    return ""


def safe_exit_nviz(glasses_nviz_node, message="脚本结束"):
    """安全退出：停止NViz设备并显示消息
    
    Args:
        glasses_nviz_node: glasses_nviz_node agent包装器
        message: 退出消息
    """
    print(f"[nviz_ur_base] {message}")
    
    try:
        print("[nviz_ur_base] Stopping device...")
        glasses_nviz_node.cmd("stop_device", {})
    except:
        print(f"[nviz_ur_base] Warning: stop_device failed")
    
    print("[nviz_ur_base] Cleanup done.")


def get_program_config(traj_id):
    """根据轨迹ID获取程序配置
    
    Args:
        traj_id: 轨迹ID
        
    Returns:
        (program_name, program_id_str): 程序名称和ID字符串
    """
    # 轨迹ID映射表
    traj_map = {
        10: ("3dof_test_3motion_28", "8-3-0"),
        11: ("3dof_front_small_motion_28", "8-3-1"),
        13: ("3dof_left_right_28", "8-3-3"),
        14: ("3dof_up_down_28", "8-3-4"),
        0: ("3dof_test_3motion", "8-3-0"),
        1: ("3dof_front_small_motion", "8-3-1"),
        3: ("3dof_left_right", "8-3-3"),
        4: ("3dof_up_down", "8-3-4"),
        21: ("mag_test1", "9-3-0"),
    }
    
    if traj_id in traj_map:
        return traj_map[traj_id]
    else:
        print(f"[nviz_ur_base] 无效的轨迹ID: {traj_id}，使用默认配置")
        return ("3dof_test_3motion_28", "8-3-0")


def wait_for_command(ur_node, goal_id, command_name="command", progress_interval=100):
    """等待非阻塞命令完成
    
    Args:
        ur_node: UR agent包装器
        goal_id: 命令goal ID
        command_name: 命令名称（用于日志）
        progress_interval: 进度打印间隔（秒）
        
    Returns:
        bool: 命令是否成功
    """
    if not goal_id:
        print(f"[nviz_ur_base] No goal_id for {command_name}, skipping wait")
        return False
    
    check_count = 0
    start_time = time.time()
    while True:
        status = ur_node.check_cmd(goal_id)
        check_count = check_count + 1
        
        if status["done"]:
            if status["status"] == "SUCCEEDED":
                print(f"[nviz_ur_base] {command_name} completed successfully")
                return True
            else:
                print(f"[nviz_ur_base] {command_name} failed: {status['result']}")
                return False
        
        elapsed = time.time() - start_time
        # 定期打印进度
        if check_count % progress_interval == 0:
            print(
                f"[nviz_ur_base] Waiting for {command_name}... "
                f"({int(elapsed)}s), last status: {status}"
            )
        
        time.sleep(1)


def execute_trajectory_recording(ur_node, glasses_nviz_node, traj_id, taker_number, glasses_sn, recorder_name, time_delay_func, record_time_func, localhost_node=None, enable_video=False, progress_callback=None, video_path=None):
    """执行UR轨迹录制完整流程
    
    Args:
        ur_node: UR agent包装器
        glasses_nviz_node: glasses_nviz_node agent包装器
        traj_id: 轨迹ID
        taker_number: 第几次录制
        glasses_sn: 眼镜序列号
        recorder_name: 记录人名称
        time_delay_func: 获取时间延迟的函数（全局状态）
        record_time_func: 获取录制时长的函数（全局状态）
        localhost_node: localhost agent包装器（可选）
        enable_video: 是否播放视频（需要localhost_node）
        video_path: 可选视频路径；为空时使用 localhost 脚本默认视频
        progress_callback: 可选进度回调 progress_callback(event, status, message)
        
    Returns:
        bool: 录制是否成功
    """
    print(f"[nviz_ur_base] 录制参数: traj_id={traj_id}, glasses_sn={glasses_sn}, recorder_name={recorder_name}, taker_number={taker_number}")
    video_success = True
    video_started = False
    
    # 在开始录制前，先关闭视频（如果之前有播放）
    if enable_video and localhost_node:
        print("[nviz_ur_base] Stopping any previous video playback...")
        localhost_node.cmd("stop_video.sh")
    
    # 获取程序配置
    program_name, program_id_str = get_program_config(traj_id)
    print(f"[nviz_ur_base] 使用程序名称: {program_name}")
    
    # 启动NViz设备
    notify_progress(progress_callback, "start_device", "running", "正在启动 NViz 设备")
    print("[nviz_ur_base] Starting NViz agent...")
    result = glasses_nviz_node.cmd("start_device", {"data_type": "3dof"})
    print(f"[nviz_ur_base] start_device result: {result}")
    if result.get("success"):
        notify_progress(progress_callback, "start_device", "success", "NViz 设备已启动")
    else:
        notify_progress(progress_callback, "start_device", "failed", f"start_device 失败: {result}")
        return False

    glasses_label = resolve_record_glasses_label(
        agent=glasses_nviz_node,
        agent_names=("glasses_nviz_node",),
        preferred_label=glasses_sn,
        allow_ssh=False,
    )
    glasses_fsn = resolve_record_glasses_fsn(
        agent=glasses_nviz_node,
        allow_ssh=False,
    )
    print(f"[nviz_ur_base] 当前眼镜标识: {glasses_label}")
    print(f"[nviz_ur_base] 当前眼镜 FSN: {glasses_fsn}")
    
    # UR移动到起始位置（非阻塞）
    notify_progress(progress_callback, "move_to_start", "running", "正在移动 UR 到起始位")
    print("[nviz_ur_base] Moving UR to start position...")
    move_result = ur_node.cmd("move_to_start", {"program_name": program_name}, wait_for_result=False)
    move_goal_id = move_result.get("goal_id")
    
    if not wait_for_command(ur_node, move_goal_id, "move_to_start", progress_interval=10):
        notify_progress(progress_callback, "move_to_start", "failed", "move_to_start 失败")
        print("[nviz_ur_base] Move to start failed, aborting")
        return False
    notify_progress(progress_callback, "move_to_start", "success", "已移动到起始位")

    date_string, file_name, dataset_name = build_record_dataset_name(
        program_id_str,
        taker_number,
        glasses_label,
        recorder_name,
        glasses_fsn=glasses_fsn,
    )
    print(f"[nviz_ur_base] 生成录制目录: {file_name}")
    
    # 执行轨迹（非阻塞）
    notify_progress(progress_callback, "execute_trajectory", "running", "正在执行轨迹")
    print("[nviz_ur_base] Executing trajectory...")
    traj_result = ur_node.cmd("execute_trajectory", {
        "program_name": program_name,
        "record_data": 1,
        "save_subpath": file_name
    }, wait_for_result=False)
    traj_goal_id = traj_result.get("goal_id")
    
    # 开始录制
    notify_progress(progress_callback, "start_record", "running", "正在开始录制")
    print("[nviz_ur_base] Starting NViz recording...")
    result = glasses_nviz_node.cmd("start_record", {"dataset_name": dataset_name})
    print(f"[nviz_ur_base] start_record result: {result}")
    if result.get("success"):
        notify_progress(progress_callback, "start_record", "running", "录制中，等待轨迹和录制尾段结束")
    else:
        notify_progress(progress_callback, "start_record", "failed", f"start_record 失败: {result}")
    
    # 开始录制后，播放视频（如果启用）
    if enable_video and localhost_node and result.get("success"):
        notify_progress(progress_callback, "play_video", "running", "正在播放视频")
        print("[nviz_ur_base] Starting video playback on secondary screen...")
        play_args = [video_path] if video_path else []
        video_result = localhost_node.cmd("play_video_on_secondary_screen.py", {"args": play_args})
        print(f"[nviz_ur_base] play_video result: {video_result}")
        if video_result.get("success"):
            video_started = True
            notify_progress(progress_callback, "play_video", "running", "视频播放中")
        else:
            video_success = False
            notify_progress(progress_callback, "play_video", "failed", f"播放视频失败: {video_result}")
    elif enable_video:
        video_success = False
        if localhost_node:
            print("[nviz_ur_base] Video playback skipped because start_record failed")
            notify_progress(progress_callback, "play_video", "failed", "start_record 失败，跳过视频播放")
        else:
            print("[nviz_ur_base] Video playback requested, but localhost node is unavailable")
            notify_progress(progress_callback, "play_video", "failed", "localhost node 不可用，无法播放视频")
    
    # 等待轨迹执行完成
    trajectory_success = wait_for_command(ur_node, traj_goal_id, "execute_trajectory", progress_interval=100)
    if not trajectory_success:
        notify_progress(progress_callback, "execute_trajectory", "failed", "轨迹执行失败")
        print("[nviz_ur_base] Trajectory execution failed")
        # 继续执行，尝试保存已有数据
    else:
        notify_progress(progress_callback, "execute_trajectory", "success", "轨迹执行完成")
    

    
    # 等待delay time后停止录制
    print("[nviz_ur_base] Getting delay time from global state...")
    delay = time_delay_func()  # 调用全局状态函数
    last_record_time = record_time_func()

    while delay is None:
        delay = time_delay_func()  # 重新获取最新的delay时间
        time.sleep(1)
    
    if delay is not None:
        delay_seconds = delay / 1000.0
        print(f"[nviz_ur_base] Time delay: {delay}ms ({delay_seconds}s)")
        
        # 向上取整到整数秒
        wait_time = min(int(delay_seconds * 2 + 0.999), int(delay_seconds) + 10)
        print(f"[nviz_ur_base] Waiting {wait_time}s (2x delay time) before stopping record...")

        while wait_time > 0:
            print(f"[nviz_ur_base] Waiting... {wait_time}s remaining")
            record_time = record_time_func()
            if record_time is not None and record_time >= last_record_time + wait_time:
                print(f"[nviz_ur_base] Required delay time reached in recording ({record_time:.2f}s), proceeding to stop.")
                break
            time.sleep(1)
        
    else:
        print("[nviz_ur_base] No delay time available, waiting default 2 seconds...")
        time.sleep(2)

    final_record_time = record_time_func()
    # 停止录制
    notify_progress(progress_callback, "stop_record", "running", "正在停止录制")
    print("[nviz_ur_base] Stopping NViz recording...")
    stop_result = glasses_nviz_node.cmd("stop_record", {})
    print(f"[nviz_ur_base] stop_record result: {stop_result}")
    if stop_result.get("success"):
        notify_progress(progress_callback, "start_record", "success", "录制已结束")
        notify_progress(progress_callback, "stop_record", "success", "stop_record 成功")
    else:
        notify_progress(progress_callback, "start_record", "failed", f"录制停止失败: {stop_result}")
        notify_progress(progress_callback, "stop_record", "failed", f"stop_record 失败: {stop_result}")

    if video_started and localhost_node:
        notify_progress(progress_callback, "play_video", "running", "正在停止视频")
        print("[nviz_ur_base] Stopping video playback...")
        stop_video_result = localhost_node.cmd("stop_video.sh", {})
        print(f"[nviz_ur_base] stop_video result: {stop_video_result}")
        if stop_video_result.get("success"):
            notify_progress(progress_callback, "play_video", "success", "视频已停止")
        else:
            video_success = False
            notify_progress(progress_callback, "play_video", "failed", f"停止视频失败: {stop_video_result}")
    
    # 停止设备
    notify_progress(progress_callback, "stop_device", "running", "正在停止设备")
    print("[nviz_ur_base] Stopping NViz device...")
    stop_device_result = glasses_nviz_node.cmd("stop_device")
    print(f"[nviz_ur_base] stop_device result: {stop_device_result}")
    if stop_device_result.get("success"):
        notify_progress(progress_callback, "stop_device", "success", "stop_device 成功")
    else:
        notify_progress(progress_callback, "stop_device", "failed", f"stop_device 失败: {stop_device_result}")

    # 等待数据落盘（根据录制时长计算等待时间）
    if final_record_time is not None and final_record_time > 0:
        # 等待时间 = max(1秒, min(录制时长 * 10%, 5秒))
        wait_for_flush = max(1.0, min(final_record_time * 0.1, 5.0))
        print(f"[nviz_ur_base] Waiting {wait_for_flush:.1f}s for data to flush to disk (record time: {final_record_time:.1f}s)...")
        time.sleep(wait_for_flush)
    else:
        print("[nviz_ur_base] Waiting 1s for data to flush to disk...")
        time.sleep(1)

    # 复制UR数据文件
    print("[nviz_ur_base] Copying UR files...")
    ur_root_path_result = ur_node.cmd("get_root_path", {})
    ur_root_path = extract_root_path(ur_root_path_result)
    print(f"[nviz_ur_base] UR root path: {ur_root_path}")
    
    ur_file_path = ur_root_path + "/" + date_string + "/" + file_name + "/*"
    
    nviz_root_path_result = glasses_nviz_node.cmd("get_root_path", {})
    nviz_root_path = extract_root_path(nviz_root_path_result)
    print(f"[nviz_ur_base] NViz root path: {nviz_root_path}")
    
    nviz_dir_path = nviz_root_path + "/" + date_string + "/" + file_name + "/"
    
    print(f"[nviz_ur_base] Copying from {ur_file_path} to {nviz_dir_path}")
    notify_progress(progress_callback, "copy_ur_files", "running", "正在复制 UR 文件")

    def update_copy_progress(progress_message):
        notify_progress(progress_callback, "copy_ur_files", "running", progress_message)

    copy_result = ur_node.copy_folder_from_remote(
        remote_path=ur_file_path,
        local_path=nviz_dir_path,
        progress_callback=update_copy_progress,
    )
    print(f"[nviz_ur_base] Copy result: {copy_result}")
    if copy_result.get("success"):
        notify_progress(progress_callback, "copy_ur_files", "success", "UR 文件复制成功")
    else:
        notify_progress(progress_callback, "copy_ur_files", "failed", f"UR 文件复制失败: {copy_result}")

    
    print(f"[nviz_ur_base] Trajectory {program_name} recording completed.")
    return bool(
        result.get("success")
        and video_success
        and trajectory_success
        and stop_result.get("success")
        and stop_device_result.get("success")
        and copy_result.get("success")
    )
