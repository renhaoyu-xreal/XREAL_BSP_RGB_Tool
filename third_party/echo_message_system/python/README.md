# Python Message System

一个轻量级的Python消息传递系统，支持发布/订阅（Publisher/Subscriber）、服务/客户端（Service/Client）和动作（Action）通信模式。

**无需ROS2依赖** - 基于ZeroMQ实现，可在任何Python环境中运行！

## 🌟 特性

- ✅ **发布/订阅模式** - 异步消息传递，支持多对多通信
- ✅ **服务/客户端模式** - 同步请求-响应通信
- ✅ **动作（Action）模式** - 异步目标-反馈-结果通信（ROS风格）
- ✅ **多话题支持** - 在同一端口上支持多个话题
- ✅ **灵活序列化** - 支持JSON和Pickle编码
- ✅ **线程安全** - 内置线程管理
- ✅ **完整日志** - 详细的日志输出
- ✅ **简单易用** - 清晰的API设计

## 📦 安装依赖

```bash
pip install pyzmq
```

## 🚀 快速开始

### 发布/订阅示例

**终端1 - 运行订阅者:**
```bash
cd python_message_system
python examples/subscriber_example.py
```

**终端2 - 运行发布者:**
```bash
python examples/publisher_example.py
```

### 服务/客户端示例

**终端1 - 运行服务端:**
```bash
python examples/service_server_example.py
```

**终端2 - 运行客户端:**
```bash
python examples/service_client_example.py 15 25
```

## 📚 API文档

### Publisher（发布者）

```python
from message_system import Publisher

# 创建发布者
pub = Publisher(
    name='my_publisher',
    topic='my_topic',
    port=5555,
    encoding='json'  # 或 'pickle'
)

pub.start()

# 发布消息
pub.publish({'message': 'Hello World!'})

pub.close()
```

### Subscriber（订阅者）

```python
from message_system import Subscriber

def callback(data):
    print(f"Received: {data}")

# 创建订阅者
sub = Subscriber(
    name='my_subscriber',
    topic='my_topic',
    callback=callback,
    host='localhost',
    port=5555,
    encoding='json'
)

sub.start()
sub.spin()  # 保持运行
sub.close()
```

### Service（服务）

```python
from message_system import Service

def handle_request(request):
    # 处理请求并返回响应
    result = request['a'] + request['b']
    return {'sum': result}

# 创建服务
service = Service(
    name='my_service',
    service_name='add_service',
    callback=handle_request,
    port=5556
)

service.start()
service.spin()
service.close()
```

### ActionServer（动作服务端）

```python
from message_system import ActionServer, GoalStatus
import time

def goal_callback(goal_id, goal_data, server):
    # 处理目标并发送反馈和结果
    for i in range(goal_data['steps']):
        # 发送进度反馈
        server.send_feedback(goal_id, {'current': i + 1, 'total': goal_data['steps']})
        time.sleep(0.1)
    
    # 发送最终结果
    server.send_result(goal_id, {'completed': True}, GoalStatus.SUCCEEDED)

# 创建动作服务
action_server = ActionServer(
    name='my_action_server',
    action_name='my_action',
    callback=goal_callback,
    goal_port=5557,
    feedback_port=5558
)

action_server.start()
action_server.spin()
action_server.close()
```

### ActionClient（动作客户端）

```python
from message_system import ActionClient

def on_feedback(goal_id, feedback):
    print(f"Progress: {feedback['current']}/{feedback['total']}")

def on_result(goal_id, result, status):
    print(f"Goal completed: {result}")

# 创建客户端
client = ActionClient(
    name='my_action_client',
    action_name='my_action',
    goal_port=5557,
    feedback_port=5558,
    timeout=5000
)

# 设置回调
client.set_feedback_callback(on_feedback)
client.set_result_callback(on_result)

# 启动监听
client.start_listening()

# 发送目标
goal_id = client.send_goal({'steps': 10})

# 等待结果
result, status = client.wait_for_result(goal_id)

client.close()
```

