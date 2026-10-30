---
title: 连线编辑与节点库分类 Agent TDD 执行规程
status: ready-for-implementation
updated: 2026-10-28
---

# 连线编辑与节点库分类 Agent TDD 执行规程

## 1. 入口与授权边界

本规程指导后续Agent实施[修改方案](plan_connection_editing_and_node_library.md)，以[66项测试通过准则](testing_connection_editing_and_node_library.md)作为独立验收依据。2026-10-28本轮仅创建文档，不代表新增代码、测试或阶段通过。收到用户启动指令后，按CNL切片直接修改、自验证、自迭代，常规决策记录后推进，不逐片请求确认。

先读根[AGENTS](../AGENTS.md)规定的主平台、重构、节点交互和涉及工作台的文档，再读本三文档、[唯一当前摘要](development_status.md)、[交付约束](plan_delivery_convergence.md)、ADR0040/0041及当前格式/SDK合同。旧NI计划中的“尚无Controller/拖拽”等是历史，不从零重建R0/R2或重做已有草稿闭环。

只在当前仓库操作，保留用户修改；不自动提交/推送/发布/安装依赖/购买，不覆盖用户音频或工程，不自动关闭/重启用户编辑器。**现有真实桌面暂停继续有效**，本专项文档和启动词不构成恢复桌面的授权；允许的纯库/组件/受控子进程/独立文件验证继续，D保持not_verified。不能因缺D证据停止独立工作，也不能把组件事件称为真实鼠标桌面验收。

不默认派生子Agent；一次只推进一个可验收行为。专项产品范围见方案第3节；完成当前专项的可执行子集后仍准确交接未验证项及原NWS恢复点，不宣称整个平台完成。

## 2. 恢复核查：短、具体、可追溯

1. 核对cwd、Git HEAD、适用AGENTS和工作区差异，读取状态顶部及最近CNL原始报告。不要reset、清理或覆盖他人修改。
2. 用`rg`核实实际画布、手势、命中、Controller/历史、库条目、原生/Python描述解析、作者Check/Apply/刷新和持久化路径。找不到目标先查CMake实际target，不把本方案API名当现成接口。
3. 核对源码/二进制hash和旧证据受影响范围；用户截图窗口版本未知，不能猜“旧版导致”。已有正向拖线/选线Delete先观察，不为证明TDD故意破坏它们。
4. 检查实际CMake/编译器/Python路径、preset和最近报告。当前存在`windows-debug`、`windows-headless`、`windows-application-only`、`windows-release`；不意味着所有目标在每个构型都可用。Python优先明确路径，不能使用碰巧在PATH的旧解释器。
5. 选择首个未完成行为；新建未占用`build/reports/loops/CNLxxxx/`。记录几行任务卡：可观察行为、精确子项、独立期望、首条Red、最小产品路径、直接回归、第二观察面、外部缺项。
6. 首片同片建立66项精确映射及最小拒假通过检查，初始缺项写not_run；无需先构建所有未来测试或空框架才允许交付第一个断线功能。

## 3. 首片与后续行为顺序

### CNL0001：保留节点的明确断线

优先首条产品Red：通过真实生产Inspector/菜单入口触发“断开节点全部连接”，断言线性节点所有关联边消失、节点ID/参数/位置/资源/锁保留、没有新增旁路、一次revision与undo；当前入口缺失应产生明确行为失败。也可先用Controller级待建命令compile_red，再补同一生产行为Red，不能只停在编译失败。

先复核已有选边Delete和Controller单边disconnect，不另建平行体系。只修本片需要的应用命令、入口及回调；批量断线用候选一次提交，不循环调用多个带历史的disconnect。保留旧DeleteNode合法线性旁路和多选删除。

### CNL0002：双向起拖且保留旧建连

Red：生产GraphCanvas中从空闲输入拖到输出无法生成规范化边。使用真实事件顺序down→move/drag→up，固定目标坐标和已知四元组作为期望，不只直接调用PortConnectionGesture。保留输出点击→输入点击、输出拖输入、新输出分支、5/6/7px阈值、缩放命中和节点不移动。

### CNL0003：原子改接和取消

按“目标端改接→源手柄改接→空白断线→失败/取消→历史/运行身份”分成最小行为，每次先写区分性Red。按下/移动时完整模型快照不变；释放时只提交一次。非法目标、占用输入、Esc/画布外/失焦、原端口no-op及工程变化分别测试，不能共用一个“没新增边”的弱断言。

