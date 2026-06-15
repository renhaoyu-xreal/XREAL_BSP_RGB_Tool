/*
 * nviz_node_subnode 独立可执行文件入口
 *
 * Agent 通过 QProcess 启动此进程：
 *   ./nviz_node_subnode --name glasses_nviz_node --host 127.0.0.1 \
 *       --goal-port 5692 --feedback-port 5693 --root-path ./output
 */
#include "recordlab/subnodes/nviz_node_subnode.h"

int main(int argc, char *argv[]) {
  // 将独立可执行入口直接转发给统一的子节点主函数。
  return recordlab::subnodes::nvizNodeSubnodeMain(argc, argv);
}
