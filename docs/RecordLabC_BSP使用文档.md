# RecordLabC BSP 录数据使用指南

本文档对应当前工程 `RecordLabC`，用于指导在 Ubuntu 22.04 + Qt6 环境下，使用 `glasses_bsp_node` 完成 BSP 眼镜数据采集。

## 1. 安装

### 1.1 使用分发包安装（推荐给普通用户）

如果拿到的是 `RecordLabC-linux-x86_64.tar.gz` 分发包，直接执行：

```bash
tar -xzf RecordLabC-linux-x86_64.tar.gz
cd RecordLabC
./install_dependencies.sh
./RecordLabC.sh
```

`install_dependencies.sh` 会自动完成：

1. 通过 `setup.sh` 安装 Ubuntu / Qt / ZeroMQ / Python 通用依赖
2. 通过 `setup_xreal_runtime.sh` 准备项目内 XREAL runtime
3. 将 `third_party/xreal_glasses/xreal_glasses-0.4.3-py3-none-any.whl` 安装到 `runtime/xreal_runtime/site-packages/`

XREAL runtime 安装在当前 `RecordLabC/` 目录内，不会写入系统 Python 全局环境。

如果只是临时打开 GUI，不使用 BSP / XREAL 真机链路，可以跳过 XREAL runtime：

```bash
RECORDLABC_SKIP_XREAL_RUNTIME=1 ./install_dependencies.sh
```

### 1.2 准备工程目录

本文默认工程目录已经存在：

```bash
cd RecordLabC
```

后续所有命令都默认在这个目录下执行。

### 1.3 安装系统依赖

推荐直接使用工程自带的一键安装脚本：

```bash
./install_dependencies.sh
```

该脚本会自动安装：

1. C++/Qt6 构建依赖
2. `sshpass`
3. `adb`
4. `pip`
5. ZeroMQ 依赖
6. 脚本运行所需的 Python 包
7. 项目内 XREAL runtime

