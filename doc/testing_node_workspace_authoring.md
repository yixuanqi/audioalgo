---
title: 自定义节点多文件工作区逐项验收方案
status: w0-frozen-w1-partial
updated: 2026-10-26
---

# 自定义节点多文件工作区：测试与通过准则

配套[详细开发方案](plan_node_workspace_implementation.md)。本文新增 **NWS-01～10、每组 a～c 共 30 条待注册必需子项**，用于明确多文件/专业编辑工作流，不替代原 NA 68 子项、PC-01～08、UPN、AR、NED 和 G。编号是验收需求，不表示测试、脚本、预设、示例已存在或通过。唯一现状以[development_status](development_status.md)为准；当前草稿卷根修复的自动化/桌面缺项沿用 PC-01，不把旧结果复制为 NWS 通过。

## 0. W0 冻结产物（2026-10-26，可执行）

| 产物 | 路径 | 作用 |
| --- | --- | --- |
| 契约 ADR | `doc/decisions/0040-node-workspace-schema-v1.md` | 冻结 schema 1 字段、规范化 JSON、运行内容白名单/排除集、`AudioAlgoNodeWorkspace/1` 清单与 hash、路径规则、预算、创建/转换事务 |
| 作者目录夹具 | `tests/fixtures/node_workspace/{python_two_files,python_two_files_v2,native_two_sources,preserve_unknown}/` | 双文件 Python、辅助文件改系数的 v2、双源 C++、未知字段保留 |
| 旧草稿与负例 | `tests/fixtures/node_workspace/legacy/print_a.aadraft`、`invalid/` + `invalid/expectations.json` | 转换来源与 6 类结构化拒绝（含期望诊断代码与所属层） |
| 独立期望工具 | `tests/fixtures/node_workspace/tools/workspace_expectation.py` | 只用 Python 标准库重算规范化 JSON、清单、整树 hash、`.aadraft` 编码；`--write` / `--check` / `--verify <目录> --draft <草稿>` |
| 冻结期望 | `tests/fixtures/node_workspace/expected/workspace_lock.json` | 逐文件字节数/SHA-256、`content_sha256`、规范化声明、入口源 hash、实际被排除但仍存在的文件、legacy 草稿字节与转换期望 |
| 逐项映射 | `tests/node_workspace_manifest.json` | NWS-01.a～10.c 共 30 条：阶段、层次、旧 NA/PC 映射、计划/已绑定测试、状态与剩余条款 |
| fail-closed 校验 | `tests/infrastructure/test_node_workspace_manifest.py`（CTest `NWS-00.contract`） | 校验 30 条 ID 与本文一致、阶段冻结、绑定的文档/ADR/夹具/工具 hash、夹具 `--check`、预算一致；`passed` 必须有已执行且证据存在的测试，`partial` 必须列剩余条款 |

W0 只交付**契约与独立期望**：它本身不产生任何产品 `passed`。当前 W1 首个行为已实现并留下 Red→Green 与独立读取证据，逐项状态仍以 `tests/node_workspace_manifest.json` 为准（NWS-01.a/b/c 为 `partial`，其余 `not_run`，桌面项 `not_verified`）。

## 1. 判定与证据规则

每个 NWS 子项逐一记录 `passed / failed / not_run / not_verified / blocked / skipped / timeout`，仅 `passed` 计通过。一个测试可支持多个子项，但必须在待建 `tests/node_workspace_manifest.json` 中列精确测试名、阶段、层次、独立产物、旧 NA/PC 映射和实际证据；只记录父标签、编译成功、按钮存在、两个同源输出相等、零测试收集或跳过均不能通过。正式 W1～W5 阶段出口要求本阶段及前序全部必需子项通过，当前 Release/真实桌面缺项保持非通过；允许报告 `development_subset`，不能标成正式阶段完成。测试注册、报告和源 hash 与已有 NA 验证器复用，不复制一套更宽松的通过器。

