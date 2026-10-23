# 下一阶段开发任务：Python 混合工程与运行服务集成

更新：2026-10-23，依据 P3-05 最新执行记录修订。本文细化[交付收口入口](plan_delivery_convergence.md)的 P3，并安排剩余 P2 与 P4 依赖；不改变原验收、阈值或持续授权。当前恢复按下方第0节及 [development_status 顶部](development_status.md)，R5 细节继续引用[原任务卡](plan_agent_next_r5.md)。第1～3节保留原逐片合同；其中初始不存在/待实现的描述只代表原规划起点，当前事实以第0节和随后有效记录为准。

## 0. 当前开发与验收排程（覆盖原始恢复起点）

最新接续：`P3-mixed-timeline/stream-port-eos/` 已完成内部固定帧流式逐输出EOS、选短输出时长输出有界drain，以及跨Python的缺EOS清理/定位/恢复，原生56份与混合/恢复26份WAV独立核验误差0。当前接续CORE-02借用视图生命周期与稳定通道ID组合证据，先查已有实现，不制造已实现行为的Red。下文旧时点的随机分块/逐端口EOS待办已由后续记录接续；通用异步、发布C ABI扩展及全部平台出口仍未通过。

### 0.1 已完成子集与尚缺出口

2026-10-23执行接续：P3-05新崩溃序列独立31份WAV已复核；P3-06组件/CLI/更新/资源与21份独立WAV已验证。候选Debug实际294/305，11失败已按Temp、worker图准备竞态、隔离SDK复制和共同装配守卫定向修复，原始失败与JUnit路径问题保留，未宣称整项passed。随后固定帧、真实Speex已知延迟、有限尾部混合子集均有Red/Green/独立产物；最新 `port-tail/` 已完成内部命名固定块双输出2/5帧尾长、部分/空有效区间和共享节点单次物化，误差0，边界及消费者定向补验通过。内部固定帧流式EOS和种子重放已由最新记录接续；C ABI v1独立尾长仍不支持。当前从CORE-02借用/身份组合等真实缺口接续，详见唯一摘要，不重做本段已验证行为。

本次规划实际读取的最新记录包括 2026-10-23 14:11 的 `P3-05/protocol-cli-check.json/xml` 和 14:15 的 `P3-05/queue-crash-check.json/xml`。恢复时先查是否已有更晚记录；工作区有后续改动时按影响判断，不将整树变化一律等同于产品证据失效。

| 能力 | 已读取证据与适用结论 | 后续处理 |
| --- | --- | --- |
| P3-01～04 | 宿主发现、纯混合计划、CLI/真实参数组件、整次代码冻结和迁移重放已有分片 Green；原生 NativePlan 保持离线任务拒绝 | 不从旧目录接入或 worker API 重做；完整 AR 仍逐项判定 |
| CLI 错误定位和协议 | `P3-05/protocol-cli-check.json/xml`，3/3、无失败/跳过，覆盖实际 CLI node/port 与真实传输/消息预算目标 | 旧 cli-diagnostic-red 是已被同目标 Green 接续的历史失败，不再作为待修起点；完整边界仍按实际用例核对 |
| 崩溃重试插入50周期 | `P3-05/queue-crash-check.json/xml`，2/2、无失败/跳过；含突发替换及崩溃后的同工程有效重试 | 该扩展序列的独立产物复核须绑定本记录；旧 queue-first-check 的31份 WAV 与心跳数值不能替代新序列 |
| Python 新插件扩展 | `P3-06/extension-lifecycle-check` 2/2及独立21份WAV；深目录runner修复后另有 `long-path/selected-extension-report.json` 与 `deep-extension-report.json` | 自动组件/CLI、更新、冻结和六类参数子集已验；不重跑已完成行为，完整桌面仍缺 |
| 总体出口 | 尚无当前最终源码完整 Release、全 AR/G/NED/UPN 累计通过证据 | 不能宣称 P3、R5/R6 或平台完成；桌面/数据/隔离环境继续单列 |

### 0.2 紧接着执行的开发和验收

按 A→B→C→D→E 推进；每个行为完成后继续，无需重复确认。A 只做一次短核对与必要独立读取，首个新增用户能力是 B，不能在证据维护中长期停留。

