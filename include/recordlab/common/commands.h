/*
 * 统一的命令常量定义
 *
 * 定义系统中所有的命令类型，确保一致性
 *
 * 所有类名、字符串值与 Python 版完全一致。
 */
#pragma once

#include <string>
#include <unordered_set>

namespace recordlab::common {

// ============================================================================
// AgentManager 进程间通信命令
// ============================================================================

/*
 * AgentManagerProcess 的 IPC 命令
 *
 */
struct ManagerAction {
  // Agent 管理
  static constexpr const char *INIT_AGENT = "init_agent";
  static constexpr const char *RELEASE_AGENT = "release_agent";
  static constexpr const char *STOP_ALL_AGENTS = "stop_all_agents";
  static constexpr const char *STOP_AGENTS = "stop_agents";
  static constexpr const char *GET_STATUS = "get_status";
  static constexpr const char *GET_AVAILABLE_AGENTS = "get_available_agents";
  static constexpr const char *GET_PRIMARY_AGENTS = "get_primary_agents";
  static constexpr const char *SHUTDOWN = "shutdown";

  // Agent 命令执行
  static constexpr const char *SEND_AGENT_COMMAND = "send_agent_command";
  static constexpr const char *SET_WATCHDOG_INIT_PARAMS =
      "set_watchdog_init_params";

  // 脚本执行
  static constexpr const char *EXECUTE_SCRIPT = "execute_script";
  static constexpr const char *GET_SCRIPT_STATUS = "get_script_status";

  // UI 弹窗交互
  static constexpr const char *SHOW_DIALOG = "show_dialog";

  // 紧急停止
  static constexpr const char *EMERGENCY_STOP = "emergency_stop";
};

// ============================================================================
// 弹窗类型定义
// ============================================================================

/*
 * 弹窗类型常量
 *
 */
struct DialogType {
  static constexpr const char *INFO = "info";
  static constexpr const char *WARNING = "warning";
  static constexpr const char *ERROR = "error";
  static constexpr const char *QUESTION = "question";
  static constexpr const char *INPUT = "input";
  static constexpr const char *CHOICE = "choice";
  static constexpr const char *MULTI_FIELD_INPUT = "multi_field_input";
};

// ============================================================================
// 辅助函数
// ============================================================================

/*
 * 检查是否为有效的 Manager Action
 */
inline bool isValidManagerAction(const std::string &action) {
  static const std::unordered_set<std::string> validActions = {
      ManagerAction::INIT_AGENT,     ManagerAction::RELEASE_AGENT,
      ManagerAction::GET_STATUS,     ManagerAction::SHUTDOWN,
      ManagerAction::EXECUTE_SCRIPT, ManagerAction::GET_SCRIPT_STATUS,
      ManagerAction::SHOW_DIALOG,      ManagerAction::SET_WATCHDOG_INIT_PARAMS,
  };
  return validActions.count(action) > 0;
}

} // namespace recordlab::common