右键菜单与原右拖框选的点击/拖动边界必须同片回归；菜单按值捕获边四元组和epoch/revision，不延迟使用vector下标。任何失败先修复，不能为了提供菜单删掉右键框选。NCE自动化出口完成后继续独立分类工作，不等待暂停的桌面。

### CNL0004：先冻结浏览合同，再贯穿生产包路径

根据方案第5节建立**新增独立**fixture与期望：browser可选字段、ID表/层次/预算/降级、来源与语言不可伪造、manifest完整匹配、作者内容hash/Ready撤销。不改旧W0夹具、独立oracle、SDK ABI或旧版本锁政策。

首条Red可为带browser的真实Python描述经过标准化后在库条目丢失分类；另以旧DLL和新manifest一致/不一致用例覆盖原生边界。新原生fixture从发布SDK独立构建，旧fixture不重编证明兼容。不允许把外部manifest的browser从比较中剔除，也不能仅让host内置节点有分类就宣布贯通。

浏览元数据放公共不依赖GUI的值类型，DSP工厂/调度不读取分类。作者通过已有声明编辑路径声明分类，不顺带实现NWS完整声明表单。声明变化按原内容身份流程重新Check/Apply，保留旧不可变包和旧工程输出。

### CNL0005：分类、搜索、筛选

分别为“分类树可操作→搜索折叠项→来源/语言组合筛选”写Red，固定独立列表oracle。生产组件中找到真实条目并点击/拖放；不能仅断言计算出来的vector正确。搜索框键盘不能触发画布删除。旧工作台入口如改成菜单，逐项验证动作/Component ID仍可定位。

### CNL0006：Apply刷新、拖放和旧版本

Red优先找真实风险：过滤开启时Apply新分类，或一次拖拽中刷新使payload失效。用事件屏障注入，不靠sleep竞态。prepare/publish失败保持旧库；成功保持有效查询/折叠/滚动/身份，所有type_id仅一默认版本。

走生产Check→Apply→新库drop/线上插入→Inspector调参→画布Run→CLI独立Run→保存重开旧v1。每份产物读取样本/元数据并与独立公式比较，不能只看exit0或WAV存在。已有工作区、外部编辑器启动/记忆相关路径按影响选择直接回归。

### CNL0007：专项出口

核对精确66项和受影响旧集合，执行当前最终源码的Release累计集合及相关独立消费者，补演示项目、可重跑命令和原始证据。D暂停就准确保留not_verified；报告“已完成自动化子集/未通过正式专项”，不等待或伪造D，也不把单例Gain通过当整体交付。

上述编号使用前查占用；本片完成且有独立工作即继续下片，直到当前可执行范围完成。新增的既有行为失败优先修复，不带已知回退进入下一产品行为。

## 4. 每个行为的TDD闭环

1. **Define**：固定初始状态、输入事件、目标四元组/独立列表/数值公式、错误状态、容差及子项。复用原断言/阈值，明确哪些状态必须保持。
2. **Red**：先写最小测试，真实运行并保留失败。compile_red、behavior_red、environment_blocked分开；编译失败不是完整行为证明。意外已通过时核实已有能力并记录，不破坏实现来制造Red。
3. **Implement**：仅改最小生产路径，复用应用控制、校验、历史、运行服务。不能先大改后写迎合实现的测试，不能为测试专设只在fixture中有效的执行分支。
4. **Green**：运行与Red相同的用例和原期望，记录非零收集/断言数、exit code、日志及源码身份；实际读取文件、边集或输出，不只检查状态字符串。
5. **Refactor**：Green后才去重/拆职责；不扩成图形框架或IDE。重构后重跑直接影响集合。
6. **Regression**：用影响图选择既有回归。直接失败必须定位修复，不删断言、不skip、不改golden/阈值求绿。本已存在且不受本片影响的失败单列来源与范围，不能把该项记通过。
7. **Independent check**：Controller状态之外观察生产组件投递；组件Run之外观察独立CLI/文件oracle；元数据解析之外观察真实包发现/Apply刷新。两条同源输出互相比不算独立期望。
8. **Evidence and continue**：更新精确映射、状态摘要、报告、demo与下一行为。相同源码已有有效直接结果不重复全套；未执行项保持未执行。

