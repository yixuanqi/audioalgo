---
title: 下一阶段 Agent 开发任务与自验证闭环（R2）
status: ready-for-implementation
updated: 2026-10-18
---

# 下一阶段 Agent 开发任务与自验证闭环

## 1. 目标、事实与适用顺序

本任务接续 RF0006，近期目标是修复已复现的参数撤销缺陷，完成 **R2 的生产接入和全部验收**；用户已给出[全部重构持续开发授权](agent_full_refactor_authorization.md)，完成后必须自行继续R3～R6直至全部重构验收完成。本文只细化R2执行顺序，不替代或放宽原验收规则，也不是整个任务的结束边界。

先按 [AGENTS](../AGENTS.md) 读取主平台、架构重构及适用节点专项文档，再读 [当前状态](development_status.md)、[架构验收](testing_architecture_refactor.md)、[Agent 规程](agent_loop_architecture_refactor.md)和最近 [RF0006 Review](../build/reports/reviews/rf0006-20261018T064805736050Z/review.md)。恢复时重新核实源码和证据，历史路径不存在就从源码及最新报告恢复，不把历史报告当当前通过。

规划时已核实的事实：

- RF0005 已修复此前六项执行/隔离问题；RF0006 已有 `EditorController`、`set_parameter`、共享 schema 和 dirty/stale 状态。
- RF0006 最终记录为 Debug 153/153、headless 152/152、Release 153/153；这些数量是历史基线，新增测试后必须按实际 inventory 统计。
- `EditorController::undo()` 返回成功但不恢复参数：0→6→Undo 后仍为6；根因是拓扑恢复方法保留当前参数。R2 参数历史尚未完成。
- `GainInspector` 仍直接写模型；`audio_editor_application` 尚未成为生产编辑状态的唯一所有者；controller 测试目前混在链接 JUCE 的测试程序里。
- R1 的实际 GUI 数值证据仍缺；R2/R3、平台 G0～G6 和 NED 未完成项继续保留。

已授权直接实施、验证和迭代，无需常规阶段确认。保留全部现有未提交/未跟踪修改；不自动提交、推送、发布、购买，不覆盖用户工程或音频。独立数据评测线专属目录不纳入本任务顺带修改。本文不授权创建额外聊天、后台自动化或默认委派子 Agent。

## 2. 本阶段完成定义

R2 的新增必需集合是 **AR-08.a～g、AR-09.a～e、AR-13.a～e，共17项**，还须累积前序要求与受影响的原平台/NED回归。单个任务卡通过不等于父 AR 或整个 R2 通过。

工程状态由 controller 唯一维护；现有 Gain 界面、拓扑操作、Undo/Redo、New/Open/Save 和运行回填均经明确的应用层边界。View 只发送意图并读取只读展示状态，不取得可变工程或算法实例。不能只添加一个未被生产调用的正确 controller。

缺 GUI/设备等必要条件时，该项保持 `not_verified` 并继续不依赖它的切片。只有全部累计必需项及证据齐全时才能宣布 R2 通过；否则交付明确的已验证子集，并继续既定范围中可独立开发的任务。

## 3. 任务卡：按顺序交付，不一次重写所有层

下列 T 编号只是本计划内部任务卡，不新增或替代 AR/NED 验收 ID。RF编号检查后分配，不能覆盖 RF0006 或旧记录。

