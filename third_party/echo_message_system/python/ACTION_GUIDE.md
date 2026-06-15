# ROS Action 支持指南

## 概述

本项目现已支持 **ROS 风格的 Action 通信模式**，这是一种异步目标-反馈-结果通信机制。

Action 特别适合长时间运行的任务，允许：
- 📤 **目标（Goal）**：客户端向服务端发送任务目标
- 📊 **反馈（Feedback）**：服务端定期向客户端发送执行进度
- 📬 **结果（Result）**：服务端最终将结果发送给客户端

## 架构

### 通信模式

```
ActionClient                           ActionServer
    |                                      |
    |--- 1. 发送目标 (Goal) ---------->    |
    |                                      |
    |<-- 2. 目标已接受 (Accepted) ----    |
    |                                      |
    |<-- 3. 反馈 (Feedback) ----------    | (可重复)
    |                                      |
    |<-- 4. 结果 (Result) -----------    |
    |                                      |
```

### 端口分配

- **Goal 端口**：用于接收目标请求（REP/REQ 模式）
- **Feedback 端口**：用于发送反馈和结果（PUB/SUB 模式）

## API 文档

### ActionServer

用于处理客户端的目标请求。

```python
from message_system import ActionServer, GoalStatus
import time

def goal_callback(goal_id, goal_data, server):
    """
    处理目标的回调函数。
    
    Args:
        goal_id: 目标ID（由服务端生成）
        goal_data: 客户端发送的目标数据
        server: ActionServer 实例，用于发送反馈和结果
    """
    # 执行任务并发送进度反馈
    for i in range(goal_data['steps']):
        server.send_feedback(goal_id, {'progress': i + 1})
        time.sleep(0.1)
    
    # 发送最终结果
    server.send_result(
        goal_id, 
        {'result': 'success'}, 
        GoalStatus.SUCCEEDED
    )

# 创建服务
server = ActionServer(
    name='my_server',                    # 节点名称
    action_name='my_action',             # Action 名称
    callback=goal_callback,              # 处理目标的回调
    goal_port=5557,                      # 接收目标的端口
    feedback_port=5558,                  # 发送反馈/结果的端口
    encoding='json'                      # 消息编码方式
)

server.start()
server.spin()  # 保持运行
server.close()
```

#### 关键方法

**`send_feedback(goal_id, feedback)`**
- 发送目标的执行进度反馈
- 可以多次调用
- 参数：
  - `goal_id`：目标ID
  - `feedback`：反馈数据（字典）

**`send_result(goal_id, result, status)`**
- 发送最终结果，标志着目标完成
- 只能调用一次
- 参数：
  - `goal_id`：目标ID
  - `result`：结果数据（字典）
  - `status`：`GoalStatus.SUCCEEDED` 或 `GoalStatus.FAILED`

### ActionClient

用于向服务端发送目标并接收反馈和结果。

```python
from message_system import ActionClient, GoalStatus

def on_feedback(goal_id, feedback):
    """接收反馈的回调"""
    print(f"Progress: {feedback['progress']}")

def on_result(goal_id, result, status):
    """接收结果的回调"""
    print(f"Complete: {result}, Status: {status.value}")

# 创建客户端
client = ActionClient(
    name='my_client',                    # 节点名称
    action_name='my_action',             # 要连接的 Action 名称
    goal_host='localhost',               # 服务端主机
    goal_port=5557,                      # 服务端接收目标的端口
    feedback_host='localhost',           # 反馈服务端主机
    feedback_port=5558,                  # 反馈服务端端口
    timeout=5000,                        # 发送目标的超时时间（毫秒）
    encoding='json'                      # 消息编码方式
)

# 设置回调函数
client.set_feedback_callback(on_feedback)
client.set_result_callback(on_result)

# 启动监听反馈和结果
client.start_listening()

# 发送目标
goal_id = client.send_goal({'steps': 10})

# 等待结果（阻塞）
result, status = client.wait_for_result(goal_id, timeout=30000)

print(f"Result: {result}, Status: {status.value}")

client.close()
```

#### 关键方法

**`send_goal(goal_data) -> str`**
- 向服务端发送目标
- 返回：目标ID（字符串）
- 参数：`goal_data` 是要发送的目标数据（字典）

**`set_feedback_callback(callback)`**
- 设置接收反馈时的回调函数
- 回调签名：`callback(goal_id, feedback)`

**`set_result_callback(callback)`**
- 设置接收结果时的回调函数
- 回调签名：`callback(goal_id, result, status)`

**`start_listening()`**
- 启动后台线程监听反馈和结果
- 自动调用 `start()` 如果客户端未启动

**`wait_for_result(goal_id, timeout=None) -> (result, status)`**
- 阻塞等待目标完成
- 参数：
  - `goal_id`：要等待的目标ID
  - `timeout`：超时时间（毫秒），None 为无限等待
- 返回：`(result_data, GoalStatus)` 元组
- 异常：`TimeoutError` 如果超时

### GoalStatus 枚举

```python
from message_system import GoalStatus

# 可用状态
GoalStatus.ACCEPTED    # 目标已被服务端接受
GoalStatus.EXECUTING   # 服务端正在执行目标
GoalStatus.SUCCEEDED   # 目标成功完成
GoalStatus.FAILED      # 目标执行失败
GoalStatus.CANCELED    # 目标被取消
```

## 实际示例

### 示例 1：计算密集型任务

服务端计算 Fibonacci 数，并通过反馈发送进度：