| 顺序 | 开发交付 | 必须验收与最小范围 |
| --- | --- | --- |
| A：接续 P3-05 | 对新50周期序列补独立复核，更新事实与精确映射 | 优先以现有 `scripts/verify_python_queue.py` 读取 queue-crash-check 的原始产物，验证31份 WAV、实际代码/NPY hash、请求身份、1次崩溃后恢复、取消目录、心跳和对象界。若已有匹配新报告则复用；不重跑无变化的产品序列 |
| B：P3-06 独立 Python 扩展 | 完成当前 user_transform 示例的发现、运行、更新、错误与文档，固定宿主 EXE | 真实部署新目录；六种参数尤其 bool/enum/float/资源参数经 controller 和 CLI 生效；main/side 同时消费且状态不重复推进。原件系数冻结后替换/删除，本次用旧副本；坏系数定位、无成功发布、随后恢复。改插件后新进程/新hash/新行为，历史不变、宿主hash不变 |
| C：Python 混合链自动化稳定出口 | 汇总 P3-01～06 的实际能力、剩余合同及构型证据 | 实际目标/CTest inventory、测试名、共享事实和证据绑定预检后，对冻结候选运行一次 Debug 累计。Python ON 为真实混合主路径；Python OFF、Runtime OFF、editor-off 按变化补消费者。未实现/桌面/环境缺项让正式验收保持非通过，不能为出口清单全绿而删掉 |
| D：混合时序与剩余 P2/R5 | File→SpeexDenoise→Python→原生 Output 的真实工程，以及逐端口尾长/有效区间和借用生命周期 | 依次定义固定帧零额外延迟→已知延迟→有限尾部→不同输出尾长/异步产出，逐行为解除混合准入限制；复用真实原生 FIFO/延迟/尾部实现。host1/127/256/480/512/1024及固定seed随机分块，长度/起点/区间/通道ID、raw/aligned与offset精确，独立直调/解析oracle符合原容差；未知/无界能力仍前置拒绝 |
| E：真实算法与交付 | Speex剩余边界/AEC→注册资源和几何任务/DAS→指标及完整工作台→离线包 | 依据第4～5节逐项完成 AR-14～20 及引用 G/NED/UPN；质量、桌面、设备、干净禁网环境齐全后执行最终源码完整 Release 累计 |

B 的独立数值期望须先在测试中按明确公式声明：enabled 时增益为 repeat×scale×polarity×coefficient×插件版本因子，否则按插件描述定义的旁路增益；main=x×gain、side=−main，示例 Mixer 再按其两个权重求和。每次 float32 边界按原数值合同处理，不能导入被测插件生成 expected。当前公式来自在建示例，只是待验收依据；若合法契约调整，先记录独立依据再改 fixture。

### 0.3 各出口的证据与通过标准

- A/B 日常：目标真实 Red（已有实现除外）→同目标 Green→直接调用方回归→一份实际产物独立读取。优先扩展现有 integration 测试及 oracle，不为每个 helper 新建验收框架。
- B 交付：独立插件目录、工程/系数 fixture、使用与更新步骤、GUI真实组件/CLI各自产物及oracle、固定EXE/新插件hash、错误/恢复记录。真实桌面仍未验证，不能由组件替代。
- C 交付：一份冻结源码候选、原始构建/执行记录、JUnit、逐需求精确映射和正式状态。分别列出 passed、failed、not_run、not_verified、timeout、skipped；组件成功或 verifier 退出0均不足以替代完整验收。Known Red 未修复不能宣布该出口通过。
- C 前不重复 M21/四构型全量、旧故障探针或无关变异。公共 SDK、构建或原生内核变化才扩相关消费者；旧 UPN 自动化结果只在相应边界未变且复用理由明确时沿用。
- D 的首个反例应来自实际 mixed_timeline_unsupported 或真实边界长度/偏移失败。不能删准入判断让未支持组合进入生产；先明确区域输入/输出时间线、补偿归属和预算，再最小接入。GUI/CLI共用计划，项目模型保持唯一权威，不添加算法 type_id 分支。
- 最终 Release 必须启用 Python 和 Speex 并覆盖全部必需项；当前 windows-release 预设 Speex OFF，只可作为其声明范围的构型。F04/F07、真实设备/桌面及正式 OS 禁网环境缺项不能继承自动化 passed。

### 0.4 立即开始时的动作