### ServiceClient（服务客户端）

```python
from message_system import ServiceClient

# 创建客户端
client = ServiceClient(
    name='my_client',
    service_name='add_service',
    host='localhost',
    port=5556,
    timeout=5000  # 毫秒
)

# 调用服务
response = client.call({'a': 10, 'b': 20})
print(f"Result: {response['sum']}")

client.close()
```

## 📖 示例程序

| 文件 | 说明 |
|------|------|
| `publisher_example.py` | 基本发布者示例 |
| `subscriber_example.py` | 基本订阅者示例 |
| `service_server_example.py` | 服务端示例 |
| `service_client_example.py` | 客户端示例 |
| `action_server_example.py` | 动作服务端示例（Fibonacci计算） |
| `action_client_example.py` | 动作客户端示例 |
| `multi_topic_example.py` | 多话题通信示例 |

## 🏗️ 项目结构

```
python_message_system/
├── message_system/          # 核心库
│   ├── __init__.py         # 包初始化
│   ├── message.py          # 消息序列化
│   ├── node.py             # 基础节点类
│   ├── publisher.py        # 发布者实现
│   ├── subscriber.py       # 订阅者实现
│   ├── service.py          # 服务实现
│   └── action.py           # 动作实现
├── examples/               # 示例代码
│   ├── publisher_example.py
│   ├── subscriber_example.py
│   ├── service_server_example.py
│   ├── service_client_example.py
│   ├── action_server_example.py
│   ├── action_client_example.py
│   └── multi_topic_example.py
├── tests/                  # 单元测试
│   ├── test_basic.py
│   ├── test_service.py
│   └── test_action.py
└── README.md              # 本文件
```

## 🔧 高级用法

### 多话题发布

```python
# 多个发布者可以使用同一端口的不同话题
pub1 = Publisher(name='pub1', topic='topic1', port=5555)
pub2 = Publisher(name='pub2', topic='topic2', port=5555)

pub1.publish({'data': 'from topic1'})
pub2.publish({'data': 'from topic2'})
```

### 使用Pickle编码复杂对象

```python
import numpy as np

# 发布者使用pickle编码
pub = Publisher(name='array_pub', topic='arrays', encoding='pickle')
pub.publish({'array': np.array([1, 2, 3, 4, 5])})

# 订阅者使用pickle解码
sub = Subscriber(
    name='array_sub',
    topic='arrays',
    callback=lambda data: print(data['array']),
    encoding='pickle'
)
```

### 自定义日志级别

```python
import logging

pub = Publisher(name='pub', topic='test', port=5555)
pub.logger.setLevel(logging.DEBUG)  # 显示调试信息
```

## 🧪 测试

```bash
cd python_message_system
python -m pytest tests/
```

## 🔍 常见问题

**Q: 为什么订阅者收不到消息？**  
A: 确保发布者和订阅者使用相同的话题名称和端口号，并且发布者先启动。

**Q: 如何在不同机器上通信？**  
A: 将订阅者和客户端的`host`参数设置为发布者/服务器的IP地址。

**Q: 支持哪些数据类型？**  
A: JSON编码支持基本类型（dict, list, str, int, float, bool, None），Pickle编码支持所有可序列化的Python对象。

**Q: 如何处理网络延迟？**  
A: 可以调整`ServiceClient`的`timeout`参数增加超时时间。

## 🆚 与ROS2的对比

| 特性 | 本系统 | ROS2 |
|------|--------|------|
| 依赖 | 仅需pyzmq | 需要完整ROS2环境 |
| 安装 | `pip install pyzmq` | 复杂的系统安装 |
| 学习曲线 | 简单 | 陡峭 |
| 性能 | 良好 | 优秀 |
| 功能 | 基础消息传递 | 完整机器人框架 |
| 适用场景 | 简单分布式应用 | 机器人系统 |

## 📝 许可证

MIT License

## 🤝 贡献

欢迎提交Issue和Pull Request！

## 📮 联系方式

有问题或建议？请提交Issue！
