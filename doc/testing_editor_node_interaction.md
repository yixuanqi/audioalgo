---
title: 节点交互专项测试与通过准则
status: ready-for-implementation
updated: 2026-10-18
---

# 节点交互专项测试与通过准则

## 1. 适用范围与通过语义

本文件是[专项开发方案](plan_editor_node_interaction.md)的验收依据，补充[平台测试准则](testing_audio_pipeline_editor.md)，不放宽现有阈值。下列 NED-01～14 都是必需项，当前是待实现契约，不是已有测试或通过声明。

一个 ID 必须覆盖所有列出的子场景；一个大测试只覆盖其中一项不能代表整个 ID 通过。`passed / failed / not_run / not_verified / blocked / skipped` 分别记录，只有 passed 算通过。缺测试、零收集、跳过、超时、无桌面或缺截图均阻止专项完成。

每条断言须对应真实的命令、模型变化、输入输出或可观察 GUI 行为。不得用测试重写一份命中/插入实现作为 oracle；不得用组件回调、直接调用命令或改 JSON 代替真实 GUI 拖放验收。

## 2. 固定 fixture、数值和几何基准

1. 复用并核实 F01/F02 的解析音频：16/48 kHz、单/双/四通道、互异频率/脉冲，长度 1/479/480/481/511/512/513/16037，保留输入 hash。空音频沿用主方案的明确接受或诊断契约。
2. 工程 fixture：空图；未连的 File/Gain/Output；完整 File→Gain→Output；带分支的合法编辑图；重叠节点；删除 Output 后的图；两个额外未连接 Gain。
3. 描述 fixture：不同名称但等价音频端口的处理节点、非音频端口、不相容方向、两个可选输入/输出组合、额外必需输入。仅注册受控测试节点，不修改通用 View/编译器的算法分支。
4. Unicode fixture：工程名 `中文 工程 … −6 dB`、WAV 名 `测试 音频.wav`、描述标题 `增益节点`；测试实际路径读写及 UI 文本，不只测试 JUCE 库函数。
5. 使用逻辑坐标：拖拽阈值 6 px，边命中距离 <=8 px，节点间保留 8 px。图元几何 oracle 使用明确预设矩形/路径点和边界关系；坐标变换通过已知控件偏移验算，不重复实现搜索算法。
6. 无碰撞落点：以节点中心为拖放锚点，提交中心相对最终预览中心误差每轴 <=1 个逻辑像素。发生避让时检验最终位置与预览一致、可见且保留间隔，不要求测试复制避让搜索顺序。
7. 音频默认容差：每个样本满足 `abs(actual-expected) <= 2e-6 + 1e-5*abs(expected)`；帧数、通道、有效区间和通道顺序精确一致。oracle 用 float64 输入和 `x * pow(10, sum(gain_db)/20)`，不调用被测 Gain 或插入服务计算期望。

## 3. 必需测试矩阵

注册名建议 `NED-03.a empty_graph_accepts_first_edge`；CTest 名称唯一，必要标签为 `node_interaction`，另用 `pure`、`component`、`desktop` 区分执行环境。同一 ID 的具体子用例清单必须写入版本化 manifest，不能仅按前缀推断覆盖完整。

