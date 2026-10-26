# AudioAlgo 开发入口

## 目标与必读文档

本仓库建设可扩展的 JUCE 音频算法可视化编辑平台，支持 C++ / Python 算法、类型化节点、调参、试听和可复现实验。

2026-10-22更新：[交付收口与独立原生节点约束](doc/plan_delivery_convergence.md)是当前排程、必要验证和新增插件边界的有效补充；覆盖旧文档中相反条款。用户C++节点必须支持在仓库外仅用发布SDK独立编译DLL、部署后重启现有宿主使用，宿主无需重新编译/链接；首版不要求热卸载。原110个AR子项保留，新增UPN-01～06单列必需验收；实际已实现子集和未验证项以`doc/development_status.md`唯一当前摘要为准，不沿用首次编写时全部未实现的历史状态。

开始代码工作前依次阅读：

1. [详细开发方案](doc/plan_audio_pipeline_editor_juce.md)
2. [测试与验收准则](doc/testing_audio_pipeline_editor.md)
3. [Agent TDD 与 Loop Engineering 规程](doc/agent_loop_audio_pipeline_editor.md)

当前算法库与编辑器分离重构，继续依次阅读：

4. [架构重构详细开发方案](doc/plan_architecture_refactor.md)
5. [架构重构测试与通过准则](doc/testing_architecture_refactor.md)
6. [架构重构 Agent TDD 与 Loop Engineering 规程](doc/agent_loop_architecture_refactor.md)

处理编辑器文字显示、节点库拖放、连线、节点插入或拓扑历史时，再继续依次阅读：

7. [节点交互专项开发方案](doc/plan_editor_node_interaction.md)
8. [节点交互专项测试与通过准则](doc/testing_editor_node_interaction.md)
9. [节点交互专项 Agent 迭代规程](doc/agent_loop_editor_node_interaction.md)

处理应用内自定义节点创建、源码编辑、检查试跑、构建封装、版本应用或节点库动态刷新时，再依次阅读：

10. [自定义节点工作台开发方案](doc/plan_node_authoring_workbench.md)
11. [自定义节点工作台测试与通过准则](doc/testing_node_authoring_workbench.md)
12. [自定义节点工作台 Agent TDD 执行规程](doc/agent_loop_node_authoring_workbench.md)

2026-10-26补充设计：[多文件工作区详细开发方案](doc/plan_node_workspace_implementation.md)与[逐项验收方案](doc/testing_node_workspace_authoring.md)规定后续文件夹节点/专业编辑路径的目标及NWS-01～10，[ADR0040](doc/decisions/0040-node-workspace-schema-v1.md)已冻结工作区 schema 1、运行文件清单/整树 hash、路径与预算、创建/转换事务。当前事实：W0 已冻结（`tests/fixtures/node_workspace/` 夹具 + 独立 `workspace_expectation.py` + `workspace_lock.json`，`tests/node_workspace_manifest.json` 30 条与 CTest `NWS-00.contract`）；W1 首个行为已实现（生产面板“Node project folder…”/节点库“Open node project”创建并重开目录，原 `.aadraft` 不变，目录模式只读）。NWS-01.a/b/c 仅 `partial`，其余 `not_run`，桌面 `not_verified`；不强制 VS Code，当前单文件工作台与PC/NA主线保持。恢复实现仍先读唯一状态摘要，按直接依赖接续。

2026-10-24用户补充：卷根草稿保存和跨语言Pipeline可观察闭环按[修改方案](doc/plan_pipeline_execution_closure.md)、[逐项测试与通过准则](doc/testing_pipeline_execution_closure.md)、[Agent TDD执行规程及启动提示词](doc/agent_loop_pipeline_execution_closure.md)接续。历史首片PC-01为生产卷根路径Red，随后PC-02 Print编写/Apply/库拖放/画布Run/日志可见，再推进日志身份/失败/预算、混合数值、兼容C++日志及工作台构建激活。NA0031最初只有只读根因探针，后续修复与剩余真实桌面缺项以唯一当前摘要为准；原NA0030通过范围不覆盖新缺口。原68个NA及AR/G/NED/UPN保留，工程SaveAs携带继续挂账；真实桌面仍暂停，不把本补充三文档当实现完成。

2026-10-24工作台已启动：从唯一当前摘要及NA0008证据接续，不重做NA-01.a草稿闭环。NA0001～07已有草稿、隔离检查、schema 2精确锁和迁移备份的历史分片证据；NA0008推进真实Apply/库刷新/画布Run，NA0009修复生产面板取消投递和放弃关闭。当前构建及各子项状态必须核对最新命令/源码身份，不把历史Green当当前累计通过。按S0～S5优先完成Python，再完成版本/兼容和C++构建激活；原P3保留作后续恢复，不扩无关重构。NA-01～17共68个必需子项另列，不替代AR/G/NED/UPN；保留缩放连线、右键多选、负坐标拖动等既有交互。真实桌面仍暂停，缺项不记通过。