同根因连续三次无进展：保留最小重现，按事件→命中→意图→Controller→模型→历史→运行身份→文件产物逐层定位；重新检查上游JUCE接口/manifest比较/作者hash设计，记录调整再继续。环境或权限不足不是行为Red，不绕过工具限制。

## 5. 真实命令入口与待建工具的区别

以下命令是当前已存在的preset/target入口示例，**本轮文档任务不执行构建**；后续根据实际工具路径和改动范围使用，首次先查询inventory。命令是否成功以真实执行为准：

```powershell
git status --short
ctest --preset windows-debug --show-only=json-v1
cmake --build --preset windows-debug --target audio_editor_application_tests
cmake --build --preset windows-debug --target audioalgo_tests AudioAlgoEditor audio_cli
ctest --preset windows-debug -R 'NCE-|NLC-|NRG-' --no-tests=error --output-on-failure
```

后三个新前缀只有实施并注册后才应收集到测试，当前为待建；零测试必须失败，不把上面命令当现成Green。优先构建本片一个目标，再扩大直接消费者；不要每片都执行上面全部命令。旧测试选择从当前inventory提取精确名称或已核实标签，防止正则误漏。`windows-release`出口使用实际Release目标和新日志，旧Debug结果不代替它。

拟建`tests/connection_library_manifest.json`、`scripts/verify_connection_library.py`及新fixture尚未存在；实现先检查是否已由前片创建。旧`scripts/verify_editor_interaction.py`固定校验原NED范围，不能强塞新ID或仅运行旧脚本就宣布66项通过。新验证器可以复用可靠的日志/inventory设施，但新增拒假通过测试必须先Red，独立检查必需ID集合与验收文档hash。

文档修改只跑链接、ID集合、引用及绑定一致性，不编译产品。若未改旧验收文档，旧manifest acceptance hash保持原字节；不能为了文档入口补充而随意重绑定旧证据。

## 6. 回归防线与交接

每次改画布至少核对原正向拖线、点击建连、选线删除、右拖多选、缩放/平移/负坐标、节点插入/相邻换序及历史；改库至少核对点击/拖动一次创建、payload/epoch、Apply事务刷新与旧版本锁；改描述至少核对Python/原生/作者三个消费者和完整manifest比较。具体直接集合按配套NRG表和最近有效证据选择，公共契约变化再扩OFF/独立消费者。

每片产物位于`build/reports/loops/CNLxxxx/`，包含`loop.md`、原始Red/Green、实际命令与退出码、源码/二进制/文档身份、inventory/JUnit/精确66项状态、独立产物/校验、demo和限制；名字可按现有设施调整，内容不能省略。历史报告不覆盖，不拼接旧数字为本片累计通过；复用列明未受影响范围与理由。

交接先写可用功能、验证范围、剩余风险及下一直接行为；唯一摘要注明专项已启动/未启动和NWS原恢复点，不改历史Green为当前Green。缺D或Release就不写“专项完成”。不因时间或单个用例Green而停在半个生产流程；必要资源不足时继续其他独立切片。

## 7. 启动提示词

> 在当前audioalgo仓库启动“连线编辑与节点库分类”专项。先按AGENTS读取必读文档、唯一当前状态和直接源码/证据，再依次读取doc/plan_connection_editing_and_node_library.md、doc/testing_connection_editing_and_node_library.md、doc/agent_loop_connection_editing_and_node_library.md。核实已有单边断线和正向建连，勿重做历史R0/R2或草稿闭环；从首个未完成CNL行为接续，一次一个可验收行为，严格执行Define→真实Red→最小生产实现→同用例Green→重构→直接回归→独立产物验算→记录并继续。优先交付保留节点断线，再双向建连/原子改接，随后分类元数据/分类搜索筛选/Apply刷新，按66个精确子项及受影响旧验收逐项记录，不删断言、不放宽阈值、不跳过必需测试。保留右键框选、缩放连线、负坐标拖动、库拖放/线上插入、撤销重做、参数/运行/日志/保存重开、Python工作台与精确包锁；保持原生ABI v1和内外manifest完整一致性、NWS/W0冻结hash政策。日常Debug增量与直接集合，稳定出口验证Release及受影响消费者；真实桌面暂停继续有效，相关项记not_verified并推进独立工作。每片同步development_status、精确映射、演示/可重跑命令与build/reports原始证据，常规阶段无需确认；不自动提交/推送/发布、不覆盖用户工程或重启用户应用，未满足全部必要证据不得宣布专项或平台完成。