| ID / 关联主需求 | 必须覆盖的子场景与通过准则 | 验证层 |
|---|---|---|
| NED-01 / UI-07、IO-01 | a. 实际中文工程名、文件名、描述名和 …/·/− 的 UI 文本码点正确；b. 中文含空格 WAV 从文件选择到 Run、保存重开均成功且输入 hash 不变；c. 画面无错误解码和缺字方框，长文本可看到完整提示 | 应用/IO + 桌面 |
| NED-02 / UI-07 | a. 当前 Gain 摘要与 IN 标签重叠用例先失败；b. 标题、摘要、每个端口文本区域互不相交，文字不越出内容区；c. DPI 100/150/200%、逻辑客户区 1100×700 和 1440×900 可用，关键控件可见可操作；d. 多端口测试节点无重叠且热区与绘制一致 | 几何/组件 + 桌面截图 |
| NED-03 / GRAPH-01、UI-01 | a. 空图创建 File/Gain/Output，先接 File→Gain 成功，再接 Gain→Output 成功；b. 反向先接第二条也成功；c. 两个额外未连接 Gain 不阻止另一个合法接线；d. 每条合法边只加一次 revision；e. 保存未完成图重开仍可编辑 | 纯编辑服务 + 桌面 |
| NED-04 / GRAPH-01、02、UI-02 | a. 缺节点、缺端口、方向错误、类型不匹配、自连、环、重复边、重复输入全部拒绝，错误定位端点；b. 拒绝前后节点、连接、参数、选择、布局、revision、dirty 和历史一致；c. 未完成图允许编辑，但 GUI/CLI Run 仍拒绝缺输入/缺文件/非法参数并给具体原因，不产生成功标记 | 纯模型/运行 + 桌面诊断 |
| NED-05 / UI-01、03 | a. 已有 demo 连续点击添加 10 个 Gain，自动放置无重叠，若无可见位置则明确拒绝且不改变工程；b. 创建按描述填充默认参数并产生唯一 ID；c. 新节点被选中、Inspector 指向同一 ID；d. 注册另一种处理节点后库条目和默认参数自动出现，无 View 类型分支；e. 未知 type 拒绝 | 编辑/组件 + 桌面 |
| NED-06 / GRAPH-03、UI-03 | a. 重叠节点命中视觉最上层；b. 节点覆盖区不选背后边；c. 端口优先于所属节点移动；d. 移动仅改布局并标记未保存，不增加计算 revision，运行输出一致；e. 所有绘制/命中位置共享坐标变换 | 纯几何/组件 + 桌面 |
| NED-07 / UI-01、07 | a. 真实从库拖 Gain 到空白落点，只新增一个节点且坐标符合第 2 节；b. 未超过阈值按点击处理一次；c. 超过阈值后释放不再触发点击添加；d. 释放画布外、Esc、切换工程、关闭视图均无新增和残留预览；e. File 拖放选择成功保留落点，取消零修改；f. 选择期间切换工程/关闭视图，旧回调不修改新工程、不崩溃 | 状态/组件 + 真实桌面 |
| NED-08 / UI-01、02 | a. 点击输出再点击输入与按下输出拖至输入释放产生相同精确端口连接；b. 拖动预览随鼠标更新；c. 无效释放/Esc 不留下边且不移动节点；d. 目标已有输入不静默替换；e. 至少一种可发现入口断线，删除精确边且只加一次 revision，随后可重新接线 | 编辑/组件 + 真实桌面 |
| NED-09 / GRAPH-03、PRJ-01 | a. 删除 Output 清理其边和选定输出；b. 重建唯一 Output 自动设置所选 sink，接线后可运行；c. 第二 Source/Output 按当前一源一 sink 能力拒绝且状态不变；d. 保存重开所选输出一致，不再报 selected_output_mismatch；e. 删除恰有一条入边和一条出边的处理节点时，只有结构校验通过才自动旁路连接，revision 只增加一次且可撤销/重做；分支处理节点删除时不猜测合并方向 | 编辑/运行 + 桌面 |
| NED-10 / UI-01、GRAPH-01 | a. 库节点拖到 A→B 的线段，预览选中边；提交得到 A→N→B，旧边恰好少一条、新边恰好多两条、节点恰好多一个、revision 恰好加一；b. 分支仅修改目标边；c. 已有未连接节点插入时不新增节点；d. 已连接节点、Source/Sink、无兼容端口插入拒绝并恢复；e. 8 px 阈值边界、最近边/同距、节点遮挡决定稳定；f. 多组合显式选择，取消不变；g. 额外必需输入留待补全并阻止 Run；h. 目标边/工程失效、失败注入不留半成品 | 原子命令/几何 + 真实桌面 |
| NED-11 / PRJ-02、UI-03 | a. 本专项 Add/Connect/Disconnect/Insert/Delete 均可撤销重做，模型及画布一致；b. 插入只有一个历史项，一次撤销恢复旧边并移除库创建的新节点；c. 已有节点插入撤销恢复原位置；d. undo/redo 计算 revision 单调增加；e. 失败命令不入历史，undo 后新操作清 redo；f. Open/New 清旧历史、待接线、拖拽和旧资源选择上下文；g. Ctrl+Z 撤销拓扑操作，Ctrl+Y / Ctrl+Shift+Z 重做，且输入焦点在编辑器子控件时可触发 | 编辑历史/组件 + 桌面 |
| NED-12 / BLD-04、LOOP-01 | a. 测试 manifest 的每个必需 ID/子用例确实注册；b. 零收集、缺 ID、缺子用例、断言失败、非零、超时、skipped/not_run/not_verified、缺桌面证据分别令专项 verifier 非零；c. 受控失败探针退出链被实际验证并清理；d. 日志关联本次源码、构建和证据，不接受旧 pass JSON 代替运行 | 验证器基础设施 |
| NED-13 / PRJ-01、E2E-01 | a. GUI 插入后 Save、关闭/重开恢复 UUID、精确端口、参数、路径、坐标和所选输出；b. 输入文件不变；c. 重开后重新渲染符合独立 oracle；d. 失败保存不破坏旧文件，沿用主 PRJ-04 的安全约束 | 保存/运行 + 桌面 |
| NED-14 / DSP-01、E2E-01、UI-01 | a. 真实 New→拖入 File/Gain/Output→接线→Run；b. 在线上插入新 Gain，原 +6 dB 与新 −6 dB 串联，相同快照 GUI/CLI 均符合恒等 oracle；c. 改新 Gain 为 0 dB 后输出符合 +6 dB oracle，证明节点真实参与执行；d. 断线→Run 失败→重连→成功，保存重开再运行；e. 无残边/幽灵节点，操作与产物可关联 | 真实 GUI + CLI + 独立数值 |