原平台重构R0～R6与AR-01～20全部字母子用例仍须逐项验收；工作台实施期间主线为NA，从唯一摘要接续，NA结束后恢复R5未完行为。历史首条产品Red为AR-03.a（不同工程、相同revision，旧运行结果不得覆盖新工程），不重做已完成行为。节点交互已有NI系列实现和未完成证据，保留NED-01～14全部必需子用例及真实桌面要求。受影响交互回归必须修复，外部证据不足不阻止独立后端切片。重构基本解耦、全阶段和平台G0～G6完成分开判定；新增用户插件UPN需另行通过。

所有方案中的待建接口、脚本、preset 和测试不是现有能力。后续 Agent 已获按规划直接开发、自验证、自迭代授权，常规阶段无需确认；方案文档就绪不表示代码已实现或验收通过。

`doc/plan_audio_pipeline_editor_agent_guide.md` 是已被替代的历史方案。当前源码根就是本仓库；不依赖 OLS/OIP，也不应修改其他仓库。所有文档中的目标接口、脚本、测试和应用均须先检查是否存在，未实现的不能当成现成功能。

## 实施约束

- 在当前工作区按阶段直接实现和验证；常规技术决策自行记录并推进，不反复请求用户确认。遵守实际工具权限；权限或外部资源不足时如实报告，并继续不受影响的工作。
- 不自动提交、推送、发布、购买许可，不覆盖用户音频或无关修改。
- 不因应用采用 JUCE 就让纯算法库依赖 GUI、项目模型或 JUCE 处理器。
- 项目模型是唯一权威；JUCE 图是音频执行投影。Python、几何、指标、整段任务由统一运行服务组织。
- 第一阶段交付 File → Gain → Output 的真实可视化闭环；不能长期停留在框架、CLI 或静态 UI。
- 新 C++ 节点通过描述与工厂注册；Python 通过 manifest 和独立 runner 扩展。新增算法不得修改通用画布或调度器的算法分支。
- 内置节点可静态注册；用户原生节点按补充约束通过版本化插件接口发现/装配，不能以“算法库可独立消费”替代“节点独立编译后宿主直接加载”。
- Python 不进入 UI 线程或设备音频回调；首版代码更新重启 runner。低延迟在线算法链不属于首版完成条件。
- 对行为、数值、状态和失败恢复采用 Red → Green → Refactor → 累积回归 → 证据记录。先验证失败原因，再写最小实现；不为装饰性样式编写镜像测试。
- 不通过放宽阈值、删断言、改期望、跳过必需测试或伪造 golden 使测试变绿。合理的契约变更须先记录依据，再更新独立验收依据。
- 每个阶段必须同时交付实际代码、测试、演示项目、可重跑命令和报告；测试缺失、未执行、被跳过或环境不满足都不算通过。
- 交接时维护 `doc/development_status.md` 与 `build/reports/`，注明已完成 Gate、真实结果、阻塞项和下一条 Red 测试。尚未实现这些产物时先创建最小版本。
- 日常只执行目标行为和直接受影响回归，优先Debug增量；公共契约/构建变化才扩相关消费者，完整矩阵集中稳定出口和最终Release。文档修改只检查一致性与绑定，不重编产品；同源码已有有效结果不重复跑全套/故障探针/变异测试。减少重复执行不免除最终必需验收，也不允许把未执行记为passed。
- 恢复以唯一当前状态摘要和直接证据为入口，不重做已完成的R0/R2或重新制造Red；每个交付出口应新增可用功能/关闭实际缺陷，代码、精确验收映射、简短状态同片更新，历史记录与当前入口分开。

## 自主迭代

用户已明确给予[全部重构持续开发授权](doc/agent_full_refactor_authorization.md)：从当前进展接续，持续自验证、自迭代，直至R0～R6、UPN及全部累计必需验收通过；常规阶段不再确认。当前交付顺序按`doc/plan_delivery_convergence.md`，R5任务细节见`doc/plan_agent_next_r5.md`；旧R2任务卡保留作历史，不是恢复入口或整个任务的结束边界。外部证据不足如实挂账并继续独立工作；工具权限、用户明确暂停/取消及真实阻塞按授权文档处理。

一次只推进一个可验收行为；发生失败先定位层次，保留重现用例。同一根因连续三次迭代没有进展时，缩小重现、检查上游接口和设计假设，记录调整后继续。只有缺少必要授权、资源或输入且无可行独立工作时才交接阻塞，不得把已知失败报告为完成。

用户要求的是持续完成既定范围，不以生成方案、完成窗口骨架或单个示例作为整个平台完成。任务中断后从证据与状态文档恢复，不根据上一条“通过”文字跳过验证。
