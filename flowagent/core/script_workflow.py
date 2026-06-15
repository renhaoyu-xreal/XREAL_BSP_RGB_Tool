"""
脚本流程显示的极简核心模块。

脚本侧只需要：
    from flowagent.core.script_workflow import WorkflowStep, set_steps, set_step, finish
"""

import threading
from enum import Enum


_thread_local = threading.local()


class WorkflowStep(Enum):
    NODES_CHECK = "nodes_check"
    CHECK = "check"
    GET_BSP_RUNTIME_STATE = "get_bsp_runtime_state"
    INIT_DEVICE = "init_device"
    START_DEVICE = "start_device"
    MOVE_TO_START = "move_to_start"
    EXECUTE_TRAJECTORY = "execute_trajectory"
    START_RECORD = "start_record"
    CAPTURE_RAW_FRAME = "capture_raw_frame"
    STOP_RECORD = "stop_record"
    DELETE_RECORD = "delete_record"
    STOP_DEVICE = "stop_device"
    GET_ROOT_PATH = "get_root_path"
    COPY_UR_FILES = "copy_ur_files"
    PLAY_VIDEO = "play_video_on_secondary_screen.py"
    STOP_VIDEO = "stop_video.sh"


STEP_LABELS = {
    WorkflowStep.NODES_CHECK: "节点准备",
    WorkflowStep.CHECK: "check",
    WorkflowStep.GET_BSP_RUNTIME_STATE: "获取BSP运行状态",
    WorkflowStep.INIT_DEVICE: "设备初始化",
    WorkflowStep.START_DEVICE: "start_device",
    WorkflowStep.MOVE_TO_START: "移动机器臂到起始点",
    WorkflowStep.EXECUTE_TRAJECTORY: "执行轨迹移动",
    WorkflowStep.START_RECORD: "start_record",
    WorkflowStep.CAPTURE_RAW_FRAME: "捕获raw_frame",
    WorkflowStep.STOP_RECORD: "stop_record",
    WorkflowStep.DELETE_RECORD: "delete_record",
    WorkflowStep.STOP_DEVICE: "stop_device",
    WorkflowStep.GET_ROOT_PATH: "get_root_path",
    WorkflowStep.COPY_UR_FILES: "copy_ur_files",
    WorkflowStep.PLAY_VIDEO: "play_video",
    WorkflowStep.STOP_VIDEO: "stop_video",
}


class SimpleScriptWorkflow:
    """维护脚本流程状态，并把完整快照发给 UI。"""

    def __init__(self, progress_queue):
        self._progress_queue = progress_queue
        self._title = ""
        self._steps = []
        self._step_index_by_key = {}
        self._message = ""
        self._finished = False
        self._success = None

    def _emit(self, action: str):
        if not self._progress_queue:
            return
        try:
            self._progress_queue.put_nowait({
                'type': 'workflow',
                'action': action,
                'title': self._title,
                'steps': [dict(step) for step in self._steps],
                'message': self._message,
                'finished': self._finished,
                'success': self._success,
            })
        except Exception:
            pass

    def set_steps(self, steps, title: str = "脚本流程"):
        self._title = title or "脚本流程"
        self._message = ""
        self._finished = False
        self._success = None
        self._steps = []
        self._step_index_by_key = {}
        for step in steps:
            if not isinstance(step, WorkflowStep):
                raise ValueError(f"set_steps 只接受 WorkflowStep 枚举元素，收到: {step!r}")
            step_key = step.value
            if step_key in self._step_index_by_key:
                raise ValueError(f"set_steps 中存在重复步骤，无法按步骤 id 更新: {step!r}")
            self._step_index_by_key[step_key] = len(self._steps)
            self._steps.append({
                "key": step_key,
                "label": STEP_LABELS.get(step, step_key),
                "status": "pending",
                "message": "",
            })
        self._emit('state')

    def _resolve_step_index(self, step_id):
        if not self._steps:
            raise ValueError("set_step 前必须先调用 set_steps")

        if isinstance(step_id, WorkflowStep):
            step_key = step_id.value
            if step_key not in self._step_index_by_key:
                raise KeyError(f"未找到步骤 id: {step_key!r}")
            return self._step_index_by_key[step_key]

        if isinstance(step_id, str):
            if step_id not in self._step_index_by_key:
                raise KeyError(f"未找到步骤 id: {step_id!r}")
            return self._step_index_by_key[step_id]

        if isinstance(step_id, int) and not isinstance(step_id, bool):
            if step_id < 0 or step_id >= len(self._steps):
                raise IndexError(f"步骤序号越界: {step_id}, 总步数={len(self._steps)}")
            return step_id

        raise ValueError(
            f"set_step 只接受 WorkflowStep、步骤 key(str) 或步骤序号(int)，收到: {step_id!r}"
        )

    def set_step(self, step_id, status: str, message: str = ""):
        status = str(status)
        step_index = self._resolve_step_index(step_id)

        target_step = self._steps[step_index]
        target_step["status"] = status
        if message:
            target_step["message"] = str(message)
            self._message = str(message)
        self._emit('state')

    def set_message(self, message: str):
        self._message = str(message)
        self._emit('state')

    def finish(self, success: bool = True, message: str = ""):
        self._finished = True
        self._success = bool(success)
        if message:
            self._message = str(message)
        self._emit('state')

    def clear(self):
        self._title = ""
        self._steps = []
        self._step_index_by_key = {}
        self._message = ""
        self._finished = False
        self._success = None
        self._emit('clear')


class _NullWorkflow:
    def set_steps(self, steps, title="脚本流程"):
        return None

    def set_step(self, step_id, status, message=""):
        return None

    def set_message(self, message):
        return None

    def finish(self, success=True, message=""):
        return None

    def clear(self):
        return None


_NULL_WORKFLOW = _NullWorkflow()


def bind_workflow(workflow):
    _thread_local.workflow = workflow


def clear_workflow_binding():
    if hasattr(_thread_local, 'workflow'):
        delattr(_thread_local, 'workflow')


def get_workflow():
    return getattr(_thread_local, 'workflow', _NULL_WORKFLOW)


def set_steps(steps, title="脚本流程"):
    return get_workflow().set_steps(steps, title=title)


def set_step(step_id, status, message=""):
    return get_workflow().set_step(step_id, status, message=message)


def set_message(message):
    return get_workflow().set_message(message)


def finish(success=True, message=""):
    return get_workflow().finish(success=success, message=message)


def clear():
    return get_workflow().clear()
