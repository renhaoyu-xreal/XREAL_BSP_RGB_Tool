# Batch recording script for BSP glasses + UR arm
# Input format: traj_id-taker_number pairs (e.g., "10-1, 11-2, 13-3")
# Each pair will be executed in order

import os

from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps
from nviz_ur_base import (
    build_record_dataset_name,
    extract_root_path,
    get_program_config,
    wait_for_command,
)
from scripts.common.record_path_helper import build_bsp_dataset_name, prompt_bsp_record_labels
from scripts.common.record_path_helper import resolve_bsp_glasses_id, resolve_record_glasses_fsn


HELEN_AGENT_NAME = "helen_node"
LEGACY_BSP_AGENT_NAME = "glasses_bsp_node"

all_agent_names = ["helen_node", "UR_node", "localhost"]

print("[bsp_batch_recording] Starting BSP batch recording script...")
print(f"[bsp_batch_recording] Available agents: {all_agent_names}")

initial_steps = [
    WorkflowStep.NODES_CHECK,
    WorkflowStep.START_DEVICE,
    WorkflowStep.MOVE_TO_START,
    WorkflowStep.EXECUTE_TRAJECTORY,
    WorkflowStep.START_RECORD,
    WorkflowStep.PLAY_VIDEO,
    WorkflowStep.STOP_RECORD,
    WorkflowStep.COPY_UR_FILES,
    WorkflowStep.STOP_DEVICE,
]
set_steps(initial_steps, title="BSP batch recording")
set_step(WorkflowStep.NODES_CHECK, "running", "Waiting for input")


def fail_if_unavailable_agents():
    unavailable = globals().get("unavailable_script_agents") or {}
    if not unavailable:
        return
    error_lines = ["Unavailable nodes:"]
    for agent_name, reason in unavailable.items():
        error_lines.append(f"- {agent_name}: {reason}")
    error_message = "\n".join(error_lines)
    set_step(WorkflowStep.NODES_CHECK, "failed", error_message)
    finish(False, error_message)
    raise SystemExit(1)


def safe_exit_bsp(glasses_bsp_node, message="Script cleanup"):
    print(f"[bsp_batch_recording] {message}")
    if not glasses_bsp_node:
        return
    try:
        glasses_bsp_node.cmd("stop_device", {})
    except Exception:
        print("[bsp_batch_recording] Warning: stop_device failed")


def parse_trajectory_list(traj_list_str):
    """Parse trajectory list string.

    Input format: "10-1, 11-2, 13-3"
    Returns: [(10, 1), (11, 2), (13, 3)]
    """
    result = []
    items = traj_list_str.split(",")

    for item in items:
        item = item.strip()
        if "-" in item:
            parts = item.split("-")
            if len(parts) == 2:
                try:
                    traj_id = int(parts[0].strip())
                    taker_number = int(parts[1].strip())
                    result.append((traj_id, taker_number))
                except Exception:
                    print(f"[bsp_batch_recording] Warning: cannot parse '{item}', skipping")
        else:
            print(
                f"[bsp_batch_recording] Warning: invalid format '{item}', use 'traj_id-taker_number'"
            )

    return result