```python
# server.py
from message_system import ActionServer, GoalStatus
import time

def fibonacci_callback(goal_id, goal_data, server):
    n = goal_data['n']
    
    a, b = 0, 1
    for i in range(n):
        a, b = b, a + b
        server.send_feedback(goal_id, {
            'current': i + 1,
            'total': n,
            'percentage': int((i + 1) / n * 100)
        })
        time.sleep(0.1)
    
    server.send_result(goal_id, {'fibonacci': a}, GoalStatus.SUCCEEDED)

server = ActionServer(
    name='fib_server',
    action_name='fibonacci',
    callback=fibonacci_callback,
    goal_port=5557,
    feedback_port=5558
)
server.start()
server.spin()
```

```python
# client.py
from message_system import ActionClient

def on_feedback(goal_id, feedback):
    print(f"[{feedback['percentage']}%] {feedback['current']}/{feedback['total']}")

client = ActionClient(
    name='fib_client',
    action_name='fibonacci',
    goal_port=5557,
    feedback_port=5558
)

client.set_feedback_callback(on_feedback)
client.start_listening()

goal_id = client.send_goal({'n': 20})
result, status = client.wait_for_result(goal_id)

print(f"Fibonacci(20) = {result['fibonacci']}")
client.close()
```

### 示例 2：错误处理

处理执行失败的目标：

```python
# server.py
from message_system import ActionServer, GoalStatus

def validate_callback(goal_id, goal_data, server):
    n = goal_data.get('n', 0)
    
    # 验证输入
    if n < 0:
        server.send_result(
            goal_id, 
            {'error': 'n must be non-negative'}, 
            GoalStatus.FAILED
        )
        return
    
    result = n * n
    server.send_result(
        goal_id, 
        {'square': result}, 
        GoalStatus.SUCCEEDED
    )

server = ActionServer(
    name='validator',
    action_name='square',
    callback=validate_callback,
    goal_port=5557,
    feedback_port=5558
)
server.start()
server.spin()
```

```python
# client.py
from message_system import ActionClient, GoalStatus

def on_result(goal_id, result, status):
    if status == GoalStatus.FAILED:
        print(f"Error: {result.get('error')}")
    else:
        print(f"Square: {result.get('square')}")

client = ActionClient(
    name='validator_client',
    action_name='square',
    goal_port=5557,
    feedback_port=5558
)

client.set_result_callback(on_result)
client.start_listening()

goal_id = client.send_goal({'n': -5})
result, status = client.wait_for_result(goal_id)

client.close()
```

### 示例 3：多目标并发处理

服务端处理多个并发目标：

```python
# server.py
from message_system import ActionServer, GoalStatus
import time
import threading

def long_task_callback(goal_id, goal_data, server):
    duration = goal_data.get('duration', 5)
    
    for i in range(duration):
        server.send_feedback(goal_id, {'elapsed': i + 1})
        time.sleep(1)
    
    server.send_result(goal_id, {'total_time': duration}, GoalStatus.SUCCEEDED)

server = ActionServer(
    name='task_server',
    action_name='long_task',
    callback=long_task_callback,
    goal_port=5557,
    feedback_port=5558
)
server.start()
server.spin()
```

```python
# client.py
from message_system import ActionClient
import threading

def submit_goal(client, duration):
    goal_id = client.send_goal({'duration': duration})
    result, status = client.wait_for_result(goal_id)
    print(f"Task {goal_id[:8]}: took {result['total_time']}s")

client = ActionClient(
    name='task_client',
    action_name='long_task',
    goal_port=5557,
    feedback_port=5558
)
client.start_listening()

# 并发提交多个任务
threads = []
for i in range(3):
    t = threading.Thread(target=submit_goal, args=(client, 5))
    t.start()
    threads.append(t)

for t in threads:
    t.join()

client.close()
```

## 常见问题

**Q: Action 和 Service 有什么区别？**

A: 
- **Service**：同步请求-响应，一问一答
- **Action**：异步目标-反馈-结果，适合长时间运行的任务

**Q: 如何处理服务端异常？**

A: 在回调中捕获异常，调用 `server.send_result(..., GoalStatus.FAILED)`

**Q: 可以取消正在运行的目标吗？**

A: 当前版本不支持，但可以通过设置自定义的中止标志来实现

**Q: 多个客户端可以同时向同一服务端发送目标吗？**

A: 是的，ActionServer 使用独立线程处理每个目标

**Q: 反馈的频率有限制吗？**

A: 没有硬性限制，但建议根据实际需求平衡频率与性能

## 最佳实践

1. **错误处理**：在回调中处理所有异常，使用 `GoalStatus.FAILED` 报告失败
2. **定期反馈**：对于长时间任务，定期发送反馈保持通信活跃
3. **资源清理**：确保调用 `client.close()` 和 `server.close()` 释放资源
4. **超时设置**：为 `wait_for_result` 设置合理的超时时间
5. **日志记录**：使用内置日志跟踪目标执行状态

## 运行示例

```bash
# 终端1 - 启动 Action 服务端
python examples/action_server_example.py

# 终端2 - 运行 Action 客户端
python examples/action_client_example.py 15
```

## 参考

- 完整示例：`examples/action_server_example.py` 和 `examples/action_client_example.py`
- 单元测试：`tests/test_action.py`
- ROS Action 文档：https://docs.ros.org/en/humble/Tutorials/Understanding-ROS2-Actions.html
