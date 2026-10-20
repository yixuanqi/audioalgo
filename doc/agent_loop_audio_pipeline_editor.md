---
title: Agent 的 TDD 与 Loop Engineering 开发规程
status: ready-for-implementation
updated: 2026-10-16
---

# Agent 的 TDD 与 Loop Engineering 开发规程

## 1. 任务与边界

执行 [JUCE 开发方案](plan_audio_pipeline_editor_juce.md)，以 [测试与验收准则](testing_audio_pipeline_editor.md)作为验收依据。这里的 Loop Engineering 指围绕一个可观察行为反复执行 **定义契约 → Red → 最小实现 → Green → 重构 → 累积回归 → 独立验收 → 记录**，不是启动后台无限循环，也不依赖某个同名工具或插件。

用户已授权按规划直接开发、修复与验证，常规选择自主完成并记录，不在每一阶段结束请求“是否继续”。遵守实际文件/网络/桌面工具权限；这些权限不能由文档绕过。没有必要授权或外部数据时如实交接具体缺口，不重复询问已授权的实现决策。

本次文档状态不等于任何代码 Gate 通过。Agent 开始后先验证实际源码与环境，从第一项未完成 Gate 推进，直至首版定义满足或遇到无法自主解决的外部阻塞。不要将计划生成、一个可运行窗口或纯 Gain 演示当作整体完成。

2026-10-17 当前优先修复项：文字显示、节点库拖放与连线插入按[专项方案](plan_editor_node_interaction.md)、[专项测试](testing_editor_node_interaction.md)和[专项执行规程](agent_loop_editor_node_interaction.md)的 NI0000～NI0006 实施。专项内持续开发、自验证、自迭代，无需每阶段确认；专项结束如实保留原平台 Gate 缺项，再按平台任务继续。主文档中的通用启动提示不能覆盖专项 NED 全部子用例和桌面证据要求。

2026-10-18 当前重构入口更新：按[重构方案](plan_architecture_refactor.md)、[重构测试](testing_architecture_refactor.md)、[重构执行规程](agent_loop_architecture_refactor.md)从R0～R6连续直接开发。首条产品Red为AR-03.a，独立算法/控制层/自动表单/插件接入均须真实验证；保留原平台与NED验收，外部桌面限制不冻结独立后端开发。此前NI优先条目作为历史背景，当前顺序以重构规程为准；常规阶段不重新请求确认。

## 2. 首次启动与恢复工作

按顺序执行：

1. 确认当前仓库根、适用 AGENTS、文件和用户改动；若无 Git，采用文件清单/hash，不自动创建 Git 仓库。
2. 读取三份有效文档和现有 `doc/development_status.md`；没有状态文档则创建初始 `not_run` 清单。
3. 检测编译器、CMake、Python 3、JUCE/Speex 可用缓存、桌面与设备条件；记录实际版本和配置，不因找不到默认命令就认定工具不存在。
4. 已有实现则执行最相关测试建立基线；只有文档则开始 G0。区分预先存在的失败与本次引入的问题。
5. 创建依赖/环境清单、测试映射和证据目录的最小形式。第三方源码下载版本与 hash 需要可追溯，不跟踪 latest。
6. 选择当前 Gate 的最小纵向切片并写第一条 Red 测试。G0 过后马上推进 G1 可视化 Gain 闭环，避免长期只写架构。

恢复工作时先检查上一份通过证据对应的源码内容；对受后续修改影响的测试重跑。不因上一位 Agent 写了“全部通过”就跳过验证。保留已有用户改动，发现无关变化时不要覆盖。

## 3. 切片卡与任务顺序

切片卡写入状态文档或 `build/reports/loops/<id>/loop.md`，最少包含：

```text
切片 ID：L0001
所属 Gate / 测试 ID：G1 / DSP-01、E2E-01
用户可观察行为：调整 Gain 后，GUI 渲染音频幅值按 dB 公式变化。
前置条件：G0 已验证，固定 F01 输入可用。
独立 oracle：float64 x * pow(10, gain_db/20)。
首条 Red：当前实现输出与解析期望不符时测试失败。
最小修改范围：算法、Node adapter、既有运行链和参数命令。
Green：目标测试、GUI/CLI 相同产物和操作证据。
回归范围：既有 Gain、图验证、项目保存用例。
证据位置：命令、exit code、Red/Green 日志、run manifest。
```

