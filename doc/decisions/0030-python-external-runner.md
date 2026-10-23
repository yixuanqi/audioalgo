# 0030 — 独立 Python runner 的第一条执行路径

2026-10-23。当前是有真实进程消费者的后端切片，尚未接入 C++ 宿主/GUI/CLI，不是 AR-18/P3 完成声明。

manifest 仅解析数据，不导入用户模块；共享目录中带 runtime=python 的包使用 schema1，block/clip、端口、六种基础参数和格式元数据经过有界校验，包注册原子且不覆盖已有 ID。端口 ID 沿用现有 Node SDK 的节点内唯一契约，示例采用 audio_in/audio_out。

Python SDK 的 AudioPort 为 float32 [channels,samples]、明确采样率/稳定通道 ID/有效区间。每节点对象在整次任务 prepare 一次，block 循环保留实例状态；clip 只调用一次；最终 reset，失败/取消清理私有载荷。NPY header/实际文件大小/aggregate budget 在样本分配前检查，禁止 pickle、float64和隐式格式转换。输入先复制并核对SHA，再从固定副本执行；所有输出验证后才发布私有结果目录和清单。

启动独立 python -m audioalgo_runner，控制使用127.0.0.1 TCP、4字节大端长度、UTF-8 JSON、1MiB上限；令牌握手后不记录令牌，用户代码导入前移出环境。request/run/revision及project/epoch/node身份原样回传。控制线程与算法线程分离，块间及可合作节点支持取消。stdout/stderr为独立管道，测试父进程实际有界排空洪泛；生产 C++ 父进程采集器与 Job Object 下一片实现，不能把测试父进程当作生产完成。

执行前冻结 manifest 和所有本地 .py 文件（8MiB/128文件预算），模块身份包含 plugin_id+code_hash。本地依赖使用相对导入。代码改变诊断 restart_required，重启后新PID/新行为，旧结果不变。仅关闭 pyc 写入不足以防旧字节码被读取；真实Red已证明同时间戳/长度的旧pyc令输出变9倍。现在自定义source loader仅编译清单内、逐文件SHA匹配的源码，拒绝未声明本地模块，不读取pyc。

已验证环境 CPython3.13.9/NumPy2.4.3；requirements-runner.txt锁定本次NumPy。尚无打包/禁网干净环境证据。进程树强制回收、无响应/崩溃预算、C++宿主发现、混合图和完整Python错误矩阵未完成，AR-18映射保留 remaining_contracts。
