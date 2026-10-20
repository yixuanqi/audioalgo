---
title: 算法与编辑器分离重构 Agent TDD 与 Loop Engineering 规程
status: ready-for-implementation
updated: 2026-10-18
---

# 重构 Agent TDD 与 Loop Engineering 规程

## 1. 执行任务与授权

2026-10-18用户进一步明确了[一次授权持续完成全部重构](agent_full_refactor_authorization.md)：当前R2任务结束后自行继续R3～R6，直到全部累计验收完成；本规程中的“下一切片/下一阶段”均包含该持续范围。缺少外部证据不能标通过，也不停止可独立推进的工作。

按[详细重构方案](plan_architecture_refactor.md)和[重构测试准则](testing_architecture_refactor.md)直接在当前仓库实施 R0～R6。用户已授权按规划进行代码修改、自验证和自迭代；常规接口选择、模块提取、测试补齐、局部重构和阶段推进由 Agent 自行记录并继续，中途不逐步请求确认。本文是后续代码任务规程；编写本文不表示产品已经改造或验收通过。

遵守实际工具的文件、网络、构建和桌面权限；用户授权不能绕过工具审批。不得自动提交/推送/发布/购买许可，不覆盖用户音频、用户工程或无关改动，不修改其他仓库。未获得实际工具权限时按工具流程处理；审批拒绝/资源缺失准确记录，继续独立工作。不要为了避免如实报告而绕过限制。

当前安排以重构 R0～R6 为实施顺序，原平台 G0～G6 和 NED-01～14 全部保留为回归与最终验收。受迁移影响的交互回归优先修复；历史 NI0000～NI0011 的记录及未完成桌面要求不得删除，也不能要求先伪造全部桌面通过才开始可独立进行的算法重构。

Loop Engineering 在本仓库指有限行为切片的 Define→Red→Implement→Green→Refactor→累积回归→独立验收→证据→下一切片，不是启动无人监管无限循环、后台自动化或自动提交。无需默认派生子Agent；若以后获得明确委派要求，仍以单一验收清单汇总，不重复修改共享文件。

## 2. 启动与恢复顺序

1. 确认仓库根、实际 AGENTS 和用户变更。先检测 Git 是否存在；当前历史记录为无Git，恢复时重新核实，无Git就使用文件清单/hash，不自动初始化或提交。
2. 按AGENTS先读三份主文档，再依次读重构方案、重构测试准则、本规程和最新 `doc/development_status.md`。改画布、文字、拖放、连线、插入或拓扑历史时再依次读三份节点交互专项文档。
3. 检查源码、测试、presets、依赖锁与所有计划接口/脚本是否真实存在；现有 `audio_run_service` 不等于完整 `audio_runtime`，现有工厂声明不等于生产路径接入。
4. 检测实际 CMake/MSVC/Windows SDK/Python3、JUCE缓存、桌面/设备与数据。Python采用明确可执行文件并验证版本，不依赖默认 `python`。不把本机历史路径当所有环境固定约束。
5. 保存本次源码/工具/依赖manifest及测试inventory，在独立输出中构建并执行受影响基线。Debug/headless历史88/87仅为参考，必须以当前实际结果替换。区分预先失败与本次新增失败。
6. 创建/读取RF切片目录、AR manifest和验证器进度，先以临时fixture证明它拒绝零测试、缺项、失败、skip、timeout及证据缺失。R0允许逐步补基础设施，随后立即做AR-03.a真实行为，不能长期只搭验证框架。
7. 从第一条未完成行为恢复：已有实现先运行验证；已通过但被新修改影响则回归。未受影响证据只有内容hash、配置、验收规则和适用范围相符且理由明确时才复用。

## 3. 第一条 Red 与切片顺序

### 3.1 首个产品行为

首条产品Red固定为 AR-03.a：运行工程A且延迟其完成；打开不同project_id但相同revision的工程B；再投递A成功。断言B的当前结果、波形数据引用、播放文件、状态和有效请求身份均不改变。随后AR-03.b覆盖错误/进度/取消晚到。