上述 a/b 等场景允许进一步拆分，但不能合并后漏验。NED-11 只验本专项拓扑历史，不代表主 PRJ-02 的完整参数历史已交付。

## 4. 桌面与 DPI 验收规则

- 使用实际 Windows 桌面输入或被允许的等价 OS 自动化，记录操作方式、OS、显示缩放、物理/逻辑窗口尺寸、可执行文件路径/hash、源码 hash、时间和工程 ID。不要复用之前截图冒充本次结果。
- 每个 DPI 100/150/200% 下都执行至少一次库拖放、文字检查、命中和拖线；两个逻辑客户区尺寸都留截图并检查工具栏、Inspector、波形/状态区未发生关键遮挡。窗口最小尺寸/布局策略必须使 1100×700 可操作。
- DPI 自动化缺权限时记录具体 not_verified，不在未经确认的情况下修改用户系统显示设置。继续模型/组件/数值工作，但专项不算通过。
- NED-10 至少记录插入前、预览、提交后三个状态和保存的工程 JSON；同时检查边集合。截图只能证明画面，不能替代拓扑和音频断言。
- NED-14 截图/日志须对应同一 project_id、run_id、revision 和输出路径，读取本次音频核对。不得只用“波形看起来变化”证明正确。
- 无桌面时可完成 component 检查，但必须分别保留 desktop 子项未验证状态，不能把二者都标 pure 或让无窗口回调伪装 OS 拖放。
- 本专项没有新增真实设备试听门槛；原平台的 UI-02/UI-04 真实播放/受控 sink 要求照常保留，不能因本专项通过而标记设备验收完成。

## 5. 测试注册与专项验证器交付

待实现文件建议：