每片保留实际 Red/Green（已存在正确行为时标记复用，不造假 Red）、Debug 目标、直接回归、独立观察、完整命令/工作目录/退出码/JUnit、源/EXE/runner/SDK/节点树/包/工程/音频 hash；失败原始日志不得由后续 Green 覆盖。纯文档变动只验链接、编号、一致性与绑定，不重编产品。OS 桌面/DPI 与独立组件截图分开；暂停桌面时 NWS-10 的桌面项记 `not_verified` 并继续独立开发。

## 2. 固定夹具与独立期望

W0 固定并版本化 3 个**作者目录** fixture：`python_two_files`（`process.py` 调用 `filter.py`，双命名输入）、`python_two_files_v2`（只改辅助文件系数）、`native_two_sources`（`node.cpp` 调用 `filter.cpp` 与头文件）；另留原 `.aadraft` 同公式迁移 fixture 和 schema 负例。夹具源码与独立 float64 oracle 分离，oracle 不 import 被测模块/生成 wrapper。Python 用原 NA 无状态 `y=0.5*a-0.25*b`，v2 改一项系数；C++ 用 `y=-2*a+0.125*b+step*n`，确保不是 Python 实现伪装原生节点。输入各通道非对称、含脉冲及非零值，独立保存原字节/hash。

数值继续使用既有 `abs(actual-expected) <= 2e-6 + 1e-5*abs(expected)`；帧数、通道 ID/顺序、采样率、有效区间和所选输出端口精确相等。基础矩阵沿用 NA：16000/48000 Hz × 1/2/4 通道 × {1,513,16037} 帧，host127；状态连续性 48000 Hz 双通道、16037 帧、host{1,127,512,1024}；C++ 主矩阵两采样率×三通道数、513 帧、host{127,512}。W2 单片先跑最小区分 fixture，稳定/最终出口再跑全矩阵及直接受影响旧用例，不重复无关矩阵。

路径 fixture 含中文、空格、超过 260 字符、卷根、UNC 结构、大小写冲突、`..`、绝对/设备路径、NTFS 链接/重解析点、构建/缓存目录。只在测试自有目录造文件；卷根实际写入和真实 UNC 需要可写测试位置及授权，不能用结构断言冒充实写。静态语法例子包含 emoji、CRLF、缺模块、错误入口；错误坐标按 W0 锁定的 Unicode 单位与原源码字节映射。所有人为崩溃/挂起只在自有 worker/probe 子进程触发。

## 3. 必需子项

### NWS-01 目录契约与旧草稿迁移（W1；纯服务＋真实文件）

- **NWS-01.a**：从应用入口创建两个合法目录，`node.json` schema 1 每个恰有一份权威声明，Python/C++ 必需字段、入口/源码清单及未知字段政策符合 W0 fixture；生成 runtime manifest 与表单节点描述一致。非法版本、重复/非法 type_id、缺入口或不支持能力逐类拒绝，未生成 Available 包。
- **NWS-01.b**：将真实 `.aadraft` 复制转换后，旧文件字节/hash/绑定不变，目录重开保留源码、type_id、端口、参数及身份；转换取消、目标非空、写入/验证/提交失败均保留旧草稿与目标原字节。旧 `.aadraft` 在新版本仍可独立打开、检查、Apply，不能被自动转格式。
- **NWS-01.c**：`tests/**`、`.vscode/**`、构建缓存不进入 runtime 包，其修改不改变运行内容 hash；修改任一 `src/**`、声明或显式资源改变 hash。清单在多次枚举/不同输入顺序下相同，源码原字节与相对路径可独立读回。

### NWS-02 路径、保存与快照事务（W1/W2；真实文件＋故障注入）

