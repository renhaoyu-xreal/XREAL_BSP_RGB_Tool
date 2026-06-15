#pragma once

#include <QString>

/*
 * Qt 日志桥
 *
 * 先提供一个统一的 Qt message handler，后面无论是接第三方库、
 * 引入 IPC、还是对接设备层日志，都可以在此基础上再扩展。
 */
namespace recordlab::core {

void installQtLogger();
void installTerminalLogTee(const QString &appRoot);

}  // namespace recordlab::core