| 任务 | 首条 Red / 可观察行为 | 主要修改范围 | 完成出口 |
| --- | --- | --- | --- |
| T01 参数撤销修复 | 0→6→Undo 必须为0，再Redo为6；当前代码应在“实际值”断言失败 | controller/history、正式回归 | 修复 Review，参数差量恢复；见第4节 |
| T02 混合历史与手势 | 参数/拓扑/布局交错；滑块一次手势只提交一条历史，取消无遗留 | 应用命令/手势事务、历史 | AR-08.c/e/f适用行为通过，NED历史保持 |
| T03 真正纯应用层验证 | controller用例可在不链接JUCE实现、不初始化消息环境的可执行文件运行 | CMake、纯测试target、注入接口 | AR-09.a/e的纯构建部分有真实证据；不能仅靠include字符串检查 |
| T04 工程与命令所有权 | controller内Add/Connect/Disconnect/Insert/Delete/Move，以及非法操作原子拒绝 | application、现有拓扑helpers | AR-08.a/b/d/g；不复制另一套拓扑语义 |
| T05 运行与生命周期边界 | 旧任务/文件选择器/订阅回调晚到不得改新工程或销毁后View | run_api、dispatcher、应用状态、外层适配 | AR-03受影响回归、AR-09.c/d/e；真worker验证 |
| T06 现有界面接入 | 实际Gain调参→controller→Undo/Redo→模型→Run产物；旁路非法值仍拒绝 | GainInspector、EditorComponent及外层装配 | AR-09.b；生产单一命令入口，保留真实GUI证据缺口 |
| T07 兼容保存与资源 | Legacy和未知插件无损往返；保存失败不损旧文件；资源重新定位 | 项目序列化、注入服务、真实fixtures | AR-13.a～e全部逐项通过 |
| T08 阶段累计验收 | 最新源码、当前二进制、完整inventory、逐项JUnit与证据均可关联 | 测试映射、报告、状态 | 完整R2判定；缺项准确列明；继续R3已授权切片 |

T03允许在T01 Green后提前完成，以后所有controller行为用例优先进入纯target。T05与T06按最小入口逐条迁移，不能为追求单个文件迁移完毕长期断开可运行Gain闭环。

### T01：参数历史的精确通过准则

必须通过生产 `EditorController` 验证以下序列；检查值、节点ID、参数ID、历史栈、revision、dirty/stale，而不只是 `undo()==true`：

| 序列 | 必须观察到的结果 |
| --- | --- |
| value=0 → set(6) → Undo → Redo | 值依次0/6/0/6；三个有效计算编辑各产生一个新revision，Undo/Redo不回退revision |
| 连续设置同一有效值 | no-op；不增加revision/历史、不清空已有redo、不改变当前结果资格 |
| bool/int/float/enum/string/path | 各已实现类型按共享schema恢复准确的类型和值；未知enum、越界、NaN/Inf等非法值不产生历史 |
| 原来缺省未存该key → 显式编辑 → Undo | 恢复key原先不存在的状态，默认值解释不漂移；Redo恢复显式值 |
| 两节点同名参数 | 只恢复目标实例，其他参数和节点不变 |
| Undo后合法新命令 / 非法命令 | 合法新命令清redo；非法命令保留redo及所有状态 |
| 参数命令与插入/删除/移动交错 | 撤销只恢复目标命令的差量；不吞掉不属于它的后续状态，删除/恢复保留稳定ID |

实现可采用命令差量或等价明确语义，但**不得一律恢复整个旧parameters快照**：旧拓扑历史要求保留不属于该命令的后续参数。必须同时重跑既有 NED-11 和其他受影响历史回归。

首条Red若意外通过，先确认是否已有并发修复、是否运行最新二进制及是否确实断言值；需求已满足就记录，不故意改坏产品制造Red。

### T02/T04：手势、布局与模型命令

- 先记录参数手势的事务契约。建议编辑草稿只作界面预览，commit形成一个合法参数命令；cancel恢复编辑前状态，不启动DSP/I/O、不保留半条历史。若选择实时模型预览，须明确revision、取消和运行资格语义并写ADR后测试，不能隐藏部分提交。
- Layout编辑改变保存状态，不改变计算revision、不触发运行、不让当前结果过期；参数/拓扑有效编辑使结果过期。布局是否单独撤销及与计算命令的先后顺序必须明确定义并测试。
- controller复用现有添加/连接/断开/插入/删除校验；失败保持工程、历史、revision、redo和有效结果完全不变。
- 不把兼容转发头永久保留为View模型所有权接口。移除兼容桥前确认生产和测试调用方全部迁移。

### T03/T05/T06：真正接入，而非隔离样例

