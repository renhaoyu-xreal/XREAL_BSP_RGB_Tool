# XREAL_BSP_RGB_Tool

从 `RecordLabC` 抽离出的独立 BSP RGB 工具。  
这个仓库只服务 `glasses_bsp_node` 的 RGB 工作流，保留原有 Qt UI 风格、页面布局和操作顺序，尽量不增加现有成员的学习成本。

## 安装依赖

```bash
git clone <your-repo-url> XREAL_BSP_RGB_Tool
cd XREAL_BSP_RGB_Tool
./tool_scripts/install_dependencies.sh
```

安装脚本会完成这些事情：

- 安装 Ubuntu 22.04 所需系统依赖
- 安装 Python 编排层依赖
- 准备项目内 XREAL runtime
- 构建 GUI 和 `bsp_main_subnode`

## 开始运行

```bash
cd XREAL_BSP_RGB_Tool
./tool_scripts/start_xreal_bsp_rgb.sh
```

启动后会直接进入 BSP RGB 工作区，默认主流程保持和原 `RecordLabC` 一致：

1. 点击 `一键启动 RGB`
2. 等待设备进入 RGB 预览
3. 在 `BSP RGB` 页执行 RAW 抓取或相关 RGB 录制脚本

`数据 + 命令` 页仍保留，方便做辅助检查与命令操作。

## 更新项目

```bash
cd XREAL_BSP_RGB_Tool
./tool_scripts/update_project.sh
```

更新脚本会执行：

- `git pull --ff-only`
- 重新安装或校验依赖
- 重新构建当前项目

## 说明

- 本仓库不依赖 `Recordlab_host` 或 `Recordlab_nodes`
- 运行时外部依赖仅包括系统包、PyPI 包和仓库内 vendored `xreal_glasses` wheel
- 当前默认目标平台为 Ubuntu 22.04
