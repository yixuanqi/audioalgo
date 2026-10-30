# ADR 0044：发布包共用 Python 定位与独立 runner 快照

- 状态：accepted，2026-10-30；用户指定发布包 Check 失败修复。
- 证据入口：`build/reports/loops/PKG0001-python-runtime/`。

工作台过去直接使用编译机解释器和源码目录；普通节点运行另有发布目录定位，且把完整 Python 安装当作 runner 快照根。真实迁移测试确认工作台使用开发 runner、旧布局运行触发 128 文件预算。这属于部署路径缺陷，不变更节点源码预算或算法契约。

统一使用 `audio_python_backend::resolve_python_runtime`：显式解释器/runner 参数优先，解释器随后采用 `AUDIOALGO_PYTHON_EXECUTABLE`，再采用程序旁 `python/python.exe`；runner 优先程序旁 `runner/audioalgo_runner`，兼容旧 `python/audioalgo_runner`。仅没有安装布局时使用编译期开发默认值；不完整安装不回退开发目录。缺文件诊断明确路径及解释器/runner层次，不附伪源码位置。

新发布布局分开 `python/`（完整解释器和 NumPy）与 `runner/`（runner导入根）。`freeze_python_runner` 只枚举导入根中的 `audioalgo_runner/**`，保留该相对前缀、原字节/hash、双次清单与内容复核、128文件/8MiB预算和链接拒绝。已冻结目录仍用原验证器；Check/Apply环境锁、普通运行共享同一捕获入口。用户节点包及原生ABI不变。

发布流程在打包前从非源码工作目录实际执行目录节点 Check→Apply→运行的测试程序；验证 runner 与解释器身份和 513 样本独立公式。测试程序不进入最终包。此项不替代干净OS安装、真实桌面或完整Release累计验收。
