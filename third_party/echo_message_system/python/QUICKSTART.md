# 快速开始指南

## 1️⃣ 安装依赖

```bash
pip install pyzmq
```

## 2️⃣ 测试系统

```bash
cd python_message_system
python tests/test_basic.py
python -m unittest tests.test_action -v
```

## 3️⃣ 运行示例

### 发布/订阅示例

**打开两个终端:**

```bash
# 终端1 - 订阅者（先启动）
cd python_message_system
python examples/subscriber_example.py
```

```bash
# 终端2 - 发布者
cd python_message_system
python examples/publisher_example.py
```

你会看到订阅者收到发布者发送的消息！

### 服务/客户端示例

**打开两个终端:**

```bash
# 终端1 - 服务端（先启动）
cd python_message_system
python examples/service_server_example.py
```

```bash
# 终端2 - 客户端
cd python_message_system
python examples/service_client_example.py 15 25
```

客户端会调用服务端计算 15 + 25 并显示结果！

### 动作（Action）示例 - ROS风格

**打开两个终端:**

```bash
# 终端1 - 动作服务端（先启动）
cd python_message_system
python examples/action_server_example.py
```

```bash
# 终端2 - 动作客户端
cd python_message_system
python examples/action_client_example.py 10
```

你会看到客户端发送一个计算Fibonacci数的目标给服务端，并实时接收进度反馈！

### 多话题示例

**打开两个终端:**

```bash
# 终端1 - 订阅者
cd python_message_system
python examples/multi_topic_example.py sub
```

```bash
# 终端2 - 发布者
cd python_message_system
python examples/multi_topic_example.py pub
```

你会看到两个不同的话题同时传输数据！

## 4️⃣ 编写你自己的代码

### 简单发布者

```python
from message_system import Publisher
import time

pub = Publisher(name='my_pub', topic='hello', port=5555)
pub.start()

for i in range(10):
    pub.publish({'message': f'Hello {i}'})
    time.sleep(1)

pub.close()
```

### 简单订阅者

```python
from message_system import Subscriber

def on_message(data):
    print(f"Got: {data}")

sub = Subscriber(
    name='my_sub',
    topic='hello',
    callback=on_message,
    port=5555
)

sub.start()
sub.spin()  # 保持运行
```

### 简单动作服务端

```python
from message_system import ActionServer, GoalStatus
import time

def process_goal(goal_id, goal_data, server):
    n = goal_data.get('n', 5)
    for i in range(n):
        server.send_feedback(goal_id, {'progress': i + 1, 'total': n})
        time.sleep(0.1)
    server.send_result(goal_id, {'done': True}, GoalStatus.SUCCEEDED)

action = ActionServer(
    name='my_action',
    action_name='my_action',
    callback=process_goal,
    goal_port=5557,
    feedback_port=5558
)

action.start()
action.spin()
```

### 简单动作客户端

```python
from message_system import ActionClient

def on_feedback(goal_id, feedback):
    print(f"Progress: {feedback}")

def on_result(goal_id, result, status):
    print(f"Done! Status: {status.value}")

client = ActionClient(
    name='my_client',
    action_name='my_action',
    goal_port=5557,
    feedback_port=5558
)

client.set_feedback_callback(on_feedback)
client.set_result_callback(on_result)
client.start_listening()

goal_id = client.send_goal({'n': 5})
result, status = client.wait_for_result(goal_id)

client.close()
```

## 🎉 完成！

现在你已经掌握了基础用法，可以查看 README.md 了解更多高级功能！
