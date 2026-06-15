"""
自由录制 BSP IMU + Camera 数据脚本

功能：
1. 使用 glasses_bsp_node 采集真实 IMU 数据（IMU0 + IMU1）和双目 SLAM 相机图像
2. 使用"运行脚本"和"停止脚本"按钮控制录制
3. 保存为标准格式：
   - imu_0.csv: IMU0 数据（gyro/acc/mag/temperature）
   - imu_1.csv: IMU1 数据（gyro/acc/temperature）
   - cam0/images/: 左相机 PGM 灰度图 + metadata.txt + timestamps.txt
   - cam1/images/: 右相机 PGM 灰度图 + metadata.txt + timestamps.txt
   - record_info.txt: 录制信息
   - glass_config.json: 眼镜配置

使用前提：
1. 在 Tab2 手动执行 start_device 命令启动设备
2. 然后在 Tab1/Tab4 执行本脚本录制数据
3. 点击"停止脚本"按钮结束录制

保存路径格式：
<project_root>/data/free_record/imu_and_cam/<glasses_id>_<fsn>_<experiment_keyword>_<recorder_name>_free_record_imu_and_cam_<timestamp>/
"""

from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps
from scripts.common.record_path_helper import build_bsp_dataset_name, prompt_bsp_record_labels

# 定义所需的 agent
all_agent_names = ['glasses_bsp_node']


# ============================================================================
# 主流程
# ============================================================================

print("=" * 60)
print("[BSP_IMU_CAM] 自由录制 BSP IMU + Camera 数据")
print("=" * 60)

set_steps(
    [
        WorkflowStep.NODES_CHECK,
        WorkflowStep.CHECK,
        WorkflowStep.START_RECORD,
        WorkflowStep.STOP_RECORD,
    ],
    title="BSP IMU + Camera 自由录制",
)
set_step(WorkflowStep.NODES_CHECK, "running", "正在检查 glasses_bsp_node")
glasses_bsp_node = script_agents.get("glasses_bsp_node")
if glasses_bsp_node is None:
    message = "缺少 node: glasses_bsp_node"
    set_step(WorkflowStep.NODES_CHECK, "failed", message)
    finish(False, message)
    print(f"[BSP_IMU_CAM] {message}")
    raise SystemExit(1)
set_step(WorkflowStep.NODES_CHECK, "success", "glasses_bsp_node 已连接")

# 1. 生成数据集名称
set_step(WorkflowStep.CHECK, "running", "正在填写录制信息")
record_labels = prompt_bsp_record_labels(dialog)
if record_labels is None:
    print("[BSP_IMU_CAM] 已取消录制命名输入，脚本退出")
    set_step(WorkflowStep.CHECK, "failed", "已取消录制命名输入")
    finish(False, "已取消录制命名输入")
    raise SystemExit

experiment_keyword, recorder_name = record_labels
dataset_name, glasses_id = build_bsp_dataset_name(
    "free_record/imu_and_cam",
    experiment_keyword=experiment_keyword,
    recorder_name=recorder_name,
    agent=glasses_bsp_node,
    agent_names=("glasses_bsp_node",),
)

print(f"[BSP_IMU_CAM] 当前眼镜标识: {glasses_id}")
print(f"[BSP_IMU_CAM] 实验关键字: {experiment_keyword}")
print(f"[BSP_IMU_CAM] 录制人: {recorder_name}")
print(f"[BSP_IMU_CAM] 数据将保存到: data/{dataset_name}/")
print("[BSP_IMU_CAM] 点击 '停止脚本' 按钮结束录制")
set_step(WorkflowStep.CHECK, "success", f"录制信息已确认: {dataset_name}")

# 2. 开始录制
print("\n[BSP_IMU_CAM] 开始录制...")
set_step(WorkflowStep.START_RECORD, "running", "正在启动录制")
result = glasses_bsp_node.cmd("start_record", {
    "dataset_name": dataset_name,
    "enable_image_recording": True,
    "enable_camera_snapshot": True,
    "enable_screen_capture": True,
    "enable_mic_recording": True,
})

if not result.get("success"):
    message = f"启动录制失败: {result.get('message', 'Unknown error')}"
    print(f"[BSP_IMU_CAM] ✗ {message}")
    set_step(WorkflowStep.START_RECORD, "failed", message)
    finish(False, message)
    raise SystemExit(1)

print("[BSP_IMU_CAM] ✓ 录制已开始")
print("[BSP_IMU_CAM] IMU0: gyro/acc/mag/temperature")
print("[BSP_IMU_CAM] IMU1: gyro/acc/temperature")
print("[BSP_IMU_CAM] Camera: cam0 + cam1 (SLAM 双目灰度相机)")
set_step(WorkflowStep.START_RECORD, "running", "录制中")

# 3. 持续显示录制状态，等待用户停止
print("\n[BSP_IMU_CAM] 录制中... 按 '停止脚本' 按钮结束录制")

while True:
    # 获取实时录制计时器
    timer = record_timer()
    if timer is not None and timer > 0:
        print(f"[BSP_IMU_CAM] 录制中... 已录制: {timer:.1f}秒")
        set_step(WorkflowStep.START_RECORD, "running", f"录制中: {timer:.1f}秒")
    else:
        print("[BSP_IMU_CAM] 录制中... 等待数据...")
        set_step(WorkflowStep.START_RECORD, "running", "录制中，等待数据")
    
    time.sleep(1)

# 注意：以下代码在正常流程中不会执行到
# 因为脚本会被"停止脚本"按钮中断
# 紧急停止时，agent_manager 会自动调用 stop_record
