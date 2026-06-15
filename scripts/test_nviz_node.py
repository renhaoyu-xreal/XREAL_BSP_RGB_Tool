# Simple example: control three agents via ScriptExecutor wrappers
# Available wrappers when Tab4 runs this: imu, delay, shell
# Use print() to stream logs into Tab4's log panel.
# Note: time module is provided by ScriptExecutor, no import needed

# 定义需要使用的 agent 列表（执行器会读取并初始化这些 agent）

from scripts.common.record_path_helper import (
    resolve_record_glasses_fsn,
    resolve_record_glasses_label,
)

all_agent_names = ['glasses_nviz_node']

print(f"[test_nviz_node] Starting script...")
print(f"[test_nviz_node] Available agents: {all_agent_names}")

print("[test_nviz_node] Starting NViz agent...")
result1 = glasses_nviz_node.cmd("start_device", {"data_type": "3dof"})
print(f"[test_nviz_node] start_device result: {result1}")

glasses_label = resolve_record_glasses_label(
    agent=glasses_nviz_node,
    agent_names=("glasses_nviz_node",),
    allow_ssh=False,
)
glasses_fsn = resolve_record_glasses_fsn(
    agent=glasses_nviz_node,
    allow_ssh=False,
)
dataset_name = (
    f"test_nviz_recording/{glasses_label}_{glasses_fsn}_"
    f"test_nviz_{time.strftime('%Y%m%d%H%M%S')}"
)
result2 = glasses_nviz_node.cmd("start_record", {"dataset_name": dataset_name})
print(f"[test_nviz_node] start_record result: {result2}")

print("[test_nviz_node] Entering main loop...")
while True:
    # 获取当前录制时长
    timer = record_timer()
    delay = time_delay()
    status = motion_status()
    
    if timer is not None:
        delay_str = f"{delay:.2f}ms" if delay is not None else "N/A"
        status_str = status if status is not None else "N/A"
        print(f"录制时长: {timer:.2f}秒, 延迟: {delay_str}, 运动状态: {status_str}")
        
        # 录制1000秒后停止
        if timer >= 1000.0:
            print("[test_nviz_node] 已录制1000秒，停止录制")
            break
    else:
        print("等待录制数据...")
    
    time.sleep(1)

glasses_nviz_node.cmd("stop_record", {})

print("[test_nviz_node] Stopping agents...")
glasses_nviz_node.cmd("stop_device", {})

print("[test_nviz_node] Done.")