- 把纯core/SDK/project/application目标与JUCE runtime配置解耦，使一个只链接纯目标的controller测试程序真实可构建/运行。当前 `AUDIO_BUILD_RUNTIME=OFF` 尚不保证application可用，先修改实际构建再写通过结论；新preset/脚本名须注明为待实现。
- Open/Save、资源、运行、播放经无JUCE接口注入；JUCE dispatcher、控件和设备实现留在外层。纯测试不能通过引入JUCE消息环境绕过边界。
- `RunReception`的身份语义必须被生产继续使用。携带project/session/request/revision的成功、失败、进度、取消均先校验再改变任何展示状态；不能公开一个无身份的“标记结果为当前”入口供旧回调绕过。
- View只持稳定ID和不可变快照/安全只读查询；删除节点、Open/New、重新绑定面板、关闭窗口后，旧指针/订阅/chooser回调不再访问失效对象。
- 任务不在UI线程执行算法/I/O；拆界面时读取既有NED规程，保留拖放/连线/插入/删除/平移等已实现行为。
- T06先接通现有Gain界面。bool/int/enum等自动表单和Main.cpp的完整组件拆分属于R3，不能靠一轮大重写与R2混合验收。

### T07：不能漏掉R2的工程兼容范围

- 使用R0冻结的Legacy fixture，保存/重开/重跑后参数、连接、ID、布局及数学语义保持；不覆盖原fixture/用户音频。
- 新建独立未知插件fixture，包含嵌套未知字段、版本和连接，逐字段比较无损往返；缺插件明确阻止运行。
- 在原子保存的真实临时文件/发布边界注入失败，旧工程文件字节完整；不通过mock“返回成功”替代文件验证。
- 相对、外部、中文空格及缺资源重新定位由资源服务处理；chooser只提供选择结果。原输入hash保持不变。
- 建立不支持schema的诊断、实际迁移/备份fixture；模型和持久化无processor/GUI指针，计算revision与布局保存状态分开。

## 4. 每个切片必须自动执行的开发循环

这是本仓库的有界Loop Engineering工作协议，不是定时后台任务，也不要求新建其他Agent。

1. **Define**：列出需求ID、初始状态/输入、独立期望、失败条件、生产入口、最小回归集合；记录任务卡。
2. **Red**：先加正式行为测试并运行，保留失败断言、实际命令/配置/源码及二进制身份。接口不存在可先compile_red，但接口出现后必须确认行为断言。工具/权限/下载失败记environment_blocked，不能当行为Red。
3. **Implement**：只修改本条行为所需代码；不改期望来迎合当前实现，不删断言、不扩大数值阈值、不屏蔽必需测试。
4. **Green**：重建并运行**同一**目标，确认正数测试/断言、0失败/skip/timeout。捕获“未匹配任何用例”、旧exe、缺依赖和环境启动失败，不按exit0自动计通过。
5. **Refactor**：收敛重复路径和临时桥接；再跑本条及直接受影响用例。不能以增加一套只供测试使用的正确controller替代生产修改。
6. **累积回归**：变更历史跑controller与NED历史；改View跑组件/交互与真实产品链；改执行边界跑原六项回归和Gain数值oracle。阶段结束才跑完整矩阵，不在无新变化时重复完整变异构建。
7. **独立验收**：至少再走一条不同观察路径，检查真实状态或产物；第5节规定最低要求。
8. **记录并继续**：写结果及下一个Red，更新工作清单。满足本切片出口后直接进入下一个，不能每完成一条就停止询问是否继续。

同一根因连续三轮没有实质进展时：保留最小重现→缩小输入/事件序列→核对上游接口与设计假设→记录ADR→换诊断方式继续；不能扩大阈值，也不是三轮后自动放弃。

### 失败分层及下一动作

