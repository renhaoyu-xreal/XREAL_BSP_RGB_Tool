"""
静态录制 BSP IMU 数据脚本 (10秒自动停止)

功能：
1. 录制 10 秒静止 IMU 数据
2. 使用 motion_status() 检测静止状态
3. 检测到非静止立即停止录制、弹窗提示并删除数据
4. 10 秒后自动调用 stop_record 停止录制
5. 双目摄像头快照 + RGB 均值（cam0/cam1/snapshots/, camera_rgb.csv）
6. 镜片屏幕截图 + RGB 均值（screenshots/, record_screen_rgb_info.csv）

保存路径格式：
<project_root>/data/still10s_imu/<glasses_id>_<fsn>_<experiment_keyword>_<recorder_name>_still10s_imu_<timestamp>/

motion_status() 返回值：
- "none": 数据不足，等待中
- "static": 静止
- "moving": 运动
- "active": 活跃
"""

from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps
from scripts.common.record_path_helper import build_bsp_dataset_name, prompt_bsp_record_labels

# 定义所需的 agent
all_agent_names = ['glasses_bsp_node']

# ============================================================================
# 配置
# ============================================================================

RECORD_DURATION = 10  # 录制时长（秒）

# 静止状态列表（这些状态视为"静止"或"等待中"）
ALLOWED_STATES = ["static", "none", None]

# ============================================================================
# 主流程
# ============================================================================

print("=" * 60)
print("[STATIC] 静态录制 BSP IMU 数据 (10秒)")
print("=" * 60)

set_steps(
    [
        WorkflowStep.NODES_CHECK,
        WorkflowStep.CHECK,
        WorkflowStep.START_RECORD,
        WorkflowStep.STOP_RECORD,
        WorkflowStep.DELETE_RECORD,
    ],
    title="BSP IMU 静止 10 秒",
)
set_step(WorkflowStep.NODES_CHECK, "running", "正在检查 glasses_bsp_node")
glasses_bsp_node = script_agents.get("glasses_bsp_node")
if glasses_bsp_node is None:
    message = "缺少 node: glasses_bsp_node"
    set_step(WorkflowStep.NODES_CHECK, "failed", message)
    finish(False, message)
    print(f"[STATIC] {message}")
    raise SystemExit(1)
set_step(WorkflowStep.NODES_CHECK, "success", "glasses_bsp_node 已连接")

# 1. 生成数据集名称
set_step(WorkflowStep.CHECK, "running", "正在填写录制信息")
record_labels = prompt_bsp_record_labels(dialog)
if record_labels is None:
    print("[STATIC] 已取消录制命名输入，脚本退出")
    set_step(WorkflowStep.CHECK, "failed", "已取消录制命名输入")
    finish(False, "已取消录制命名输入")
    raise SystemExit

experiment_keyword, recorder_name = record_labels
dataset_name, glasses_id = build_bsp_dataset_name(
    "still10s_imu",
    experiment_keyword=experiment_keyword,
    recorder_name=recorder_name,
    agent=glasses_bsp_node,
    agent_names=("glasses_bsp_node",),
)

print(f"[STATIC] 当前眼镜标识: {glasses_id}")
print(f"[STATIC] 实验关键字: {experiment_keyword}")
print(f"[STATIC] 录制人: {recorder_name}")
print(f"[STATIC] 数据将保存到: data/{dataset_name}/")
print(f"[STATIC] 录制时长: {RECORD_DURATION} 秒")
print("[STATIC] 注意：检测到运动将立即停止录制并删除数据")
set_step(WorkflowStep.CHECK, "success", f"录制信息已确认: {dataset_name}")

# 2. 开始录制（启用双目相机快照 + 镜片截图，不录制 PGM 图像）
print("[STATIC] 开始录制...")
set_step(WorkflowStep.START_RECORD, "running", "正在启动录制")
result = glasses_bsp_node.cmd("start_record", {
    "dataset_name": dataset_name,
    "enable_camera_snapshot": True,
    "enable_screen_capture": True,
    "enable_mic_recording": True,
})

record_started = False
if not result.get("success"):
    message = f"启动录制失败: {result.get('message', 'Unknown error')}"
    print(f"[STATIC] {message}")
    set_step(WorkflowStep.START_RECORD, "failed", message)
    finish(False, message)
    dialog.error("提示", message)
else:
    record_started = True
    print("[STATIC] 录制已开始，请保持眼镜静止...")
    set_step(WorkflowStep.START_RECORD, "running", "录制中，请保持静止")

# 3. 录制循环 - 检测到非静止立即停止
if record_started:
    elapsed = 0
    recording_success = True
    failure_reason = ""
    
    while elapsed < RECORD_DURATION:
        # 获取录制计时
        timer = record_timer()
        if timer is not None and timer > 0:
            elapsed = timer
        
        # 检查运动状态
        status = motion_status()
        
        if status not in ALLOWED_STATES:
            print(f"[STATIC] 检测到运动! 状态: {status}")
            print("[STATIC] 立即停止录制并删除数据...")
            recording_success = False
            failure_reason = f"IMU 数据不静止，原因:{status}。录数据失败。"
            set_step(WorkflowStep.START_RECORD, "failed", failure_reason)
            break
        
        status_display = status if status else "waiting"
        print(f"[STATIC] 录制中... {elapsed:.1f}s/{RECORD_DURATION}s | 状态: {status_display}")
        set_step(
            WorkflowStep.START_RECORD,
            "running",
            f"录制中: {elapsed:.1f}s/{RECORD_DURATION}s | 状态: {status_display}",
        )
        time.sleep(1)
    
    # 4. 停止录制（自动停止 CameraSnapshotWorker + ScreenCaptureWorker）
    print("[STATIC] 停止录制...")
    set_step(WorkflowStep.STOP_RECORD, "running", "正在停止录制")
    stop_result = glasses_bsp_node.cmd("stop_record", {})
    
    if stop_result.get("success"):
        print("[STATIC] 录制已停止")
        set_step(WorkflowStep.STOP_RECORD, "success", "录制已停止")
    else:
        print(f"[STATIC] 停止失败: {stop_result.get('message', 'Unknown error')}")
        set_step(WorkflowStep.STOP_RECORD, "failed", f"停止失败: {stop_result.get('message', 'Unknown error')}")
    
    # 5. 处理结果
    print("=" * 60)
    if recording_success:
        print("[STATIC] 录制完成，全程保持静止")
        print(f"[STATIC] 数据保存在: data/{dataset_name}/")
        set_step(WorkflowStep.START_RECORD, "success", "全程保持静止")
        set_step(WorkflowStep.DELETE_RECORD, "success", "无需删除数据")
        finish(True, "静止录制完成")
    else:
        # 删除录制数据
        print("[STATIC] 正在删除失败的录制数据...")
        set_step(WorkflowStep.DELETE_RECORD, "running", "正在删除失败数据")
        delete_result = glasses_bsp_node.cmd("delete_record", {"dataset_name": dataset_name})
        if delete_result.get("success"):
            print("[STATIC] 数据已删除")
            set_step(WorkflowStep.DELETE_RECORD, "success", "失败数据已删除")
        else:
            print(f"[STATIC] 删除失败: {delete_result.get('message')}")
            set_step(WorkflowStep.DELETE_RECORD, "failed", f"删除失败: {delete_result.get('message')}")
        
        print("[STATIC] 录制失败：检测到运动")
        finish(False, failure_reason)
        dialog.error("提示", f"[imu数据静止10秒钟]实验。 {failure_reason}")
    print("=" * 60)