读取唯一摘要和最新 A/B 证据，比较相应源码/二进制及实际变化；已有匹配 Green 直接沿用。先完成新 queue-crash-check 的独立读取（若尚缺），随后直接在已有独立插件示例上补首个缺失验收行为。P3-06 目标/测试名可能仍在创建，先确认存在和 inventory，再决定最小构建/筛选；不得照抄不存在的命令当作已执行。

后续 agent 持续推进到下一行为；阶段恢复不要求重新执行 P3-01～04，也不重做已修的 CLI 定位 Red。桌面暂停、不自动提交推送发布和全部持续授权边界保持有效。

## 1. 下一交付目标

交付一个可重跑工程：File → C++ Gain → Python Stateful → C++ Gain → Output。CLI 与编辑器通过同一目录、controller/RunService 和任务计划运行，得到符合独立 oracle 的 WAV；异常、取消、代码更新后能恢复运行，报告能够追溯实际代码、环境、参数、端口和请求身份。

先交付 CLI 与真实 GUI 组件的自动化子集。桌面仍暂停，真实桌面、设备和原 E2E/NED 要求继续挂账；该子集通过不等于 AR-18.e、P3 或完整平台正式通过。完成一个行为后按本卡继续下一行为，常规技术决策无需确认。

## 2. 原始规划起点（历史，仅用于追溯）

以下是 P3-01 实施前的状态，不再作为当前恢复事实；其中“仍直接调用原生图”“未提交目录接入待验证”等描述已被第0节及 P3-01～05 的真实记录接续。

| 已核实内容 | 恢复时的处理 |
| --- | --- |
| Python runner 的 manifest、PCM/clip、状态、源码快照、协议和恢复，已有 `P3-python-runner/ctest-green.json` 10/10 | 根据改动范围复用，保留不完整输入/协议组合的剩余项；不重建 runner 框架 |
| `libs/audio_python_backend/PythonTask.{h,cpp}` 与 `P3-cpp-process/recovery-green.json` 5/5 | 已覆盖 C++ 监督、实际进程树回收、日志有界、EOF/启动错误恢复；不把它当成 RunService/GUI/CLI 端到端证据 |
| `NativePlugins.cpp`、`NodeSDK.h`、`NativePlan.cpp`、CMake 和 `test_python_process.cpp` 已有未提交目录接入代码 | 保留并验证。最近 5/5 记录不覆盖随后新增的目录测试和接入修改，不能据此报通过 |
| 当前 RunService 仍直接调用原生图；NativePlan 对 `requires_offline_task` 返回 `task_boundary_required` | 从真实宿主集成缺口接续，不移除拒绝检查来让 Python 进入音频回调 |
| M21 图能力、P1 SDK、P2 已有定向证据 | 不重做历史 Red；只有本片实际影响的消费者才回归 |
| 验收清单 AR-18.d 的 remaining_contracts 仍含已实现的监督器行为 | 实现片中按精确测试名与真实报告更新措辞，保留尚缺 PY-04、协议组合和宿主集成；不能直接删除整行剩余约束 |

规划期间不执行产品构建或测试，不把本卡登记为新增 passed。旧报告及源码 hash 不改写。

## 3. 执行顺序与出口

原始依赖顺序：P3-01 → P3-02 → P3-03 → P3-04 → P3-05 → P3-06。以下保留逐项合同，当前从第0节的真实缺口接续，已验证行为不重做。每行可拆成多个单行为循环；新行为先在生产调用路径保留真实失败，再做最小实现。

### P3-01：宿主发现 Python 节点

用户行为：部署一个 Python 插件目录，重启同一宿主，GUI/CLI 获得一致节点、端口和参数，扫描不执行用户代码。

- 首先验证已存在的 `PY-01 C++ host composition and CLI discover Python metadata without importing user code`。检查实际构建和 inventory；若现有代码已通过，直接保留记录进入缺失行为，不重做实现。
- 扩展同一组测试覆盖包级原子注册、重复 plugin/type ID、坏 metadata、描述在扫描后变动、中文/空格路径；C++ 与 Python manifest 校验同一组有效/无效 fixture，防止两端契约漂移。
- Python OFF 构型应明确呈现不可用状态/诊断，不启动解释器、不留下可执行的半注册节点；原生 DLL 与内置 Gain 继续工作。
- 核对 bool/int/float/enum/string/path 描述与现有表单兼容；实际参数变化交给 P3-03 的运行产物证明。