| 失败层 | 应采取的动作 | 不允许的处理 |
| --- | --- | --- |
| 编译/链接 | 检查实际依赖、配置和调用方，保存诊断 | 用旧exe补一个pass |
| 行为/数值 | 最小化失败输入，修产品，保留回归 | 改期望/阈值或只改mock |
| 状态/竞态 | 使用barrier/受控dispatcher固定顺序，验证完整状态 | 随机sleep直到偶然成功 |
| 环境/权限 | 遵守工具审批；使用已允许的工作区TEMP；准确记未执行 | 当作Red或产品通过；绕过审批 |
| 证据关联 | 冻结源码并重新生成真实关联产物 | 只改旧JSON中的hash/退出码 |
| 桌面/设备缺失 | 保留not_verified并继续纯后端切片 | 用启动smoke或fake service替代 |

## 5. 防止“自验证变成自证”的独立检查

| 改动 | 第一观察面 | 必需的第二观察面 |
| --- | --- | --- |
| 参数历史 | controller逐步断言 | 独立固定事件表/reducer检查实际值、key是否存在、revision、完整工程差异 |
| 混合命令 | 每类命令单测 | 固定seed有界事件序列；合法/非法/Undo/Redo/New交错；reference不调用生产history/helper |
| 命令接线 | 组件事件测试 | 真实控件发意图、订阅展示状态、检查实际controller快照；不能直接调用setter冒充事件 |
| schema一致 | 单元拒绝非法值 | 绕过控件从controller和runtime分别提交同一非法值并比对定位诊断 |
| 构建隔离 | 头文件/依赖守卫 | 独立纯应用测试target的实际编译、链接输入及运行；无JUCE消息环境 |
| 运行身份 | 受控晚到事件 | 至少一次真实后台RunService及消息投递，旧工程成功/失败均不得回填新工程 |
| GUI与算法 | controller/组件正确 | 本次GUI实际产物和CLI产物分别对独立float64 oracle，不能只比较两份同源输出 |
| 保存/未知字段 | 序列化断言 | 真实文件失败注入、字节保护、重开重跑、原资源hash不变 |

参数切片至少包含两节点、两种参数类型及三类失败输入，不能只覆盖Gain的浮点成功路径。事件序列/随机检查必须记录seed和最小失败序列，不增加不必要的第三方测试依赖。

## 6. 可执行入口与待实现能力

以下入口已经存在，解释器与工具路径恢复时重新检测：

```powershell
& 'E:/0qyx/res/miniconda/python.exe' --version
cmake --build --preset windows-debug --target audioalgo_tests --parallel 4
ctest --test-dir build/windows-debug -C Debug --show-only=json-v1 --no-tests=error
& './build/windows-debug/bin/audioalgo_tests.exe' --list-tests
& './build/windows-debug/bin/audioalgo_tests.exe' '[AR-08.b]'
& 'E:/0qyx/res/miniconda/python.exe' tests/infrastructure/test_ar05d_isolation.py
& 'E:/0qyx/res/miniconda/python.exe' tests/infrastructure/test_editor_application_boundary.py
```

`[AR-08.c]`等新增筛选名、纯controller executable、新preset、手势API和注入服务均需**先实现并检查inventory**，不能把本方案文字当现成功能。正式执行使用 `scripts/record_command.py --output <唯一新路径> [--artifact <产物>] -- <命令>` 保留记录；构建需记录当前测试程序/CLI/editor及inventory涉及的所有工作区测试可执行文件hash。仅某个测试程序exit0而筛选未匹配，仍记not_run。

阶段验证使用已有 `verify_architecture_refactor.py` 的 `--stage R2 --build-dir --config --junit --ctest-record --build-record --evidence`，传实际参数并核对累计全部17个R2子项；`--pure`只是开发子集，不能使正式阶段通过。不能把新增用例只挂CTest而不更新既定AR映射。

## 7. 源码冻结、证据与自迭代记录

### 7.1 防止状态文档使验收循环失效

当前source manifest包含文档；验收后再修改development_status会改变hash。采用下列顺序，禁止反复改记录中的hash迁就旧产物：

