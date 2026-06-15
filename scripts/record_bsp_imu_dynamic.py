"""
非静态录制 BSP IMU 数据脚本 (10秒自动停止)

功能：
1. 录制 10 秒非静止 IMU 数据
2. 10 秒后自动调用 stop_record 停止录制
3. 双目摄像头快照 + RGB 均值（cam0/cam1/snapshots/, camera_rgb.csv）
4. 镜片屏幕截图 + RGB 均值（screenshots/, record_screen_rgb_info.csv）

保存路径格式：
<project_root>/data/nostill10s_imu/<glasses_id>_<fsn>_<experiment_keyword>_<recorder_name>_nostill10s_imu_<timestamp>/
"""

from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps
from scripts.common.record_path_helper import build_bsp_dataset_name, prompt_bsp_record_labels

# 定义所需的 agent
all_agent_names = ['glasses_bsp_node']

# ============================================================================
# 配置
# ============================================================================

RECORD_DURATION = 10  # 录制时长（秒）

# ============================================================================
# 主流程
# ============================================================================

print("=" * 60)
print("[BSP_IMU_DYNAMIC] 非静态录制 BSP IMU 数据 (10秒)")
print("=" * 60)

set_steps(
    [
        WorkflowStep.NODES_CHECK,
        WorkflowStep.CHECK,
        WorkflowStep.START_RECORD,
        WorkflowStep.STOP_RECORD,
    ],
    title="BSP IMU 非静止 10 秒",
)
set_step(WorkflowStep.NODES_CHECK, "running", "正在检查 glasses_bsp_node")
glasses_bsp_node = script_agents.get("glasses_bsp_node")
if glasses_bsp_node is None:
    message = "缺少 node: glasses_bsp_node"
    set_step(WorkflowStep.NODES_CHECK, "failed", message)
    finish(False, message)
    print(f"[BSP_IMU_DYNAMIC] {message}")
    raise SystemExit(1)
set_step(WorkflowStep.NODES_CHECK, "success", "glasses_bsp_node 已连接")

# 1. 生成数据集名称
set_step(WorkflowStep.CHECK, "running", "正在填写录制信息")
record_labels = prompt_bsp_record_labels(dialog)
if record_labels is None:
    print("[BSP_IMU_DYNAMIC] 已取消录制命名输入，脚本退出")
    set_step(WorkflowStep.CHECK, "failed", "已取消录制命名输入")
    finish(False, "已取消录制命名输入")
    raise SystemExit

experiment_keyword, recorder_name = record_labels
dataset_name, glasses_id = build_bsp_dataset_name(
    "nostill10s_imu",
    experiment_keyword=experiment_keyword,
    recorder_name=recorder_name,
    agent=glasses_bsp_node,
    agent_names=("glasses_bsp_node",),
)

print(f"[BSP_IMU_DYNAMIC] 当前眼镜标识: {glasses_id}")
print(f"[BSP_IMU_DYNAMIC] 实验关键字: {experiment_keyword}")
print(f"[BSP_IMU_DYNAMIC] 录制人: {recorder_name}")
print(f"[BSP_IMU_DYNAMIC] 数据将保存到: data/{dataset_name}/")
print(f"[BSP_IMU_DYNAMIC] 录制时长: {RECORD_DURATION} 秒")
set_step(WorkflowStep.CHECK, "success", f"录制信息已确认: {dataset_name}")

# 2. 开始录制（启用双目相机快照 + 镜片截图，不录制 PGM 图像）
print("\n[BSP_IMU_DYNAMIC] 开始录制...")
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
    print(f"[BSP_IMU_DYNAMIC] ✗ {message}")
    print("[BSP_IMU_DYNAMIC] 脚本退出")
    set_step(WorkflowStep.START_RECORD, "failed", message)
    finish(False, message)
else:
    record_started = True
    print("[BSP_IMU_DYNAMIC] ✓ 录制已开始")
    print("[BSP_IMU_DYNAMIC] 请正常移动眼镜...")
    set_step(WorkflowStep.START_RECORD, "running", "录制中，请正常移动眼镜")

# 3. 录制循环 - 显示进度和运动状态
if record_started:
    elapsed = 0
    
    while elapsed < RECORD_DURATION:
        # 获取录制计时
        timer = record_timer()
        if timer is not None and timer > 0:
            elapsed = timer
        
        # 获取运动状态
        status = motion_status()
        status_str = status if status else "unknown"
        
        print(f"[BSP_IMU_DYNAMIC] 录制中... {elapsed:.1f}s/{RECORD_DURATION}s | 状态: {status_str}")
        set_step(
            WorkflowStep.START_RECORD,
            "running",
            f"录制中: {elapsed:.1f}s/{RECORD_DURATION}s | 状态: {status_str}",
        )
        
        time.sleep(1)
    
    # 4. 停止录制（自动停止 CameraSnapshotWorker + ScreenCaptureWorker）
    print("\n[BSP_IMU_DYNAMIC] 停止录制...")
    set_step(WorkflowStep.STOP_RECORD, "running", "正在停止录制")
    stop_result = glasses_bsp_node.cmd("stop_record", {})
    
    if stop_result.get("success"):
        print("[BSP_IMU_DYNAMIC] ✓ 录制已停止")
        set_step(WorkflowStep.STOP_RECORD, "success", "录制已停止")
    else:
        print(f"[BSP_IMU_DYNAMIC] ✗ 停止录制失败: {stop_result.get('message', 'Unknown error')}")
        set_step(WorkflowStep.STOP_RECORD, "failed", f"停止录制失败: {stop_result.get('message', 'Unknown error')}")
    
    # 5. 输出结果
    print("\n" + "=" * 60)
    print("[BSP_IMU_DYNAMIC] ✓ 录制完成")
    print(f"[BSP_IMU_DYNAMIC] 数据保存在: data/{dataset_name}/")
    print("=" * 60)
    set_step(WorkflowStep.START_RECORD, "success", "录制完成")
    finish(bool(stop_result.get("success")), "非静止录制完成" if stop_result.get("success") else "非静止录制停止失败")