- **NWS-02.a**：拒绝 `..`、绝对/设备路径、ADS、大小写冲突、重解析点逃逸/循环、重复规范化路径；检查前后路径替换不读入目录外字节。合法中文/空格/长路径可保存、重开、Check；真实卷根/UNC 写入条件不足时分别标 `not_verified`，不误记通过。
- **NWS-02.b**：注入创建、写入、验证、原子替换、磁盘满与权限失败，目录原有文件字节/hash不变，面板明确未保存且可再次选择目标；恢复副本不会覆盖外部 IDE 刚保存的源码。目标目录非空必须拒绝或明确确认，不删除无关文件。
- **NWS-02.c**：复制第 1 个源文件后并发修改第 2 个，或在枚举/复制/验证间换链接，Check 返回 `source_changed_during_snapshot` 或等价结构化诊断，不产生 Ready/Apply；稳定重试成功后 staging 每个源文件/hash与原始清单一致。使用可控 barrier，不靠偶然 race。

### NWS-03 Python 多文件真实执行（W2；独立 runner＋GUI/CLI 音频）

- **NWS-03.a**：`process.py` 真正调用 `filter.py`，从目录 Check/Apply/库拖入/连线/Run；GUI 与 CLI 各自产物对独立 oracle，不能只比较两个同源输出。删除或改动 `filter.py` 分别给缺模块或新系数结果，不能由旧缓存/宿主路径意外成功。
- **NWS-03.b**：两节点/版本使用相同 Python 模块名但不同实现，各自导入冻结包；多实例状态隔离、按 host 分块连续、重跑/取消后重置。源目录在 Check 后被改动/移走，已应用旧版本仍按旧字节运行；新 Check 得到新版本。
- **NWS-03.c**：每个 Python 文件语法错误给本目录相对文件/行/列，且静态阶段不执行顶层副作用；缺依赖/错误 entrypoint/导入异常只在受控子进程发生，宿主可继续正常 Run。Python OFF 明确不执行 Python 新包，旧 C++/内置节点保留；不自动联网或安装依赖。

### NWS-04 Ready、Apply 与异步身份（W2；服务＋组件＋包文件）

- **NWS-04.a**：Ready 同时锁定工作区 ID/generation、整树运行内容、声明、解释器/runner/wrapper 或 SDK/工具链及 request；改辅助文件、资源、声明或环境后旧 Ready 失效，Apply 明确拒绝。只改排除的作者测试或编辑器缓存不误失效。
- **NWS-04.b**：Apply 仅读冻结候选；重复同内容幂等且只产生一个包/目录条目，同显示版本但不同内容不覆盖旧包。写包、候选注册、索引、内存目录刷新逐阶段注入失败；重启只见旧或新完整版本，旧 Run/包/原始草稿字节保持。
- **NWS-04.c**：检查 A 时修改工作区并发起 B，按 A 成功/失败/取消/日志任意晚到顺序注入；只有与当前 workspace、generation、request、project/session/revision 皆匹配的结果可更新 Ready/日志/按钮。不同工程同 revision、不同目录同 generation 不串结果，关闭面板无悬空回调。

### NWS-05 面板、声明表单与冲突（W3；生产组件＋桌面）

- **NWS-05.a**：生产入口可打开目录，显示真实路径和文件状态；源码/文件列表优先，端口与六类参数由表单编辑同一 `node.json`，高级 JSON 和空日志可折叠。至少两个文件的错误可分别定位；无日志时不占据主要编辑面积。布局、分隔条、状态的可用性需组件及真实桌面证据，不为纯装饰写镜像测试。
- **NWS-05.b**：外部 IDE 改 `node.json` 与面板未保存表单并发时，预期 hash 冲突；比较、重新载入或另存均保留两边字节，取消不清空任何一边。外部改源码使旧 Ready/Apply 禁用；Save 成功、恢复已备份、检查通过、已应用/待重启四类状态互不冒充。
- **NWS-05.c**：快速单文件源码焦点 Ctrl+Z/Y 仅改文本，专业目录模式无第二份源码缓冲；回到画布后工程撤销仍正确。错误/取消/关闭/重新打开后正文和声明不丢，旧节点库与图不变，普通 Save/SaveAs/恢复回归保持。

