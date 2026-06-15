cmake_minimum_required(VERSION 3.10)
project(echo_message_system CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置库类型选项
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_STATIC_LIBS "Build static libraries" ON)

# 版本信息
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

# 查找依赖
find_package(PkgConfig REQUIRED)

# 优先使用本地的ZeroMQ库
set(LOCAL_ZEROMQ_DIR "${CMAKE_SOURCE_DIR}/external/zeromq")
if(EXISTS "${LOCAL_ZEROMQ_DIR}")
    message(STATUS "使用本地ZeroMQ库: ${LOCAL_ZEROMQ_DIR}")
    set(ZMQ_INCLUDE_DIRS "${LOCAL_ZEROMQ_DIR}/include")
    set(ZMQ_LIBRARY_DIRS "${LOCAL_ZEROMQ_DIR}/lib")
    set(ZMQ_LIBRARIES "${LOCAL_ZEROMQ_DIR}/lib/libzmq.a")
    link_directories(${ZMQ_LIBRARY_DIRS})
else()
    message(STATUS "使用系统ZeroMQ库")
    pkg_check_modules(ZMQ REQUIRED libzmq)
endif()

# nlohmann/json (header-only)
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
)

# 兼容旧版本CMake的获取方式
FetchContent_GetProperties(json)
if(NOT json_POPULATED)
    FetchContent_Populate(json)
    add_subdirectory(${json_SOURCE_DIR} ${json_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${ZMQ_INCLUDE_DIRS}
)

# 源文件
set(SOURCES
    src/context.cpp
    src/publisher.cpp
    src/subscriber.cpp
    src/service.cpp
    src/action.cpp
    src/master.cpp
    src/parameter_server.cpp
)

# 共享库链接选项
set(LIB_LINKS
    ${ZMQ_LIBRARIES}
    nlohmann_json::nlohmann_json
    pthread
)

# 创建静态库
if(BUILD_STATIC_LIBS)
    add_library(echo_message_system STATIC ${SOURCES})
    target_link_libraries(echo_message_system
        ${LIB_LINKS}
    )
    set_target_properties(echo_message_system PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
else()
    add_library(echo_message_system SHARED ${SOURCES})
    target_link_libraries(echo_message_system
        ${LIB_LINKS}
    )
    set_target_properties(echo_message_system PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

# 创建动态库 (如果同时编译静态库和动态库)
if(BUILD_SHARED_LIBS AND BUILD_STATIC_LIBS)
    add_library(echo_message_system_shared SHARED ${SOURCES})
    target_link_libraries(echo_message_system_shared
        ${LIB_LINKS}
    )
    set_target_properties(echo_message_system_shared PROPERTIES
        OUTPUT_NAME echo_message_system
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
elseif(BUILD_SHARED_LIBS)
    # 如果只编译动态库，使用标准名称
    if(NOT TARGET echo_message_system)
        add_library(echo_message_system SHARED ${SOURCES})
        target_link_libraries(echo_message_system
            ${LIB_LINKS}
        )
        set_target_properties(echo_message_system PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR}
        )
    endif()
endif()

# Master 可执行文件
add_executable(echo_master tests/master_main.cpp)
target_link_libraries(echo_master echo_message_system)

# 测试程序
add_executable(test_pubsub tests/test_pubsub.cpp)
target_link_libraries(test_pubsub echo_message_system)

add_executable(test_service tests/test_service.cpp)
target_link_libraries(test_service echo_message_system)

add_executable(test_action tests/test_action.cpp)
target_link_libraries(test_action echo_message_system)

add_executable(test_all tests/test_all.cpp)
target_link_libraries(test_all echo_message_system)

# 参数服务器可执行文件
add_executable(param_server tests/parameter_server_main.cpp)
target_link_libraries(param_server echo_message_system)

# 参数服务器测试
add_executable(test_parameter tests/test_parameter.cpp)
target_link_libraries(test_parameter echo_message_system)

# 参数服务器使用示例
add_executable(parameter_examples tests/parameter_examples.cpp)
target_link_libraries(parameter_examples echo_message_system)

# ===================== 安装配置 =====================

# 安装库文件
if(BUILD_STATIC_LIBS)
    install(TARGETS echo_message_system
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
            RUNTIME DESTINATION bin)
endif()

if(BUILD_SHARED_LIBS AND BUILD_STATIC_LIBS)
    install(TARGETS echo_message_system_shared
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
            RUNTIME DESTINATION bin)
elseif(BUILD_SHARED_LIBS)
    install(TARGETS echo_message_system
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
            RUNTIME DESTINATION bin)
endif()

# 安装头文件
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
        DESTINATION include/echo_message_system)

# ===================== 编译信息 =====================

message(STATUS "")
message(STATUS "============== Echo Message System ==============")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "")
message(STATUS "Build Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Shared Libs: ${BUILD_SHARED_LIBS}")
message(STATUS "  Build Static Libs: ${BUILD_STATIC_LIBS}")
message(STATUS "")
message(STATUS "Compiler:")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  Compiler Flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  ZMQ Found: YES")
message(STATUS "  ZMQ Version: ${ZMQ_VERSION}")
message(STATUS "  ZMQ Libraries: ${ZMQ_LIBRARIES}")
message(STATUS "")
message(STATUS "Build Targets:")
message(STATUS "  Libraries: echo_message_system")
message(STATUS "  Executables:")
message(STATUS "    - echo_master (ROS Master)")
message(STATUS "    - param_server (Parameter Server)")
message(STATUS "    - test_pubsub (Pub/Sub Test)")
message(STATUS "    - test_service (Service Test)")
message(STATUS "    - test_action (Action Test)")
message(STATUS "    - test_all (Complete Test)")
message(STATUS "    - test_parameter (Parameter Server Test)")
message(STATUS "    - parameter_examples (Parameter Examples)")
message(STATUS "")
message(STATUS "Usage:")
message(STATUS "  Build both static and shared:")
message(STATUS "    cmake -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=ON ..")
message(STATUS "  Build only static:")
message(STATUS "    cmake -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON ..")
message(STATUS "  Build only shared:")
message(STATUS "    cmake -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF ..")
message(STATUS "")
message(STATUS "Install:")
message(STATUS "  make install")
message(STATUS "")
message(STATUS "================================================")
message(STATUS "")