一次只解决一个主要行为；跨层功能可触及多个模块，但不要顺带搭建未来功能。优先顺序为：真实 File/Gain/Output UI → 命名端口/状态/尾部 → Python runner → 真实算法 → 分析与播放完善 → 扩展与打包。

这不是只按测试 ID 从前到后写完单元测试。每阶段以用户可操作的完整切片推进，同时实现对应底层契约。允许在同一阶段先用 fixture 验证框架，再换真实算法；相关算法 Gate 必须使用真实库。

## 4. 每个循环的具体动作

### 4.1 Define：固定契约

- 从需求选择一个测试 ID 和最小场景；明确输入、初始状态、参数、预期数值/行为、失败条件和容差。
- 从解析公式、锁定原库直调、独立指标或外部已确认数据获得 oracle；不能复制实现逻辑制造期望。
- 检查需求是否已有测试；优先补关键边界或修改既有断言，避免大量重复镜像测试。

### 4.2 Red：先看到正确的失败

- 先添加或收紧测试，再运行最小目标，保存命令、退出码及关键断言。
- API 尚不存在时，首次允许编译失败，但记录为 `compile_red`；接口可用后必须出现语义断言失败的 `behavior_red`，才算证明测试能捕获目标缺陷。
- 环境/下载失败属于 `environment_blocked`，不当成行为 Red。
- 测试意外通过时先确认已有能力；若需求已满足，不为制造 Red 故意破坏产品。可在测试隔离分支/受控 mutation 中证明断言灵敏度，随后完整恢复且验证无残留。
- 修复缺陷必须先保留可重现的回归用例；不能先大改实现再编一个只证明当前行为的测试。

### 4.3 Implement：最小正确实现

- 修改最少的产品路径，使真实行为满足契约；不只让 mock 返回期望值。
- 遵守模块边界、所有权、状态和异步规则。新增算法只扩展插件/适配及注册，不在通用 View/编译器添加算法名字判断。
- 需要改变方案中的常规细节时自行记录 ADR，解释实际上游约束和对测试的影响；不把架构讨论作为停工理由。

### 4.4 Green：重跑目标

- 先运行 Red 对应的同一目标，检查真实断言与非零测试数量。
- 涉及算法输出时检查文件内容、样本数、通道和数值，不能只看 exit code 或文件存在。
- 涉及 GUI 时通过实际界面操作确认命令到模型到输出的链路，不以 Controller 单元测试代替交互。

### 4.5 Refactor：保持行为收敛

- Green 后去掉重复、修正命名与依赖；不引入未验收功能。
- 重构后重跑受影响测试；接口变化扩展到调用方/序列化/Python 协议对应检查。
- 不为可逆的颜色/间距微调写镜像测试；关键布局、DPI、交互需实际观察。

### 4.6 Regression：执行有限的累计回归

- 每个切片完成运行相关模块及依赖方向上的回归；每个 Gate 结束运行全部已要求的累计集合。
- 测试通过后没有新变更/新失败/未解决疑虑，不反复运行同一套测试耗时；继续下一个切片。
- Release 打包前执行一次最终源码的完整必需集合、GUI/设备及隔离环境验收，检查缺失/跳过项。

### 4.7 Evidence：记录并立即继续

- 更新切片状态、源码 hash、实际命令、exit code、测试数量和原始日志位置。
- 更新当前 Gate 的完成/未验证/阻塞项与下一条 Red。
- Gate 通过后直接推进下一 Gate，无需等待用户确认。全部 Gate 未过时不得宣称首版完成。

## 5. 自验证不能自证正确

对核心路径至少使用两个独立观察面：

| 变更 | 第一观察面 | 独立验收面 |
|---|---|---|
| Gain/格式转换 | 单元数值与边界 | GUI/CLI 产物对解析 oracle |
| 固定帧/延迟 | 队列计数与生命周期测试 | 随机分块/脉冲序列及 JUCE 实际图输出 |
| 第三方算法 | 原库直调与封装一致 | clean-reference/场景指标与听评 |
| Python | SDK/协议测试 | 真实外部进程故障注入和后续成功运行 |
| 工程保存 | 序列化往返 | 重启应用打开后重跑同一工程 |
| 自动参数 | Descriptor/命令测试 | 真正改变控件后输出变化 |
| 扩展性 | 插件局部测试 | 新插件加入且通用代码 diff 不变 |

独立验收可由同一 Agent 执行，但必须实际读取产物、错误路径和需求，不只复述实现。无需默认创建子 Agent。禁止自生成 golden 并自动批准；现有阈值不达标就修实现/数据契约或保留失败。