### NWS-06 外部编辑器协议（W3；实际启动器＋文件/诊断）

- **NWS-06.a**：配置通用编辑器与至少一种实际专业编辑器（VS Code、Visual Studio 或用户选定者），另有系统打开文件夹后备；点击打开真实工程目录，适配器支持时错误可定位实际源文件/行。路径含中文、空格、`&`、引号、反引号或 `$()` 时只是路径参数，不执行额外命令；未找到/启动失败/取消不改目录、节点包或图。
- **NWS-06.b**：外部编辑器写入辅助文件后，面板通过通知或主动刷新显示“磁盘已变”，下一次 Check 重新完整扫描；退出外部进程本身不触发 Check/Apply。未安装 VS Code 仍可用其他编辑器完成 Python/C++ 路径，编辑器设置不进入运行包。
- **NWS-06.c**：AudioAlgo 提供不依赖具体编辑器的同一 Check 命令/请求接口；从应用内和外部终端调用同一目录，返回一致的快照身份与结构化诊断。若附带 VS Code 或其他 IDE 任务模板，必须真实调用该接口、不覆盖已有用户配置，删除模板不影响 Check/Apply；**模板本身不作为必装/必用条件**，不能只以 `Open Folder` 代替核心检查。

### NWS-07 C++ 多文件 SDK 构建（W4；仓库外消费者＋probe＋宿主）

- **NWS-07.a**：`src/node.cpp` 调用 `src/filter.cpp` 和头文件；从发布安装 SDK 与目录声明生成独立 staging 工程，在仓库外编译 DLL，无 JUCE/宿主源码树/宿主链接。包 manifest 与 DLL 描述一致，新增/更新插件前后宿主 EXE hash 一致；实际 Run 音频符合独立 C++ oracle。
- **NWS-07.b**：缺源/头、语法/链接错误、缺 SDK/编译器、错误 ABI/架构、描述不符、probe 崩溃/挂起逐类诊断到源文件/行或确切阶段；失败保留旧版本。基础模式不执行节点目录用户 CMake 脚本或下载依赖，构建取消仅回收自有进程树。
- **NWS-07.c**：新 DLL 仅在独立 probe 成功后原子安装并 PendingRestart；当前宿主继续旧版本，不加载候选/热替换。保存失败或取消不重启，正常重启后使用同 EXE 和精确新包，两个实例状态隔离，旧工程/历史运行仍旧版；不把 probe 存活当正式原生崩溃隔离。

### NWS-08 版本、携带及双格式无回退（W4/W5；IO＋GUI/CLI）

- **NWS-08.a**：新图锁定 v1，目录 Apply v2 不改变原图 revision/音频；显式升级才按现有 NA-09/10 预览并一次事务切换，参数/连接/布局/资源/所选输出及 undo/redo 保留。缺包或改包 hash 结构化拒绝，不自动 latest。
- **NWS-08.b**：项目 Save As/导出携带精确版本、必要资源和输入，搬至中文/长路径并移走测试原目录，GUI/CLI 重开仍对独立 oracle；作者目录本身不用随图隐式复制，除非选择“包含源码”。源/目标失败不改用户原件，不能把项目 Save As 的旧未完成项误标通过。
- **NWS-08.c**：旧 `.aadraft`、项目 schema 1、外部 Python/native manifest 及 SDK 独立消费者原合同保持；Python OFF、editor/runtime OFF 构型按受影响范围构建/运行。旧 UPN、NA、PC 和 AR/NED 直接受影响子项不删除、不降阈值。

### NWS-09 预算、取消和资源归零（W2～W5；真实进程＋心跳）