1. 完成产品/测试/验收映射/状态文档候选更新；状态文档写明本轮待验收、唯一证据目录、已实现行为和下一任务，不预写通过。
2. 为本轮创建不可复用的输出目录，冻结源码manifest。环境/构建/故障传播探针/CTest/桌面证据均基于此快照，期间不改源码及被纳入manifest的文档。
3. 各构型按实际依赖顺序完成，避免共享构建目录并发写；长耗时故障注入已隔离，确认无活动树写入后才纳入完整回归。
4. 最终结果写入 `build/reports/` 的本轮汇总和稳定索引，状态文档通过链接引用它；状态字段和数字从实际JUnit/验证器生成，build输出不改变源码manifest。
5. 若必须随后更改状态正文或产品，明确开启新快照，先前报告成为历史。重新完成受影响的证据关联与验收，不擅自给旧记录重贴hash。不得为图省事把产品、测试或验收文档排除出hash；若确需调整manifest策略，另立ADR和对抗测试后实施。

### 7.2 每个任务卡的最低证据

建议新增RF目录下每个切片和每次尝试使用独立子目录，至少保留：

```text
task.md / loop.md       需求、观察、假设、迭代与下一条Red
red.json + red.log     原始失败；编译失败与行为失败分开
green.json + green.log 同一行为Green
regression*.json/log   精确命令、实际测试数和配置
junit-<构型>.xml        本次唯一JUnit，不覆盖历史尝试
independent/            独立事件表、产物oracle、桌面或纯构建证据
summary.json            当前源码/binary/hash、逐ID状态和未验证原因
```

脚本和汇总格式若新增，先用最小实现，不建设与产品无关的大型编排框架。现有record_command拒绝覆盖旧输出，直接遵守。未知、skipped、not_run、timeout、not_verified均不得计入passed。

### 7.3 一条切片是否可以继续的判定

- 原定Red已由正式测试真实观察，或已有能力核验并说明无需制造Red。
- 同一目标Green、相关历史回归通过，独立验收未发现未处理缺陷。
- 运行的可执行文件对应当前构建；日志/退出码/正数用例/源码及产物hash齐全。
- 已知产品失败不能标完成；外部证据缺失则标部分完成，继续不受影响的工作。
- 汇总准确区分“已实现”“已自动验证”“桌面未验证”“未实现”，列出下一条Red。

本轮结束时应交付实际可运行的编辑器/CLI、纯应用测试程序、Legacy/未知数据fixtures和可重跑命令。R2全通过继续R3，再持续推进R4～R6；未通过且还有可独立完成项就继续开发，不以单次全绿、一份计划或一个正确样例结束任务。全部累计完成条件以持续授权文档和原架构验收为准。

## 8. 给接手 Agent 的启动提示词

> 在当前audioalgo工作区持续完成全部架构重构，先按AGENTS读取必读文档、doc/agent_full_refactor_authorization.md、doc/plan_agent_next_r2.md、最新development_status和RF0006 Review。保留全部现有修改，不重置、不自动提交/推送。先核实并修复“参数0→6→Undo仍为6”的缺陷，按参数差量恢复并保持拓扑历史语义；完成混合历史、手势合并/取消与真实纯应用测试target。随后按本计划T04～T07逐片迁移controller工程所有权、拓扑/布局命令、运行身份与生命周期、现有Gain界面意图接线以及AR-13工程兼容，持续保持真实Gain闭环可运行。每片执行Define→Red→最小实现→同目标Green→Refactor→受影响回归→独立验收→证据→下一片，验证实际值和产物而不是只看返回值/数量；三轮同根因无进展就最小化重现并核对假设后继续。冻结源码后生成构建/故障传播/CTest/桌面证据，按真实JUnit汇总，禁止重贴hash或覆盖失败日志。R2验收包括AR-08.a～g、AR-09.a～e、AR-13.a～e及累计要求；GUI不足保持not_verified并继续独立切片，不能声称R1/R2全过。常规过程无需确认，按工具权限实施，不覆盖用户数据、不侵入独立数据评测线；R2后自行继续R3～R6，直至全部110个AR子用例及各阶段引用的原平台/NED和必需证据累计通过才宣布全部重构完成。