影响范围：`audio_native_plugins`、组合根、描述/工厂及现有测试；目录发现职责不进入 View。

验收归属：AR-18.a、PY-01、EXT-02 的发现子集、BLD-03。出口为 GUI 组合根/CLI 目录等价、副作用标记不存在、负例可恢复及精确映射；完整 EXT-02 后续补齐。

### P3-02：按真实边界编译混合任务计划

用户行为：相同工程得到 source → native region → offline task → native region → sink 的明确依赖。

- 在纯 `audio_runtime` 中扩展或新增值类型的混合计划（例如 PreparedPlan，名称待实现确定）。仅消费项目、目录和已解析格式，不导入 Python、不构造处理器、不做文件 I/O。
- 保留现有 NativePlan 作为兼容原生区域的执行投影；上层计划根据能力和端口类型切区，执行器通过注册能力取得，不增加算法 type_id 分支。
- 使用 `(node_id,port_id)` 路由；沿用选定输出的反向闭包、共享上游只执行一次及全局结构检查。闭包外任务零构造、零执行、零资源物化。
- 分区后的依赖必须仍为 DAG；用跨边界旁路与分支合流反例验证，不能简单合并所有相连原生节点而制造区域依赖环。
- 物化边界持有自有数据，保存采样率、稳定通道 ID、时间/有效区间和延迟语义。首片不支持的变格式组合在执行前定位拒绝，完整必需能力仍留在后续清单。
- 审查当前 NodeSDK 新增运行上下文字段与 run_api 依赖；任务身份/取消/产物职责放在实际调用所需边界，纯算法与安装算法消费者继续独立。发布 C ABI v1 不因内部 C++ 接口调整而隐式变化。

验收归属：AR-14.a/c、AR-18.e 的计划子集。最小集合是纯计划、选择裁剪、命名路由与新 DAG 反例；公共 SDK/构建依赖变化时补 Runtime OFF 和相关安装消费者。

### P3-03：RunService 真实执行混合链

用户行为：上述示例通过 CLI 和真实参数组件运行，Python 输出能够继续进入原生区域。

- RunService 在 worker 中按依赖执行计划，复用现有 PythonTask 监督器和真实 JUCE 原生区域；输入/输出按命名端口绑定。区域虚拟入口只属于执行投影，不污染权威项目模型。
- 每次任务内 Python Stateful 只创建/prepare 一次，至少跨三块连续处理；不同节点实例隔离，clip 整段只调用一次。两个输出与分支共享不得导致重复推进状态。
- 在分配/复制载荷前验证实际大小、shape、float32、通道/区间及聚合预算；默认每 Python 任务输入上限仍为 64 MiB，控制消息上限 1 MiB。输出同样有界；超限定位拒绝，不能靠提高限额通关。
- 使用独立公式验证 Gain→Stateful→Gain，oracle 显式覆盖每个 float32 物化边界的舍入，按原默认容差比较全部样本。至少覆盖 host 1/127/512、非整尾、两实例、多输出和 clip；其余固定帧组合复用 P2 并按实际影响扩测。
- 通用参数表单经 controller 改变 Python 参数，真实输出和运行报告同步改变；GUI/CLI 使用同工程，分别读取产物对 oracle。
- 通过真实 worker/消息线程/音频回调调用链与受控线程记录确认：导入、进程启动和 Python 执行不在 UI 或设备回调中。

新增示例建议放 `examples/python_stateful_demo/`，包含 manifest、源码、工程与重跑说明；该目录当前待建。交付真实 WAV、任务边界元数据和独立检查结果，不仅是 worker API 测试。

验收归属：AR-18.b/e、PY-02/03/07、E2E-03 自动化子集；真实桌面仍未验证。

### P3-04：整次运行冻结 Python 代码与环境归属

用户行为：请求冻结后修改或删除原脚本，本次仍执行固定版本；下一请求重启 runner 使用新版本，旧结果仍可追溯。

