cmake_minimum_required(VERSION 3.21)

# RecordLabC 是完全独立的新工程。
# 这里刻意不引用旧工程的 Python 构建逻辑，避免在迁移初期出现
# "新旧工程耦合、相互污染"的问题。
project(RecordLabC VERSION 0.1.0 LANGUAGES CXX)

# 当前统一使用 C++17。
# 这样可以兼顾 Ubuntu 22.04 上常见工具链的可用性，同时满足 Qt 6.5
# 和后续 IPC / 设备层实现的基本表达能力。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 启用 Qt 常用的自动代码生成能力，减少后续手工 moc/uic/rcc 管理负担。
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ============================================================================
# 依赖：Qt6
# ============================================================================
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network)

# ============================================================================
# 依赖：ZeroMQ（echo_message_system 需要）
# ============================================================================
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

# ============================================================================
# 依赖：nlohmann/json（echo_message_system 和消息类型需要）
# ============================================================================
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
)
FetchContent_GetProperties(json)
if(NOT json_POPULATED)
    FetchContent_Populate(json)
    add_subdirectory(${json_SOURCE_DIR} ${json_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# ============================================================================
# 子库：echo_message_system (cpp-refactor)
#
# 直接编译其源文件作为静态库，避免子目录 CMakeLists.txt 的冲突。
# ============================================================================
set(ECHO_MSG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/echo_message_system/cpp-refactor)

add_library(echo_message_system STATIC
    ${ECHO_MSG_DIR}/src/context.cpp
    ${ECHO_MSG_DIR}/src/publisher.cpp
    ${ECHO_MSG_DIR}/src/subscriber.cpp
    ${ECHO_MSG_DIR}/src/service.cpp
    ${ECHO_MSG_DIR}/src/action.cpp
    ${ECHO_MSG_DIR}/src/master.cpp
    ${ECHO_MSG_DIR}/src/parameter_server.cpp
)

target_include_directories(echo_message_system PUBLIC
    ${ECHO_MSG_DIR}/include
    ${ZMQ_INCLUDE_DIRS}
)

target_link_libraries(echo_message_system PUBLIC
    ${ZMQ_LIBRARIES}
    nlohmann_json::nlohmann_json
    pthread
)

target_link_directories(echo_message_system PUBLIC
    ${ZMQ_LIBRARY_DIRS}
)

# ============================================================================
# 主可执行文件
# ============================================================================
add_executable(recordlabc
    # --- main ---
    src/main.cpp

    # --- app (UI 壳) ---
    src/app/main_window.cpp
    src/app/workspace_page.cpp

    # --- bsp (BSP 页面与工作流) ---
    src/bsp/bsp_guide_status.cpp
    src/bsp/bsp_rgb_page.cpp
    src/bsp/bsp_asset_resolver.cpp
    src/bsp/bsp_orchestration_bridge.cpp
    src/bsp/bsp_sdk_locator.cpp

    # --- workflow (统一工作流控制器) ---
    src/workflow/workflow_controller.cpp
    src/bsp/xreal_runtime_locator.cpp
    src/bsp/native_glasses_adapter.cpp

    # --- core ---
    src/core/app_context.cpp
    src/core/legacy_config_loader.cpp
    src/core/legacy_paths.cpp
    src/core/logger.cpp
    src/core/usb_device_catalog.cpp

    src/script/python_script_bridge.cpp

    # --- widgets ---
    src/widgets/simple_curve_plot_widget.cpp
    src/widgets/camera_display_thread.cpp
    src/widgets/image_display_widget.cpp

    # --- common (Phase 1 新增) ---
    src/common/data_registry_client.cpp
    src/common/camera_shared_memory.cpp

    # --- flowagent (Phase 2 新增) ---
    src/flowagent/agents/base_agent.cpp
    src/flowagent/agents/legacy_remote_action_client.cpp
    src/flowagent/agents/localhost_agent.cpp

    # --- subnodes (Phase 2 新增) ---
    src/subnodes/base_subnode.cpp

    # --- Phase 3: 后端进程层 ---
    src/core/global_state.cpp
    src/core/ui_message_bridge.cpp
    src/backend/data_registry_server.cpp
    src/backend/agent_manager_process.cpp
    src/backend/agent_watchdog.cpp
    src/flowagent/core/agent_manager.cpp

    # --- Phase 4: glasses_bsp_node 核心链路 ---
    src/subnodes/main_subnode.cpp
    src/subnodes/bsp_main_subnode.cpp

    # --- Phase 5: 数据层 & 脚本执行 ---
    src/common/motion_detector.cpp
    src/flowagent/core/script_executor.cpp

    # --- Phase 5-6: 数据接收 ---
    src/backend/data_receiver.cpp

    # --- Q_OBJECT 头文件（AUTOMOC 需要在 sources 中列出） ---
    include/recordlab/app/main_window.h
    include/recordlab/app/workspace_page.h
    include/recordlab/backend/agent_watchdog.h
    include/recordlab/subnodes/base_subnode.h
    include/recordlab/backend/agent_manager_process.h
    include/recordlab/backend/data_receiver.h
    include/recordlab/backend/data_registry_server.h
    include/recordlab/bsp/bsp_rgb_page.h
    include/recordlab/workflow/workflow_controller.h
    include/recordlab/core/ui_message_bridge.h
    include/recordlab/flowagent/agents/base_agent.h
    include/recordlab/flowagent/agents/localhost_agent.h
    include/recordlab/flowagent/core/agent_manager.h
    include/recordlab/flowagent/core/script_executor.h
    include/recordlab/script/python_script_bridge.h
    include/recordlab/subnodes/main_subnode.h
    include/recordlab/subnodes/bsp_main_subnode.h
    include/recordlab/widgets/camera_display_thread.h
    include/recordlab/widgets/image_display_widget.h
)

# 所有头文件统一从 include/ 暴露，保持新工程结构清晰。
target_include_directories(recordlabc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 将当前源码根目录注入到可执行文件中。
# 这样启动时可以可靠地从兄弟目录只读定位旧版 RecordLab。
target_compile_definitions(recordlabc
    PRIVATE
        RECORDLABC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# ui_message_bridge.cpp 的 include 路径含 "ui_" 前缀, CMake AUTOUIC 会误判为 .ui 文件。
# 手动关闭此文件的 AUTOUIC 扫描。
set_source_files_properties(
    src/core/ui_message_bridge.cpp
    PROPERTIES SKIP_AUTOUIC ON
)

# 链接所有依赖
target_link_libraries(recordlabc
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        echo_message_system
)

# 保持编译告警尽量严格，方便在迁移早期尽快暴露问题。
if(MSVC)
    target_compile_options(recordlabc PRIVATE /W4)
else()
    target_compile_options(recordlabc PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 输出名单独固定，后面即使 target 名有调整，也不影响最终可执行文件名。
set_target_properties(recordlabc PROPERTIES
    OUTPUT_NAME "recordlabc"
)

# ============================================================================
# 独立子节点可执行文件: bsp_main_subnode
# Agent 通过 QProcess 启动子节点进程
# ============================================================================
add_executable(bsp_main_subnode
    src/subnodes/bsp_main_subnode_entry.cpp
    src/bsp/native_glasses_adapter.cpp
    src/bsp/xreal_runtime_locator.cpp
    src/common/camera_shared_memory.cpp
    src/subnodes/bsp_main_subnode.cpp
    src/subnodes/main_subnode.cpp
    src/subnodes/base_subnode.cpp
    src/common/motion_detector.cpp
    src/common/data_registry_client.cpp

    # 这些 Q_OBJECT 头文件必须显式列给该 target，
    # 否则独立子节点目标的 AUTOMOC 不会生成对应 moc，
    # 最终在链接阶段炸成 vtable / signal 缺失。
    include/recordlab/subnodes/base_subnode.h
    include/recordlab/subnodes/main_subnode.h
    include/recordlab/subnodes/bsp_main_subnode.h
)

target_include_directories(bsp_main_subnode
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(bsp_main_subnode
    PRIVATE
        RECORDLABC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(bsp_main_subnode
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        echo_message_system
)

set_target_properties(bsp_main_subnode PROPERTIES
    AUTOMOC ON
)

# ============================================================================
# 诊断工具: recordlabc_agent_probe
# 用于单独验证 init_agent / check / init_device / start_device 链路
# ============================================================================
add_executable(recordlabc_agent_probe
    src/tools/agent_probe_main.cpp
    src/backend/agent_manager_process.cpp
    src/backend/agent_watchdog.cpp
    src/core/global_state.cpp
    src/core/app_context.cpp
    src/core/legacy_config_loader.cpp
    src/core/legacy_paths.cpp
    src/common/data_registry_client.cpp
    src/common/motion_detector.cpp
    src/flowagent/core/agent_manager.cpp
    src/flowagent/agents/base_agent.cpp
    src/flowagent/agents/legacy_remote_action_client.cpp
    src/flowagent/agents/localhost_agent.cpp
    src/bsp/native_glasses_adapter.cpp
    src/bsp/xreal_runtime_locator.cpp
    src/common/camera_shared_memory.cpp
    src/subnodes/base_subnode.cpp
    src/subnodes/main_subnode.cpp
    src/subnodes/bsp_main_subnode.cpp

    include/recordlab/backend/agent_manager_process.h
    include/recordlab/backend/agent_watchdog.h
    include/recordlab/flowagent/core/agent_manager.h
    include/recordlab/flowagent/agents/base_agent.h
    include/recordlab/flowagent/agents/localhost_agent.h
    include/recordlab/subnodes/base_subnode.h
    include/recordlab/subnodes/main_subnode.h
    include/recordlab/subnodes/bsp_main_subnode.h
)

target_include_directories(recordlabc_agent_probe
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(recordlabc_agent_probe
    PRIVATE
        RECORDLABC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(recordlabc_agent_probe
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        echo_message_system
)

set_target_properties(recordlabc_agent_probe PROPERTIES
    AUTOMOC ON
)

# ============================================================================
# 脚本兼容工具: recordlabc_agent_cmd
# 让 Python 兼容运行时通过 C++ action 协议附着到现有 agent
# ============================================================================
add_executable(recordlabc_agent_cmd
    src/tools/agent_cmd_main.cpp
    src/flowagent/agents/legacy_remote_action_client.cpp
)

target_include_directories(recordlabc_agent_cmd
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(recordlabc_agent_cmd
    PRIVATE
        Qt6::Core
        Qt6::Network
        echo_message_system
)

target_compile_definitions(recordlabc_agent_cmd
    PRIVATE
        RECORDLABC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
