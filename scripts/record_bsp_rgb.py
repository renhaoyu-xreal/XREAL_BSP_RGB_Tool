"""
已退役的 BSP RGB 脚本占位。

请改用 record_bsp_rgb_raw.py。
"""

from flowagent.core.script_workflow import WorkflowStep, finish, set_step, set_steps


set_steps(
    [
        WorkflowStep.CHECK,
    ],
    title="BSP RGB 自由录制（已退役）",
)

message = "record_bsp_rgb.py 已退役，请使用 record_bsp_rgb_raw.py"
set_step(WorkflowStep.CHECK, "failed", message)
print(f"[BSP_RGB] {message}")
finish(False, message)
raise SystemExit