- 将现有“任务开始冻结代码”提升到整次运行的准备边界：闭包内全部插件 manifest/本地模块在首个执行任务前固定，所有实例消费同一份已验证快照。不能等上游 C++ 处理完才读取可变脚本。
- 为冻结后、尚未进入 Python 任务的窗口加受控同步点，真实修改/删除测试自有脚本；本次仍符合旧 oracle，下一次新 PID/身份与新 oracle 一致。
- 延续仅执行已列入 hash 清单源码、同名模块隔离和拒绝旧 pyc 的能力。完整复制/绑定本地依赖与资源，不采用仅执行前后 hash 比较替代固定副本。
- run.json/任务记录绑定实际默认参数、完整 RunIdentity、计划、代码/manifest hash、解释器/依赖版本、运行文件及物化端口元数据；混合任务临时结果不独立发布为工程成功。
- 重放必须消费记录中的固定代码和资源，不重新发现原插件目录；快照迁移到新父目录、原件删除后仍可验证。环境/本地扩展变化使用新进程并验证兼容性，不能继续复用旧实例。

验收归属：AR-16.b/e、AR-18.c、PY-05、RUN-02。已有 runner 级测试保留，新增重点是 RunService 冻结时机、双实例一致性和重放。

### P3-05：混合任务的取消、失败与最终发布

用户行为：异常→取消→修正/更新→成功重跑均走同一宿主流程，旧成功结果不被污染。

- 将同一请求取消标志传到原生区域和 PythonTask；协作最多 2 秒、进程树总回收最多 5 秒。复用已通过的 Job Object 实现，只补真实 RunService/队列消费者和必要失败边界。
- Python 语法错误、运行异常、缺依赖、错误 dtype/shape/NaN/Inf、断连与日志洪泛有 node/port/脚本/traceback；CLI 非零，controller 保留工程且可再次运行。
- 原生和 Python 产物统一由 RunService 校验后发布，complete 最后写；取消、崩溃或磁盘发布失败无成功标记、旧成功不变、临时数据清理后诊断仍可查。
- 验证“一个 active + 一个最新 pending”对混合请求同样有界；New/Open/关闭与晚到回调保持 project/epoch/request/revision 隔离，不靠强杀原生线程回收。
- 独立补齐完整 50 周期的编辑/运行/取消/重开与真实 worker 存活检查；组件消息线程心跳按 p95≤100ms、max≤500ms，桌面交互证据单列。

验收归属：AR-16.c/d、AR-17.a～e、AR-18.d/e、PY-04/06/08、RUN-01/03～06。共享报告可被多个需求引用，未覆盖的协议分片/粘连、错误 token/版本和实际边界仍逐项补齐。

### P3-06：首个 Python 扩展示例与可复核出口

用户行为：加入一个全新 Python 插件目录，无需重编译宿主，即可发现并执行；更新后新进程生效。

- 在固定宿主二进制上部署第二个独立示例，覆盖 bool/enum/float/资源参数及多输出，验证参数、执行、错误和更新流程；保留 EXE hash。资源参数经统一快照绑定，不在插件中重新读取原件。
- 汇总 PY-01～08、EXT-02、E2E-03、AR-18.a～e 的精确行为与剩余项，消除“已实现仍写 unimplemented”及“子集通过使整项通过”两种失真。
- 保存示例、真实输出、原始命令/JUnit、oracle、源码/构建/二进制/环境关联和简短出口报告。当前源码执行一次明确的 Debug 累计候选；先检查 inventory/共享证据，再执行必要集合。
- Python OFF、Runtime OFF、editor-off 等按实际边界改动运行受影响消费者，不把 OFF 配置作为 Python ON 的集成证明。Release 完整累计保留最终出口要求。

完成声明使用“Python 混合链自动化子集已交付”，逐项保留桌面、环境/打包及其余未验收行为；不能据此宣布 R5/R6、P3 或整个平台完成。

## 4. P3 出口后的依赖顺序

