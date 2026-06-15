"""
RecordLabC 本地脚本包。

添加这个文件的目的很直接：
1. 让 `scripts.common.*`、`scripts.runtime.*` 这类导入在不同 Python 环境下都更稳定；
2. 避免把整个工程复制到新电脑后，依赖“隐式 namespace package”行为才能运行；
3. 继续保持旧版脚本目录命名习惯，方便对照迁移。
"""

