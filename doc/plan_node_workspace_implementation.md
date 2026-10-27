---
title: 自定义节点多文件工作区详细开发方案
status: w2-python-loop-partial
updated: 2026-10-27
---

# 自定义节点多文件工作区：详细开发方案

配套[选型概览](plan_node_development_workspace.md)、[逐项验收](testing_node_workspace_authoring.md)、[既有工作台方案](plan_node_authoring_workbench.md)和[唯一当前摘要](development_status.md)。

## 0. 实施现状（2026-10-27，正式出口未通过）

- **W0 已冻结**：`doc/decisions/0040-node-workspace-schema-v1.md` 固定作者工作区 schema 1、规范化 JSON、运行内容白名单/排除集、`AudioAlgoNodeWorkspace/1` 清单与整树 hash、路径与预算、创建/转换事务；`tests/fixtures/node_workspace/` 提供 4 个作者目录夹具、legacy `.aadraft`、6 类非法负例、只用标准库实现的独立期望工具与冻结 `workspace_lock.json`；`tests/node_workspace_manifest.json` 登记 NWS-01.a～10.c 并绑定文档/ADR/夹具/工具 hash，CTest `NWS-00.contract` 做 fail-closed 校验。
- **W1 创建/转换与回收已有实现**：生产入口创建、转换和重开目录；原 .aadraft 字节保持。NWS0002 修复 passed 清单状态矛盾与嵌套 src 失败残留，均有真实 Red/Green。W1 正式出口仍未通过，剩余 C++ 生产入口/完整拒绝矩阵、reparse/事务和外部文件系统证据。
- **W2 Python 最小生产闭环已有自动化证据**：稳定整树快照、并发修改拒绝、全 Python 文件静态检查、独立 runner 调用辅助模块、Ready/Apply 不可变版本、库刷新、生产库 drop/端口连线/Inspector/画布 Run 和独立 CLI 已执行；五份 WAV 的独立 oracle、实际 helper SHA 与树身份已核验，v2 发布后保存的 v1 工程仍运行原包。`audio_cli check-node <directory> <work-directory>` 调用同一检查服务。NWS0005 修复内部 wrapper 挤占128文件配额；NWS0006 补源码旁数据和显式资源冻结/试跑。极长清单/大声明与极限源码组合、完整故障/状态矩阵仍未收口，不能写 W2 passed。
- **W3 必需可见基础已随闭环交付**：创建成功直接进入工程界面，目录/文件列表优先、只读磁盘预览、后台/主动刷新、通用 .exe 编辑器选择与系统打开文件夹后备、诊断相对文件/行/列和预览跳转；高级定义与日志可折叠。目录会话 source 为空，不写第二份 .aadraft/恢复源码。声明表单仍只读、冲突比较及编辑器真实启动尚未验收。
- 原始证据：`build/reports/loops/NWS0001-workspace-create/`、`NWS0002-review-fixes/`、`NWS0003-python-tree/`。精确状态以 manifest 和 development_status 为准；W1/W2/W3 正式出口均未通过。真实桌面/原生选择器/DPI 继续 `not_verified`，未启动或重启用户窗口。

本文其余部分仍是完整目标设计；W4/C++ 多文件独立工作台构建、probe、待重启与 W5 尚未交付。旧 .aadraft 单文件编辑/恢复/应用路径保留。

## 1. 目标、决策与边界

用户能从 AudioAlgo 新建或转换一个自定义节点目录，在任意专业编辑器里拆分 Python 模块或 C++ 源/头文件；回到 AudioAlgo 后可检查、试跑、Apply、从节点库加入画布并 Run。编辑器只是代码前端，**节点目录及其声明是唯一草稿事实来源**，运行从冻结的不可变包读取。旧 `.aadraft` 仍可单文件编辑、恢复和应用；迁移是显式、可撤销的复制，不静默改写旧文件。

