# Echo Message System

基于 ZeroMQ 实现的轻量级 ROS 风格消息系统。

## 特性

✅ **四种通信模式**
- **Topic (Pub/Sub)**: 发布/订阅模式，支持多对多通信
- **Service**: 请求/响应模式，同步调用
- **Action**: 异步任务模式，支持进度反馈和取消
- **Parameter**: 参数服务器，集中管理系统参数 ✨ **新增**

✅ **核心设计**
- 每个订阅者使用独立的 ZMQ SUB socket
- 利用 ZMQ 原生 topic 过滤机制
- Action 的 Goal/Cancel 基于 Service 实现
- Action 的 Result/Feedback 基于 Topic 实现
- Service 使用 ZMQ REQ/REP 模式
- ROS Master 风格的服务发现和参数管理
- 参数服务器支持JSON格式的参数存储和文件持久化

✅ **支持的数据类型**
- IMU 数据 (6维双精度数据)
- 延迟数据 (时间戳数据)
- 图像数据 (二进制图像)
- 二进制数据 (通用二进制)

✅ **技术栈**
- C++17
- ZeroMQ 4.x
- nlohmann/json (JSON 序列化)
- 自定义轻量级日志系统

## 目录结构

```
c++/
├── include/              # 头文件
│   ├── message.h        # 消息定义
│   ├── publisher.h      # 发布者
│   ├── subscriber.h     # 订阅者
│   ├── service.h        # 服务端/客户端
│   ├── action.h         # Action 服务端/客户端
│   ├── master.h         # Master 服务器
│   ├── parameter_server.h # 参数服务器 ✨ 新增
│   └── logger.h         # 日志管理器
├── src/                  # 源文件
├── tests/                # 测试程序
├── CMakeLists.txt        # 构建配置
└── run_tests.sh          # 测试脚本
```

## 编译

### 依赖

```bash
# Ubuntu/Debian
sudo apt-get install cmake g++ libzmq3-dev pkg-config

# 或从源码安装 ZeroMQ
wget https://github.com/zeromq/libzmq/releases/download/v4.3.4/zeromq-4.3.4.tar.gz
tar xzf zeromq-4.3.4.tar.gz
cd zeromq-4.3.4
./configure && make && sudo make install
```

### 构建

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## 运行测试

```bash
chmod +x run_tests.sh
./run_tests.sh
```

或手动运行：

```bash
cd build

# 1. 启动 Master
./echo_master &

# 2. 运行测试
./test_pubsub      # Pub/Sub 测试
./test_service     # Service 测试
./test_action      # Action 测试 (待完善)
./test_all         # 综合测试

# 3. 停止 Master
pkill echo_master
```

## 使用示例

### Pub/Sub

```cpp
#include "publisher.h"
#include "subscriber.h"
#include "message.h"

// 订阅者
auto sub = std::make_shared<echo::Subscriber>(
    "/sensor/imu",
    [](const echo::Message::Ptr& msg) {
        auto imu_msg = std::dynamic_pointer_cast<echo::ImuMessage>(msg);
        // 处理消息
    }
);

// 发布者
auto pub = std::make_shared<echo::Publisher>("/sensor/imu");
auto msg = std::make_shared<echo::ImuMessage>(
    "/sensor/imu", timestamp, type,
    ax, ay, az, gx, gy, gz
);
pub->publish(msg);
```

### Service

```cpp
#include "service.h"

// 服务端
auto server = std::make_shared<echo::ServiceServer>(
    "/compute/add",
    [](const nlohmann::json& req) -> nlohmann::json {
        int result = req["a"].get<int>() + req["b"].get<int>();
        return {{"result", result}};
    }
);

// 客户端
auto client = std::make_shared<echo::ServiceClient>("/compute/add");
nlohmann::json req = {{"a", 10}, {"b", 20}};
nlohmann::json resp = client->call(req);
int result = resp["result"];  // 30
```

### Action

```cpp
#include "action.h"

// Action 服务端
auto server = std::make_shared<echo::ActionServer>(
    "/robot/move",
    [](uint32_t goal_id, const nlohmann::json& goal,
       std::function<void(const nlohmann::json&)> send_feedback,
       std::atomic<bool>& should_cancel) {
        // 执行长时间任务
        for (int i = 0; i < 100; i++) {
            if (should_cancel) return;
            send_feedback({{"progress", i}});
            // do work...
        }
    }
);

// Action 客户端
auto client = std::make_shared<echo::ActionClient>("/robot/move");
uint32_t goal_id = client->sendGoal(
    {{"target_x", 10}, {"target_y", 20}},
    [](uint32_t id, const nlohmann::json& fb) {
        // 处理反馈
    },
    [](uint32_t id, const nlohmann::json& res, bool success) {
        // 处理结果
    }
);
```

## 架构设计

### Topic 模式

```
Publisher (PUB socket, port: random)
           ↓
        Master (注册)
           ↓
Subscriber (SUB socket, 查询并连接)
```

- 每个 Subscriber 独立 socket + 独立接收线程
- ZMQ 原生 topic 过滤（高效）

### Service 模式

```
ServiceServer (REP socket)
              ↓
           Master (注册)
              ↓
ServiceClient (REQ socket, 连接)
```

- 请求-响应模式
- 支持超时控制

### Action 模式

```
ActionServer:
  - /action_name/send_goal (Service)
  - /action_name/cancel (Service)
  - /action_name/feedback (Publisher)
  - /action_name/result (Publisher)

ActionClient:
  - send_goal/cancel (ServiceClient)
  - feedback/result (Subscriber)
```

- Goal/Cancel 是请求-响应（适合 Service）
- Feedback/Result 是通知（适合 Topic）

## 性能特点

✅ **优势**
- 每个订阅者独立并发接收
- ZMQ 层面的高效过滤
- 无中心化消息分发（去除 MessageBus）
- 原生支持网络透明性

⚠️ **权衡**
- 每个订阅者需要独立资源（socket + 线程）
- 适合订阅者数量中等的场景（< 100）

## 与 ROS 的对比

| 特性 | Echo | ROS 1 |
|------|------|-------|
| Master | ✅ 服务发现 | ✅ roscore |
| Topic | ✅ 独立 socket | ⚠️ 共享 |
| Service | ✅ REQ/REP | ✅ 类似 |
| Action | ✅ Service+Topic | ✅ actionlib |
| 序列化 | JSON | ROS msg |
| 语言 | C++ | C++/Python |

## License

MIT

## 作者

Echo Message System Team
