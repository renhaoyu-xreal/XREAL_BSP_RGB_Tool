# XREAL_BSP_RGB_Tool 使用文档

## 1. 安装依赖

```bash
cd XREAL_BSP_RGB_Tool
./tool_scripts/install_dependencies.sh
```

如果只是临时检查 GUI，不需要初始化 XREAL runtime，可以执行：

```bash
RECORDLABC_SKIP_XREAL_RUNTIME=1 ./tool_scripts/install_dependencies.sh
```

## 2. 启动软件

```bash
cd XREAL_BSP_RGB_Tool
./tool_scripts/start_xreal_bsp_rgb.sh
```

软件会直接进入 `glasses_bsp_node` 的 RGB 工作区，保持原 `RecordLabC` BSP RGB 的 UI 风格和主操作顺序。

## 3. 标准操作顺序

1. 在 `BSP RGB` 页点击 `一键启动 RGB`
2. 等待设备进入 RGB 模式并出现预览
3. 根据实验需要执行 RAW 抓取或 BSP RGB 相关录制脚本
4. 如需查看运行态、命令结果或辅助检查，可切换到 `数据 + 命令` 页

## 4. 更新项目

```bash
cd XREAL_BSP_RGB_Tool
./tool_scripts/update_project.sh
```

这个脚本会自动拉取最新代码，并重新安装依赖与构建。