如果你不想使用一键脚本，也可以手动安装：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config sshpass adb python3-pip
sudo apt-get install -y qt6-base-dev qt6-declarative-dev libqt6network6
sudo apt-get install -y libgl-dev libopengl-dev libegl1-mesa-dev libglx-dev
sudo apt-get install -y libzmq3-dev
```

### 1.4 安装 Python 依赖

以下命令均在项目根目录 `RecordLabC/` 下执行。

#### 1.4.1 安装本地子模块

首先安装 `echo_message_system` 子模块：

```bash
python3 -m pip install -e ./third_party/echo_message_system/python
```

#### 1.4.2 安装项目其他依赖

```bash
python3 -m pip install pyzmq paramiko numpy Pillow PySide6
```

> **注意**：上一条命令中的 `-e`（`--editable`）表示开发模式安装，对本地源码的修改会立即生效，无需重新安装。

#### 1.4.3 安装 XREAL runtime

真机 BSP 链路还需要安装 XREAL runtime：

```bash
bash ./setup_xreal_runtime.sh
```

其中 `xreal_glasses-0.4.3-py3-none-any.whl` 会被安装到：

```text
runtime/xreal_runtime/site-packages/
```

### 1.5 编译

首次使用，或修改过 C++ 代码之后，需要重新编译：

```bash
./build.sh
```

---

## 2. 启动 RecordLabC

### 2.1 启动

在项目根目录 `RecordLabC/` 下执行：

```bash
chmod +x start.sh run.sh    # 首次运行时建议先加执行权限
./start.sh
```

`start.sh` 会自动转发到新的 `run.sh` 启动器。启动脚本会自动完成以下操作：

1. 清理旧的 `recordlabc` / `bsp_main_subnode` / 脚本运行时残留进程
2. 清理相机共享内存相关的 Qt IPC 残留
3. 检查可执行文件是否存在
4. 启动控制中心 UI

如果启动失败，优先先执行：

```bash
./doctor.sh --json
```

若 `doctor.sh` 提示找不到 `build/recordlabc_doctor`，说明你还没有完成编译，请先执行 `./build.sh`。

### 2.2 关闭

- 正常情况下，直接关闭 UI 窗口即可
- 如果关闭后仍然发现下次启动时有旧日志、旧曲线、旧设备状态残留，可以手动清理：

```bash
pkill -9 -f "recordlabc|bsp_main_subnode|recordlabc_agent_cmd|run_recordlab_script.py"
```

> **注意**：如果刚刚点过“停止脚本”，请尽量等几秒让录制完成收尾，再关闭整个程序。

---

## 3. 控制中心 UI 操作

启动后请选择主 agent：`glasses_bsp_node`，然后进入控制中心窗口。当前 BSP 录制的标准流程仍然是：

1. 连接子节点
2. 启动设备
3. 看到设备进入可录制状态
4. 执行录制脚本

### 3.1 连接子节点（Tab3：Agent 管理）

在执行录制命令之前，需要先确保 BSP 子节点已经连接：

1. 切换到 **Tab3**（Agent 管理）
2. 找到 `glasses_bsp_node`
3. 点击 **Connect** 按钮

如果已经显示为已连接，则可以跳过这一步。

> **连接失败的可能原因**：
>
> - 眼镜未通过 USB 正确连接电脑
> - 眼镜 SSH 不可达，默认地址为 `169.254.2.1`
> - 上次残留进程占用了端口，需要先执行 `pkill -9 -f "recordlabc|bsp_main_subnode"`

### 3.2 启动设备（Tab2：数据 + 控制）

在执行任何 BSP 录制脚本之前，需要先让设备进入可录制状态：

1. 切换到 **Tab2**（数据 + 控制）
2. 确认右侧 Agent 选择的是 `glasses_bsp_node`
3. 等待初始化完成
4. 进入可录制状态

当前版本中，最稳定的判断标准是看到类似状态：

```text
[状态 6] 设备已就绪，可以进入录制阶段
```

或者你已经能在界面里看到：

- IMU 频率不为 0
- IMU 曲线开始刷新
- 双目图像正常出图

### 3.2.1 一键启动 `glasses_bsp_node`

为了减少重复操作，现在 **Tab1** 和 **Tab2** 右侧都提供了 **`一键Glasses_bsp_node`** 按钮。

该按钮会自动完成以下动作：

1. 检查当前主 agent 是否为 `glasses_bsp_node`
2. 自动发起 Connect
3. 等待 watchdog 完成 `init_device`
4. 自动执行 `start_device`

**使用方法：**

1. 启动 RecordLabC 后，进入 **Tab1** 或 **Tab2**
2. 点击 **`一键Glasses_bsp_node`**
3. 等待日志输出完成
4. 看到设备进入 **状态 6** 后，再执行脚本

**说明：**

- 这是当前最推荐的启动方式
- 遇到异常时，仍然可以回到 **Tab3 + Tab2** 手动排查

### 3.2.2 眼镜热插拔说明

当前版本支持眼镜运行中的热插拔，但插拔过程中出现以下现象属于正常：

**拔下眼镜后：**

- UI 停留在拔下瞬间的最后一帧
- IMU 频率变成 `0`
- 可能出现 SSH 失败日志

**重新插上眼镜后：**

- watchdog 会尝试重新初始化设备
- `一键Glasses_bsp_node` 会在重新可启动后恢复可点击
- 重新进入 **状态 6** 后即可继续录制

**推荐操作：**

1. 重新插上眼镜
2. 等待几秒
3. 再次点击 **`一键Glasses_bsp_node`**
4. 确认设备重新进入可录制状态后再启动脚本

### 3.3 执行录制脚本（Tab1 / Tab4：脚本执行）

1. 切换到 **Tab1** 或 **Tab4**
2. 在脚本列表中勾选要执行的脚本
3. 点击 **“运行脚本”**

**重要提醒：**

- 自由录实验不要批量执行
- 同一条录制请在同一个 Tab 内完成“启动 -> 观察日志 -> 停止”
- 不要在 **Tab1** 启动脚本，却跑到 **Tab4** 去停止
- 如果日志提示 **“上次录制正在收尾，请等待”**，说明上一轮录制还没有完全收尾，不要重复点击启动

当前版本脚本页会自动复用已经准备好的 `glasses_bsp_node`。因此正常情况下：

- 先把设备准备到 **状态 6**
- 再运行脚本
- 不需要在脚本启动后自己重复执行 `init_device` / `start_device`

### 3.3.1 双目图像点击放大查看（Tab1 / Tab2）

现在 **Tab1** 和 **Tab2** 里的双目图像支持点击放大查看。

**使用方法：**

1. 在 **Tab1** 或 **Tab2** 中看到双目预览正常显示后，直接点击左图或右图
2. 系统会弹出单独的图像查看窗口
3. 可以执行以下操作：
   - 鼠标滚轮缩放
   - 点击 `+` / `-` 放大缩小
   - 点击 **重置** 恢复初始大小
   - 点击 **保存截图** 把当前图像保存到你自己选择的位置

该功能只用于查看和手动保存，不影响后台录制。

---

## 4. 四个 IMU 录制脚本

### 4.1 自由录 IMU 数据 (`record_bsp_imu.py`)

| 项目 | 说明 |
|------|------|
| 用途 | 不限时长的自由录制 IMU 数据 |
| 启动 | 勾选脚本 -> 点击“运行脚本” |
| 停止 | 点击 **“停止脚本”** 按钮 |
| 保存路径 | `data/free_record/only_imu/<眼镜SN>_<实验关键字>_<录制人>_free_record_only_imu_<时间戳>/` |

**产出文件：**

```text
data/free_record/only_imu/<眼镜SN>_<实验关键字>_<录制人>_free_record_only_imu_<时间戳>/
├── imu_0.csv                    # IMU0 数据（gyro/acc/mag/temperature）
├── imu_1.csv                    # IMU1 数据（gyro/acc/temperature）
├── record_info.txt              # 录制信息 + 设备属性
├── glass_config.json            # 眼镜配置
├── mic_record.wav               # 麦克风录音
├── camera_rgb.csv               # 双目相机 RGB 均值（每约 1 秒）
├── record_screen_rgb_info.csv   # 镜片屏幕 RGB 均值（每约 10 秒）
├── cam0/snapshots/              # 左相机快照（开始/每分钟/结束）
├── cam1/snapshots/              # 右相机快照（开始/每分钟/结束）
└── screenshots/                 # 镜片屏幕截图（开始/每分钟/结束）
```

目录最后一级命名格式为：

```text
<眼镜SN>_<实验关键字>_<录制人>_free_record_only_imu_<时间戳>
```

---

### 4.2 静止录 IMU 数据 (`record_bsp_imu_static.py`)

| 项目 | 说明 |
|------|------|
| 用途 | 录制 10 秒静止 IMU 数据 |
| 启动 | 勾选脚本 -> 点击“运行脚本” |
| 停止 | 10 秒后自动停止 |
| 保存路径 | `data/still10s_imu/<眼镜SN>_<实验关键字>_<录制人>_still10s_imu_<时间戳>/` |

**运行逻辑：**

- 录制过程中持续检测 `motion_status()`
- 检测到 `"moving"` 或 `"active"`，会立即停止录制
- 失败时会删除本次数据目录
- 全程静止则保留数据

**产出文件：**

与自由录 `record_bsp_imu.py` 基本一致，只是保存路径不同。

---

### 4.3 非静止录 IMU 数据 (`record_bsp_imu_dynamic.py`)

| 项目 | 说明 |
|------|------|
| 用途 | 录制 10 秒非静止 IMU 数据 |
| 启动 | 勾选脚本 -> 点击“运行脚本” |
| 停止 | 10 秒后自动停止 |
| 保存路径 | `data/nostill10s_imu/<眼镜SN>_<实验关键字>_<录制人>_nostill10s_imu_<时间戳>/` |

**说明：**

- 录制期间请正常移动眼镜
- 该脚本同样会生成相机快照、屏幕截图、RGB 统计和麦克风录音

**产出文件：**

与自由录 `record_bsp_imu.py` 基本一致，只是保存路径不同。

---

### 4.4 自由录 IMU + Camera 数据 (`record_bsp_imu_cam.py`)

| 项目 | 说明 |
|------|------|
| 用途 | 不限时长录制 IMU + 双目相机图像 |
| 启动 | 勾选脚本 -> 点击“运行脚本” |
| 停止 | 点击 **“停止脚本”** 按钮 |
| 保存路径 | `data/free_record/imu_and_cam/<眼镜SN>_<实验关键字>_<录制人>_free_record_imu_and_cam_<时间戳>/` |

**产出文件：**

```text
data/free_record/imu_and_cam/<眼镜SN>_<实验关键字>_<录制人>_free_record_imu_and_cam_<时间戳>/
├── imu_0.csv
├── imu_1.csv
├── record_info.txt
├── glass_config.json
├── mic_record.wav
├── camera_rgb.csv
├── record_screen_rgb_info.csv
├── cam0/
│   ├── images/                  # 左相机灰度图
│   │   ├── 000000.pgm
│   │   ├── 000001.pgm ...
│   │   ├── metadata.txt
│   │   └── timestamps.txt
│   └── snapshots/               # 左相机 PNG 快照（开始/每分钟/结束）
├── cam1/
│   ├── images/                  # 右相机灰度图
│   │   ├── 000000.pgm
│   │   ├── 000001.pgm ...
│   │   ├── metadata.txt
│   │   └── timestamps.txt
│   └── snapshots/               # 右相机 PNG 快照（开始/每分钟/结束）
└── screenshots/                 # 镜片屏幕截图
```

与纯 IMU 版本相比，这个脚本额外会保存 `cam0/images/` 和 `cam1/images/` 下的高频灰度图像。

---

## 5. 批量执行多个脚本

当前支持在 **Tab1 / Tab4** 中同时勾选多个脚本，点击“运行脚本”后会按顺序依次执行。

**建议：**

- 批量执行只用于固定时长脚本
- 更适合批量执行的是：
  - `record_bsp_imu_static.py`
  - `record_bsp_imu_dynamic.py`
- 不建议把以下两个自由录脚本放进批量执行：
  - `record_bsp_imu.py`
  - `record_bsp_imu_cam.py`

例如：

1. 勾选 `record_bsp_imu_static.py`
2. 再勾选 `record_bsp_imu_dynamic.py`
3. 点击“运行脚本”

系统会先执行静止 10 秒实验，再执行非静止 10 秒实验。

---

## 6. 数据文件说明

| 文件 | 格式 | 说明 |
|------|------|------|
| `imu_0.csv` | CSV | IMU0 的陀螺仪、加速度计、磁力计、温度数据 |
| `imu_1.csv` | CSV | IMU1 的陀螺仪、加速度计、温度数据 |
| `camera_rgb.csv` | CSV | 双目相机 RGB 均值，约每 1 秒记录一次 |
| `record_screen_rgb_info.csv` | CSV | 镜片屏幕 RGB 均值，约每 10 秒记录一次 |
| `mic_record.wav` | WAV | 麦克风录音 |
| `cam0/snapshots/*.png` | PNG | 左相机快照（开始、每分钟、结束） |
| `cam1/snapshots/*.png` | PNG | 右相机快照（开始、每分钟、结束） |
| `screenshots/*.png` | PNG | 镜片屏幕截图（开始、每分钟、结束） |
| `cam0/images/*.pgm` | PGM | 左相机灰度图（仅 `record_bsp_imu_cam.py`） |
| `cam1/images/*.pgm` | PGM | 右相机灰度图（仅 `record_bsp_imu_cam.py`） |
| `cam0/images/metadata.txt` | TXT | 左相机图像元数据 |
| `cam0/images/timestamps.txt` | TXT | 左相机图像时间戳 |
| `cam1/images/metadata.txt` | TXT | 右相机图像元数据 |
| `cam1/images/timestamps.txt` | TXT | 右相机图像时间戳 |
| `record_info.txt` | TXT | 录制信息、设备属性、辅助记录信息 |
| `glass_config.json` | JSON | 眼镜配置文件 |

---

## 7. 常见问题

### Q: 脚本执行时报 `glasses_bsp_node action server 不可用`

**A**：通常是以下几种原因：

1. 设备其实还没有进入 **状态 6**
2. 你运行的是旧的 `recordlabc` 进程
3. 上一次修改过 C++ 代码但没有重新编译

建议按下面顺序处理：

1. 先完整退出程序
2. 在项目根目录执行 `cmake --build build`
3. 重新启动 `./start.sh`
4. 重新点击 **`一键Glasses_bsp_node`**
5. 等设备进入 **状态 6** 后再执行脚本

### Q: 自由录脚本点击“停止脚本”后，日志里显示“脚本被停止”，这是异常吗？

**A**：不是异常。自由录脚本本来就是通过 **“停止脚本”** 按钮中断的，所以日志里出现“脚本被停止”是正常现象。关键是停止后数据目录里应当已经落下本次录制产物。

### Q: 静止实验结束后数据目录不见了，是否异常？

**A**：这通常不是异常。`record_bsp_imu_static.py` 的设计就是：

- 如果录制过程中检测到运动
- 就立即停止录制
- 删除本次数据目录

因此只有“全程静止成功”的那次录制才会保留目录。

### Q: `camera_rgb.csv` 全是 `-1`，或者 `cam0/snapshots/`、`cam1/snapshots/` 里没有图片

**A**：先做以下检查：

1. 确认你运行的是最新 build
2. 完整重启 `recordlabc`
3. 确认录制前双目图像在 UI 中已经正常显示
4. 再重新执行脚本

如果双目预览本身就没图，那么快照和 RGB 统计也不会正常。

### Q: `screenshots/` 目录为空，或者 `record_screen_rgb_info.csv` 没有内容

**A**：镜片截图依赖眼镜侧命令：

```bash
display_debug capture
```

该命令会在眼镜 `/usrdata/` 下生成 `dump_vi_*.yuv`。如果眼镜 SSH 不通，或 `display_debug capture` 连续失败，就会导致屏幕截图为空。此时优先检查：

1. 眼镜是否通过 USB 正确连接
2. `169.254.2.1` 是否可达
3. 是否能正常 SSH 到眼镜

### Q: 麦克风录音为空

**A**：请先检查本机是否能识别录音设备：

```bash
arecord -l
```

如果列表里看不到对应设备，优先检查 USB 连接和系统声卡识别状态。

### Q: 眼镜拔掉后，界面停在最后一帧、IMU 频率变成 0，是否异常？

**A**：这在当前热插拔流程里属于正常现象。重新插上眼镜后，再次点击 **`一键Glasses_bsp_node`** 即可。

### Q: 改完代码之后要不要重新 build？

**A**：分情况：

- 改了 `src/`、`include/`、`CMakeLists.txt`：要重新 build
- 只改了 `scripts/`、`docs/`、`config/`：通常不用 build