try:
    fail_if_unavailable_agents()

    record_labels = prompt_bsp_record_labels(dialog)
    if record_labels is None:
        print("[bsp_batch_recording] User canceled record label input")
        raise SystemExit

    experiment_keyword, recorder_name = record_labels

    fields = [
        {
            "name": "traj_list",
            "label": "Trajectory list (traj_id-taker, comma separated)",
            "default": "4-1, 4-2",
        },
        {
            "name": "enable_display",
            "label": "Enable display? (1=yes, 0=no, no-system default=0)",
            "default": "0",
        },
        {
            "name": "play_video",
            "label": "Play video? (1=yes, 0=no)",
            "default": "1",
        },
        {
            "name": "video_path",
            "label": "Video path (empty=default old_video.mp4)",
            "default": "",
        },
    ]

    dialog_result = dialog.multi_field_input(
        "BSP batch recording",
        "Please input BSP batch recording parameters",
        fields,
    )

    if not dialog_result:
        print("[bsp_batch_recording] User canceled input")
        set_step(WorkflowStep.NODES_CHECK, "failed", "Input canceled")
    else:
        traj_list_str = dialog_result["traj_list"]
        enable_display = int(dialog_result.get("enable_display", "0")) == 1
        enable_video = int(dialog_result["play_video"]) == 1
        video_path = dialog_result.get("video_path", "").strip()

        print(f"[bsp_batch_recording] Video playback: {'Enabled' if enable_video else 'Disabled'}")

        trajectory_pairs = parse_trajectory_list(traj_list_str)

        if not trajectory_pairs:
            print("[bsp_batch_recording] Error: no valid trajectory pairs, exiting")
        else:
            print(f"[bsp_batch_recording] Total trajectories: {len(trajectory_pairs)}")
            for traj_id, taker_number in trajectory_pairs:
                print(f"[bsp_batch_recording]   - traj_id={traj_id}, taker={taker_number}")

            success_count = 0
            failed_count = 0

            glasses_agent = script_agents.get(HELEN_AGENT_NAME) or script_agents.get(LEGACY_BSP_AGENT_NAME)
            ur_agent = script_agents.get("ur_node")
            localhost_agent = script_agents.get("localhost")
            device_ready_from_bootstrap = (
                os.environ.get("RECORDLAB_SCRIPT_ASSUME_DEVICE_READY") == "1"
            )
            if device_ready_from_bootstrap:
                print("[bsp_batch_recording] BSP device was pre-started by UI bootstrap")

            for index, (traj_id, taker_number) in enumerate(trajectory_pairs, 1):
                print("\n[bsp_batch_recording] ========================================")
                print(f"[bsp_batch_recording] Start {index}/{len(trajectory_pairs)}")
                print(f"[bsp_batch_recording] traj_id={traj_id}, taker={taker_number}")
                print("[bsp_batch_recording] ========================================\n")

                steps = [
                    WorkflowStep.NODES_CHECK,
                    WorkflowStep.START_DEVICE,
                    WorkflowStep.MOVE_TO_START,
                    WorkflowStep.EXECUTE_TRAJECTORY,
                    WorkflowStep.START_RECORD,
                    WorkflowStep.PLAY_VIDEO,
                    WorkflowStep.STOP_RECORD,
                    WorkflowStep.COPY_UR_FILES,
                    WorkflowStep.STOP_DEVICE,
                ]
                set_steps(steps, title=f"Trajectory {index}/{len(trajectory_pairs)}")

                set_step(WorkflowStep.NODES_CHECK, "running", "Checking node connections")
                missing_agents = []
                if glasses_agent is None:
                    missing_agents.append(HELEN_AGENT_NAME)
                if ur_agent is None:
                    missing_agents.append("UR_node")
                if enable_video and localhost_agent is None:
                    missing_agents.append("localhost")

                if missing_agents:
                    agent_target_info = {
                        HELEN_AGENT_NAME: "IP=localhost, goal_port=5696, feedback_port=5697",
                        "UR_node": "IP=192.168.10.213, goal_port=5557, feedback_port=5558",
                        "localhost": "IP=localhost, no ports",
                    }
                    error_lines = ["Missing nodes:"]
                    for agent_name in missing_agents:
                        target_info = agent_target_info.get(agent_name, "IP=unknown, ports=unknown")
                        error_lines.append(f"- {agent_name}: {target_info}")
                    error_message = "\n".join(error_lines)
                    set_step(WorkflowStep.NODES_CHECK, "failed", error_message)
                    finish(False, error_message)
                    failed_count += 1
                    continue

                set_step(WorkflowStep.NODES_CHECK, "success", "All nodes connected")

                try:
                    program_name, program_id_str = get_program_config(traj_id)
                    run_success = True
                    error_message = ""
                    video_started = False

                    if enable_video and localhost_agent:
                        print("[bsp_batch_recording] Stopping any previous video playback...")
                        localhost_agent.cmd("stop_video.sh")

                    if device_ready_from_bootstrap:
                        set_step(
                            WorkflowStep.START_DEVICE,
                            "success",
                            "BSP device already started by UI",
                        )
                        device_ready_from_bootstrap = False
                    else:
                        set_step(WorkflowStep.START_DEVICE, "running", "Starting BSP device")
                        start_device_result = glasses_agent.cmd(
                            "start_device",
                            {
                                "camera_mode": "none",
                                "enable_display": enable_display,
                            },
                        )
                        start_device_message = str(
                            start_device_result.get("message")
                            or start_device_result.get("error")
                            or start_device_result
                        )
                        if start_device_result.get("success") or "Already started" in start_device_message:
                            set_step(WorkflowStep.START_DEVICE, "success", "start_device success")
                        else:
                            error_message = f"start_device failed: {start_device_result}"
                            set_step(WorkflowStep.START_DEVICE, "failed", error_message)
                            finish(False, error_message)
                            failed_count += 1
                            safe_exit_bsp(glasses_agent, "Device start failed, cleanup")
                            continue

                    set_step(WorkflowStep.MOVE_TO_START, "running", "Moving UR to start")
                    move_result = ur_agent.cmd(
                        "move_to_start",
                        {"program_name": program_name},
                        wait_for_result=False,
                    )
                    move_goal_id = move_result.get("goal_id")
                    if wait_for_command(ur_agent, move_goal_id, "move_to_start", progress_interval=10):
                        set_step(WorkflowStep.MOVE_TO_START, "success", "move_to_start success")
                    else:
                        error_message = "move_to_start failed"
                        set_step(WorkflowStep.MOVE_TO_START, "failed", error_message)
                        finish(False, error_message)
                        failed_count += 1
                        safe_exit_bsp(glasses_agent, "Move to start failed, cleanup")
                        continue

                    glasses_id = resolve_bsp_glasses_id(
                        glasses_agent,
                        agent_names=(HELEN_AGENT_NAME, LEGACY_BSP_AGENT_NAME),
                    )
                    glasses_fsn = resolve_record_glasses_fsn(
                        agent=glasses_agent,
                        allow_ssh=True,
                    )
                    dataset_name, glasses_id = build_bsp_dataset_name(
                        "ur/3dof",
                        experiment_keyword=experiment_keyword,
                        recorder_name=recorder_name,
                        leaf_token_override=f"traj_{traj_id}_t{taker_number}",
                        glasses_id_override=glasses_id,
                        glasses_fsn_override=glasses_fsn,
                        agent=glasses_agent,
                        agent_names=(HELEN_AGENT_NAME, LEGACY_BSP_AGENT_NAME),
                    )

                    date_string, file_name, _ = build_record_dataset_name(
                        program_id_str,
                        taker_number,
                        glasses_id,
                        recorder_name,
                        glasses_fsn=glasses_fsn,
                    )
                    print(f"[bsp_batch_recording] BSP dataset: {dataset_name}")
                    print(f"[bsp_batch_recording] UR save subpath: {file_name}")

                    set_step(WorkflowStep.EXECUTE_TRAJECTORY, "running", "Executing trajectory")
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
                        error_message = f"execute_trajectory failed to start: {traj_result}"
                        set_step(WorkflowStep.EXECUTE_TRAJECTORY, "failed", error_message)
                        finish(False, error_message)
                        failed_count += 1
                        safe_exit_bsp(glasses_agent, "Trajectory start failed, cleanup")
                        continue

                    set_step(WorkflowStep.START_RECORD, "running", "Starting BSP recording")
                    start_record_result = glasses_agent.cmd(
                        "start_record",
                        {
                            "dataset_name": dataset_name,
                            "enable_camera_snapshot": False,
                            "enable_screen_capture": False,
                            "enable_mic_recording": False,
                        },
                    )
                    if start_record_result.get("success"):
                        set_step(WorkflowStep.START_RECORD, "running", "Recording")
                    else:
                        run_success = False
                        error_message = f"start_record failed: {start_record_result}"
                        set_step(WorkflowStep.START_RECORD, "failed", error_message)

                    if enable_video and localhost_agent and start_record_result.get("success"):
                        set_step(WorkflowStep.PLAY_VIDEO, "running", "Playing video")
                        print("[bsp_batch_recording] Starting video playback...")
                        play_args = [video_path] if video_path else []
                        video_result = localhost_agent.cmd(
                            "play_video_on_secondary_screen.py",
                            {"args": play_args},
                        )
                        if video_result.get("success"):
                            video_started = True
                            set_step(WorkflowStep.PLAY_VIDEO, "running", "Video playing")
                        else:
                            run_success = False
                            if not error_message:
                                error_message = f"play_video failed: {video_result}"
                            set_step(WorkflowStep.PLAY_VIDEO, "failed", error_message)
                    elif enable_video:
                        run_success = False
                        if not error_message:
                            error_message = "start_record failed, skip video"
                        set_step(WorkflowStep.PLAY_VIDEO, "failed", error_message)
                    else:
                        set_step(WorkflowStep.PLAY_VIDEO, "success", "Video disabled")

                    if wait_for_command(ur_agent, traj_goal_id, "execute_trajectory", progress_interval=100):
                        set_step(WorkflowStep.EXECUTE_TRAJECTORY, "success", "execute_trajectory success")
                    else:
                        run_success = False
                        if not error_message:
                            error_message = "execute_trajectory failed"
                        set_step(WorkflowStep.EXECUTE_TRAJECTORY, "failed", "execute_trajectory failed")

                    if start_record_result.get("success"):
                        delay = time_delay()
                        last_record_time = record_timer()
                        delay_wait_count = 0
                        while delay is None:
                            delay_wait_count += 1
                            if delay_wait_count == 1 or delay_wait_count % 5 == 0:
                                print(f"[bsp_batch_recording] Waiting for time_delay data... {delay_wait_count}s")
                            delay = time_delay()
                            sleep(1)

                        if delay is not None:
                            delay_seconds = delay / 1000.0
                            wait_time = min(int(delay_seconds * 2 + 0.999), int(delay_seconds) + 10)
                            set_step(WorkflowStep.START_RECORD, "running", f"Recording tail {wait_time}s")
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

                    set_step(WorkflowStep.STOP_RECORD, "running", "Stopping recording")
                    stop_record_result = glasses_agent.cmd("stop_record", {})
                    if stop_record_result.get("success"):
                        set_step(WorkflowStep.START_RECORD, "success", "Recording finished")
                        set_step(WorkflowStep.STOP_RECORD, "success", "stop_record success")
                    else:
                        run_success = False
                        if not error_message:
                            error_message = f"stop_record failed: {stop_record_result}"
                        set_step(WorkflowStep.START_RECORD, "failed", error_message)
                        set_step(WorkflowStep.STOP_RECORD, "failed", error_message)

                    if video_started and localhost_agent:
                        set_step(WorkflowStep.PLAY_VIDEO, "running", "Stopping video")
                        stop_video_result = localhost_agent.cmd("stop_video.sh", {})
                        if stop_video_result.get("success"):
                            set_step(WorkflowStep.PLAY_VIDEO, "success", "Video stopped")
                        else:
                            run_success = False
                            if not error_message:
                                error_message = f"stop_video failed: {stop_video_result}"
                            set_step(WorkflowStep.PLAY_VIDEO, "failed", error_message)

                    if final_record_time is not None and final_record_time > 0:
                        wait_for_flush = max(1.0, min(final_record_time * 0.1, 5.0))
                        sleep(wait_for_flush)
                    else:
                        sleep(1)

                    set_step(WorkflowStep.COPY_UR_FILES, "running", "Copying UR files")
                    print("[bsp_batch_recording] Copying UR files...", flush=True)
                    ur_root_path_result = ur_agent.cmd("get_root_path", {})
                    bsp_root_path_result = glasses_agent.cmd("get_root_path", {})
                    ur_root_path = extract_root_path(ur_root_path_result)
                    bsp_root_path = extract_root_path(bsp_root_path_result)
                    print(f"[bsp_batch_recording] UR root path result: {ur_root_path_result}", flush=True)
                    print(f"[bsp_batch_recording] BSP root path result: {bsp_root_path_result}", flush=True)
                    if not ur_root_path or not bsp_root_path:
                        raise RuntimeError(
                            f"get_root_path failed: UR={ur_root_path_result}, BSP={bsp_root_path_result}"
                        )
                    ur_file_path = ur_root_path + "/" + date_string + "/" + file_name + "/*"
                    bsp_dir_path = bsp_root_path + "/" + dataset_name + "/"
                    print(
                        f"[bsp_batch_recording] Copying from {ur_file_path} to {bsp_dir_path}",
                        flush=True,
                    )

                    def update_copy_progress(progress_message):
                        set_step(WorkflowStep.COPY_UR_FILES, "running", progress_message)

                    copy_result = ur_agent.copy_folder_from_remote(
                        remote_path=ur_file_path,
                        local_path=bsp_dir_path,
                        progress_callback=update_copy_progress,
                    )
                    if copy_result.get("success"):
                        set_step(WorkflowStep.COPY_UR_FILES, "success", "UR files copied")
                    else:
                        run_success = False
                        if not error_message:
                            error_message = f"copy_ur_files failed: {copy_result}"
                        set_step(WorkflowStep.COPY_UR_FILES, "failed", error_message)

                    set_step(WorkflowStep.STOP_DEVICE, "running", "Stopping device")
                    stop_device_result = glasses_agent.cmd("stop_device")
                    if stop_device_result.get("success"):
                        set_step(WorkflowStep.STOP_DEVICE, "success", "stop_device success")
                    else:
                        run_success = False
                        if not error_message:
                            error_message = f"stop_device failed: {stop_device_result}"
                        set_step(WorkflowStep.STOP_DEVICE, "failed", error_message)

                    if run_success:
                        success_count += 1
                        finish(True, "Trajectory done")
                        print(
                            f"\n[bsp_batch_recording] ✓ Trajectory {index} done (traj_id={traj_id}, taker={taker_number})"
                        )
                    else:
                        failed_count += 1
                        finish(False, error_message or "Trajectory failed")
                        print(
                            f"\n[bsp_batch_recording] ✗ Trajectory {index} failed (traj_id={traj_id}, taker={taker_number})"
                        )

                except Exception as e:
                    failed_count += 1
                    set_step(WorkflowStep.EXECUTE_TRAJECTORY, "failed", f"Exception: {e}")
                    finish(False, f"Exception: {e}")
                    print(f"\n[bsp_batch_recording] ✗ Trajectory {index} exception: {e}")

                if index < len(trajectory_pairs):
                    cooldown_message = (
                        "请等待 2 分钟让眼镜降温。\n"
                        "如果已经完成降温，可以点击“确定”继续。\n"
                        "如果没有操作，2 分钟后会自动继续下一条轨迹。"
                    )
                    try:
                        dialog.info(
                            "眼镜降温提示",
                            cooldown_message,
                            timeout_ms=2 * 60 * 1000,
                        )
                    except Exception:
                        print("[bsp_batch_recording] 无法显示降温提示，等待 2 分钟...")
                        time.sleep(2 * 60)

            print("\n[bsp_batch_recording] ========================================")
            print("[bsp_batch_recording] Batch recording finished")
            print(f"[bsp_batch_recording] Total: {len(trajectory_pairs)}")
            print(f"[bsp_batch_recording] Success: {success_count}")
            print(f"[bsp_batch_recording] Failed: {failed_count}")
            print("[bsp_batch_recording] ========================================")

except Exception as e:
    print(f"[bsp_batch_recording] Script error: {e}")
    try:
        safe_exit_bsp(glasses_agent, "Script exception cleanup")
    except Exception:
        pass
finally:
    try:
        safe_exit_bsp(glasses_agent, "Batch recording cleanup")
    except Exception:
        pass
