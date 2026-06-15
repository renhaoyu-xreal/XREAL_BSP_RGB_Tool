# Simple example: control three agents via ScriptExecutor wrappers
# Available wrappers when Tab4 runs this: imu, delay, shell
# Use print() to stream logs into Tab4's log panel.
# Note: time module is provided by ScriptExecutor, no import needed

# 导入公共函数库
from nviz_ur_base import execute_trajectory_recording, safe_exit_nviz
from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps

# 定义需要使用的 agent 列表（执行器会读取并初始化这些 agent）
all_agent_names = ['glasses_nviz_node','UR_node','localhost']

print(f"[test_nviz_node] Starting script...")
print(f"[test_nviz_node] Available agents: {all_agent_names}")


def update_workflow(event, status, message):
    step_map = {
        "start_device": WorkflowStep.START_DEVICE,
        "move_to_start": WorkflowStep.MOVE_TO_START,
        "execute_trajectory": WorkflowStep.EXECUTE_TRAJECTORY,
        "start_record": WorkflowStep.START_RECORD,
        "play_video": WorkflowStep.PLAY_VIDEO,
        "stop_record": WorkflowStep.STOP_RECORD,
        "stop_device": WorkflowStep.STOP_DEVICE,
        "copy_ur_files": WorkflowStep.COPY_UR_FILES,
    }
    step = step_map.get(event)
    if step:
        set_step(step, status, message)


# 主执行逻辑
try:
    # 输入：1. 轨迹id，2. 眼镜sn，3.记录人名称,4.第几次录制,5.是否播放视频,6.视频路径
    # 先获取用户输入参数
    fields = [
        {"name": "traj_id", "label": "轨迹ID", "default": "10"},
        {"name": "glasses_sn", "label": "眼镜标识（空=自动识别）", "default": ""},
        {"name": "recorder_name", "label": "记录人名称", "default": "xlz"},
        {"name": "taker_number", "label": "第几次录制", "default": "1"},
        {"name": "play_video", "label": "是否播放视频 (1=是, 0=否)", "default": "1"},
        {"name": "video_path", "label": "视频路径（空=默认 old_video.mp4）", "default": ""}
    ]
    
    dialog_result = dialog.multi_field_input("录制参数设置", "请输入录制参数", fields)
    
    if not dialog_result:
        print("[test_nviz_node] 用户取消输入")
        # 直接退出，让finally清理
    else:
        # 获取用户输入的参数
        traj_id = int(dialog_result["traj_id"])
        glasses_sn = dialog_result["glasses_sn"]
        recorder_name = dialog_result["recorder_name"]
        taker_number = int(dialog_result["taker_number"])
        enable_video = int(dialog_result["play_video"]) == 1
        video_path = dialog_result.get("video_path", "").strip()
        
        print(f"[test_nviz_node] Video playback: {'Enabled' if enable_video else 'Disabled'}")

        set_steps(
            [
                WorkflowStep.NODES_CHECK,
                WorkflowStep.START_DEVICE,
                WorkflowStep.MOVE_TO_START,
                WorkflowStep.EXECUTE_TRAJECTORY,
                WorkflowStep.START_RECORD,
                WorkflowStep.PLAY_VIDEO,
                WorkflowStep.STOP_RECORD,
                WorkflowStep.STOP_DEVICE,
                WorkflowStep.COPY_UR_FILES,
            ],
            title="3DoF 单条轨迹录制",
        )

        glasses_agent = script_agents.get("glasses_nviz_node")
        ur_agent = script_agents.get("ur_node")
        localhost_agent = script_agents.get("localhost")

        missing_agents = []
        if glasses_agent is None:
            missing_agents.append("glasses_nviz_node")
        if ur_agent is None:
            missing_agents.append("UR_node")
        if enable_video and localhost_agent is None:
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
        else:
            set_step(WorkflowStep.NODES_CHECK, "success", "所有节点已连接")

            # 执行轨迹录制流程
            run_success = execute_trajectory_recording(
                ur_agent,
                glasses_agent,
                traj_id,
                taker_number,
                glasses_sn,
                recorder_name,
                time_delay,
                record_time,
                localhost_node=localhost_agent,
                enable_video=enable_video,
                progress_callback=update_workflow,
                video_path=video_path or None
            )
            finish(run_success, "3DoF 单条轨迹录制完成" if run_success else "3DoF 单条轨迹录制失败")

except:
    print(f"[test_nviz_node] 脚本执行出错")
    try:
        finish(False, "脚本执行出错")
    except Exception:
        pass
    try:
        safe_exit_nviz(script_agents.get("glasses_nviz_node"), "脚本异常退出")
    except Exception:
        pass
finally:
    try:
        safe_exit_nviz(script_agents.get("glasses_nviz_node"), "脚本结束清理")
    except Exception:
        pass
