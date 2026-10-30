---
title: 连线编辑与节点库分类修改方案
status: ready-for-implementation
updated: 2026-10-28
---

# 连线编辑与节点库分类修改方案

## 1. 任务与文档状态

落实2026-10-28用户截图反馈：①保留节点时断开连接，端口拖拽建连、断连与改接；②Node Library按成熟音频功能体系分类。本文件与[逐项测试通过准则](testing_connection_editing_and_node_library.md)、[Agent TDD规程及启动提示词](agent_loop_connection_editing_and_node_library.md)配套。

**本次只交付文档，没有实现或验证新增产品能力。** 新接口、脚本、fixture和测试名均是待建目标，恢复时先核实存在性。实现状态以[唯一当前摘要](development_status.md)及对应源码/原始报告为准，不照搬旧NI、NA、PC、NWS的历史通过数字。

后续收到启动本专项的指令时，以本专项为当前有限范围，逐行为完成后回到原NWS→C++多文件独立构建/probe/待重启→W5主线；本次文档任务不自动启动实现，不取消原AR/G/NED/UPN/NA/PC/NWS缺项。遵守[交付收口约束](plan_delivery_convergence.md)、根AGENTS、真实桌面暂停、不自动提交/推送/发布、不覆盖用户工程/音频的要求。常规实现和验证选择无需反复确认。

## 2. 当前事实与根因边界

2026-10-28只读分析基线HEAD：`49d7c93`。下表是源码事实，不是对用户运行窗口或二进制版本的确认。

| 能力 | 实际入口 | 结论 |
| --- | --- | --- |
| 选中边后Delete/Backspace只删边 | `apps/editor/EditorGraphCanvas.h`的`delete_selected`；`EditorController::disconnect` | 已实现，先保留和复核，不人为制造Red |
| 输出端口拖到输入端口、输出点击后点击输入 | `mouseDown/mouseDrag/mouseUp/connect_to` | 有路径；实际不能操作时必须核对构建身份、命中和事件，不能仅凭源码宣布正常 |
| 从输入起拖、拖开已有输入、已有线端点改接 | `EditorNodeDragDrop.h::PortConnectionGesture`只保存source | 缺少手势语义和对应原子命令 |
| 节点整体断开但保留 | 现有Controller仅单边断开入口 | 需批量原子操作与明显入口 |
| 分类/搜索 | `EditorNodeLibrary.h`逐条生成按钮；`EditorNodeCreation.h::NodeLibraryEntry`平铺 | 没有分类/搜索模型；`NodeDescriptor`无专门浏览分类字段 |
| 原生插件身份校验 | `libs/audio_native_plugins/NativePlugins.cpp` | 外部manifest与DLL导出完整JSON比较；禁止为分类跳过此校验 |

截图中的PrintA输入已有连接。当前新连线命令拒绝占用输入，不能据此判断合法空闲端口的建连也失败；两种场景分别重现。已有测试中手势类调用不等于真实画布事件，更不等于真实桌面拖拽。

## 3. 范围与不回退原则

本专项必需：明确断线入口、节点/端口批量断线、双向起拖建连、已有边端点改接、取消/失败恢复、功能分类、搜索/筛选、Apply后刷新及旧包兼容。

不增加：DSP算法、新运行拓扑能力、隐式类型转换、通用IDE、插件热卸载、收藏/最近使用、任意自定义分类树、自动上游下游旁路。特别是“断开节点全部连接”不等于“从链中取出并旁路”；后者不在本次必需实现内。现有删除节点的合法线性旁路必须保留，不能误用新的断线命令改变原删除语义。

ProjectModel继续为唯一工程事实，JUCE图为运行投影。浏览元数据不能决定工厂、后端、端口兼容或运行能力，通用画布/调度器不增加算法名分支。纯算法库不能因此依赖GUI、工程或插件浏览器。

## 4. 连线交互契约

### 4.1 操作表

