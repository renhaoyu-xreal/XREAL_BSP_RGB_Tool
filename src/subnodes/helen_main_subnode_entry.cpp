/*
 * helen_main_subnode 独立可执行文件入口
 *
 * Helen 是无 Linux 系统眼镜的独立节点名。当前底层复用 BSP/XREAL 采集实现，
 * 但默认打开 SSH optional 模式，避免 helen_node 再走旧 BSP 的 SSH 前置链路。
 */
#include "recordlab/subnodes/bsp_main_subnode.h"

#include <QByteArray>
#include <QString>
#include <vector>

int main(int argc, char *argv[]) {
  std::vector<QByteArray> args;
  args.reserve(static_cast<std::size_t>(argc) + 1);

  bool hasSshOptional = false;
  for (int i = 0; i < argc; ++i) {
    const QByteArray arg(argv[i]);
    if (arg == "--ssh-optional") {
      hasSshOptional = true;
    }
    args.push_back(arg);
  }
  if (!hasSshOptional) {
    args.push_back("--ssh-optional");
  }

  std::vector<char *> argvWithHelenDefaults;
  argvWithHelenDefaults.reserve(args.size());
  for (auto &arg : args) {
    argvWithHelenDefaults.push_back(arg.data());
  }

  return recordlab::subnodes::bspMainSubnodeMain(
      static_cast<int>(argvWithHelenDefaults.size()),
      argvWithHelenDefaults.data());
}