| 后续顺序 | 具体交付 | 进入/完成依据 |
| --- | --- | --- |
| 剩余 P2/R5 组合 | 不同输出独立尾长/有效区间、异步消耗产出、借用视图生命周期，补 AR-15.a～f 与完整 New/Open/发布 | P3 集成暴露的相关缺陷立即修复；其余逐行为完成，不重复已过 FIFO/时映射全套 |
| Speex 边界与 AEC | 补 Denoise profile-OFF/缺依赖/未支持 flush/动态更新；新增 AEC core、薄节点及 capture/render 双输入工程 | F05 解析回声及近端分量、锁定原库直调→节点→CLI oracle，角色/偏移/状态/异步时钟拒绝；对应 AR-19.a/b |
| 注册资源任务与 DAS | 利用真实几何消费者完成所需非 PCM 类型/资源注册；Python DAS clip 工程接回原生输出 | 从 source/sink 注册解除“所有 source=WAV/path”假设，几何不伪装音频；F06 解析信号及锁定库直调，AR-14.d、AR-19.c |
| P4 工作台 | 注册指标与 AR-12.e、声谱、A/B、区间、长文件缓存、历史/错误及设备流程 | 对照 UI-04～07、PERF-01～03、RUN-06、E2E-05 逐项交付；已有数据评测接口先检查再复用 |
| 最终交付 | 发布宿主/SDK/插件示例/Python 离线依赖与使用说明 | 最终源码完整 Release 累计、干净环境禁网安装运行、全部 AR/G/NED/UPN 必需项真实通过 |

质量数据、桌面与包装准备在达到依赖边界时及时推进，不要求等全部算法实现后才发现缺口。最终 Release preset 当前 Speex 为 OFF，最终算法验收必须另有 Speex ON 的明确构型与产物，不能把默认配置当作全功能交付。

## 5. 外部验收与恢复条件

- 桌面：保持暂停。用户明确恢复且工具可用后，完成真实拖放、控件、保存/重开、混合图、DPI、设备及 UPN GUI 证据；组件测试不得替代。
- F04：至少三条、每条十秒以上的自有/授权干净语音及固定噪声/混合分量，许可、活动区间、seed、SHA 可追溯；按原 0/10 dB 与质量公式验证，不用合成音代替语音质量。
- F07：授权真实同步四通道阵列，几何、目标/干扰方向、通道/时钟说明及有效分量参考齐全；探索性混合录音不能替代正式 QUAL-03。
- 隔离交付：正式受控干净环境、OS 禁网、声明依赖/本地 wheelhouse、无开发机源码或绝对路径依赖；补 UPN-06 与 PKG-01。准备安装布局可先做，正式环境不足仍记 not_verified。

缺项只阻挡对应完成声明；继续不依赖该条件的任务。当前规划不触发桌面恢复、外部下载、发布或许可购买。

## 6. 日常验证与记录

1. 每次只选择上文一个可观察行为；写明现有测试、真实缺口、独立期望、最小目标和扩测理由。失败先区分工具/编译/产品/证据层。
2. 优先扩展 `tests/integration/test_python_process.cpp` 及现有 runner 测试；混合计划放纯应用测试，RunService/组件验收放实际消费者测试。下列命令只适用于当前已经存在的目标；后续测试先确认 inventory 再使用过滤器。
3. 构建默认 Debug 增量；产品边界变化才扩相关消费者。接口和构建未变化时，不为文档、helper 或恢复重复四构型矩阵、故障探针和变异。
4. 每片使用 `scripts/record_command.py` 保存唯一命令名、原始日志、JUnit 与实际产物。已有报告根为 `build/reports/loops/P3-host-integration/`；在对应切片选择新的唯一命令名，不覆盖旧记录。
5. 同片更新 `tests/architecture_refactor_manifest.json` 的实际测试名/remaining_contracts 和 development_status 唯一摘要；新增规划本身不改验收来源 hash、不迁移任何 passed。共享证据按影响复用，旧日志/hash 不改写。
6. 每个出口以新增可用行为说明结果；正式验收必须逐项满足，不以测试数量或 CLI 一次成功代替完整合同。

历史 P3-01 命令示例（已有该片通过记录，不作为当前恢复操作；本次规划未执行）：

```powershell
# 首片仅验证已有目录接入；若 CMake 注册发生变化，先按现有 preset 重新配置。
cmake --build build/windows-debug --config Debug --target audio_python_process_tests --parallel 4
ctest --test-dir build/windows-debug -C Debug -N -R '^PY-01 C\+\+ host composition'
ctest --test-dir build/windows-debug -C Debug -R '^PY-01 C\+\+ host composition' --output-on-failure --no-tests=error
```

实际命令经现有证据采集入口包装并记录二进制与源码对应。当前恢复和下一行为按第0节执行，不从 M21/R0/R2 或已验证 P3-01～04 重新开始。
