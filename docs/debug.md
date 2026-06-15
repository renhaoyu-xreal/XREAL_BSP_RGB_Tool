# Debug Notes

## BSP 数据链路

- `open_glasses` 只打开 SDK 对象；真正开始出数据的是后续 `start_sensors`。
- XREAL SDK 数据最先进入 Python bridge：
  - `self.glasses.imuUpdated -> _on_imu_updated`
  - `self.glasses.camUpdated -> _on_cam_updated`
- Python bridge 和 C++ 不直接互调，二者通过 `QProcess` 的 stdin/stdout 自定义二进制帧通信：
  - C++ `NativeGlassesAdapter::sendRequest()` 写 request 到 bridge stdin。
  - Python bridge 收到 SDK signal 后写 event 到 stdout。
  - C++ `NativeGlassesAdapter::handleStdoutReadyRead()` 读取 stdout 并拆帧。
- C++ 侧 bridge event 流向：
  - `NativeGlassesAdapter::handleBridgeMessage()`
  - `GlassesFactory` 设置的 callback
  - `BspDevice::onImuData()` / `BspDevice::onCamData()`
  - `MainSubnode::imuDataCallback()` / `MainSubnode::imageDataCallback()`
- `MainSubnode` 再把标准化后的数据发布到 echo-message-system：
  - IMU 发布到 `imu_data`
  - Camera metadata 发布到 `camera_data`
  - `time_delay`、`motion_status`、`record_timer` 也由 `MainSubnode` 发布
- `DataReceiverProcess` 是 echo 总线下游消费者：
  - 通过 `echo::Subscriber` 订阅 `imu_data` / `camera_data` 等 topic。
  - 收到后转换为 UI 友好的格式，放入 pending 队列。
  - `DataReceiverManager` 每 100ms 轮询 pending 队列并更新 UI/曲线缓存。
- Camera 特殊点：
  - 图像字节由 bridge 传给 `NativeGlassesAdapter` 后写入共享内存。
  - echo 的 `camera_data` 主要传 metadata 和 `shm_seq`，UI 显示线程再读共享内存。
- 录制链路：
  - IMU CSV 写入在 `MainSubnode::imuDataCallback()` 内部直接处理原始回调。
  - 不依赖 `DataReceiver`，也不依赖 UI 订阅到的 echo 预览频率。

## IMU 曲线 15s+ 延迟

- 现象：晃动眼镜后，`IMU0-acc` 曲线约 15 秒甚至更久才明显变化。
- 已排除：
  - Python bridge stdout 队列未积压：`out_queue` 大多 `0/2048`，最高约 `5/2048`。
  - `DataReceiverManager` 和曲线缓冲未积压：`pending_before` 约几十，`curve buffered` 约 6-7。
  - UI 不是主要原因。
- 关键证据：
  - `DataReceiver` / `DataMonitorWidget` 打印的 `age_ms` 可达 `16000+ms`。
  - 说明样本到达 `DataReceiver` 前已经是旧数据。
- 根因判断：
  - `MainSubnode -> echo/ZMQ -> DataReceiver` 之间发生积压。
  - 原因是 `MainSubnode` 把高频 IMU 全量发布到 `imu_data` topic，约数千条 JSON/s，订阅端开始追旧消息。
- 中间修复尝试：
  - 曾在 `MainSubnode::imuDataCallback()` 对发往 echo 总线的 IMU 预览数据做 `100Hz/type`、后续 `1000Hz/type` 限流，能缓解 UI 延迟。
  - 但这与旧 Python 项目行为不一致：旧版 `IMU0-gyro`、`IMU0-acc` 等单路应接近 `1000Hz`，限流会让左侧频率显示偏低。
- 当前修复：
  - `scripts/xreal_bridge_worker.py` 中 IMU 回调改为 `Qt.DirectConnection`，避免 Python/Qt queued event loop 压低高频回调。
  - bridge worker 不再逐条 IMU 写 stdout，而是攒成 `imu_batch` 批量帧发送，减少 `QProcess` 自定义帧协议的跨进程开销。
  - `NativeGlassesAdapter::handleBridgeMessage()` 支持 `imu_batch` 并在 C++ 侧展开成原来的单条 IMU payload。
  - `MainSubnode::imuDataCallback()` 对 `imu_data` 全量发布，不再按处理时间或设备时间戳做预览限流。
  - `DataReceiverProcess` 对 `imu_data` 使用 raw JSON 热路径解析，避免每条消息完整 `json::parse` 成为瓶颈。
  - UI 仍通过 `DataReceiverProcess::uiSendIntervalForData()` 和 `DataReceiverManager` 轮询/合并做展示抽帧，频率统计基于原始接收消息，不等同于 UI 刷新率。
- 注意：
  - `IMU0-gyro`、`IMU0-acc`、`IMU1-gyro`、`IMU1-acc` 等单路频率应作为是否恢复旧版行为的主要观察项。
  - 曾临时增加过 `IMU-total` 统计，但它只表示 `imu_data` 总吞吐，不能替代单路 1000Hz 判断，已撤掉。
  - 如果单路仍明显低于 `1000Hz`，下一步应在 `BspDevice::onImuData()` 入口按 `type` 统计原始样本数，区分 SDK/bridge 入站不足和 echo 发布订阅损耗。
