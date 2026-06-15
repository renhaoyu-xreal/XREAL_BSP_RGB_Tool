/*
 * Qt / nlohmann::json 兼容层
 *
 * 这个头文件专门解决一类很隐蔽、但在 Qt 工程里非常常见的问题：
 *
 * 1. 某个 QObject 的 signal/slot 直接暴露 `nlohmann::json`
 * 2. Qt moc 在生成元对象代码时，会尝试为该类型构造 QMetaType
 * 3. `nlohmann::json` 的模板与 Qt 的类型探测会在 `qfloat16` 这类类型上发生
 *    深层模板实例化，最终报出“incomplete type / qfloat16 / QMetaType”之类的错误
 *
 * 因此：
 * - 这里显式包含 qfloat16 相关定义，避免 Qt 类型探测时拿到不完整类型
 * - 同时声明 `nlohmann::json` 为 Qt metatype，供跨线程 queued signal 使用
 *
 * 这层不改变业务语义，只是把 Qt 与 json 的边界处理稳定下来。
 */
#pragma once

#include <QtCore/QMetaType>
#include <QtCore/qfloat16.h>

#include <nlohmann/json.hpp>

Q_DECLARE_METATYPE(nlohmann::json)

