# 原生计划编译与执行投影

最新[ADR0035](decisions/0035-per-port-stream-eos.md)允许内部命名固定帧流式节点声明独立输出尾长/EOS。图会额外驱动闭包内较长输出到有界终点，单独记录 `drain_padding_frames`；所选短输出raw/aligned长度和有效区间不增加。进展预算为0、未知时序/flush仍拒绝。下方ADR0034最初的流式限制已由此扩展，发布C ABI v1边界保持原状。

2026-10-23时序补充：内部命名固定块节点可用完整 `capabilities.output_tail_frames` 映射声明各输出尾长，标量 `tail_frames` 必须为最大值。计划保存 `output_tails`，逐边决定输入EOS和所选输出长度，适配器按各输出结束帧写零；混合边界统一补偿延迟后分别裁剪raw/aligned和有效区间。见[ADR0034](decisions/0034-independent-native-output-tails.md)。流式EOS与发布C ABI v1不支持此映射；不同输入长度/区间的任务仍明确拒绝。本文件下方旧G1边界描述仅为原切片背景，当前范围以唯一状态摘要为准。

`audio_runtime/NativePlan` 是纯C++元数据编译模块，只依赖core/SDK/project。它通过NodeCatalog只读接口验证工程、格式、端口类型及块约束，不访问工厂、音频文件、GUI或算法类型分支。无工厂的元数据目录也能编译合法计划。不支持的typed端口/rate/fixed-frame声明在构造任何处理器前定位拒绝。

计划保存按拓扑排列的节点描述/版本/实际默认参数、source_boundary/native_region/sink_boundary任务、以(node_id,port_id)命名的PCM连接以及通道基址/数量。结果是值快照，调用方后续修改参数不改变计划。生产JuceGraph先编译计划，再按region节点创建处理器、按计划路由连接，JUCE只是执行投影。

当前边界沿用G1：一个已物化WAV源、一个所选sink、具备supports_named_audio_ports声明的block节点可有多个命名PCM输入/输出组、统一已支持格式；固定帧/尾帧不匹配仍明确拒绝。三个任务边界并不表示已经实现通用source/sink任务注册、多个独立源或FIFO。对应AR14.b/c/d、AR15及其完整数据/时序要求继续实施。计划完整落盘、依赖版本/取消/队列亦按AR16/17后续切片推进。

纯计划测试放在audio_application_tests，关闭AUDIO_BUILD_RUNTIME/EDITOR时实际构建和运行，未初始化JUCE。真实后端、参数控件、CLI、资源快照及边界回归验证实际调用链；不以源文件字符串检查替代这些行为。

T24新增借用NamedAudioBlockView：每个端口组保留稳定id与通道span，生命周期限于一次process_named_block。旧平面接口兼容单输入/单输出；新增组必须显式声明能力并实现命名回调。每个端口组投影为独立JUCE bus，计划携带组索引×通道数作为路由基址。prepare预分配指针、视图和输入暂存，回调先暂存全部输入再暴露输出，具备零分配回归。一次节点回调同时处理所有输出，不按出边重复执行。

真实验证包含单源分流至main/side再按primary/reference角色合流，24个帧数/host/角色组合、连续状态、readonly输入与实际RunService落盘WAV。独立RIFF oracle精确匹配两种非零解析输出。测试插件test.named_split/test.named_mix仅在测试registry注册，所导出演示不能用默认CLI直接重放。两个独立文件源及生产Mixer仍是下一行为；AR14.b/c整体未完成。