当前结果校验位于 `apps/editor/Main.cpp`，不应另写一份未被生产调用的“正确状态机”当修复。可以为可测性先把现有结果接收判断提取成无控件组件并保持旧行为，再用旧行为触发断言；修复后生产 RunJob/回填都必须使用该入口。控制完成顺序使用barrier/latch或fake service，独立验证用真实后台原生任务。

API缺失首次可记 compile_red，但接口出现后必须看到目标语义的 behavior_red。环境配置失败不算Red。若需求已存在且测试通过，核实事实并记录；不要故意破坏产品制造Red。对架构调用链可在临时副本中受控mutation验证断言灵敏度，恢复并检查无残留。

### 3.2 后续切片

先R0身份与基线，再R1算法单一实现和通用wrapper；随后R2控制层、R3参数/界面、R4第二算法/导出库、R5通用计划/状态/快照，最后R6 Python/真实算法及交付。阶段内按方案任务卡选择一个最小可观察行为，不一次同时重写模型、SDK、GUI和序列化。

R0/R1持续保留可运行的默认File→Gain→Output和CLI；每阶段完成执行累计相关要求。缺桌面/设备/真实数据不使对应阶段passed，但不阻止没有该依赖的后续切片。报告使用“实现完成、自动化通过、桌面待验证”等明确事实，不用统一“完成”掩盖缺口。

## 4. 每次循环的操作

### Define：固定可观察契约

选择一个AR字母子用例，列出关联G/NED要求；确认实际输入、初始状态、请求身份、独立期望、失败条件、模块边界和最小回归范围。写切片卡并检查现有测试是否可直接扩展，避免重复镜像实现。

算法期望来自解析公式/锁定原库直调；状态期望来自明确事件序列；架构边界由真实构建/消费者及调用链证明；GUI由真实控件/桌面动作及产物证明。结构目录数量、字符串搜索结果或“程序没崩”均不是充分独立验收。

### Red：真实运行并诊断

先加行为测试并执行最小目标，保存命令、退出码与完整日志，指出失败断言。判断是构建/工具、实现/契约、第三方、数据/指标、交互还是证据门禁。不能先修改产品再补只会验证当前实现的测试。

对接入链验证可在隔离副本替换算法内核，证明独立/节点/CLI oracle都能捕获变化；不得修改用户算法或用户工程，不把故障探针留在正常产品/测试集合中。

### Implement：最小实现

只修改所选行为需要的模块，保持旧格式/命令/示例兼容。新算法通过插件描述和工厂接入，不能在画布/编译器/调度器加type_id特判。纯算法不引入SDK/模型/JUCE依赖；UI不获取可变模型或算法对象。

常规技术调整写ADR：问题、选择、约束、影响及对应测试。跨文件迁移允许短暂兼容桥接，但标记删除切片；R4基本解耦验收时不得保留两套可选择的Gain生产实现。

### Green：运行同一失败测试

确认同一断言在真实产品路径变绿，检查执行数量和skip。算法读取本次WAV/manifest逐样本对独立oracle；controller检查完整状态和revision；GUI操作具体控件并检查对应节点参数及真实产物，不只看截图按钮颜色。

### Refactor：保持已有行为

移除重复运算、旧注册/参数绑定、泄漏的后端类型和临时桥接；再运行受影响测试。不要趁机扩展到尚未定义的算法能力；不为颜色/间距装饰写镜像单元测试，文字/DPI/命中按NED实际验证。

### Regression：有限累计回归

每切片跑被改变模块及下游消费者，阶段结束跑该阶段累计集合、原平台受影响集合和交互回归；R6最终执行当前源码完整Release累计必需集。测试已通过且无新变化/失败/未解疑虑时，不反复跑同一套，继续下一行为。

构建editor-off不代表纯算法隔离；纯算法消费者不含JUCE才证明该边界。纯controller测试不能初始化JUCE以掩盖依赖。GUI smoke只证明启动/退出，不能代替真实拖放、DPI、试听和保存流程。