推荐外部 IDE 作为复杂代码的主编辑路径，首个适配候选是 VS Code，C++ 可使用 Visual Studio；必须有通用“选择编辑器/打开文件夹”后备路径。AudioAlgo 内置面板仍是简单节点的可用入口，承担端口/参数表单、检查、诊断、版本和试听。并不把 VS Code 作为产品运行依赖或预先确定的唯一编辑器。Monaco/WebView 仅在本方案稳定、真实工作流反馈证明窗口切换明显妨碍使用时单独评估；JUCE 官方 [WebBrowserComponent](https://docs.juce.com/develop/classjuce_1_1WebBrowserComponent.html) 在 Windows 需要明确 WebView2 配置，嵌入一个代码控件并不能替代文件/构建/调试契约。

不在本片实现 Python 在线低延迟链、任意 pip 依赖自动安装、任意 CMake 工程受托构建、C++ 热替换/热卸载或原生正式运行的崩溃隔离。Python 仍在独立 runner；C++ 仍走发布 Node SDK 的 C ABI v1，独立构建 DLL，probe 后待宿主重启。已安装包与运行产物不可写；用户源码/音频不被覆盖。

## 2. 用户流程与可见状态

1. **新建**：在节点库选择 Python/C++；填名称、唯一 type_id、端口、参数及模板；选择新目录，先展示将创建的文件清单和目标冲突，再原子创建。路径未选或创建失败时留在未保存草稿，不出现“已保存”。
2. **转换**：从已保存 `.aadraft` 选择“转换为节点工程”。仅复制内容至新目录，保留 type_id、声明中未知字段和原字节；成功后打开新目录，原 `.aadraft` 原样留存。转换失败/取消仍绑定原草稿。转换结果的音频和节点描述与原单文件版本一致。
3. **编辑**：专业模式显示工程路径、文件列表、声明表单、检查/诊断与当前版本；“打开代码工程”交给所选编辑器。代码文件在磁盘上是权威；面板不能同时握有另一份待保存的源码缓冲。表单若有未保存值，则必须先写 `node.json` 或明确保留为未保存，并对外部修改给比较/重载/另存选择。
4. **检查与应用**：Check 先稳定复制、计算输入快照身份，再做语法、契约和真实试跑；Ready 只绑定该快照。磁盘内容、声明、依赖或环境变更立刻失效；Apply 从 Ready 冻结包原子提交，不重新读取工作目录。Python 成功后刷新节点库；C++ 成功后显示 PendingRestart，保存后正常重启现有宿主才激活。
5. **重开与使用**：工程内节点锁定精确内容版本。重开目录看到真实文件状态；重开图则从已安装精确包运行，不从开发工作目录或 latest 悄悄取代码。缺文件/缺环境/坏 hash 时保留图和诊断，不能改为 Gain 或旧版本。

建议布局：顶栏为名称、路径、`未保存/磁盘已变/检查中/Ready/已应用/待重启` 与动作；左侧小文件树和问题数；中央快速源码或当前文件/诊断摘要；右侧端口与参数表单；底部日志默认折叠，仅错误和主动查看时展开。高级 JSON 默认折叠。区域可拖动，最小逻辑尺寸与现有工作台约束一致。`Save`、`Check`、`Apply` 三个状态分开，不能用“Available in node library”遮盖文件未保存。

## 3. 权威目录格式与兼容

初版每目录**一个节点**，一个根 `node.json`。这是新的**作者工作区 schema 1**，不等于 `.aaproj` schema 2，也不等于生成包的 Python/native `manifest.json` schema 1。`node.json` 仅有一份可编辑的 SDK 节点声明；`manifest.json`、Python wrapper、C++构建文件和 DLL 均由冻结输入生成，不能作为第二份可编辑定义。正式字段在 W0 固定 fixture 后才写生产解析器；至少包含：`workspace_schema`、`language`、`node`（现有 SDK 节点描述原字段）、`python.entrypoint` 或 `native.sources/include_dirs`、显式 `resources`。未知的 `node` 字段按现有兼容政策保留，未知工作区控制字段拒绝或原样无损保留须在 W0 fixture 中选定，不允许静默删除。例如 Python 目标声明的外壳如下，省略的 `node` 属性仍沿用当前 SDK 校验；这不是当前已支持的导入格式：

```json
{
  "workspace_schema": 1,
  "language": "python",
  "node": { "type_id": "user.print_a", "version": "1.0.0", "display_name": "Print A" },
  "python": { "source_root": "src", "entrypoint": "print_a.process:process" },
  "resources": []
}
```

```text
PrintA/
  node.json                  # 工作区唯一声明；Python入口 print_a.process:process
  src/print_a/__init__.py
  src/print_a/process.py
  src/print_a/filter.py
  tests/test_process.py      # 作者测试；不进入运行包
```

```text
MyFilter/
  node.json                  # 显式列出 src/node.cpp、src/filter.cpp、include/
  include/my_filter/filter.h
  src/filter.cpp
  src/node.cpp
  tests/filter_test.cpp      # 作者测试；不进入运行包
```

Python 入口使用 `src` 下相对模块名＋函数名，不能是绝对文件路径；同目录的辅助模块按显式源码树一起冻结，在独立 runner 的包根加载。不同节点或不同版本的同名模块不得串入；不从宿主 `sys.path`、当前工作目录或其他用户包碰运气。文件名和模块名边界在 W0 fixture 固定。C++ 基础模式只接受 `node.json` 指定的源/头文件、资源和发布 SDK；在 staging 生成 CMake 项目和薄 ABI 适配，不执行节点目录中的自定义 `CMakeLists.txt`。需要任意 CMake/第三方链接的高级用户继续使用外部 SDK 插件工作流，后续可另立“受信工程导入”契约。

Runtime 源码白名单、排序及规范化必须固定并可重现：至少覆盖 `node.json`、`src/**`、C++ `include/**` 和显式资源；`tests/**`、`.git/**`、`.vscode/**`、编辑器缓存、staging 与构建输出不打入运行包，也不影响 Ready 的**运行内容 hash**。文件清单记录相对路径、字节数和 SHA-256，按 UTF-8 规范化路径排序后计算整树身份；保持原源码字节（不改换行/编码）。解释器、runner、wrapper、SDK/ABI/编译工具链及锁定依赖另进入环境身份。源码预算复用既有 Python 128 文件/8 MiB、协议 1 MiB 与任务载荷 64 MiB；资源和 C++ 输入预算在 W0 与既有包限额核对后明确锁定，不能暗中放宽。禁止 `..`、绝对/设备路径、ADS、NTFS 重解析点/链接逃逸、Windows 大小写冲突、重复规范化路径及循环；真实读取与冻结均需复核，避免检查后路径替换。

`.aadraft` 保持原格式与已保存字节；文件夹工程使用新对象（建议 `WorkspaceDraft`）或带明确变体的文档模型，不向旧 `Draft::source` 拼接多个文件、不把整个目录序列化到 `.aadraft`。转换和旧版打开必须有独立 fixture。编辑工作区路径只属于作者侧；已发布包内容/项目相对携带与工程精确版本锁沿用现有策略。

## 4. 服务和事务边界

建议分四个纯服务接口，名称是设计目标，不代表当前存在：`WorkspaceStore`（创建/列举/外部变更/表单原子保存）、`WorkspaceSnapshot`（安全文件清单与冻结）、`WorkspaceChecks`（调用既有 PythonAuthoring/runner 或 SDK 构建/probe）、`EditorLauncher`（配置和启动外部编辑器）。`NodeAuthoringPanel` 只呈现状态和意图；纯算法库不依赖 JUCE、文件树或外部 IDE。

目录创建采用独立临时目录、验证及同卷提交；拒绝覆盖非空目标。`node.json` 由应用表单保存时使用预期原字节 hash、临时写/验证/原子替换；外部 IDE 修改使预期 hash 不符时不覆盖，对比/重载/另存明确展示。源码文件由外部 IDE 保存，不需要应用再提供第二个 Save。恢复副本仅保存 AudioAlgo 内部尚未落盘的表单或快速编辑内容，不能悄悄还原覆盖专业编辑器刚保存的源码。监控可以防抖用于 UI 提示，但 Check 前必须重新枚举并核对全部相关文件，不能把文件系统通知当正确性证据。

Check 在 worker 上生成 `snapshot_id`（workspace_id、generation、运行内容 hash、声明 hash、环境身份、request_id），复制所有输入到专有 staging，并在复制前后校验清单；若期间修改，返回 `source_changed_during_snapshot`，不继续执行/Apply。Python 对每个 `.py` 先静态解析，定位 `file:line:column`，只在受控子进程导入/运行；外部依赖缺失明确报告，不自动下载。C++ 只从冻结目录和发布 SDK 在自有构建目录生成 DLL，编译诊断映射回原相对文件；probe 在独立子进程进行。日志及取消/超时沿用当前 NA/PC 预算，不能在 GUI/设备回调中编译、import、扫描整个树或读取大文件。

检查成功产生 `ReadyPackage` 的工作区变体/通用快照引用。接入现有 `prepare_apply`/原子索引时需要先为旧单文件和新目录分别写契约测试：Apply 只读 Ready 的不可变候选及其 hash，不再碰开发目录；同内容幂等，不同内容即使显示版本未变也拒绝覆盖旧包。异步 UI 回填仍比较完整 workspace/draft、generation、request、项目/session/revision 身份。项目只是精确锁包，目录变更本身不增加工程 revision；显式节点升级才按现有 controller 事务增加一次。运行报告应保存整树内容身份、工具/runner/SDK 身份与包 hash，不能只记录首文件。

## 5. 编辑器集成协议

配置为 `executable`＋受控参数模板（工程目录、相对文件、1-based 行列），每次以结构化参数创建进程，不拼接用户路径到 shell。优先系统关联/用户自选编辑器；VS Code 可适配官方 [`code <folder>` 和 `code -g file:line:column`](https://code.visualstudio.com/docs/configure/command-line)，Windows 的 `.cmd` 启动器需单独验证引号与元字符，不可直接将任意路径拼入 `cmd /c`。Visual Studio/其他 IDE 用各自适配器或保底只打开工程目录。找不到编辑器、启动失败或用户取消时工作区仍可 Check/Apply，返回清晰诊断，不修改目录/包/图。

诊断规范统一为工作区内相对路径、1-based 行/列和明确的 Unicode 列单位；Python/编译器字节列转换到 JUCE/编辑器所需坐标需用有 emoji/中文及 CRLF 的 fixture 检验，不能猜偏移。诊断文件越界或已删除时显示原始错误并禁用跳转。外部编辑器修改后不因进程退出自动 Apply；文件通知只刷新状态，用户明确 Check/Apply。可生成本地 VS Code tasks 模板调用同一 CLI 检查入口，但它不是包运行依赖，且生成失败不能影响核心流程。若现有 CLI 尚无同等检查命令，应在对应阶段实现并验收，而不是写不可运行的 task 模板。

## 6. 分片、依赖与交付

| 阶段 | 唯一可验收增量 | 先决条件与主要改动 | 必交付 |
| --- | --- | --- | --- |
| W0 契约冻结 | schema/文件清单/迁移样例与独立期望固定 | 核对现有 Python/SDK/包预算，现有 PC/NA 当前状态；不重做已完成保存缺陷 | 版本化 fixture、ADR/本方案及验收映射；纯文档检查，不宣称功能通过 |
| W1 目录草稿 | 新建/转换/关闭重开文件夹，原 `.aadraft` 不变 | `WorkspaceStore`、路径/事务、旧面板入口；先 Red，真实文件读回 | Python 双文件和 C++ 多源码样例目录、故障注入、Debug 目标、报告 |
| W2 Python 多文件闭环 | 辅助模块真实参与 File→节点→Output 音频；Check/Ready/Apply/版本冻结 | `WorkspaceSnapshot`、runner 导入/错误定位、既有 PythonAuthoring 桥接 | 独立数值 oracle、旧版本/新版本和 GUI/CLI 产物、证据 |
| W3 编辑体验 | 声明表单、状态减负、外部工程打开/错误跳转/冲突恢复 | W1/W2 的单一目录事实来源；保留快速单文件 | 组件和真实桌面交互记录（暂停时 not_verified）、通用编辑器与 VS Code 场景 |
| W4 C++ 多文件闭环 | 发布 SDK 独立构建、probe、待重启、精确包运行 | 现有 PC-07/NA-12/13 的基本构建激活先有有效实现与证据；不重编宿主 | 仓库外 SDK 消费者、真实新 DLL/EXE hash、独立公式和重启证据 |
| W5 累计出口 | 可移植重开、旧功能无回退、当前 Release/桌面 | W1～W4 及受影响 NA/PC/UPN/AR/NED；真实桌面暂停时正式出口不通过 | 精确 manifest、完整矩阵和缺项报告，用户文档 |

顺序上先收口当前 PC-07/NA 主线的直接缺口，再让 W1～W4按单一行为切片接续；若用户专门要求提前做多文件 Python，W1/W2 可先行，C++ W4 仍受 PC-07 基础闭环约束。每片遵循[现有 TDD 规程](agent_loop_node_authoring_workbench.md)：真实行为 Red（已有能力则复用证据）→最小 Green→直接回归→独立文件/音频/消费者观察→更新 `development_status.md` 与 `build/reports/`。阶段必须同时有代码、测试、演示、可重跑命令和报告；本文与验收文档完成本身不算 W1～W5 通过。