## 6. 失败迭代与停滞处理

每次失败记录“观察 → 假设 → 区分假设的检查 → 最小修复 → 结果”。先减少输入、节点、线程和依赖，定位算法/适配/调度/UI 的哪一层首次偏离。

同一根因连续三轮无进展时必须改变诊断策略：

1. 保留最小重现与日志，停止重复相同修改/相同下载。
2. 阅读锁定上游源码或官方接口，检查假设：幅值、帧长、时钟、所有权、JUCE 线程/延迟语义、Python 模块身份等。
3. 将问题拆成独立可测契约；在 `doc/decisions/` 记录调整或替代路径。
4. 可行时继续独立切片，不让一个外部数据缺口冻结整个平台。

三轮规则要求变换方法，不是允许自动放弃需求。外部条件无法解决时准确列出缺失的权限/数据/依赖，保留失败状态与恢复命令；不通过 mock、跳过或变更通过准则隐藏问题。涉及工具权限的执行按工具要求申请，不使用其他通道规避。

## 7. 状态、日志与交接模板

`doc/development_status.md` 在实现开始时创建，保持简短可恢复：

```text
最后更新：实际日期时间
源码与环境：commit 或 source_tree_hash；环境清单路径
当前 Gate：Gx；passed / in_progress / blocked
已完成：切片 ID + 对应测试 ID + 证据路径
当前失败：实际断言/退出码；最小重现
未验证：设备、桌面、真实数据等具体项目
下一条 Red：输入、预期、待实现行为
下一步命令：当前实际可运行的命令
设计决定：ADR 路径
已知限制：不要把未来能力列为完成
```

`build/reports/loops/<id>/` 保存 `red.log`、`green.log`、回归报告及 `loop.json`；最终 Gate 报告保存于 `build/reports/gates/<gate>/`。日志若含用户路径/音频元数据，保持本地，不自动上传。恢复时确认 build/ 中证据仍存在，文档链接不能替代丢失日志。

报告必须包含实际命令参数数组、工作目录、开始/结束时间、退出码、失败/通过/跳过数、环境和产物 hash。上一次成功结果不能覆盖本次失败日志。

最终交接说明：完成的功能和 Gate、可运行应用/示例/命令、测试与独立验收证据、真实限制、尚未满足的验收项。不得只给代码列表或宣称“理论可行”。

## 8. 执行伪代码

```text
read effective instructions, plan, acceptance, previous evidence
inspect actual source and toolchain
establish reproducible baseline
for gate in G0..G6:
    while gate.required_behavior remains:
        choose smallest vertical slice
        define observable contract and independent oracle
        write and run test; classify actual Red correctly
        implement minimum product behavior
        rerun target until Green or diagnosed external blockage
        refactor and run impacted regression
        inspect actual artifact / UI / failure recovery
        record source-linked evidence and next Red
        if same-root-cause stalled three times:
            minimize, inspect upstream, revise diagnosis, continue
    run cumulative gate verification with inventory check
    if required checks fail/missing/skipped: keep gate incomplete
    else record gate passed and continue without confirmation
perform final Release, package and real-device/data acceptance
report completion only when definition of done is satisfied
```

## 9. 可直接交给开发 Agent 的启动提示词

> 在当前 audioalgo 仓库直接开发 JUCE+C++20 音频算法可视化编辑平台。先读取根 AGENTS.md、doc/plan_audio_pipeline_editor_juce.md、doc/testing_audio_pipeline_editor.md 和本规程，检查实际源码、用户改动和工具链，然后从第一项未完成 Gate 持续推进到 G6。第一阶段就交付可连线、调参、运行、看波形及保存的 File→Gain→Output 可视化闭环，随后完成 C++/Python Node 扩展、独立 runner、Speex 降噪/AEC、Python 波束形成、试听/A-B、声谱与打包。每个行为严格执行契约→Red→最小实现→Green→重构→相关回归→独立产物/GUI 验收→证据记录，按测试文档的数值与行为准则判定；不放宽阈值、不跳过必需测试、不伪造 golden 或完成结果。同一根因连续三轮无进展就缩小重现、查上游接口并调整诊断，继续自迭代。维护 doc/development_status.md 和 build/reports/，常规技术选择自主记录并执行，中间无需确认；遵守实际权限，不自动提交/推送/发布/购买。无法取得的设备、真实数据或授权如实标注未验证并继续其他工作；只有全部必需 Gate 通过才能声明首版完成。最后交付可运行应用、演示工程、重跑命令、测试证据和真实限制。