### Independent acceptance：第二观察面

| 变更 | 自动化第一观察面 | 独立观察面 |
| --- | --- | --- |
| 核心Gain/ChannelSwap | 数学oracle与输入只读检查 | CLI/GUI真实产物再对oracle |
| 算法库隔离 | target/header依赖检查 | 独立安装消费者、无JUCE构建 |
| 节点扩展 | 注册/生命周期/格式测试 | 新插件加入时通用源码hash不变、真实GUI运行 |
| 参数面板 | descriptor→控件/命令测试 | 实际鼠标/键盘调参后输出与报告变化 |
| 结果权限/取消 | 受控事件次序 | 真实worker、New/Open、关闭及产物状态 |
| 兼容保存 | 只读Legacy fixture往返 | 重启应用重开后重跑 |
| 固定帧/延迟 | consumed/produced与FIFO边界 | 真实JUCE图脉冲/随机分块对oracle |
| Python/真实算法 | 独立进程/原库直调 | 混合链、固定质量suite、真实数据与听评 |

独立观察可由同一Agent执行，必须实际观察与读取产物；不是要求默认新建子Agent或聊天任务。

### Evidence：记录后立即继续

保存源码/验收/binary/产物hash、逐用例原始结果、命令与环境。更新阶段报告、状态文档和下一条Red，然后继续，不以“是否继续”结束已授权开发。

## 5. Loop记录、命名与失败调整

先检查已有RF编号再分配；建议阶段使用RF0000起，子切片可用RF0000-01等，不能覆盖既有目录。每份loop.md至少包含：

```text
切片：RFxxxx-yy；阶段：Rx；状态：in_progress/verified/blocked
需求：AR-xx.a；关联平台/NED：...
行为：输入与状态 → 用户可观察结果
独立oracle与固定阈值：...
首条Red：命令参数数组、cwd、开始/结束、退出码、关键失败断言
失败分类与定位：观察 → 假设 → 区分检查 → 最小修复
产品路径：实际修改文件、生产入口、依赖方向
Green/回归：命令、通过/失败/跳过/超时数、日志与hash
第二观察面：真实文件/消费者/GUI/进程/质量证据
限制：未验证条件、实际权限/资源缺口
下一条Red与实际可运行命令：...
```

同一根因连续三次迭代无实质进展，停止重复尝试：保留最小重现，缩小节点/输入/线程，核对锁定JUCE/Speex/Python源码接口和设计假设，写ADR后换诊断策略继续。三次规则不是自动结束整个任务的理由，也不是允许扩大阈值。

真正缺必要资源且无可行独立工作才交接阻塞；列出缺项、影响范围、已尝试路径与恢复命令。不要把构建权限失败报告成算法Red，也不要把不可用库视作可选跳过。原生挂死限制、无真实阵列/设备/桌面的未验证要求单独保留。

## 6. 实际命令与待实现入口