| 初态/操作 | 松开或点击结果 |
| --- | --- |
| 输出点击→输入点击 | 保留原建连方式；已占用输入明确拒绝，不静默替换 |
| 空闲输出→兼容空闲输入拖动 | 新增边 |
| 空闲输入→兼容输出拖动 | 新增相同方向的规范化边，模型始终output→input |
| 已有分支的输出端口再起拖 | 新建分支，保留原有边，即使目前只有一条出边也不隐式抓走它 |
| 没有待连接手势时，从已连接输入端口起拖 | 改接该输入端点，保留原source；按下时不改模型 |
| 已选中边的源/目标端点手柄起拖 | 只改该条边的对应端点；多分支时不猜测要改哪条 |
| 改接到合法空闲端口 | 一次替换旧边，保留其他所有边 |
| 改接释放在画布可编辑空白区域 | 一次断开原边，保留节点；预览明确提示“松开以断开” |
| 新建线释放在空白 | 取消，不改图 |
| 改接释放到原端口 | unchanged；不新增历史，不清空redo |
| 方向/类型不合法、输入已占用、自连/成环 | 拒绝、显示具体原因、原图不变；不以非法端口或节点本体充当“空白” |
| Esc、画布外释放、失焦、视图隐藏、工程替换、目标失效 | 取消预览；同工程未变化时原图逐字段不变；工程已变化时不得恢复旧快照覆盖新工程 |

画布空白仅指有效viewport中的未被节点、端口、边或浮层占据区域；释放在其他边上不自动执行节点插入或断线。若端口允许多条入边但无法唯一确定边，应先选择具体边的手柄，本专项不扩展现有输入基数能力。

### 4.2 手势与坐标

区分Idle、ArmedCreate、CreatingConnection、ArmedRewire、RewiringConnection、MovingNode、Marquee、Panning、LibraryDrag、PendingResource；名称可调整，状态职责不得合并成“任何拖动都删线”。从事件局部逻辑坐标量起拖位移：**>6 px才进入拖动，<=6 px为点击**，不按模型坐标或系统DPI重复缩放。手势期间不能同时移动节点、框选和连线。

已连接输入无拖动的点击可选中它的唯一边，不断线；已有输出点击建连模式继续有效。预览只持值类型的边四元组、被移动端、起始epoch/revision和鼠标坐标，不保存vector元素地址。按下至松开不修改项目/dirty/revision/历史。原边可在视图中淡化并绘制ghost，模型保留到提交。

选中边显示端点手柄。节点/端口优先于背后边；手柄仅在该边已选中时可命中，输出原有新建分支语义保留：可在节点外沿已选线的端点邻域布置手柄，避免与普通socket命中冲突。实现前用实际几何fixture固定优先级和命中区域；端口沿用现有放大热区，边沿用<=8逻辑px阈值，不缩小可用区域。

缩放、平移、绘制、命中和预览使用现有共享坐标变换。缩放时鼠标锚点和当前推迟缩放策略保留；负坐标节点仍可拖动和连接。

### 4.3 可发现入口与右键兼容

- 边：点击高亮，Delete/Backspace和菜单“断开连接”均复用Controller命令。
- 端口：菜单列出具体边及“断开此端口全部连接”；没有边时禁用。输出端口不因一次点击就全部断开。
- 节点：Inspector提供“断开全部连接”，菜单同义入口；操作不删除节点、不移动节点、不改变参数/资源/精确包锁/选定输出。
- 多选节点：断开所选节点关联边的并集，内部边只删一次；仍为一个可撤销操作。原Delete selected多选删除保持原义。
- 右键按下只记录候选：位移>6 px进入原右键框选，释放不弹菜单；<=6 px点击在命中对象打开菜单。空白右击保留原选择且不编辑；Esc取消菜单/框选。选中集合内右击节点不把多选缩成一个，集合外右击明确将菜单目标切为该节点；延迟菜单执行再次校验工程/对象身份。
- Tooltip/状态栏说明输入拖开、输出新分支、手柄改接、空白断线与Esc取消；错误同时有文字反馈，不仅靠颜色。

### 4.4 应用命令与原子性

在`audio_editor_application`扩展现有命令/历史，建议值类型接口为`ReconnectConnection(old,new)`、`DisconnectPort(node,port)`、`DisconnectNodes(ids)`，名字不是已存在API。连线四元组为身份，不以易变数组下标跨事件/菜单保存选择。拒绝缺边、重复/歧义边、缺节点/端口、旧epoch/revision、revision耗尽及并发参数手势；诊断带code/node/port和相关端点。

改接先复制候选，精确删除一条旧边，再加入新边并进行**编辑结构校验**，最后一次提交；不得先调用现有disconnect再connect形成两条历史或短暂半图。断端口/节点同样合并为一次事务。成功每次revision+1、dirty、历史+1；undo/redo恢复正确边集且revision继续单调递增；no-op/拒绝/取消不改变模型、布局、dirty、undo/redo。批量节点ID按集合规范化，不重复删除边。

断线允许形成不完整工程并保存重开；Run仍严格校验缺输入/资源/输出，不自动忽略孤立节点，不静默旁路。拓扑改变使当前结果失效；旧运行的迟到消息由现有epoch/revision/run身份门禁拒绝，Undo也不能用旧revision冒充新运行。仅浏览/筛选/折叠节点库不使工程dirty或结果失效。