- `tests/unit/test_editor_commands.cpp`：分层校验、事务、revision、历史、Output。
- `tests/unit/test_editor_geometry.cpp`：位置、图元区域、层级、边命中；不依赖算法执行。
- `tests/gui/test_editor_interaction.cpp`：JUCE 消息环境中的真实组件输入状态/文本绑定。
- `tests/gui/` 下桌面步骤或受控输入脚本：真实库拖放、拖线、插入、DPI、保存重开。
- `tests/editor_interaction_manifest.json`：版本化必需 ID/子用例、层次、runner、注册名、所需证据；不填写预设通过状态。
- `scripts/verify_editor_interaction.py`：实际执行并汇总专项验收，失败非零。

这些路径目前尚未实现。后续 Agent 应先检查真实存在情况再创建；可按最小结构调整并同步本文，不改验收含义。

专项 verifier 必须先读取 CTest JSON inventory，比对 manifest 的全部自动化子用例，再执行对应测试并解析逐用例结果（如 JUnit/CTest Test.xml），不能仅根据总退出码和测试名前缀推定通过。桌面部分核对步骤记录、实际截图、工程/运行产物、时间及 hash 关联；视觉正确性仍须实际观察并记录，不把自填 `passed` 文件当独立证明。纯模式报告只用于阶段开发，缺 desktop 时最终状态必须非 passed。

现有 `verify_gate.py` 只扫描主测试表。本次新增专项 ID 不加入原 G0/G1 表以免文档修改直接使现有 inventory 合约变化；专项以单独 verifier 验收。后续如接入主 verifier，先为完整子用例与证据门禁写 Red，不删除主测试或降低要求。NED 结果只能补充主 UI/GRAPH/PRJ ID 对应子集。

验证器受控探针在独立临时测试目录/配置运行，不破坏产品源码或用户工程；保留失败日志，清除临时注册并再次检查正常 inventory。为验证 verifier 的“拒绝假通过”契约可使用合成失败输入，但产品行为验收必须执行真实实现。

## 6. 回归、命令与完成报告

现有命令（执行前检查工具、preset 和路径）：

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --parallel 4
ctest --test-dir build/windows-debug -C Debug --show-only=json-v1 --no-tests=error
ctest --test-dir build/windows-debug -C Debug --output-on-failure --no-tests=error
cmake --preset windows-headless
cmake --build --preset windows-headless --parallel 4
ctest --test-dir build/windows-headless -C Debug --output-on-failure --no-tests=error
& 'E:\0qyx\res\miniconda\python.exe' scripts/verify_gate.py --gate G1 --build-dir build/windows-debug
```

headless 不注册/执行依赖编辑器或桌面的专项用例；纯编辑服务测试应在 editor-off 下可构建运行。不得为了 headless 全绿将必须的 GUI 子用例删除或改成可选。主 G1 verifier 当前预期仍会报告原验收缺项，应保留真实失败与原因。

以下是待实现命令契约，脚本不存在前不得报告执行成功：

```powershell
& 'E:\0qyx\res\miniconda\python.exe' scripts/verify_editor_interaction.py --build-dir build/windows-debug --config Debug
```

开发基线及最终专项验收都应重建受影响 target 后运行；不能仅运行旧二进制。每切片执行受影响专项子集及原 GRAPH/PRJ/IO/DSP/E2E 回归；专项结束运行完整 Debug 与 headless 回归。项目打包前完整 Release 累计回归仍属主方案要求，不因专项 Debug 通过而免除。

最终报告至少含 source_tree_hash、构建配置及可执行文件 hash、command 参数数组/cwd、UTC 起止和 Asia/Shanghai 展示时间、exit code、注册/执行/通过/失败/跳过/未验证数量、每个 NED 子用例结果、日志路径/hash、截图、project/run ID、输入/输出 hash、实际误差最大值及容差、限制。任何必需子项不通过均令总体非 passed。