以下为仓库已有入口，运行前检查当前工具与preset是否仍适用。解释器历史可用路径为 `E:\0qyx\res\miniconda\python.exe`，先读取实际版本；可迁移到已验证Python3/venv并记录，不创建未经声明的环境依赖。

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --parallel 4
ctest --test-dir build/windows-debug -C Debug --show-only=json-v1 --no-tests=error
ctest --test-dir build/windows-debug -C Debug --output-on-failure --no-tests=error
cmake --preset windows-headless
cmake --build --preset windows-headless --parallel 4
ctest --test-dir build/windows-headless -C Debug --output-on-failure --no-tests=error
& 'E:\0qyx\res\miniconda\python.exe' scripts/verify_gate.py --gate G1 --build-dir build/windows-debug
& 'E:\0qyx\res\miniconda\python.exe' scripts/verify_editor_interaction.py --build-dir build/windows-debug --config Debug
```

现有主/专项verifier可能因真实缺项非零，应记录原因并修复对应缺项，不改返回码掩盖。当前脚本能力与本文目标不同，不能假定已实现skip/quality/全部桌面检查。

以下入口为**待开发契约**，先实现manifest/脚本、注册与执行探针，再使用，未实现前记not_run：

```powershell
& 'E:\0qyx\res\miniconda\python.exe' scripts/verify_architecture_refactor.py --stage R0 --build-dir build/windows-debug --config Debug
& 'E:\0qyx\res\miniconda\python.exe' scripts/verify_architecture_refactor.py --stage R4 --build-dir build/windows-debug --config Debug
cmake --preset windows-algorithms
cmake --build --preset windows-algorithms
ctest --preset windows-algorithms --output-on-failure --no-tests=error
```

`windows-algorithms`为建议新增preset，必须先支持纯算法配置/安装消费者再宣称通过；可以通过ADR采用等价清晰名称，不改变独立消费准则。实际验证命令按产物记录，不把示例命令当执行日志。

## 7. 持续状态和结束条件

`doc/development_status.md` 至少维护：当前Rx与RF切片、真实源码manifest、各AR子用例状态、已完成的代码/实际命令、预先失败与新增失败、G0～G6/NED未完成项、binary是否对应当前源码、外部限制、下一条Red。旧历史记录可归档并链接，不重复堆叠相互矛盾的“最新通过”。

阶段验收通过即记录并继续下一阶段，无需询问；外部证据受限时保留阶段未通过并推进独立工作。R0～R4仅为基本解耦，不能停在目录拆分/接口声明/一个测试示例就报告完整任务结束。R6还需原G0～G6、NED和全部AR累计验收；首版性能/设备/真实数据/打包缺任何必需项均如实未完成。

最终交付应含当前可运行应用/CLI/独立算法库与插件示例、构建/测试命令、真实通过数量、源与binary/产物关联、桌面/设备/质量/打包证据、全部限制和可恢复下一步。使用“已实现”“已验证”“未验证”“未实现”精确分开。

## 8. 可直接交给开发 Agent 的启动提示词

> 在当前 audioalgo 仓库直接实施算法库与编辑器分离重构。先按 AGENTS.md 读取三份主文档，再读 doc/plan_architecture_refactor.md、doc/testing_architecture_refactor.md、doc/agent_loop_architecture_refactor.md 和最新 doc/development_status.md；涉及画布、文字、拖放、连线、插入或历史时继续读取三份节点交互专项文档。检查实际源码、工具、依赖、测试inventory及证据，重建建立真实基线，不把计划接口、旧测试数量或旧二进制当现成功能。按R0～R6持续实施：先以AR-03.a“工程A运行未结束，打开相同revision的工程B，再完成A，B状态不得被覆盖”为首条行为Red；再让生产Gain通过节点Adapter和通用JUCE wrapper复用唯一algorithms实现，完成独立算法库、描述/工厂成对注册、无控件应用控制层、统一参数/拓扑命令、描述驱动界面，并用ChannelSwap证明新增算法不改通用View/编译器/调度器，随后推进命名多端口、固定帧/状态/延迟/尾部、快照/取消、独立Python runner、真实算法及完整交付。每次只推进一个AR字母子用例，执行Define→真实Red→最小实现→同目标Green→Refactor→累计回归→独立验收→证据记录，持续维护doc/development_status.md及build/reports/loops/RFxxxx/。全部AR子用例、原G0～G6和NED必需测试及当前源码的桌面/设备/真实数据/Release证据均按文档验收；不放宽阈值、不删断言、不跳过必需项、不伪造golden或截图。三次同根因无进展就最小化重现、核对上游接口和设计假设、写ADR后继续。已授权按规划直接修改代码和自验证自迭代，常规阶段无需向我确认；遵守实际工具权限，不自动提交/推送/发布/购买，不覆盖用户音频和无关改动。外部条件不足如实保留未验证并继续独立工作，不能把基本解耦或纯测试全绿当整个平台完成；最后交付真实产物、命令、证据和剩余缺项。
