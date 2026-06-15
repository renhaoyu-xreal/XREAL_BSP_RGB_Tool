# Localhost Agent 使用说明

## 概述
LocalhostAgent用于控制本地计算机，无需SubNode，直接执行本地操作。

## 配置
在`config/agents_config.json`中已配置：
```json
{
  "localhost": {
    "name": "localhost",
    "scripts_dir": "scripts"
  }
}
```

## 支持的命令

### 1. check - 检查状态
```python
result = await agent.cmd('check')
```

### 2. play_video - 播放视频
```python
result = await agent.cmd('play_video', {
    "file": "/path/to/video.mp4",
    "player": "vlc"  # 可选: vlc, mpv, xdg-open(默认)
})
```

### 3. play_audio - 播放音频
```python
result = await agent.cmd('play_audio', {
    "file": "/path/to/audio.mp3",
    "player": "mpv"  # 可选: mpv, vlc, aplay, xdg-open(默认)
})
```

### 4. run_script - 执行脚本
```python
# 执行scripts目录下的脚本
result = await agent.cmd('run_script', {
    "script": "example_notification.sh",
    "args": ["Custom Message", "Custom Title"]
})

# 或直接用脚本名作为命令
result = await agent.cmd('example_notification.sh', {
    "args": ["Hello", "Test"]
})
```

### 5. stop_all - 停止所有播放器
```python
result = await agent.cmd('stop_all')
```

## 脚本目录
将自定义脚本放在`scripts/`目录下：
- Shell脚本: `.sh`
- Python脚本: `.py`
- 其他可执行文件

## 示例脚本
已创建`example_notification.sh`示例脚本，用法：
```bash
./scripts/example_notification.sh "Your Message" "Title"
```

## 眼镜屏视频播放
C++ 版本的 3DoF/NViz 脚本通过 `play_video_on_secondary_screen.py` 调用 `mpv`，默认使用无边框窗口定位到非主显示器，避免 Wayland 下全屏窗口回到当前显示器。

安装依赖：
```bash
sudo ./setup.sh
```

手工验证：
```bash
python3 scripts/test_play_video_on_glasses_screen.py /path/to/video.mp4
```

## 脚本参数弹窗
RecordLabC 中脚本所需的业务参数由脚本自身通过 `dialog.multi_field_input(...)` 弹窗获取。界面只负责启动脚本、显示日志和流程状态，不再为 BSP/RAW/NViz 脚本维护固定参数输入框。

## 使用示例
```python
from flowagent.core.agent_manager import AgentManager

# 初始化
manager = AgentManager()
manager.initialize_agent("localhost")
agent = manager.get_agent("localhost")

# 播放视频
await agent.cmd('play_video', {"file": "/path/to/demo.mp4"})

# 运行自定义脚本
await agent.cmd('run_script', {
    "script": "my_script.sh",
    "args": ["arg1", "arg2"]
})
```