## 5. 节点功能分类

### 5.1 标准依据和本项目扩展

参考[VST3官方subCategories](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/group__plugType.html)的音频功能术语、[LV2 Core](https://lv2plug.in/ns/lv2core)的类层次、[CLAP官方features](https://github.com/free-audio/clap/blob/main/include/clap/plugin-features.h)的多标签方式。这是AudioAlgo的本地浏览分类，不宣称是三种插件格式的完整互换标准，也不要求加入VST/LV2/CLAP执行后端。

| 稳定主分类ID | 中文名称 | 示例子类/关键词（不表示已实现算法） |
| --- | --- | --- |
| `audio.io` | 输入与输出 | file、generator、output、export |
| `audio.routing` | 路由与格式转换 | mixer、channel-mapping、resampling |
| `audio.dynamics` | 电平与动态 | gain、normalization、compressor、limiter |
| `audio.filters` | 滤波与均衡 | fir、iir、eq |
| `audio.effects` | 延时与音效 | delay、reverb、modulation、distortion、pitch |
| `audio.restoration` | 语音增强与修复 | denoise、aec、dereverb |
| `audio.spatial` | 空间音频与阵列 | beamforming、localization、spatialization |
| `audio.analysis` | 分析与评估 | spectrum、loudness、features、metrics |
| `math` | 数学与几何 | scalar、matrix、geometry |
| `utility` | 工具与调试 | logging、inspection |
| `uncategorized` | 未分类（兼容兜底） | 无分类、未知分类、不可用的浏览元数据 |

树最多两级，单一主分类/可选子类，tags支持交叉用途。固定顺序按表；未分类最后；不显示空类别，不创建虚假节点占位。现有File/Output、Gain、Mixer/ChannelSwap由各自描述/注册声明分类，不由View推断名字。PrintA等旧用户包无元数据时保持未分类；不因名字含Print就修改用户包或强行归类。

### 5.2 浏览元数据契约（实现前冻结独立fixture）

拟在节点描述JSON增加**可选**`browser`对象，由公共解析规则投影成不依赖GUI的浏览值类型；内置节点注册提供同样值类型。建议形式：

```json
{"browser":{"schema_version":1,"category_id":"audio.dynamics","subcategory_id":"gain","tags":["gain","volume","增益"],"description":"调整音频电平"}}
```

`browser`缺省等价于未分类、空标签/描述。category_id使用上表稳定ID；subcategory_id是该主类下已登记的短ID，缺省不分子类。category/subcategory各<=96 UTF-8字节，tags<=32项、每项非空且<=64 UTF-8字节，description<=2048 UTF-8字节；边界含等号。原包/JSON总预算、UTF-8和重复键规则仍优先，不因浏览字段扩大预算。合法tags按原顺序去重，仅影响展示/检索。

浏览字段类型错误、超预算、未知browser版本：浏览信息整体退为未分类/空并给非致命诊断，不让原本可运行的有效节点消失。未知主类同样退未分类，但保留合法tags/描述；未知子类退到已知主类。未知字段按原源格式政策保留，不能因新增解析删除作者文件字段。非法执行描述、包签名/身份不符仍按旧规则拒绝，不能被此降级政策掩盖。

来源（内置/用户/第三方）及语言（C++/Python）从可信注册/包来源派生，不由任意browser字段覆盖；“自定义”作为筛选维度，不作为功能类别。多用途节点选主类、其余用途放tags，禁止从代码语言推断功能。

### 5.3 跨格式、ABI与不可变内容

1. 内置、外部Python、原生DLL、单文件作者、目录工程Check/Apply及目录刷新均使用相同浏览规则，不能只改View或漏掉Python描述标准化中的可选字段。
2. 保留`AA_NODE_ABI_V1`结构布局、函数签名和可选日志扩展；旧无browser DLL直接部署后仍能启动加载，无需重编旧DLL。只扩展host值类型时按消费者范围重建，不等于可改C ABI。
3. 新原生包若声明browser，其DLL导出manifest与外部manifest仍须完整一致；只改安装目录manifest会触发原有不一致诊断。不得strip browser后比较，不得补丁旧DLL或替用户重写安装包。
4. 单文件作者沿既有声明JSON承载可选browser，不另改.aadraft封装/迁移规则；目录工程沿唯一node.json的node段承载。**不借本专项交付完整声明表单**：首版可通过已有声明JSON入口/目录外部编辑声明分类，浏览展示为必需；目录表单仍按NWS主线挂账。
5. node.json/manifest字节变化继续改变既有内容/包hash；分类不参与DSP运算不等于分类不参与包身份。更新作者声明须撤销旧Ready、重新Check/Apply为新不可变内容；旧工程精确锁定的旧包及原字节/音频结果保留。不得让旧快照借“仅浏览字段变化”绕过校验。
6. ADR0040/0041的冻结fixture、整树hash、作者身份及默认发布版本政策保持；新增夹具不改旧夹具/lock/oracle。没有分类的旧.aaproj/.aadraft/工作区/插件继续可用，不自动迁移或保存。

## 6. 节点库展示与刷新

顶部保留New custom node、Open draft、Open node project、Recover draft、Upgrade project的全部有效入口及原动作；可将次要入口收进有文字的菜单，但不可移除、改成另一动作或改变Component ID导致原生产定位失效。搜索框与可折叠分类使用紧凑布局，在当前最小窗口及1100/1440逻辑宽度可操作，长中文标签不覆盖按钮/滚动条。

搜索范围为display_name、type_id、description、tags和分类显示名；UTF-8文本去首尾空白，按空白拆词，所有词AND匹配，每词可命中任意字段，拉丁字母忽略大小写，不引入模糊匹配/pinyin。搜索时忽略树折叠以展示所有匹配项，清空后恢复原折叠状态；零结果显示明确说明与清除入口。

来源和语言筛选均提供All；不同维度AND，同一维度仅单选；与搜索共同作用。类别固定顺序；同类按display_name的不区分大小写显示键排序，同键按type_id的UTF-8字节序稳定排序；测试用独立固定期望验证，不调用同一排序函数生成期望。每个当前可创建type_id只展示一个默认版本，旧工程仍按精确包锁解析，不在库里混出重复版本。

分类/搜索只处理已解析的只读目录快照；不得在UI线程扫描源码树、加载DLL或import用户Python来补分类。磁盘/包读取沿现有worker服务，遵守既有PERF-01交互预算，不因分类新增同步I/O或放宽原阈值。

条目点击/拖拽继续沿现有type_id/versioned payload和epoch协议。分类/搜索变化仅改变浏览；已有拖拽持稳定payload，不引用销毁的按钮/descriptor地址。刷新中可安全取消拖拽并明确保持工程不变，禁止错建另一节点或一次手势创建两次。Apply更新使用prepare/publish事务，过滤后选取新版本；保留仍有效的搜索、筛选、折叠、选中项和滚动位置并钳位范围。刷新失败保持旧库和旧默认版本可用。

## 7. 切片、验收与收口

| 顺序 | 用户可观察出口 | 测试归属 |
| --- | --- | --- |
| CNL0001 | 明确单边/端口/节点断线入口，保留节点且一次撤销；同片补最小66项映射与拒假通过检查 | NCE-01/02/07，NRG-01/04 |
| CNL0002 | 输入/输出双向拖线，原点击建连保留 | NCE-03/06 |
| CNL0003 | 端点改接、空白断线、取消与占用拒绝 | NCE-04/05/08/09/10，NRG-02 |
| CNL0004 | 元数据贯穿内置/Python/原生/作者路径，旧包兼容，声明与身份规则先冻结 | NLC-01/02/07/08，NRG-03 |
| CNL0005 | 分类树、搜索、来源/语言筛选可用 | NLC-03/04/05 |
| CNL0006 | Apply刷新后保持浏览状态、点击/拖放/线上插入和精确版本 | NLC-06，NRG-01/03 |
| CNL0007 | 当前最终源码直接累计回归、独立GUI/CLI产物、Release出口及桌面证据收口 | 全66项和受影响旧集合 |

编号恢复前查`build/reports/loops/`，已占用则递增，不覆写。切片可以按直接依赖细拆，不合并多根因大改。第一个可用功能与最小证据映射同片交付，不能只建验证器长期停在基础设施。

新增22组×a/b/c共**66必需子项**，全部定义见配套测试文档；单列于既有验收，不替代NED/AR或扩大旧通过范围。专项正式完成需全部必需自动化/桌面/数值证据passed且受影响旧回归无失败；真实桌面仍暂停时如实not_verified，继续可独立后端/组件切片，不自动恢复桌面、不把暂停当通过。

日常Debug增量+目标/直接回归；公共元数据/构建改变才扩相关消费者；稳定出口和最终Release跑累计集合，不重复无变更全矩阵。每片更新唯一摘要、精确映射、演示项目/重跑命令、源码身份与`build/reports/loops/CNLxxxx/`原始报告。本文文档状态不提升任何产品验收状态。
