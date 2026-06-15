/*
 * bsp_main_subnode 独立可执行文件入口
 *
 * Agent 通过 QProcess 启动此进程:
 *   ./bsp_main_subnode --name BspMainSubnode --host 127.0.0.1 \
 *       --goal-port 5690 --feedback-port 5691 --root-path ./output
 */
#include "recordlab/subnodes/bsp_main_subnode.h"

int main(int argc, char *argv[]) {
  // 将独立可执行入口直接转发给统一的子节点主函数。
  return recordlab::subnodes::bspMainSubnodeMain(argc, argv);
}