- **NWS-09.a**：Python 源码恰 128 文件/8 MiB 与越界1分别通过/拒绝；协议1MiB、载荷64MiB、日志UI≤256KiB和完整日志≤16MiB沿用旧边界。新目录含大量缓存/链接时不静默截断、不过预算运行；资源/C++上限按 W0 锁定值同样做边界两侧测试。
- **NWS-09.b**：真实慢导入、挂起 worker、C++构建/probe、日志洪泛取消，保留默认试跑30秒/构建600秒的可配置记录和短 deadline 夹具；Python协作取消≤2秒、总回收≤5秒，构建/probe自有树≤5秒，无关编辑器/编译进程存活。
- **NWS-09.c**：20轮成功/语法错误/取消/外部修改交替后活动任务/进程归零，无逐轮保留；GUI 20ms×10秒心跳 p95≤100ms、max≤500ms，文件扫描、import、编译和算法执行不进入 UI/设备回调。失败后合法节点立即可再 Check/Run。

### NWS-10 真实产品与正式出口（W5；OS 桌面＋Release＋独立读取）

- **NWS-10.a**：真实桌面从新建 Python 目录→外部编辑辅助模块→Check/试跑→Apply→节点库拖入→接 File/Output→调参→Run；制造错误、跳转修复、重开工程及 v2 显式升级，输出音频对独立 oracle。组件事件和截图不能代替 OS 输入，也不能只证明可打开 VS Code。
- **NWS-10.b**：真实桌面 C++ 多文件编译错误→修复→probe→PendingRestart→保存与正常重启→库拖入/Run，新旧 DLL、宿主 EXE hash 和 C++ 数值独立核验；100/150/200% DPI 与逻辑 1100×700、1440×900，中文/长路径、按钮/状态/诊断不遮挡。不得未经授权修改系统 DPI；桌面暂停时本项 `not_verified`。
- **NWS-10.c**：当前源码正式 Release、GUI/CLI、发布 SDK 仓库外消费、便携重开、受影响旧验收、精确 manifest/证据全部通过且无必需 failed/skipped/not_run/not_verified/timeout 才可写“多文件工作区通过”。这仍不等于整个 R6/G6、NA/PC/UPN 全平台完成。

## 4. 阶段映射和最小执行集合

| 阶段 | 新增必需子项 | 当前阶段最小直接回归 | 正式出口额外条件 |
| --- | --- | --- | --- |
| W0 | 固定 fixture/字段/预算；无产品 passed | 文档链接、ID、schema 样例、独立期望一致性 | 无源码能力声明 |
| W1 | NWS-01.a～c、02.a/b | 既有 `DraftFiles`、NA-01 Save/SaveAs/恢复/关闭、PC-01 路径 | 真实目录读回、错误后原字节保留 |
| W2 | NWS-02.c、03.a～c、04.a～c、09.a | Python catalog/check/run、NA-03～08、PC Print/日志身份 | GUI/CLI 独立音频、旧版冻结、源码/环境身份 |
| W3 | NWS-05.a～c、06.a～c | 面板快捷键/关闭、节点库/Inspector、NI 分隔条、PC 日志 | 外部真实文件冲突、诊断跳转与桌面缺项单列 |
| W4 | NWS-07.a～c、08.a、09.b | PC-07、NA-12/13、UPN 独立 SDK、旧 v1 DLL | 新 DLL 真正运行、同宿主 EXE、重启证据 |
| W5 | NWS-08.b/c、09.c、10.a～c | NA-15/17、PC-08、AR/NED/图交互、正式矩阵 | 全部累计 NWS 与受影响旧项、Release/桌面通过 |

Python OFF 与 runtime/editor OFF 在公共契约变化阶段增量验证，不每片重复完整矩阵；公共 SDK/构建变更扩一个独立消费者，最终 Release 集中全覆盖。同一源码已有有效结果不重复跑；文档变更不编译产品。每阶段提交可执行演示项目、真实程序路径、测试名、命令和报告，缺工具链/桌面资源记真实状态并继续不依赖它们的切片。
