# 0033 原生/Python物化边界的时序归属

后续补充：内部命名固定块的逐输出2/5帧尾长已由[ADR0034](0034-independent-native-output-tails.md)补齐，下方单输出限制保留原切片背景。混合worker入口现可借用显式 `host_partitions`，在每个原生区域从分区序列起点独立重放；所有分区在代码冻结/工厂前校验为1..maximum block。区域任务记录完整序列。Python自有任务保持其声明的最大块合同，不能把原生回调分区误用为Python设备回调。真实固定480 FIR→Python→固定512验收保存回调块长trace，两次独立进程重放同字节WAV；不表示所有异步/借用组合通过。

2026-10-23，混合时序切片。现有NativePlan、通用JUCE wrapper已验证固定帧FIFO、已知延迟和有限drain；混合层复用这些能力，不添加Speex或其他算法type_id分支。

每个原生区域创建临时命名输入/收集端投影，不修改ProjectModel。先按实际入口PCM长度/有效区间编译NativePlan，预检raw+aligned聚合缓冲预算，再采集完整raw区间。原生区域延迟包括固定帧适配和算法已知延迟，由该边界裁剪一次。下游收到逻辑对齐的自有PCM；run.json保留区域raw/aligned长度、offset、输入帧数、有限尾长及每输出文件/hash/有效区间。最终工程Output不重复裁剪这些区域延迟。

有限尾部从区域入口的实际长度延伸，只有原有效区间非空且延伸到输入末端时才扩展有效区间。Python按该自有区间执行并保持shape，随后原生区域使用新的入口长度，而非回读整次运行原始帧数。跨区域多输入长度/有效区间不一致明确拒绝；当前一个带尾部原生区域只允许一个对外输出，独立多输出尾长/有效区间仍需独立合同，不把较短端口补零伪称有效。未知延迟/尾部、无公开flush的能力仍由计划预检拒绝。

实际验收分别为固定480→Python→固定512（六host，独立raw/aligned/WAV/NPY公式）、真实Speex→Python→Output（锁定原库直调与六host组件/CLI）和三抽头FIR两帧尾部→Python→原生输出。随机混合分块、逐输出不同区间及剩余CORE-02借用组合并未由这些子集自动通过。

前置缺陷：JUCE 8.0.12的prepareToPlay在worker调用时异步安排消息线程图重建，立即读getLatencySamples存在竞态。确定性factory握手/暂缓消息泵测试真实Red；在消息线程完成rebuild后worker再读取诊断/延迟。图准备仍遵守JUCE消息环境约束，Python导入/进程/执行/磁盘载荷工作仍在worker，未进入设备回调。该等待依赖宿主继续驱动其消息循环，不宣称无响应原生宿主可被强杀线程恢复。
