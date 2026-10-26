# Development status





## 唯一当前摘要（2026-10-26：PC0402～PC0408、PC0303～09及PC0501～06及PC0601～12自动化子项已推进；PC与平台完整专项未通过）





入口：[交付收口](plan_delivery_convergence.md)、[工作台方案](plan_node_authoring_workbench.md)、[工作台测试](testing_node_authoring_workbench.md)、[工作台TDD规程及启动提示词](agent_loop_node_authoring_workbench.md)。原P3恢复见[P3任务卡第0节](plan_agent_next_p3.md)、[时序边界ADR0033](decisions/0033-owned-native-timeline-boundaries.md)。当前主线为NA，原P3暂停接续；旧R0/R2/M21起点、历史“下一条”和旧数量均不作为当前起点。完整R5/R6、AR/G/NED/UPN和NA阶段均未通过。真实桌面继续暂停。





当前版本出口NA0024～30：生产工作台首次保存/另存为/外部打开 `.aadraft`，覆盖确认及外部修改拒绝；恢复与用户文件基线分离，恢复原路径/更新时间及撕裂绑定警告；工作/恢复目录可选择并探测，配置失败明确仅本次会话。节点库全部不可变版本可复制校验至空目录并设置下次启动，当前库保持有效、重启前禁用新Apply，源库保留。Windows启动脚本将相同字节部署到正常标签目录后再启动/检查令牌，不改ACL；深任务路径使用独立短scratch cwd，音频/包仍留在所选长目录。用户当前窗口未关闭或重启。用法见 [节点文件](custom_node_files.md)、[启动](build_and_run.md)。





PC0101已修复生产草稿路径：用户文件、恢复、绑定、列举与目录探针在逻辑路径上计算父目录，再生成Win32扩展路径。真实Red保留扩展D盘父目录丢分隔符断言；同用例Green，普通/扩展卷根和UNC共享根结构断言、304字符中文路径真实保存/重开/恢复绑定通过。独立解析草稿generation7、恢复generation8及原字节绑定成功。8个纯路径/草稿测试通过，另20个直接生产保存/关闭/取消/目录回归通过，无skip。最初SDK读取拒绝与组件启动0xc0000142保留为环境失败，工具获准运行后通过；未安装依赖或改权限。卷根实写、真实UNC共享写入仍not_verified，结构Green不替代实写。证据`build/reports/loops/PC0101-root-save/`。PC0201已接入底部Waveform/Run logs与Copy日志，真实Print编写/检查/Apply、库源与画布drop输入、端口连线、保存、两次生产Run通过；每次513帧/host512显示两次打印及不同运行身份，独立WAV/NPY恒等误差0、执行源码hash等于安装包。证据`build/reports/loops/PC0201-print/`。OS原生拖放路由、桌面及完整PC仍未通过；PC0301补齐失败stdout与终态异常、精确受管内容hash，旧身份四字段拒绝和New清空日志；档案保留在run-logs/<uuid>/events.jsonl，失败载荷仍清理且不发布成功。独立读取4档案/3成功/1失败，当前重建消费者直接回归51/51无skip，含混合冻结/重放/取消/50轮队列心跳。PC0302补齐独立Check日志区域和复制、绑定草稿generation/源码/声明hash/请求，独立读取验证检查5次与Run2次分离，直接15/15无skip；证据`build/reports/loops/PC0302-check-log/`。PC0401补齐每任务stdout/stderr合计16MiB完整日志，恰好允许，超1字节/持续洪泛触发log_limit回收，自有后续任务恢复；5种真实原始日志独立大小/内容/hash通过，直接33/33。证据`build/reports/loops/PC0401-log-budget/`。PC0402当前生产Run日志闭环：增量UTF-8解码、stdout/stderr实时投递、New清空；PC0403同源码Debug构建后生产UI/直接回归79/79、0失败/跳过，覆盖Canvas终态前日志、chunk拼接与10秒/20ms心跳。20轮独立探针执行60个真实Python任务，原始stdout与事件归档一致，运行后无存活进程。PC0402/03报告按源码快照分开收口：旧84项回归的单独心跳失败保留；此前malformed用例0xc0000142启动失败也保留，随后在PC0404集成源码/二进制上单项1/1通过，准确核对15原始字节、discarded_bytes=0。PC0403的79/79 UI回归绑定source_tree_hash 51943e4785...、audioalgo_tests.exe f89c0fae...及Editor b2f7feec...；独立核验只额外加入检查脚本，source_tree_hash为652ce3b7...。malformed复验绑定PC0404的source_tree_hash 6bbf14c1...及audioalgo_tests.exe 66cc8238...。后续PC0305及其后的Debug重链接已更新工作区当前二进制；这些交叠报告不按累计数量相加。Python-OFF 32/32及纯库10/10属于早期面板快照。PC0404实现生产Run日志按节点筛选；实际Check/Apply/库拖放两份Print实例并连成File→Print→Print→Output，Run后切换节点仍保留隐藏日志与请求身份；五项直接UI回归5/5、0失败/跳过，心跳10秒/20ms阈值保持。独立核对保存工程、两个节点日志/原始stdout及513帧/16kHz恒等音频。完整PC-04仍未通过：洪泛生产组件子集见PC0405，级别子集见PC0406，历史有日志子集见PC0407、GUI20轮队列/进程子集见PC0408；全Check/原生预算与无日志历史仍待验；PC-03.c活动生产Check取消与同面板恢复已验证：任务父/子进程在5秒内回收、无关进程存活，取消stdout档案和恢复源码独立核对；PC0305三秒per-task超时与恢复另有通过子集，活动面板/View/应用关闭及确定性晚到日志组件子集见PC0304/06；全Check墙钟和完整队列仍待验。证据`build/reports/loops/PC0303-check-cancel/`。PC0307已关闭顺序切换场景下同revision双工程、双版本/双实例日志身份子集；活动Run服务Apply冻结见PC0308，真实慢旧Run交错/日志/当前输出与预览目标见PC0309；完整终态、历史全部范围与桌面仍未闭合。真实桌面暂停。证据`build/reports/loops/PC0402-live-log/`、`build/reports/loops/PC0403-ui-bound/`、`build/reports/loops/PC0404-node-filter/`。





后续Agent交接文档已补齐：[逐项测试通过准则](testing_pipeline_execution_closure.md)规定PC-01～08的a～d条款及旧要求归属，[TDD执行规程和启动提示词](agent_loop_pipeline_execution_closure.md)规定首片、执行命令、无回退集合及报告收口，已接入AGENTS。补充文档初始为纯文档交付；当前PC0101～PC0403已实施部分自动化子集，32条完整补充验收及桌面仍未全部通过。PC编号映射见`tests/node_authoring_manifest.json`的独立补充表，不增加或替代68个NA子项。





NA0030时点Debug编辑器/CLI/消费者及独立Release编辑器构建通过（历史快照）；`NA0030-draft-close/exit-regression.json/xml` 为118/118、0失败/跳过（含全部49个真实NA测试及受影响画布/保存/运行），Python OFF54/54、无Runtime/Python/GUI纯库6/6。Python OFF54项在最终仅修改ON进程临时目录所有权保护前执行；该禁用构型再次增量构建成功、测试与所消费产品路径未变，复用明确禁用合同，不称Python运行通过。独立读取13份草稿、SaveAs相同字节/中文长路径/恢复绑定；最终CLI在原库临时不可用时从复制库运行两个精确版本，两份513帧WAV最大误差3.722e-9，源码/输入hash一致、原库恢复。独立Release正常副本验证字节一致、文件RID8192、验证工具进程RID12288（High，不能替代普通用户新GUI令牌证据），未启动GUI。68个必需子项全部保留，49测试精确绑定，父项/阶段不提升passed；完整证据与失败保留见 `build/reports/loops/NA0030-draft-close/loop.md`、`summary.json`。早期SDK访问失败、相对JUnit落点、错误验收根、配置参数拆分和MSVC COMDAT失败均已保留；COMDAT同源重编成功，精确工具链原因未验证。





PC0303已验证活动Check显式取消，PC0304补活动Check面板关闭/销毁/重开恢复：原合并回归3/4失败保留；只读MSVC探针确认递归进入长python-code/hash目录时报错3，改为有限层级查找并原子发布完整PID标记，同失败用例1/1、扩展直接回归8/8共172断言通过，独立源码/原始日志/PID屏障核验成功。无产品Red，未改产品或阈值。证据`build/reports/loops/PC0304-check-close/`。PC0305补生产Check三秒短deadline：真实超时诊断/此前日志、父子进程回收、无关进程存活及同面板恢复通过；保留编译Red、行为Red和中间陈旧链接失败。库/EXE标记不一致后重新链接，对应源码/二进制快照上的同用例1/1、直接9/9共204断言、Python OFF禁用合同2/2；源码/原始日志/3000ms请求元数据独立核验。另一构建与14项批次交叠，该批次不作精确binary出口、不累计相加。默认每任务30秒不变；全Check跨准备/两阶段墙钟仍未闭合。证据`build/reports/loops/PC0305-check-timeout/`。PC0306补活动Check下View销毁/应用关闭协调及晚到日志：两个真实进程屏障场景、旧View已排队日志在销毁后拒绝及载荷释放通过；当前同源码直接12/12、0失败/跳过，含原失败种子。原首轮11/12仅超时后恢复Ready失败，raw stdout已打印5次但未保存该次终态，原因未确认；已补终态先同步及诊断文件，未放宽阈值，不能称该偶发问题已修复。两场景源码/原始日志独立核验；Debug编辑器/CLI已刷新至PC0305产品源码。证据`build/reports/loops/PC0306-check-view-close/`。PC0307补双版本/双实例日志身份：两个显示版本同为1.0.0但内容hash不同的包、两个gain不同实例、不同工程同revision7，生产Run按v1→v2→v1执行并拒绝旧真实请求日志；直接8/8共7063断言，独立核对3份WAV/6份实际执行源码、事件与raw stdout，逐阶段float32误差均0。证据`build/reports/loops/PC0307-log-versions/`。PC0308补活动Run期间版本/源码冻结：v1真实Run进屏障后经既有服务Apply v2并更新默认，实际修改测试自有v1安装源码为异常代码并回读，当前两节点仍执行冻结v1；随后v2/重开v1归属正确。9/9直接回归共8690断言，独立3份音频误差0、6份执行源码/原始日志/精确hash、默认v2及v1原字节恢复核验成功。无产品改动；不称UI Apply或桌面通过。证据`build/reports/loops/PC0308-active-version/`。PC0309修复实际Open饥饿：活动Run占满唯一worker，Open工作排队后原实现要等采用新工程才取消旧Run，形成阻塞。非空Open请求准入即取消当前/排队Run，保留成功后才采用模型/catalog；空选择器取消不动作。原5秒Red保留；相同三场景（换工程同rev、同工程改rev、同rev换request）1/1共1675断言通过，独立源码/原始日志/取消档案/当前播放路径及3份音频误差0。直接27项原记录26通过/1系统Temp环境错误保留，仅该PRJ-01改进程本地仓库Temp后1/1；另15项接收/文档合同通过，不相加伪称新27/27。证据`build/reports/loops/PC0309-live-identity/`。PC0405修复日志全量排版阻塞：原真实洪泛十秒心跳max3047ms，独立面板64KiB追加406～2495ms；窗口缩小/定时方案仍出现持续日志超标及短错误前序日志隐藏，失败完整保留并弃用。最终以既有CodeDocument/CodeEditor只读可见行渲染替换日志TextEditor，完整256KiB尾部可滚动/复制、筛选与16MiB原文件保持；原内容/心跳阈值未改。目标5/5共4405断言，按原失败种子直接20/20共13933断言无失败/跳过；独立3份原始stdout各精确16MiB、超1字节与持续超限计数/诊断、执行源码及3份成功/恢复WAV误差0。洪泛p95=22.8644ms/max=23.6042ms；未称新20轮端到端全部通过。Play夹具改验实际点击拒绝预览，播放实现未改。证据`build/reports/loops/PC0405-live-flood/`。PC0406补生产日志级别筛选，与节点条件取交集且保留隐藏事件，All恢复、过滤复制及New清空均验证。真实Check/Apply/拖放/连线/两次Run，stdout-info与stderr-error分离仍带精确身份；目标2/2，直接22/22共15059断言无失败/跳过，独立4份CRLF原始流/事件/执行源码及2份恒等WAV误差0。初始独立脚本误按LF预期的失败保留并按Windows实际字节修正；不改产品输出。证据`build/reports/loops/PC0406-level-filter/`。PC0407补History/Current Run浏览，现有单worker发现当前工程最近128个有日志的运行，受限尾部读取并拒绝混杂身份/坏JSON；历史明确标识且不采用旧音频。两次真实Run旧/当前切换、当前试听归属，活动Run屏障下排队历史读取经New/Open撤销、关闭drain通过；目标扩展2/2，当前直接24/24共16360断言无失败/跳过，独立2份WAV误差0、执行源码/原始档案/试听路径、7000行reader夹具与清空UI均核验。无日志Run不列入，完整文件/分页及全部权限/链接/并发写矩阵未闭合；活动Render期间历史需等待同一worker。用法见[pipeline_run_logs](pipeline_run_logs.md)，证据`build/reports/loops/PC0407-log-history/`。PC0408验证生产GUI20轮：1次预热加每轮成功/错误/取消/恢复，共81真实Run；每轮active/pending/worker归零、自有父子进程退出及无关canary存活，预热与20轮结束句柄均174，日志尾部max3023字节。独立81份stdout/事件身份与诊断、41份执行源码/恒等WAV误差0；目标1/1共51714断言，直接4/4、独立纯目标3/3分开记录。原调度未改，仅新增只读计数；精确源码差异核验后复用PC0407的24项/心跳历史范围，不拼累计。证据`build/reports/loops/PC0408-gui-cycles/`。PC0501补两方向明确公式：现有C++ Gain与一个用户Python stateful类型通过生产Run/新CLI执行，48000Hz/2ch/513frames/host127；每方向GUI重复2次及CLI1次，共6次，各自独立逐阶段oracle/WAV/18份NPY边界误差0，真实reset观察每节点5calls/max127、每Run样本0起。编译Red与请求127却报告512的真实Red保留；组件新增默认512运行配置并传既有RunRequest，无调度/模型重构。直接6/6、实际默认消费者512/512核验；仅夹具trace后变更经精确差异核对复用原回归范围。当前Python-OFF组件消费者重建及2禁用/目录合同通过，不称Python执行或OFF app启动。独立脚本的代码库存/输入相对基准误判及受限NPY读取失败保留，按实际映射且获准只读后通过，无ACL改动。证据`build/reports/loops/PC0501-mixed-formulas/`。PC0502已完成上述两方向完整数值矩阵：16000/48000×1/2/4×1/513/16037共36配置、host127，另补48000双通道16037的host1/512/1024共6配置（host127不重复），42配置各GUI1次/新CLI1次共84真实Run；全部实际WAV/NPY、执行闭包/资源/hash/身份及reset trace由独立逐阶段float32 oracle核验，最大误差0。目标1/1共1313537断言。初始P3复用审计漏掉host_nodes经PUBLIC传递的AuthoringRuntime依赖，已明确撤销为superseded并保留原审计；旧二进制2项记录仅属历史。随后重建当前消费者，固定480/512、延迟/尾部/EOS、冻结/重放、错误及取消恢复20/20共131844断言通过，无失败/跳过；不相加为累计数。PC0501直接6项/OFF2项经精确源码差异审计只复用原合同范围。证据`build/reports/loops/PC0502-mixed-matrix/`。PC0503补齐用户Python共享上游→双原生分支→命名双输入/双输出的生产Run与新CLI：明确选择main/side，每端口GUI2次/CLI1次共6次；逐阶段float32独立读取6WAV/36NPY边界与源码/资源/身份/原始日志，误差0，每次共享上游513帧仅5calls且从0开始。目标1/1共6319断言，直接4/4共8922断言，无失败/跳过。首轮新夹具JUCE字符串编译问题修正，原报告保留，不冒充产品Red；无产品/调度/ABI修改，精确逆向差异证明PC0502原矩阵和P3当前20项范围可复用，不拼累计。证据`build/reports/loops/PC0503-named-shared/`。PC0504当前原生时序/partial progress/零进展预算/零分配及EOS直接17/17共588276断言，原工作台输出拒绝/恢复1/1；新增v1流式/离线/尾部能力在加载前拒绝1/1，实际manifest/非DLL哨兵/诊断独立读取通过，合法能力控制达到DLL加载失败以区分层次，不冒充旧DLL兼容。静态已知延迟仍按现有SDK支持。未改产品；PC0502已重建混合20项经消费源码/二进制身份审计复用，逐条见`PC0504-timing-contracts/clause-map.json`。原生17项是独立C++期望的当前断言，无新WAV独立读取声明；旧stream-port-eos独立报告保持原快照，未贴当前身份。首轮新断言字符串类型编译失败保留。PC0505补真实中间Python输出采样率/通道身份/有效区间错误：3类均定位python/main、停止本次发布、旧成功字节/实际源码不变，同图恢复；当前目标与原错误/取消直接3/3共232断言。独立3份失败stdout/诊断档案、4份恢复/基线WAV及12份NPY边界/代码闭包/资源/身份核验误差0。首轮唯一失败为新夹具混合换行字符串与JUCE序列化后的CRLF比较，原实际源码等于首个冻结副本，按写入后真实字节建立屏障后同用例通过；首个观察器把未受管包origin误当空，已按真实plugin_id/manifest SHA/version精确校验，失败均保留。未改产品与共享fixture，精确逆向差异保留原P3其他用例范围，不拼当前累计。证据`build/reports/loops/PC0505-output-metadata/`。PC0506真实Red确认混合源预检缺失端口：rate/通道数/通道身份/帧数/区间5场景均已拒绝且0工厂，唯一5失败为port空；只在既有Diagnostic补audio，与原生b/audio一致，同用例1/1共1071断言Green。已重建当前RunService/CLI/组件/编辑器，直接混合22/22共132967断言、组件4/4共12709断言，无失败/跳过；独立当前6双向公式+6命名共享+4元数据恢复共16成功Run/66NPY边界、3失败档案及Red/Green源WAV控制误差均0。单字段产品逆向差异审计通过，PC0502的84Run仍保留原快照范围，不贴当前累计。构建记录误写的AudioAlgoEditor.exe保留missing，实际`AudioAlgo Editor.exe`身份已在binary-identity.json精确补齐，未启动桌面。目标的多源/区间仅实际worker绑定，不称GUI/Python目标；另有明确生产Run回归。证据`build/reports/loops/PC0506-source-port/`。PC0601新增独立可选C日志ABI声明与预分配实例SPSC队列，旧node_abi_v1.h头/布局/入口字节保持；真实旧v1 DLL d01ffde5...身份与原P1记录匹配并冻结。仓库外仅导出SDK的C11/C++20消费者1/1：64条×单条1024字节、边界/UTF-8/结构版本/借用复制、关闭、68接受/1满/9非法、双实例并发20000条，回调/pop的C++分配0，并发队满序号及字节计数关系通过。独立编译输入/源码/SDK/消费者运行前后EXE/hash核验无仓库/JUCE依赖，CLI/编辑器及所有apps/libs/plugins/根构建字节未变，不重编宿主；旧SDK缺头编译Red及新消费者漏C11失败均保留。仅SDK队列合同，**宿主日志入口协商、真实DLL绑定、worker消费/归档/UI/预算/生命周期尚未实现，不称旧DLL在新日志宿主已通过**。证据`build/reports/loops/PC0601-native-log-queue/`，说明[native_node_sdk](native_node_sdk.md)。PC0602先补可选入口表协商：仓库外仅SDK编译6个真实DLL，四种坏表原被接受的产品Red已保留，最小在主v1描述校验后/原子注册前协商，大小/版本/缺set_sink拒绝与明确协商失败均有诊断；缺入口保留v1计算。相同Debug10/10、Python-OFF10/10、当前真实旧v1双实例/参数/输出及catalog销毁后模块存活2/2、Python混合/重放/元数据恢复3/3通过，无失败/跳过。独立11份实际WAV误差0，PE导出/内嵌JSON字段、实际MSVC输入无仓库/JUCE依赖、六DLL Red/Green/OFF字节一致，旧真实DLL d01ffde5...与v1头保持原hash；本片平台升级后宿主hash另列，DLL外部构建前后原宿主未变。首轮测试导入库输出冲突、观察器模块路径/源CRLF误判均保留；原外部数值输出Access is denied只记环境失败，移到自有工作区报告后原数值全部通过，未查证令牌/ACL根因也未修改权限。**PC0602时点未绑定日志sink、worker归档/UI/预算；当前接入见PC0603**；PC0601队列证明保留SDK原范围，不把旧宿主hash贴到升级后。证据`build/reports/loops/PC0602-log-negotiation/`。PC0603已接实际原生实例sink与worker消费、完整身份raw归档及既有UI：初始8个日志Red保留；独立核验再发现异步releaseResources早于最终reset/drain，增强同用例出现6个reset Red，复用真实消息线程释放栅栏后同目标1/1共3163断言通过。最终当前组件24/24共606921断言、混合22/22共132967断言、Python OFF3/3、纯计划18/18/应用40/40及Runtime OFF18/18，SDK CLI ON/OFF各10/10、真实旧DLL当前组件2/2分别通过，无失败/跳过，不拼累计。独立读取11份实际WAV/6NPY、6混合及2纯CLI原生raw流，数值误差0；ON/OFF实际绑定回调/reset C++分配0、12条只由绑定worker消费，catalog销毁后模块仍存活。原DLL与C v1头hash不变。首次直接组件工具审批超时发生在启动前，同命令获准重试后通过，不冒充产品Red。当前产品/测试/SDK编译输入及7个Debug消费者hash逐一绑定；较早79项/84Run等仅保留各自快照。**PC0603时点尚缺原生精确/聚合洪泛预算、文字/级别展示、旧DLL能力说明及完整生命周期；所测文字子集见PC0604、能力说明见PC0605，其余仍挂账**；证据`build/reports/loops/PC0603-native-live/`。PC0604修复实际原生NUL截断：raw队列97字节完整但JUCE归档/UI在NUL处截断，隔离同目标6个真实Red；仅对NUL消息增加显式无损字节编码，历史返回原字节，UI/Copy用\0及\xNN显示控制字符后缀，普通事件格式保持。相同2226断言Green；当前直接组件24/24共20704断言、混合20/20共132787、OFF5/5、真实旧DLL2/2通过。另两项混合恢复原批次1通过/1取消探针parent句柄空失败保留；新增空标记确定性探针证明PID0/error87窗口，取消夹具私有源码改原子发布且reader取得完整父子句柄后才取消，同失败用例+探针2/2共39断言Green，原5秒/2秒断言不变。首次失败未记录PID，精确原因仍未确认；不把原批次改写成全过。精确逆向hash证明其他测试/共享fixture及产品取消代码未改，复用原通过范围。独立ON/OFF四份实际WAV、四份97字节raw/编码/历史/四级筛选及SDK编译身份，误差0；当前洪泛另3WAV/3档案误差0，10.006秒心跳p95=23.1446ms/max=26.3363ms。观察器tuple/list误判、首次历史标签夹具/路径参数及driver启动错误均保留。PC0604时点原生完整预算、能力提示、生命周期/C++构建激活仍缺；能力提示后续见PC0605，证据`build/reports/loops/PC0604-native-text/`。PC0605已补实际旧v1/缺扩展/新SDK DLL的日志能力说明：实际协商结果写入既有native元数据，节点库条目及Inspector显示当前版本是否提供用户日志，fresh CLI目录一致；不读UI线程磁盘/模块、不改C ABI/SDK/公开类型或调度。15个真实Red→同用例1/1共86断言，当前直接24/24共20709断言、OFF3/3、SDK CLI ON/OFF各10/10、真实旧DLL双实例/模块租约2/2通过，无失败/跳过。独立读取8份原生数值WAV及另2份当前NUL文字WAV/raw，误差0；旧v1头及外部DLL hash不变，当前Debug/OFF消费者与源码精确绑定。历史矩阵只保留各自快照，不拼累计；真实tooltip展示/桌面仍未验证。证据`build/reports/loops/PC0605-native-capability/`。PC0606修复原生任务预算按实例重复授予：实际双实例合计16MiB+1及32MiB仍发布成功、CLI错误成功，单例错误码也不符；14个真实Red保留。仅增加worker所属native render任务共享计数，在raw写入前检查并于最终reset/drain沿用，返回log_limit；不改DSP/SDK队列/C v1字节。相同8332断言Green；当前原生直接22/22共594903断言、混合23/23共132981、OFF4/4、纯计划18/应用40/Runtime OFF18、真实旧DLL2/2及SDK CLI ON/OFF各10分别通过，无失败/跳过。独立ON/OFF各8份成功WAV、6份每任务16MiB raw档案、4次失败无载荷，原成功及恢复数值误差0；另兼容8WAV与NUL文字2WAV分别核验通过，不拼累计。首轮新夹具漏父目录及真实超预算Red均保留；SDK补充命令曾因自动审批额度未启动，2026-10-26重置后正常审批执行通过，无绕过或权限修改。当前7Debug/2OFF/1RuntimeOFF消费者及源码精确绑定。**PC0606时点尚缺近限隔离/原生尾部/消息拒绝等，后续分别见PC0607～10；原生GUI洪泛心跳及完整生命周期/C++构建激活仍缺**；证据`build/reports/loops/PC0606-native-budget/`。PC0607补齐实际SDK队满/非法消息生产组件证据：外部SDK DLL与独立消费者验证9种状态，生产Inspector/Save/Run覆盖队满、UTF-8、1025字节、结构大小/版本/级别/null及普通/空消息，7失败逐次恢复，另2次真实CLI失败。既有拒绝实现首次目标1/1共10429断言通过，无产品改动也不制造Red；直接8/8共23290、OFF4/4共12805，独立SDK消费者单次1CTest/9模式通过，宿主侧callback/pop C++分配0（不覆盖DLL独立CRT/malloc）。独立ON/OFF各9WAV/7GUI失败/2CLI失败、18身份及raw/事件/UI/参数/编译/PE核验，数值误差0；full64接受/1拒绝1024字节、attempted65549/raw64525，非法输入各1拒绝并保留28字节前缀/状态。精确逆向SHA证明只追加测试，产品及现有用例/SDK/CLI/Editor等消费者不变，PC0606有效结果仅复用原范围，不拼累计。证据`build/reports/loops/PC0607-native-rejection/`。PC0607时点的跨区域/并行近限隔离与尾部缺口后续见PC0608～10；UI洪泛/完整生命周期及C++工作台等仍待验；PC0608验证预算按native region归属：实际File→SDK2x→已Check/Apply精确锁Python.25x→SDK2x→Output，每个原生区域16MiB、整Run32MiB允许；仅C超1字节定位native-C/log_limit、无失败载荷，旧成功保持，随后32MiB精确预算重跑成功。既有实现首次目标1/1共2113断言，直接4/4共15837通过，无产品改动/假造Red。独立2WAV/4NPY、6份各16MiB raw/代码/包/runner/输入/身份，数值误差0。精确追加测试逆向SHA及宿主/SDK字节审计复用PC0606/07原范围，不把Python OFF特性跳过当通过；本片是顺序RunService子集，不宣称GUI/CLI近限归属或并行通过。证据`build/reports/loops/PC0608-region-budget/`。PC0609验证共享不可变catalog/DLL的两个独立RunService消费者并行预算：相同工程/revision/node、不同request，两实际worker在首1024字节原始日志后屏障同时阻塞；各16MiB均成功，随后A超1字节仅A/log_limit，B仍成功，旧成功字节不变。目标1/1共3170断言、直接3/3共13656、Python OFF同目标1/1共3170通过。ON/OFF独立各3WAV/4份16MiB raw/两组屏障，精确2x数值误差0。无产品改动或假造Red；精确追加审计复用PC0606/07/08原范围，不累计重叠批次，不宣称编辑器并行调度或完整取消/关闭。曾被自动审批拒绝的条件化断言方案未执行；改为严格REQUIRE进入屏障，其他断言全部无条件保留并已通过。证据`build/reports/loops/PC0609-parallel-budget/`。PC0610补齐worker所属原生每流64KiB捕获尾部与结束摘要：真实ASCII65536/65537、三字节UTF8共65538及NUL65537的4次Run原先音频/raw成功但无尾部，95条产品Red保留；仅NativePlugins消费路径保存有界完整字符尾部，NUL用Base64精确携带，SDK拒绝/原始持久化/尾部丢弃分别计数。目标1/1共5315、直接8/8共29583、OFF适用2/2共5379、真实旧DLL消费者2/2共34通过，均0失败/跳过。独立ON/OFF各4WAV/raw/summary误差0，ON另读46份直接运行摘要含16MiB超限、队满、非法消息、跨区域/并行；旧DLL另2WAV核验，ABI头/DLL未变。首轮夹具枚举错误、两次编译错误、中间4条路径拼写断言、误选Python-only用例的OFF1skip及拒绝该集合的observer均保留；最终仅实际适用两项0skip，不修改预算或删断言。原生UI discarded_bytes仍指SDK队满字节，新摘要tail_discarded_bytes另列；完整UI计数/洪泛及取消关闭仍未闭合。证据`build/reports/loops/PC0610-native-tail/`。PC0611修复同步detach期间已接受日志遗漏：SDK上下文借用至set_sink(NULL)返回，真实DLL在返回前emit20字节成功，旧宿主两实例/两Run每流accepted36但persisted16、仍报告成功，共漏80字节；36条产品Red保留。仅在detach返回后、queueclose和摘要之前补一次worker消费；同目标1/1共2199、直接5/5共21084、OFF适用3/3共7578、真实旧DLL2/2共34通过，均0失败/跳过。独立ON/OFF各2WAV/4流，事件/raw/尾部/摘要均完整36字节2消息，数值4x误差0、旧成功保持。精确最小逆向SHA复用PC0610预算/并行原范围，公共SDK/process/reset/调度未变；完整活动取消/关闭/晚到/模块及原生GUI洪泛仍未通过。证据`build/reports/loops/PC0611-detach-drain/`。PC0612验证实际生产EditorRunQueue活动原生取消：首条native-A真实raw16字节后worker观察屏障，主线程断言活动队列/请求再调用queue.cancel；两实例最终detach日志保留，run_cancelled/无波形/失败载荷清理，旧成功字节不变，同一队列随后恢复且active/pending/claimed/jobs均归零。既有实现首次目标1/1共2280、直接5/5共2841、Python OFF同目标1/1共2280通过，0失败/跳过，无产品修改或假造Red。独立ON/OFF各2WAV/6raw与摘要/事件/3身份，4x数值误差0，取消耗时9.9827/10.7264ms，原5000ms上限未改。精确追加测试及宿主/SDK字节审计复用PC0610/11原范围；本片是生产队列组件，未称外层GUI按钮、OS子进程回收或完整View/应用关闭。证据`build/reports/loops/PC0612-native-cancel/`。当前下一条为**PC0613活动原生View/队列关闭及晚到投递拒绝**；PC-04工作台全Check墙钟、原生GUI洪泛/完整生命周期及历史全部范围仍挂账；PC-05双向公式及数值矩阵已有上述子集证据，完整其他条款仍逐项闭合。PC0404按节点筛选、PC0406级别交集子集已有自动化证据；PC0407已补有日志历史子集，无日志历史/完整矩阵及正式桌面仍挂账。之后按PC-06～08推进C++日志兼容、独立构建激活与SaveAs资源携带；沿用已有混合调度，不重写已过功能。原**工程Save As新目录的输入资源与精确包携带**仍保留，属于不同任务，按补充方案接续。慢运行晚到、多类型/迟到预览、完整定义/矩阵/预算及C++未完成；S0～S5均未通过。真实桌面选择器、正常Release新进程、DPI继续not_verified；独立Release为Python ON/Speex OFF构建，不能替代最终Python/Speex ON完整矩阵。





| 用户能力/事项 | 实现及直接有效证据 | 当前缺口 |


| --- | --- | --- |


| 应用内自定义节点工作台（S0/S1自动化子集） | NA0001～07历史草稿/独立检查/检查前冻结/schema2精确锁/首次迁移备份及故障恢复保留。2026-10-24审查LNK2001已通过真实PythonAuthoring::apply修复，当前Debug编辑器/测试构建成功。NA0008不可变包、原子索引、幂等Apply、生产按钮/即时库刷新/插入/Inspector调参/端口连线/保存/真实Run，排队Run提交时拥有catalog副本。NA0009两个确定性时序Red已纳入生产面板正式测试，取消撤销晚到Ready、失败保存后检查中放弃能关闭且保存仍1次。NA0010共同宿主恢复已接入编辑器worker启动/新CLI，缺精确锁拒绝；NA0011已创建处理器执行前验证原版本并从同hash副本运行，改源码前置拒绝、原字节修复后恢复。NA0012后台准备包/索引→UI预备条目→后台原子提交→稳定catalog交换，目录准备失败不发布且可重试，提交校验并发索引。NA0013名称/type字段维护同一JSON，中文名称实际Apply、新建独立默认type通过24/24及独立草稿/manifest字节核验。NA0014可选择状态模板，initialize/process/reset跨块和重跑归零，实际Apply/插入/Inspector step=1e-5显示与2e-5提交已验证；模板元数据补1e-6步长。NA0015命名双输入/双输出生成/试跑及两个真实WAV源分别选main/side运行通过，独立读取两路NPY/所选WAV/资源快照和精确源码。历史 `NA0015-named-template/direct-green.json/xml` 58/59（全部27个NA及画布/端口/所选输出通过），唯一UPN文档换行/哈希写入差异已修复并1/1复验；Runtime OFF/Python OFF/Editor OFF纯库重建3/3。此前NA0014工作台25项+参数5项30/30。独立安装索引/包/工程锁/实际执行源码/513帧WAV核验，组件误差1.42e-8，新CLI误差3.72e-9。 | 68项逐项映射 `tests/node_authoring_manifest.json`，NA0030出口49个真实测试均已发现并绑定，当前版本切片结果见上文；每条执行通过只属于所记快照，不能作为完整NA项/阶段passed。完整故障/中断恢复、同包多类型锁/升级、迟到预览与慢运行归属、完整不兼容迁移、携带/重放、完整定义表单/队列预算、C++构建/probe/激活尚待闭合。首次schema升级GUI提示未做，NA0007备份IO已完成部分。NA0008直接61/63，UPN旧文档哈希/外部TEMP两项已定位并各1/1复验；重链接的Python消费者24/24；NA0011 Python OFF18/18仅诊断/组件子集，不代表Python执行通过。NA0012事务后重建消费者直接67/67；NA0013另有28/28（编译交叠绑定限制见报告）。NA0014首轮两条5秒内未完成检查的失败仍保留，复验通过但原等待原因未完全定位；完整预算/稳定性仍缺项，未放宽阈值。S0～S5均未正式通过，真实桌面/DPI仍not_verified。 |


| 节点自由拖动 | NI0014移除拖动预览中`x>=8/y>=42`模型坐标夹取；四倍率+平移后可落在(4,20)、(0,0)、(-80,-60)，鼠标锚点、连线、布局撤销/重做与计算revision保持正确。真实Red→Green，Debug测试/宿主已构建，直接回归16/16；证据 `build/reports/loops/NI0014-unbounded-node-drag/`。 | 保留底层有限数值校验及视口裁剪；真实桌面/DPI未验证，未自动重启用户应用。完整平台状态不提升。 |


| 一键编译启动 | `build_and_run.cmd` 支持Debug/Release/并行/BuildOnly，已运行检查及原NI0013证据保留。NA0026/30新增正常运行目录、`-RunDirectory`、逐文件hash/EXE标签/新进程令牌检查，低标签目的地及损坏副本拒绝；Debug及独立Release实际副本校验成功。用法 `doc/build_and_run.md`。 | 没有关闭或启动用户窗口；真实新GUI进程与DPI未验证，正式Release完整矩阵仍缺。独立Release构建在 `build/windows-release-user-files/`，旧Release输出与现有窗口保留。 |


| 用户节点文件与目录 | NA0024～30首次Save/SaveAs/Open/Recover、原文件冲突/覆盖确认、保存取消/失败关闭保留、正常放弃、恢复独立基线/原绑定；工作/恢复目录和设置优先级；完整版本库复制及下次启动生效。中文长文件与两版本新CLI独立音频核验通过，当前118/118集合包含49个NA用例。 | 本轮目录/文件方案自动化子集通过；磁盘满等真实故障矩阵、原生选择器/DPI/正常GUI启动仍未验收。无`.py`导出，工程SaveAs携带仍后续；详见当前摘要及 `custom_node_files.md`。 |


| 画布缩放与右键框选 | NI0012统一投影节点完整几何/端口/文字，右键优先框选，Inspector刷新保留多选；0.4/1/1.25/2.5倍缩放+平移的真实组件输入、连续连线像素、多端口命中、批量删除/一次撤销、取消路径已验证。Debug宿主已构建；证据 `build/reports/loops/NI0012-canvas-zoom-marquee/summary.json`、`loop.md`。直接CTest首轮85/89，4项仅系统Temp拒绝写入；仅这4项改用工作区临时目录后4/4，原失败保留。 | 属于本次用户指定缺陷切片，完整NED/平台仍未通过；组件渲染图已查看，真实桌面/DPI继续未验证，不自动重启用户应用。 |


| 基础编辑与原生图 | Gain/ChannelSwap、controller/参数表单、多源Mixer、命名路由、所选端口裁剪已实现。M21冻结候选Debug242/242，仅代表当时快照；旧证据在RF0006/M21-r5-graph-milestone。 | 桌面/设备、注册非PCM任务与工作台尚未闭合；不重做旧Red。 |


| 独立C++用户节点 | 安装C ABI v1 SDK、仓库外DLL、启动加载、两实例/错误/更新及固定宿主hash已有P1证据。最新后端Python OFF外部SDK消费者 `P3-mixed-timeline/finite-tail/external-consumer-check.json/xml` 1/1。说明 `native_node_sdk.md`；SDK包 `build/releases/AudioAlgoNodeSDK-1.0.0.zip`。 | UPN真实GUI桌面与正式干净OS禁网环境未验证；运行中热卸载后置，原110 AR之外另保留6 UPN。 |


| 原生时序、快照及队列 | 固定480/512 FIFO、partial progress、有界尾部/延迟、取消/发布与pending均有分片证据；最新 `stream-port-eos/` 新增独立输出EOS，选短输出时长输出仍有界结束，原有EOS严格失败/零分配/恢复回归通过。未知延迟/尾部/flush及零进展预算继续前置拒绝。 | 通用异步跨帧缓存、完整CORE-02借用/通道身份组合与更宽区间组合仍未闭合。 |


| Python主路径P3-01～04 | 宿主发现/原子校验、纯混合计划、worker组件/CLI、状态/clip/多输出、整次代码/环境冻结、迁移重放已有Green；原生回调不执行Python。各分片在 `build/reports/loops/P3-host-integration/`。 | AR-18各项保留完整剩余合同；原生扩展环境兼容与最终独立安装仍未验证。 |


| P3-05协议/恢复/队列 | protocol-cli-check 3/3、queue-crash-check 2/2；新序列由 `P3-05/queue-crash-independent-read.json` 读取31份WAV/代码/NPY hash，误差0，含崩溃重试、取消清理/身份，对象active/pending各峰值1。心跳p95=4.0478ms/max=18.6268ms。候选PERF-01另实测十秒484点，p95=22.0344ms/max=22.3524ms。 | 真实桌面/完整宿主生命周期及剩余协议/预算组合仍按清单逐项判定；不用旧序列数值替代新序列。 |


| P3-06独立Python扩展 | `examples/plugins/python_user_transform/` 与 `examples/python_extension_demo/README.md` 可用；六类真实参数、main/side同时消费、运行前系数冻结、错误恢复、代码更新后新PID/hash/行为、宿主EXE不变通过。初始独立21份WAV；最新runner下 `long-path/selected-extension-report.json` 只读核验实际通过两用例的19份WAV/NPY及plugin/runner快照hash，误差0；深目录CLI v1→错误→v2亦通过。 | 组件/CLI是自动化子集，不替代EXT-02真实桌面与打包。旧结果/日志/hash保持原归属。 |


| 混合固定帧及已知延迟 | F480→Python→F512六host与固定seed worker重放均有独立数值证明；实际 `examples/speex_python_demo/speex_python.aaproj` 可跑File→SpeexDenoise→Python→原生Output，锁定原库+独立公式误差0，区域offset959/final offset0/32037帧。当前直接原生回归保留真实Speex组件/CLI验收。 | 更宽异步、分支/区间组合未闭合；worker分区重放不等于新增UI/CLI随机分块选项。 |


| 混合有限尾部/EOS | ADR0034命名固定块逐输出2/5帧尾长已由ADR0035扩展到内部有界固定帧流式接口。六host及原生固定seed覆盖跨帧边界，短输出只额外驱动3帧但不扩展WAV/有效区间。长输出缺EOS拒绝发布、清理后同工程恢复，错误定位before/side。最新56份原生及26份Python/恢复WAV、raw/aligned、main/side NPY与代码/runner hash独立核验误差0，旧结果完整字节保留。 | 发布C ABI v1不支持此独立尾长/流式接口；未完整声明或未知能力仍拒绝。不同入口区间不隐式补齐，通用异步及完整借用组合后续验收。 |


| 累计候选及缺陷修复 | `RF0007-P3-candidate/debug-cumulative.json` 实际294/305；8项Temp访问、Speex worker图准备竞态、隔离副本漏SDK、旧共同装配守卫共11失败，均有定向Green（12/12与mutation-repair-check 1/1）。原JUnit落点/原missing记录保留，正式R6仍非通过。 | 原候选不改写成当前源码全量passed；新增时序后未再重复全量，最终Release必须Python/Speex ON并完整累计。 |


| 深目录runner缺陷 | 独立检查发现长路径NPY复制失败；真实独立进程Red→Green后规范Windows文件系统根，未改预算/时限。Python消费者 `long-path/python-consumers` 37/38，唯一self-modify夹具重复前缀已按原断言修复，同目标 `fault-recovery-green` 1/1；其余37项不重跑。 | 原非零集合仍保留失败状态，不把已过子集提升为整次passed。 |


| 构型与外部验收 | `stream-port-eos/` 重建Debug宿主、Python OFF CLI/目录/插件消费者及Runtime OFF纯计划；纯计划两构型各313断言通过，Python OFF负例/原生Gain和仓库外发布SDK消费者定向通过。桌面继续暂停，F04授权语音/F07真实同步阵列/设备及正式禁网环境缺项保留。 | AEC/DAS、注册资源/几何/指标、完整工作台、质量/设备/Release及离线交付仍未完成。 |





有效区间补验（2026-10-23）：`P3-mixed-timeline/valid-interval/target.json/xml` 对真实固定480 FIR→Python main/side→原生Gain链验证四种区间，8次运行、16432以上数值断言通过。输入区间 `{7,1018}/{7,1000}/{7,0}/{1025,0}` 在1025→1027帧后分别为 `{7,1020}/{7,1000}/{7,0}/{1025,0}`；空区间不制造有效样本。`independent-boundaries.json` 独立读取8份WAV、16份输出NPY、raw/aligned和代码/runner快照，误差0。现有实现无需修改，未制造Red；首次验收编译的字符串类型错误及独立检查容器比较/沙箱长路径失败均保留。该记录只证明部分/空区间；独立尾长与流式EOS由后续记录补齐。





端口尾长边界补验：内部SDK与纯计划变化后重新构建Debug编辑器/CLI/原生及Python测试、Python OFF CLI/目录/插件消费者、Runtime OFF纯计划；`port-tail/pure-debug` 和 `pure-runtime-off` 各253断言通过，Python OFF负例与仓库外发布SDK消费者各1/1。原候选累计报告保持原状态，不能提升为当前整平台passed。





随机mixed分块已接续：`P3-mixed-timeline/random/red.json/xml` 真实回调1024≠127；worker入口传递/预检/记录借用分区后 `green.json/xml` 通过。固定种子20261023、固定480 FIR→Python→固定512两次独立进程，独立重建分区并检查实际原生回调trace、两侧raw/aligned、main/side NPY与代码/runner hash，WAV字节一致、误差0。`admission-green` 以原生/离线工厂双计数验证0/-1/1025三类非法分区均在冻结前拒绝；`direct-regression` 和当前宿主 `default-speex` 通过，最新12份真实组件/CLI WAV独立核验。此入口仅显式worker重放，不新增UI/CLI随机分块选项。





下一条Red：从NA0023当前可构建快照继续Save As到新目录时的输入资源与精确包携带，原工程字节不变，新工程脱离原目录仍Run；普通Save后整体移动已验证。随后导出/历史重放、缺旧包历史恢复、多类型及慢运行/迟到预览、剩余Python六参数/完整矩阵与预算，再推进C++独立构建/probe/重启激活。全部68项保留，S0～S5均未正式通过；不重做NA-01.a，不扩无关P3，桌面暂停且不自动提交/推送/发布。





本轮详细循环与唯一命令记录：`build/reports/loops/P3-host-integration/P3-05/`、`P3-06/`、`long-path/`，`build/reports/loops/P3-mixed-timeline/{worker-prepare,fixed,known-delay,finite-tail}/` 及 `build/reports/loops/RF0007-P3-candidate/`。源码/二进制/解释器/快照/产物hash按各自执行记录归属；后续纯文档变化按实际产品影响复用，不改写旧日志或hash。





最新自动化出口汇总：`build/reports/loops/P3-mixed-timeline/stream-port-eos/summary.json` 绑定当前源码/Debug宿主、逐输出EOS/失败恢复/独立公式和定向消费者。`random/summary.json`、`port-tail/summary.json` 等保留原快照归属。完整累计与平台状态仍为非通过。





## 历史切片记录（以下“当前/下一条”均仅适用于各自快照）





- **2026-10-21 A3 / AR-14.c 所选端口反向裁剪已验证，进入 M21 出口**：工程模型新增端口级输出选择 `selected_output_port_id`（schema1 可选字段，legacy 工程往返不变；缺端口/方向错/类型错结构化拒绝，缺节点只报一次）。选择解析单点实现 `audio_runtime` 的 `evaluate_plan_selection`（端口级直接取 (node,port)，legacy 节点级仍要求唯一 sink 且匹配；BFS 反向闭包），计划编译/图渲染/RunService 源物化/资源重写/控制器全部消费同一闭包：闭外节点零准入零执行（probe 计数器全零）、闭外源零物化（pruned.wav 不存在运行仍成功、run.json 仅 1 资源）、闭内共享节点每块恰一次执行（split 计数器 1/1/ceil(513/host)）、闭外损坏连接仍报 `connection_source_port_missing`（裁剪不掩盖结构错误）；闭包内无 sink 时 JuceGraph 为所选端口补隐式 audioOutputNode。控制器 `set_selected_output_port` 单一 undoable revision，删除所选节点同时清空 node/port 两字段。五层定向 `[AR-14.c]` 全 Green（core 3 用例 6893 断言、pure plan 1 用例 43 断言、controller 1 用例 30 断言）；当前源码全量 Debug 矩阵 **core 140/140（297873 断言）+ 纯应用 11/11（140 断言）+ editor 34/34（891 断言）**，0 failed/skipped；CLI 组合根独立 oracle（纯 Python RIFF）双分支工程输出=2a、max_abs_error 1.48e-08、pruned 源未物化。证据 `build/reports/loops/RF0006/A3-selected-port-pruning/`（loop.md + 三组定向 + 三组全量记录 + oracle-verification.json）。**下一步 M21 出口：A0～A3 产品行为集中核对 + 双源 CLI/组件产物独立 oracle + Debug/headless/Runtime OFF/Release 四构型受影响集合**，随后 B 组任务卡（注册任务/类型边界/AR-14.d/AR-12.e）。桌面继续暂停。





- **2026-10-21 F1/T25/A2 三片已验证，进入 A3**：Review F1（第二输出 side 写 NaN 误报 main）以命名端口反查修复，诊断定位 side 且保留零分配与失败恢复契约；T25 双独立 WAV 源全链贯通（多源 `AudioSourceBinding` 成对预校验、纯计划多源、逐源资源快照/解码/重放，legacy 单缓冲入口对多源报 `source_binding_ambiguous`），负例（格式/帧数/通道/区间不匹配零工厂零 prepare）齐备；当时全矩阵 235/235 + 独立 oracle 6 用例误差 0。A2 生产 `builtin.mixer` 完成：算法核心 `libs/audio_algorithms/Mixer.{h,cpp}`（仅依赖 core，公式 10^(p/20)·primary+10^(s/20)·secondary，−60..12 dB）、薄适配 `plugins/builtin/MixerNodeAdapter`、注册进单一 builtin 目录（AR-05.e/AR-06.d 目录快照 4→5，属 A2 合法契约增长并已注明）；GRAPH-04 两测试 Green（加权角色 host{1,127,256}×接线、扇出上游每块一次执行、链式实例权重隔离、CLI 组合根运行+删原件+重放三路一致）；当前源码全量 Debug 矩阵 **137/137（290980 断言）**，独立 Python oracle 三路输出 max_abs_error 2.9e-08、捕获资源字节一致；插件规则以 apps+libs 清单对照证明通用文件零修改（仅新增 Mixer 算法核心两文件）。证据 `build/reports/loops/RF0006/T26-f1-multioutput-port-attribution/`、`T25-independent-native-sources/`、`A2-production-mixer/`（loop.md + full-debug-matrix-final + oracle-verification.json）。**下一条 Red A3：所选 (node_id,port_id) 输出选择与反向裁剪**——兼容旧 schema1 工程的输出选择迁移、从所选端口反向构造执行依赖（未选分支不创建处理器/不执行/不物化资源）、共享节点单次执行、结构校验不因裁剪掩盖损坏连接；随后 M21 出口（A0～A3 产品行为 + 双源 CLI/组件产物独立 oracle + 四构型受影响集合）。桌面继续暂停。





- **2026-10-21 Review 接续入口**：见 [当前进展 Review](review_architecture_progress_20261021.md) 与 [R5 下一开发任务](plan_agent_next_r5.md)。本次未改产品代码；核实 T25/red-build 的源码/二进制绑定并实际执行定向集合，**11 testcase：10 passed / 1 failed / 0 skipped**，唯一失败是双独立源被当前单源 RunService 拒绝，已有真实 Red。另以独立探针复现 **F1：第二输出 side 写 NaN 却把错误标为 main**；先修 F1，再完成已有 T25，随后生产 Mixer、所选端口裁剪及 R5/R6。验证器基础设施14/14通过；原始记录在 `build/reports/reviews/progress-20261021T052228Z/`。M20四构型全绿是旧冻结快照，不能当作最新工作树累计通过。桌面继续暂停，缺失证据保留真实状态，常规开发持续授权不变。以下各条为历史切片记录，不重复其旧“下一条”任务。





- **2026-10-21 T24 命名PCM端口切片已验证**：保留缺接口编译Red及接口补齐后的port_mapping_unsupported行为Red；通用bus/路由基址和预分配借用接口实现后4/4 Green，含24种frames/host/角色组合、每节点每块一次执行、连续状态、readonly与回调零分配。真实RunService双角色WAV逐样本独立oracle误差0；直接消费者66/66，Runtime OFF SDK消费者增量构建及16/16纯行为通过。详情T24-named-native-ports/summary.json。先前16项纯构建记录附错Debug目录的缺失二进制路径，保留原记录；pure-green绑定正确bin路径，不能把缺失路径当构建hash。首次契约选择在构建尚未结束时只执行2项，保留partial说明；构建结束后完整3项及新增文件流4项记录才是有效证明。AR14.b/c整体仍未通过：尚缺两个独立源、选择端口裁剪等。**下一条Red T25：两个独立WAV源按primary/reference角色合流，互换接线后符合2*A+3*B+frame/4096，物化并回放两份资源；先复现RunService/计划单源拒绝。** 后续生产Mixer、FIFO/状态/尾部/队列与R6。桌面继续暂停，not_verified。





- **2026-10-21 T23 / AR-14.a 原生计划切片已验证**：真实Red的typed/rate/fixed三种拒绝均已构造处理器（计数1）；最小前置准入后同目标Green，再把元数据编译提取为纯audio_runtime/NativePlan，由生产JuceGraph按显式任务/节点/命名路由投影，描述版本和默认参数为值快照。提取后4/4通过；runtime-off纯构建与身份/计划9/9、直接消费者/边界62/62通过，含本次真实CLI独立WAV误差0与资源/控件/worker/NED回归。见T23-ar14a-native-plan/summary.json和doc/native_execution_plan.md。AR14.a已有实际映射；当前仍单WAV/单sink/首端口组，未声称通用IO注册、多源/多端口、FIFO或R5累计通过。下一条Red **T24：一个源经双命名输出分流，再接双命名输入处理器，按端口地址连接后得到非零解析结果；先扩展借用端口接口并复现当前port_mapping_unsupported，再实现通用总线投影**。随后两个独立文件源、所选(node,port)裁剪、状态/FIFO/尾部/队列和R6。桌面暂停与R4指标缺项不变。





- **2026-10-21 T22原生运行期定位缺陷关闭**：三个受控目标真实Red共7条结构化字段断言失败；保留宿主instance ID，传递prepare参数/过程端口，缺省过程端口回退输入端口；同目标3/3 Green。新增真实RunService参数资源内容错误→定位gain/coefficient→修复后129帧输出恢复，连同原生边界/Graph、资源快照、真实控件/CLI和扩展范围集合21/21通过；零分配既有断言保持通过，编辑器/CLI/native测试增量构建完成。见T22-native-error-context/summary.json。AR07.e/10.c已补实际精确映射，M20和T21旧执行hash不重贴为当前证据。下一条Red **T23 / AR-14.a：编译器对不支持类型预检时不得构造处理器，之后把显式任务/原生区域计划抽入纯编译模块并由生产后端消费**；继续命名多端口、队列/FIFO及R5/R6，其余缺项/桌面暂停不变。





- **2026-10-21 T21 / AR-16.a 原生资源快照已验证**：prepare前物化WAV源与明确输入资源，保存原逻辑/默认参数工程与相对副本重放工程；原WAV/系数被替换或删除后真实执行仍使用固定版本，调用方模型改动不串入请求。正确行为Red为source_modified_during_run/coefficients缺失两分支失败，最小实现后同目标Green；两组直接下游回归33/33+7/7，编辑器/CLI/native测试增量构建完成。四份原生/重放WAV与两份真实内置CLI产物独立oracle误差均0；真实CLI在测试自有原始工程/音频删除后仍从副本成功重放，仓库示例hash不变。见T21-ar16a-native-resource-snapshot/summary.json和doc/native_resource_snapshots.md。早期日志宏与替换夹具错误均保留，不冒充产品Red。AR16.a新增实际映射；R5累计及headless/Release新切片未验收，M20仍仅代表旧冻结快照。下一条Red **T22：原生wrapper错误的结构化node_id/port_id必须随prepare/process失败保留并可恢复**，随后AR14命名任务/端口与其余R5/R6。桌面仍暂停，R4指标注册仍未实现。





- **2026-10-21 M20稳定里程碑已审计，接续T21 / AR-16.a**：冻结源码eb533ce1…的真实累计Debug221/221、headless220/220、纯应用79/79、Release221/221，0 failed/error/skipped。三构型七参数预设独立oracle最大误差2.98e-8，六份ChannelSwap组件/CLI输出每构型误差0。新增自动通过AR-04.e、AR-10.e/f、AR-11.a、AR-12.a/b/c；R0正式18passed，R4累计66passed/6not_run/2not_verified，清单/执行关联无错误。见 `build/reports/loops/RF0006/M20-r4-automated-milestone/milestone-summary.json` 与latest.json，首轮219/221失败和中止恢复均原样保留。桌面暂停、指标注册、平台/NED、R5/R6未闭合，不能宣布基本解耦/全部阶段通过。原生wrapper运行期结构化node/port丢失已在源码审查中登记T22，尚待受控Red。下一条 **T21：原WAV/系数资源在prepare前被替换/删除后，真实运行仍使用固定内容，并从保存副本重放**；先执行产品Red，再最小实现/同目标Green/受影响回归，不为helper重复完整矩阵。T21开始后源码不同于M20冻结快照，M20数量不当作新源码累计通过。





- **2026-10-21 T20采集节奏修复**：六个源码证据扫描改为遍历时剪枝，保留各工具原有排除集/排序/摘要格式；183文件清单与旧实际冻结记录完全一致，现树独立rglob对照亦等价。原扫描会访问大规模build后才排除，实际采集命令约8秒、剪枝扫描约0.02秒；此前超过一分钟是整个恢复进程观察，未独立计量冷态扫描，修正记录见T20/performance-observation-correction.json。真实“不得访问排除目录”Red→同目标Green及基础设施/NED验证12/12通过，无阈值/断言削弱。M20/recovery-sharedfix在完整测试前中止，已有冻结/构建记录和interrupted.json保留；新稳定候选完整累计入口为 **M20-r4-automated-milestone/recovery-pruned/summary.json**（未通过前不宣称累计通过）。产品能力与R4/桌面缺项不变；矩阵后立即推进T21 AR-16.a真实磁盘资源不可变快照。





- **M20首轮累计219/221，T20共享验收映射修复8/8 Green**：全部产品/数值/安装消费者/隔离/变异/NED自动回归通过，两个基础设施失败同根因：验证器未支持验收文档明确允许的共享事实。新增精确共享声明（测试名、全部AR引用、事实说明）并保留逐项桌面/失败判定；未声明/错引用/空理由/重复/陈旧声明继续拒绝。同目标Red→Green及8项基础设施回归通过，见T20-shared-acceptance-mapping。首轮M20日志/JUnit/源码hash原样保留；正在冻结修正候选并以最多两个独立构型并行执行完整累计，最终摘要位于M20-r4-automated-milestone/recovery-sharedfix/summary.json，未生成/未通过前不报累计通过。





- **当前RF0006/T18 R4扩展自动候选，T19通用诊断修复完成**：ChannelSwap纯算法/描述/adapter/注册、安装独立消费者、帮助和513帧双声道示例已实现。组件从真实节点库添加、连接、切换参数、Undo后运行；组件/CLI六份WAV独立oracle最大误差0，mono/四通道/错误参数按描述拒绝并定位。通用定位缺陷先用既有fixed/rate/ghost fixture独立Red→修复，11/11通过；在修复后未注册的新基线上恢复插件，59个通用产品文件无增删改。原冻结/失败均保留，不能声称最初基线无变化。T18组合相关7/7、安装消费者/范围/演示3/3。AR-04.e、AR-12.a～c已有实际自动映射；AR-12.d桌面暂停未验证，AR-12.e指标注册缺项使整项仍unassigned。当前完整累计尚未执行，不沿用M15数量。下一出口为 **M20稳定R3/R4自动候选累计矩阵**，然后推进R5命名端口/快照/状态等独立行为及R6，桌面不恢复。





- **当前已推进RF0006/T17 / AR-11.a，进入R4扩展准备**：Main仅保留应用/窗口/服务/插件组装和生命周期；画布、库、参数、波形、运行状态组件与外层IO/设备适配已分开，生产Workspace注入registry。新真实组件意图链与重建所有Debug下游后的受影响回归 **134/134**，0 failed/skipped；职责审查、JUnit与当前二进制证据见 `build/reports/loops/RF0006/T17-ar11a-view-components/`。这是保持既有行为的结构迁移，不制造人工Red；实际桌面仍暂停。R3自动行为候选稳定，但当前源码累计矩阵/桌面缺项未全过，不能宣称R3通过或基本解耦完成。下一条Red为 **R4 ChannelSwap未接入/未交换输出**，先保存通用产品文件清单与hash，接入只改允许范围；R4出口集中执行R0～R4全部累计验证，再继续R5/R6。





- **当前已推进RF0006/T16 / AR-10.e**：受限类型化条件控制参数可见/启用状态，合法编辑及Undo后同步；非法条件描述原子拒绝；未知嵌套结构只读显示并说明，实际保存重开保留值。真实编译Red、组件行为Red和同目标Green齐全，受影响参数/注册/持久化/路由集合 **17/17**，Debug编辑器和测试增量构建成功。见 `build/reports/loops/RF0006/T16-ar10e-conditions-and-opaque/loop.md`。AR-10.a～g当前均有自动行为映射和Green，当前新源码全量累计仍待下一稳定里程碑；桌面AR-10.h保持暂停/未验证。下一实现为 **AR-11.a 剩余View职责拆分**，随后R4 ChannelSwap、R5、R6，常规阶段继续授权有效。





- **当前已推进RF0006/T15 / AR-10.f**：通用摘要已替换画布Gain/File类型分支，使用描述标签、默认值、单位和稳定枚举显示标签；完整长路径/中文值保留在tooltip。真实Red为11断言中10失败，同目标与参数/文字/几何/路由集合Green **16/16**，编辑器增量构建成功。证据 `build/reports/loops/RF0006/T15-ar10f-descriptor-summary/loop.md`。下一实现Red为 **AR-10.e 条件参数与未知结构只读**，随后AR-11.a与R4～R6；未恢复桌面交互。


- **M15此前冻结快照已正式汇总**：Debug212/212、headless211/211、纯应用76/76、Release212/212，0 failed/error/skipped；三构型七预设真实产物独立oracle均通过，最大绝对误差约2.98e-8。R2累计54passed/1not_verified；R3累计59passed/8not_run/1not_verified，清单/证据关联无错误。详情为 `build/reports/loops/RF0006/M15-parameter-milestone/summary.json`；首轮8项系统Temp权限失败与空筛选记录均保留，workspace TEMP同目标复测及完整矩阵随后通过。**T15源码已不同于M15冻结快照**，当前只对T15受影响集合报告Green，不把旧完整矩阵数量当新源码全量通过。





- **RF0006/T12～T14 稳定候选，集中累计验收入口**：生产Inspector已从Main提取；AR-10.c数值文本严格拒绝与错误恢复、后端独立拒绝/资源定位，AR-10.d描述默认恢复/历史/控件同步，AR-10.g六控件→controller→真实WAV/报告闭环均有真实Red和同目标Green。T14相关集合18/18、T12最终拒绝集合5/5；七份实际产物由独立Python按固定倍率、原容差核对，最大绝对误差约2.98e-8，tag只变报告，参数快照及输入资源hash已核对。证据分别位于 `build/reports/loops/RF0006/T12-ar10c-validation/`、`T13-ar10d-default-reset/`、`T14-ar10g-component-render/`。这些数量只代表切片执行，当前新快照累计矩阵待执行，不能沿用T11数量宣称本轮全套通过。


- 下一实现出口：**AR-10.e 条件字段/未知结构只读、AR-10.f 通用描述摘要、AR-11.a View职责拆分**，随后R4 ChannelSwap扩展证明并持续R5/R6。最新累计里程碑报告将写入 `build/reports/loops/RF0006/M15-parameter-milestone/summary.json`；报告不存在或有失败时不得视为通过。R3桌面/设备项和其他平台/NED缺项仍保持not_verified，未经用户明确恢复不尝试桌面操作。





- **恢复入口已推进至 RF0006/T11 / AR-10.b**：恢复审计逐文件确认原快照源码无变化，记录中 Debug/headless binary hash 均与现有文件相符；实际 JUnit 为 Debug **208/208**、headless **207/207**、纯应用 **76/76**，0 failed/error/skipped，Debug/headless 均确有 AR-10.b 执行。T11 交接与审计见 `build/reports/loops/RF0006/T11-ar10b-node-instance-binding/loop.md` 和 `resume-audit-20261020.json`。下方T10数量及报告是历史快照，不是恢复开发起点。


- 用户本次已明确恢复持续开发；被Escape中断的桌面交互仍暂停，未经明确恢复不重试，对应项保持 `not_verified`。当前下一行为 **T12 / AR-10.c 参数错误诊断与恢复**；随后默认恢复/历史同步及真实组件到运行产物闭环。文档候选更新后的新快照将在稳定里程碑集中构建与验收，旧日志不重贴hash。





- 用户已[一次授权持续完成全部重构](agent_full_refactor_authorization.md)：R2后自行推进R3～R6，自验证、自迭代直至全部累计验收通过，常规阶段无需确认。本条授权不代表任何新增功能已实现或验收通过。


- 按 [下一阶段 Agent 任务与自验证闭环](plan_agent_next_r2.md) 接续 RF0006。


- **RF0006/T10 / R3 AR-10.a 累计验收快照**（源码树 hash `0ef687c2a7c359e0283b41d414f8c051fe2fe941f8738f30ca01aadacab12833`）：真实 JUCE `ParameterPanel` 按描述符生成 bool/int/float/enum/string/path 控件，通用 inspector 通过选中节点快照和 `EditorController` 路由；测试检查真实控件类型、标签、默认值、范围/步长、单位/help 与 enum 稳定值。AR-10.a Red/Green、AR-06.b 数值步长 Red/Green 和定向路由/NED 回归记录在 T10 目录。


- 同一源码快照下 Debug CTest **207/207**、Headless **206/206**、runtime-off 应用构型 **76/76** 均为 0 failed/skipped/timeout；Release 的 AR-06.b、AR-10.a、NED-11 全子用例及 UI-00 **12/12**。Debug、Headless、纯应用、Release 全目标构建均成功。累计 JUnit/命令/二进制 hash 均在 `build/reports/loops/RF0006/T10-ar10a-descriptor-parameter-controls/`。


- AR-01.a–d 在当前 Debug/Headless/runtime-off 库存下通过。AR-04.a/b/c 三个独立真实消费者构建测试、AR-05.d 隔离内核突变并恢复及 NED-12.a–d 均在对应完整套件实际执行通过。当前 AR-02.g G0 failure-propagation 探针先让 Gate 因真实断言失败退出，再恢复 fixture 后 Gate 通过；源绑定证据 `build/reports/gates/G0/failure_probe-20261020T054741.112086Z.json`。


- 当前 R2 独立验收报告 `build/reports/architecture_refactor/20261020T054851.331507Z/report.json`：**54 passed、0 failed、0 not_run、1 not_verified**。唯一未核验为 AR-05.b 真实桌面运行数值与截图证据。当前 R3 报告 `build/reports/architecture_refactor/20261020T054915.974991Z/report.json`：**55 passed、0 failed、12 not_run、1 not_verified**；AR-10.a 已通过，剩余 12 项是尚未实现的 AR-10.b–h/AR-11 行为，AR-05.b 的外部证据缺项也继续挂账。两份报告均使用上述 T10 源码快照。


- AR-05.b 真实编辑器证据尝试已记录：Computer Use 两次 `launch_app` 超时；经获准的本地进程创建后，编辑器进程73804和窗口8726684确实存在，但截图调用先后以 `FrameArrived timed out`、`window capture timed out` 失败。没有截到画面，AR-05.b仍为`not_verified`，记录 `desktop-capture-attempt.json`；不把窗口存在等同于GUI数值验收。


- R2自动/纯应用行为继续通过；因AR-05.b缺少真实桌面截图，阶段报告仍未通过。T10 已按外部证据不足时继续独立工作的授权完成 AR-10.a Red/Green 与累计验收，保留桌面复测待办并继续 R3 自动行为。


- RF0006/T01 参数 Undo 差量恢复、T02 controller 手势事务、T03 runtime-off 纯应用测试 target、T04a–T04f controller 命令切片均已实现并通过各自行为 Green；AR-08.a 仍待生产视图路由，AR-08.c 整体仍未通过，因为生产 GainInspector 尚未接入手势 API。


- T03 证据：`build/reports/loops/RF0006/T03-pure-application/`。runtime-off Debug 累积 CTest 为20/20、0 failed/skip；独立边界守卫3/3。应用 target 链接行无 JUCE。AR-09.a仍未通过：Open/Save、资源和播放的无JUCE服务注入尚缺。


- T04a AddNode 通过纯controller用例检查完整工程、默认参数、revision、dirty/stale、Undo/Redo及拒绝时redo保留；runtime/editor构建通过，工作区Temp下NED回归69/69。详情见 `build/reports/loops/RF0006/T04a-add-node/`。


- T04b Connect/Disconnect 复用共享端口校验及纯断开helper，拒绝保持完整模型和undo/redo，错误诊断含节点/端口，且不能与参数手势交错。纯回归22/22，runtime/editor构建通过，NED回归69/69。详情见 `build/reports/loops/RF0006/T04b-connect-disconnect/`。


- T04c 将新库节点插入接入纯controller，检查所选端口、原边替换、旁支保持、Undo/Redo及拒绝原子性；runtime/editor构建通过，纯回归23/23，NED回归69/69。详情见 `build/reports/loops/RF0006/T04c-insert-node/`。


- T04d 将既有未连接节点插入接入纯controller，检查稳定ID和多类型参数保留、仅按请求更新位置、分支保持、Undo/Redo及失效/已连接目标原子拒绝；纯回归24/24、NED 69/69、runtime/editor构建通过。详情见 `build/reports/loops/RF0006/T04d-existing-node-insertion/`。


- T04e controller DeleteNode 已完成真实 Red→Green：直线 block 可合法旁路、分支删除不误连、删除所选 Output 清空选择且 Undo 恢复；拒绝路径保留完整工程、历史与 redo。纯回归27/27、NED 69/69、桌面编辑器构建通过。NED 首次沙箱启动失败的两份记录已保留；提升权限后单项及完整套件通过。详情见 `build/reports/loops/RF0006/T04e-delete-node/`。


- T04f controller MoveNodeLayout 实现共享纯命令和坐标专属历史项；行为检查覆盖 revision/stale、参数历史交错、无效/重复 ID、NaN、手势冲突和非布局字段保护。冻结快照 Debug CTest 为171/171、runtime-off纯应用31/31、NED69/69、边界守卫3/3；AR-01.a/c/d均通过。首轮完整Debug CTest为148/153，暴露源码/库存哈希过期及AR-04.c漏建纯editor application测试目标；未跳过必需测试，已修复并刷新映射。详情及失败记录见 `build/reports/loops/RF0006/T04f-move-node-layout/`。


- T05a 异步文件选择器生命周期已完成一个可验收行为：旧A chooser回调不能取走新B chooser的completion。真实Red复现旧回调改写B路径/状态并消费B回调；token修复后8断言通过。冻结快照Debug CTest 172/172、runtime-off纯应用32/32、NED69/69、边界守卫3/3，当前Debug/Headless构建和AR-01.a/c/d源码绑定校验通过。证据与JUnit见 `build/reports/loops/RF0006/T05a-async-file-chooser-completion/`。这只覆盖AR-09.c的chooser路径，不表示AR-09.c或R2通过；真实桌面原生chooser及View/worker关闭生命周期仍待验。


- T05b RunService 注册表生命周期 lease 已完成一个可验收子行为，AR-09.d 整体仍未通过：真实 Red 先因共享 owner 构造器缺失而编译失败；加入故意借用 owner 的接口后，受控 worker 在释放编辑器侧 owner 后观察到 registry 已过期，5 项断言中 1 项失败。修复将 shared owner 从 EditorComponent 传入 RunJob，再由 RunService 持有；同步引用构造保留给同步调用。worker 实测完成 Gain 渲染，join 后 weak registry 过期。测试需泵送真实 JUCE 消息循环，避免非消息线程离线图异步重建的等待。最终定向 Green 为39断言通过，并核对实际增益6.0dB、48kHz、双声道、513帧、127帧块，run identity/manifest/completion字段及 SHA-256。最终源码绑定的产物位于 `build/reports/loops/RF0006/T05b-run-service-registry-lease/artifacts/worker-run-final/`，Red、构建、Green与日志在同目录。首次完整 Debug CTest 为170/173，只有 AR-01.a/c/d 因环境快照、Headless legacy inventory 与当前源码 build records 未刷新而失败；刷新后 AR-01.a/c/d 为3/3，纯 application 32/32、边界守卫3/3、NED69/69、完整 Debug CTest 173/173，0 failed/skip。最终可重跑记录为 `build/reports/loops/RF0006/T05b-run-service-registry-lease/debug-full-final-current-source-3.json` 及同名 JUnit。首次失败记录保留。未覆盖 View 销毁后丢弃迟到回调及超过5秒的 pool teardown/reclaim。


- T05c 生产完成投递生命周期切片已验收：首次 Green review 发现 JUCE `WeakReference` 明确不支持跨线程访问，故未保留 worker 跨线程复制 `SafePointer` 的设计；最终适配器使用共享失效状态，worker 仅传递弱令牌，组件指针只在消息线程读取，析构先原子失效。真实 JUCE 消息循环 Green 为7项断言通过，活组件在消息线程收到一次回调，销毁组件的排队回调被丢弃。Debug/Headless 全目标构建、runtime-off纯应用32/32、边界守卫3/3、NED自动用例69/69、AR-01.a/c/d 3/3及完整Debug CTest 174/174（0失败、0跳过）通过。首次AR-01.d因构建记录漏附二进制哈希失败，补齐真实产物哈希后独立复验通过；失败记录保留。源绑定报告与JUnit见 `build/reports/loops/RF0006/T05c-safe-completion-dispatch/`。这只覆盖迟到投递，不表示AR-09.d或R2通过。


- T05d 异步池关闭/回收切片已验收：锁定版 JUCE 源码确认 `ThreadPool` 的5秒 remove timeout 后还会对线程做有限 stop，可能强制终止 job；在 message thread 等待也会挡住 RunService 所需的JUCE消息。真实 Red 因 `EditorWorkerPool` 生产适配器缺失而编译失败。15断言受控 Green 让活动 job 阻塞5200ms，在5000ms timeout之后继续等待期间投递并等待 message-thread 握手；并验证取消信号、队列 job 删除、completion target 失效、registry/job回收、pool销毁及UI线程完成回调。实现将 drain 移到独立 shutdown thread，排空后才退出应用；UI-00 smoke 已通过 `systemRequestedQuit` 验证同一入口。当前源码 Debug/Headless构建、纯应用32/32、边界3/3、NED69/69、AR-01.a/c/d 3/3、三个AR-09.d行为56断言及完整Debug CTest175/175（0失败/跳过）通过；JUnit与源/二进制hash在 `build/reports/loops/RF0006/T05d-worker-pool-shutdown/`。这只完成一个AR-09.d行为切片，整体manifest行仍未标passed。


- T05e 生产 RunJob/关闭 View 切片已有真实 Green：首条编译 Red 确认没有可直接测试的生产 RunJob API。抽取后的 `EditorRunJob` 同时供 `Main.cpp` 和原生测试使用；测试让一个真实 Gain 任务先完成，再让第二个真实 RunService 任务在自定义 block processor 中暂停，关闭并销毁其 View，随后继续处理。验收核对 48kHz、双声道、513 帧、127帧块、RunIdentity、输入/输出 SHA、实际 WAV 样本和波形摘要；关闭 View 的运行仍产生有效产物，迟到投递未触达 View；存活 View 的结果在消息线程到达，registry 在 worker回收后释放。focused test 60断言通过，JUnit与六项 WAV/manifest/completion 产物、完整构型和累计验收命令记录保存在 `build/reports/loops/RF0006/T05e-production-runjob/`；`summary.json` 汇总该快照的实际源码/二进制哈希、inventory、退出码、失败和跳过数。若汇总缺失或源码哈希不匹配，不得把该切片视为累计验收完成。AR-09.d仍是部分完成，不标父项通过。


- T05f chooser 关闭生命周期切片：先加受控回调回归，Debug 编译因生产 completion slot 缺少永久 `close()` 能力而以 C2039 失败；新增关闭状态后，关闭会撤销待处理 token 并拒绝新 chooser。`Main.cpp` 在异步 shutdown 开始和析构时立即 close；工程替换仍走可继续创建 chooser 的 `invalidate()`。取消、工程替换后的旧回调、关闭期间迟到回调及新 View 重开隔离由3个纯应用行为用例覆盖，共18断言；Debug/Headless 的定向 AR-09.c 各3/3，UI-00启动退出1/1，Debug/Headless/Release/runtime-off构建通过。累计冻结回归、二进制/JUnit哈希与记录索引见 `build/reports/loops/RF0006/T05f-chooser-shutdown/summary.json`。AR-09.c 父项仍未通过：manifest 保留尚未实现的延迟 service 通知与删除节点/View 快照用例。


- T05g 删除节点后的迟到运行通知：当时的行为测试由真实 `EditorController::delete_node` 递增 revision、移除 Gain 并置 `results_stale`，再调用生产 `RunReception::accepts` 身份门禁；旧完成不改变选择、结果路径、波形、状态或播放资格。该纯状态测试1用例/10断言通过，源码路径核对确认 `Main.cpp::run_finished` 在任何展示更新前执行门禁。T05g时的 AR-09.c 快照与生产路由缺口由后续切片继续处理。证据见 `build/reports/loops/RF0006/T05g-delete-stale-service/`。


- T08 / AR-09.e 异步文件工作切片：新增 UI worker job，完成投递只在 JUCE 消息线程发生；`EditorProjectDocuments` 保存/打开完成接口用 session identity 与 document generation 拒绝过期结果。Save/Open、演示包复制、源路径引用/重定位均已接入工作池；Run 不再在界面线程探测资源。AR-13.d真实 Red证明缺失资源诊断的node_id/port_id为空，修复后 file/path 定位断言通过（48断言）；runtime-off纯应用构建成功，独立AR-08/09/13与R2集合41/41。真实 Python 子进程运行10秒期间，20ms消息循环心跳采样487次，p95间隔21.31ms、最大22.15ms；原生 AR-09.e、PERF-01及异步路由四项均通过。修正旧同步路由断言后，Debug R2 累计集合56/56通过、0失败/跳过；NED全套自动用例69/69通过。随后为 AR-09.c 新增生产 `run_finished` 身份门禁/工程替换路由守卫（2/2），因此这组56项累计记录不含新守卫，需随下一次全量回归刷新。首次累计记录保留了3项过期静态守卫失败，修订后同集合通过；最终源文档更新后的二进制/JUnit/源码绑定记录待刷新。平台桌面/DPI/真实设备证据尚未验证，R2仍未宣布通过。详细命令与测量见 `build/reports/loops/RF0006/T08-message-thread-io/`。


- T09 / AR-09.c 真实迟到运行通知：先以缺少可直接验证的生产身份门禁编译失败作为 Red；抽出 `apply_if_current_run` 并由 `Main.cpp::run_finished` 实际调用。真实 `EditorRunJob` 在 worker 中运行保存的 File → Gain → 阻塞节点 → Output 快照；消息线程收到 processor 已进入阻塞的握手后，通过 `EditorController` 删除 Gain，再放行原运行。controller 实际 revision 4→5、dirty/results_stale 均置位、Gain 消失且剩余3节点/2连接；旧运行仍生成48kHz、双声道、513帧的 WAV 和 run/complete 文件，SHA-256 为 `ccfba2aa949a2161579ab89833ad2ec22b092b6ae3a8e01fc65871643e9b6cbf`，三组样本与独立 +6dB oracle 相符。生产完成 handler 未调用、未查询已删节点，先前结果ID、WAV路径、状态、波形与播放资格逐项不变。定向JUnit 1/1、51断言；生产路由守卫3/3、110项manifest结构验证12/12；Debug原生测试与编辑器构建通过。持久产物及Red/Green记录见 `build/reports/loops/RF0006/T09-delayed-run-notification/`。


- 基线跟进：AR-01.a的四个preset核对现已通过；AR-01.d递归查找任务子目录的源码hash/构建命令/产物校验也通过。完整Debug CTest发现AR-01.c的Headless映射来自较早的200项库存，而最近配置出的纯应用测试目标使实际库存成为205项；差异是5个已注册且实际构建的AR-08.e/AR-09.a/e测试，旧集合无丢失。映射已刷新并通过。第二轮完整Debug CTest为205/206通过，AR-01.a在完整回归末段被10秒CTest进程上限截断（日志耗时10.04秒），其独立执行约7.4秒且断言通过；第三轮仍为205/206，AR-01.d在完整回归中被10秒上限截断，其他行为及长时隔离测试通过。两项都需重新计算完整源码hash并检查多份真实证据，现将AR-01.a/d纯证据采集进程上限校准为20秒，不改断言、产品时限或验收阈值；失败记录保留，重采环境/build hash后再跑完整回归。


- 历史下一步 Red（已由T11完成，勿重复）：R3 / AR-10.b。验证两个同类型、不同 node_id 的实例时，改动必须仅写入当前选中 node_id+parameter_id；切换节点或重建面板不得触发旧控件回写或把旧节点值带入新节点。AR-05.b真实桌面数值与截图仍为`not_verified`；平台 DPI/真实设备证据不得由后台测试替代。新切片代码修改后按源码 hash 重采受影响构建和累计证据。


- 各切片证据按具体源树 hash 关联；代码或源文档变更后，需按对应范围刷新环境、构建绑定和累计验收记录。





## R2 首片（RF0006，2026-10-18）：无控件的参数命令





- 新增真实 target `audio_editor_application`（`libs/audio_editor_application/EditorController.*`），


  只依赖 `audio_project`/SDK，无 JUCE、无 View、无文件对话框；`EditorTopologyHistory` 迁入该层，


  `apps/editor/EditorTopologyHistory.h` 降级为兼容转发头（注明移除切片）。


- `EditorController::set_parameter` 成为唯一参数写入路径：走 `validate_parameter_value` 共享 schema，


  经 `history_.apply` 应用 → 一次编辑恰好一个 revision，并维护 `dirty` / `results_stale`。


- **AR-08.b 已实现**（76 断言）：合法修改一个 revision 且可追踪实际值；相同值 no-op；


  越界/类型错/未声明 enum/未知参数/未知节点全部拒绝且不改变模型、历史、revision；


  保存只清 dirty，过期标记保留到重新运行；`adopt_project` 清历史与两个状态位。


  Red 形态为“接口不存在”的编译级失败（`EditorController.h` 原本不存在），未把运行期断言说成 Red。


- 新增 `R2_editor_application_boundary_guard`（3 检查）：应用层不得 include JUCE/View/IO/Service，


  controller 必须使用共享 schema 与 history，CMake target 不得链接 JUCE。


  该守卫**不映射**到任何必需 ID，避免用部分证据宣称 AR-09.a 通过。


- RF0006/T01 修复 `EditorTopologyHistory` 的参数恢复：Undo/Redo 只还原该历史项实际改变的参数键，保留其他参数，并精确保留键原先缺省/不存在的状态。正式 Red 中 Undo 后实际值仍为 6.0，期望 0.0；修复后固定事件表 reducer 检查完整工程、revision、dirty/stale 与历史栈。


- T01 累积验证：Debug 最终记录 `final-snapshot-regression.json` / JUnit 13/13，0 failed、0 skipped，覆盖 AR-08.b、参数历史类型/缺省键/多实例以及 NED-11.a–g；NED-11.d 确认拓扑 Undo 继续保留后续参数与布局变化。当前二进制 SHA-256 `31d8189f7849bda83b6fd434090824efd8fd374f0812600a7569a2bfe3f80be4`。测试源树 hash 见对应 RF0006/T01 命令记录。


- controller 参数历史和手势事务用例均通过；生产滑块尚未调用 controller API。AR-08.c manifest 暂保留 `unassigned/not_run`，避免把应用层部分覆盖记成完整通过。`GainInspector::apply_gain()` 仍直接写模型，后续需将 `EditorComponent` 工程所有权移交 controller；AR-08.a、AR-09、AR-10/11 未完成。


- RF0006/T02 新增 `begin/update/commit/cancel_parameter_gesture`：更新只暂存并共享参数 schema 验证；commit 合并为一条参数差量历史，cancel 不写模型。有效手势期间拒绝普通参数命令、Undo/Redo 和嵌套手势；New/Open 接管工程时清理手势。


- T02 行为 Red 观察到 3 次 update 将 revision 3→4→5→6、建立3条历史，并且 cancel 后状态仍变化；修复后最终冻结记录 `final-snapshot-regression.json` 15/15 Debug 回归通过（AR-08.b、当前 AR-08.c 部分行为、NED-11.a–g），0 failed/skip。源树/二进制/JUnit hash 见该命令记录。生产 `GainInspector::apply_gain()` 仍直接写模型，因此 AR-08.c 完整验收仍挂账；AR-08.a、AR-09、AR-10/11 未完成。








## R1 续接（RF0005，2026-10-18 第三轮）：两轮 Review 的六项执行/隔离缺陷





证据目录 `build/reports/loops/RF0005/`。上一轮 F1–F5 只是首轮 Review；本轮处理


`build/reports/reviews/framework-20261018T040137637400Z/review.md` 与


`build/reports/reviews/continuation-20261018T042655330487Z/review.md` 的剩余项。


**每一项都先有独立失败断言，再修复，再回归**：





| 缺陷 | 正式 Red | 修复 | Green |


| --- | --- | --- | --- |


| F1 快速块接口绕过非有限输出校验 | `AR-07.e block output must be finite and prepare must not throw` 在 NaN 断言处失败 | 块路径在接受结果前无分配扫描输出，NaN/±Inf/有限运算溢出全部拒绝，诊断带节点与输出端口并清空该块 | 138 断言通过 |


| F2 固定帧节点收到短尾块 | `AR-06.e fixed-frame tails…` 在“257 帧必须被拒绝”处失败 | 原生区域执行前要求 `frames % fixed == 0`，无固定帧适配时明确拒绝 | 6 断言通过 |


| F3 插件 prepare 异常逸出 | `AR-07.e a throwing prepare…` 两条断言失败（`prepareToPlay` 抛出） | prepare 包 try/catch → `node_prepare_exception`（带节点定位），并 reset 清理半准备状态 | 7 断言通过 |


| F4 变异隔离目录可被并行构型互删 | `test_ar05d_isolation.py`：`AttributeError: create_isolation_root` | 改用 `tempfile.mkdtemp` 原子创建唯一短目录；`reserve_isolation_root` 对已存在目录直接报错，绝不删除他人目录 | 6 用例通过 |


| N1 无块能力的节点仍进分配式回调 | `AR-07.d the prepared callback allocates nothing for generic nodes too` 在 owned-only 断言失败 | 原生回调**删除分配式回退**；无块能力的实例在 prepare 阶段以 `block_processing_unsupported` 拒绝，回调永不执行它 | 10 断言通过 |


| N2 非音频端口被当作 PCM 执行 | `AR-07.e non-audio ports are refused by the native boundary` 失败 | wrapper 构造期与图编译期逐端口要求 `audio.pcm@1`，否则 `port_type_unsupported` | 8 断言通过 |





- **通用节点零分配补测**：`AR-07.d` 现在同时覆盖 Gain 与一个**非 Gain** 的通用块节点


  （两者回调均 0 次分配），并证明 owned-only 节点被拒绝时回调分配为 0；


  不再用 Gain 单点结果代表整个 wrapper。


- 因删除分配式回退，`AR-07.a` 的测试夹具 `OffsetNodeProcessor` 改为实现借用块路径——


  这是被收紧的产品契约所要求，而不是放宽断言；`AR-07.a` 仍通过 1028 个断言。


- 修复后 Debug 全量只余 `AR-01.a/c/d` 证据刷新项失败，无产品回归。








- 续接 [架构重构交接](handoff_architecture_refactor_20261018.md)。证据目录 `build/reports/loops/RF0004/`


  （`loop.md` 为完整 Red→Green 记录）。未提交、未推送；全部已修改与未跟踪文件均保留。


- **首轮 Review F1–F5 修复（仅指 `refactor-20261018T021553919118Z`，不等于全部 Review 闭环；


  另两轮的六项见上方 RF0005）**：


  - F1 故障注入改为“源码副本 + 独立 build”。变异只写隔离目录；活动


    `Gain.cpp` 与 `audio_algorithm_tests.exe`/`audioalgo_tests.exe`/`audio_cli.exe`


    运行前后逐一哈希相同（`active_tree_unchanged: true`）。基线 3 oracle 通过 → 变异 3 oracle 全部以


    退出码 42 捕获 → 还原后 3 oracle 通过。新增常驻用例 `AR-05.d_isolation_guard`。


  - F2 桌面证据分支先用例执行状态再验证据，截图与可执行文件必须真实存在且哈希匹配。


    pre-fix 快照把 `passed/skipped/failed` 全映射成 `passed`，当前为


    `not_verified/skipped/failed`（`f2f3-adversaries.log`）。


  - F3 新增 `acceptance_stage_map()`，按验收文档第 2 节表格逐条比对 ID→阶段与每阶段数量；


    把 `AR-03.a` 移到 R6 会被拒绝。


  - F4 `NodeSDK` 新增共享 `validate_parameter_value()`，描述默认值与工程实例共用同一 schema；


    Review 的同一 `probe.cpp` 现在输出 `invalid_enum_accepted=false, diagnostic_count=1`。


  - F5 主 Gate 逐用例解析状态、计数互斥；必需用例 skipped/not-run/timeout 直接失败；


    `UI-01/02/04/07` 需 `--evidence` 提供哈希校验过的截图与可执行文件。


    受控 skip fixture 修复前 `exit=0/status=passed`，修复后 `failed`。


- **AR-06.d 证据闭环**：继承既有迁移，未重造 Red；全量回归通过并已绑定 manifest。


- **R1 剩余契约已实现**：`AudioNodeProcessor` 增加 `prepare/reset/process_block` 与借用块视图


  `AudioBlockView`；wrapper 在 prepare 一次分配通道指针表与输入暂存区，处理回调只做指针填充和


  `copy_n`。新用例断言数：AR-05.e 1301、AR-06.e 14、AR-06.f 405、AR-07.b 16842、AR-07.c 15、


  AR-07.d 4（**处理回调零动态分配**）、AR-07.e 138。


- 修复既有回归：`NodeExecutionCapabilities` 的输入/输出上限由固定 1 改为 `std::optional`


  （未设置按声明端口推导），`NED-02.d`、`NED-10.f`、`NED-10.g` 的多端口描述不再被默认上限拒绝。


- **累计回归口径修正（对应 framework Review 的 F5）**：上一稿把“Release 146/146、0 failed”写成结论，


  但当时的 `release-final-ctest.json` 实际是 exit 8、`release-junit.xml` 为 144/146，两项失败是


  `AR-01.a`/`AR-01.d` 的证据不对应。失败记录与日志全部保留，未被覆盖。


  现在的规则是**只引用最终完成记录并按其 JUnit 汇总**，不再写预期数量。


- RF0005 当时的最终记录在 `build/reports/loops/RF0005/`：


  Debug / headless / Release 各自的 `*-current-final-build.json`、`*-final-ctest.json`


  与对应 `*-junit.xml`。该轮 inventory 为 Debug 151、headless 150、Release 151；


  三者 CTest 记录均 `exit 0`、`0 skipped`，通过数以各自最终 JUnit 为准。


  加入 R2 首片后 inventory 会再变，最终数字见 `build/reports/loops/RF0006/`。


- **验证器结果以 RF0005 报告为准**：R0 与 R1 的阶段报告路径写在本文件末尾“Next Red”之前的最新


  运行记录中；R1 仍为 19/20 自动通过，唯一 `not_verified` 是 AR-05.b 的实际 GUI 运行证据。


- **GUI 证据尝试（诊断，不计验收）**：编辑器已能在后台启动（未抢占前台），UIA 树可读、


  `Run` 可由 UIA 调用；但 GUI 运行被沙箱拒绝写入其产物目录，状态行显示


  `run_directory_create_failed: Access is denied.`。截图与哈希记录在


  `build/reports/loops/RF0004/ar05b_gui_attempt.json`（`scope: diagnostic_only_not_acceptance`）。


  未修改产品代码，AR-05.b 保持 `not_verified`。


- **环境前置条件（必须保留）**：本会话 `pwsh` 在默认 sandbox 下任何进程启动返回 `0xC0000142`，


  按工具规程以单次 `danger-full-access` 升级执行；`%LOCALAPPDATA%\Temp` 下旧 sandbox 会话遗留的


  `AudioAlgo *` 草稿目录会被当前文件沙箱拒绝写入，导致 11 个使用 `temp_directory_path()` 的用例报


  `create_directories: Access is denied`。把 `TEMP`/`TMP` 指向工作区内 `build/tmp` 后全部通过；


  属环境问题，未改测试、未放宽断言。GUI 运行的产物目录同样受该沙箱限制。


- **R1 未宣布通过**：20 个 R1 子用例中 19 个自动通过，AR-05.b 的验收文本要求“实际 GUI 运行”证据，


  仍为 `not_verified`。R0 已在本轮重新验证。平台 G0–G6 与 NED 桌面缺项保持不变。





## Previous implementation record (2026-10-18; historical, before the handoff above)





- Active sequence: R0–R6. **R0 passed** on source hash `424f758e09d993982a9ac673354dd2e2d9e98c755f529f98a01dcc379def08e0`. R1 is in progress; AR-05.d and AR-06.a–c have focused Red/Green evidence, and a generic-wrapper AR-07.a prototype passes its focused oracle. Production graph migration remains open. No platform Gate or full R1 pass is claimed. The checkout is based on Git commit `98fe518926cbca77c24b70245ed616bf9e9c7e16` with local changes. Refreshed toolchain and dependency details are in `build/reports/environment.json`.


- Before behavior changes, the rebuilt Debug/headless baseline passed **88/88** and **87/87**. Current full Debug/headless suites passed **106/106** and **105/105**; Release built successfully and its complete inventory is **106** tests. Baseline and final logs/JUnit, current source manifests, and executable hashes are recorded in RF0000 and RF0002.


- R0 frozen legacy fixture: `build/reports/loops/RF0000/legacy_fixture/` contains read-only copies of the existing Gain project and PCM16 input. Project SHA-256: `767b18ddd84911c68a04dbfee073a73212851f4812414078dbb2fde7b248b76d`; input SHA-256: `156b5229f494b119bc604d0736ae89149e5665bb96cd00da015efd2df1df4c3e`. A real CLI run produced 96,000 frames (`run_id=dcf8448e08934e158eccf475d161650d`); the independent float64 oracle passed 192,000 samples with maximum absolute error `2.3616929079040005e-08`.


- AR-03.a produced the required Red: projects A and B both had revision 7, and the revision-only predicate allowed A to replace B. The stale-binary attempt is preserved as failed evidence. After rebuilding, AR-03.a–f and the real RunService/message-thread AR-03.g passed in the 102/101 cumulative suites. Implementation and first evidence are in RF0001.


- `RunIdentity` carries project ID, session epoch, request ID and revision. The editor allocates identity before queueing, revokes reception on New/Open, and guards error/success mutations. `RunService` returns and records epoch/request ID and associates editor `run_id` with the request.


- The versioned architecture manifest lists all **110** AR subcases. The strict R0 verifier passed **18/18**: AR-01.a–d, AR-02.a–g, AR-03.a–g; zero failed/skipped/timeout/not_run/not_verified. Its report is `build/reports/architecture_refactor/20261018T010443.593471Z/report.json`. AR-02.g used a real isolated assertion failure, observed the nonzero failure in G0 with all required IDs present, then removed the probe and passed a clean G0. AR-01 evidence includes exact old-to-current names for all **88/87** baseline Debug/headless cases, unchanged desktop-required NED entries, frozen fixture/oracle, current environment, and executable hashes. Red/Green logs are in RF0002.


- R1 code/evidence: AR-04.a installed consumer passed at `build/reports/architecture_refactor/ar04_consumer/20261018T011110.475982Z/report.json`; AR-04.b/c/d have separate fresh-build, pure-test and borrowed-view evidence under `build/reports/architecture_refactor/ar04_b/`, `ar04_c/`, and `ar04_d/`. AR-05.d's controlled +0.25 dB mutation was independently caught by the float64 API oracle, Node/JUCE oracle, and RunService/CLI oracle. The exact Gain.cpp SHA-256 returned to its byte-for-byte original after the mutation; all three oracles passed again. Its successful report is `build/reports/architecture_refactor/ar05d_mutation/20261018T020541.613170Z/report.json`.


- AR-06.a first failed on the current split descriptor/factory registration path: the empty factory was rejected but left a descriptor behind. `NodeRegistry::register_node` now validates before insertion and rolls back the descriptor if factory insertion fails; version conflicts, empty factories, and duplicate ports leave the existing catalog and factory usable. Focused Red/Green records are under `build/reports/loops/RF0003/ar06a-red-*.json` and `ar06a-green-*.json`.


- AR-06.b Red proved that a floating parameter with an integer default was accepted and fully registered. Validation now rejects wrong default types, reversed/non-finite bounds, out-of-range defaults, duplicate parameters, invalid/unknown enum values, zero/duplicate formats, inconsistent port limits, and contradictory fixed/variable block declarations. Each rejection carries a field path and leaves the descriptor/factory maps empty; a valid enum/format/fixed-block descriptor registers successfully. Evidence is in `build/reports/loops/RF0003/ar06b-red-*.json` and `ar06b-green-*.json`.


- AR-06.c Red proved that a throwing processor factory escaped `NodeRegistry::create_processor`. Creation now returns an error with the node ID; `create_plan` builds into a temporary and returns no partial plan if a later factory throws. Tests create two same-type stateful instances, verify independent processing counts, and verify an earlier instance is destroyed after a later failure. Evidence is in `build/reports/loops/RF0003/ar06c-red-*.json` and `ar06c-green-*.json`.


- AR-07.a prototype Red confirmed that no generic SDK-to-JUCE wrapper existed. `NativeNodeAudioProcessor` now injects any SDK processor plus named input/output metadata, and Gain and an independent offset processor pass separate numerical oracles through that same wrapper. Evidence: `build/reports/loops/RF0003/ar07a-red-build.json` and `ar07a-green-build-retry.json`/`ar07a-green-ctest.json`. This proves the new wrapper seam; production `render_audio_graph` still uses the older Gain-specific JUCE factory, so AR-07.a is not yet accepted end-to-end.


- Cumulative Debug regression after the wrapper prototype passed **20/20**, including AR-04.d, AR-05.a–d, AR-06.a–c, AR-07.a, GRAPH-02/03, and E2E-01: `build/reports/loops/RF0003/r1-cumulative-after-ar07a.json`, source hash `876f616cf7b54f3b45ded9774a8612b13c495cf2d14b340c098812f79e602357`. Full Debug/headless/Release inventories and legacy mappings have not yet been refreshed for the newly registered cases. AR-05.b/c actual GUI evidence also remains `not_verified`.


- Next Red: AR-06.d, move builtin descriptor/adapter registration into the plugin package and route the production graph through injected SDK processors plus the generic wrapper; verify the app sees a read-only catalog and GUI/CLI compose identical builtins. R1 and platform Gates remain open.


- R0 does not close platform G0/G1 or NED requirements. Desktop window interaction, DPI, device playback, missing/wrong-resource GUI diagnostics, and the previously recorded 60 NED desktop evidence rows remain `not_verified`.





Refactor documentation history (2026-10-18): the architecture plans are the active R0-R6 entry points. The original documentation-only assessment remains under build/reports/architecture/20261018; implementation and acceptance evidence now live under RF0000/RF0001. The historical document review does not count as product acceptance.





Architecture assessment (2026-10-18): see `build/reports/architecture/20261018/assessment.md`. This review changed no product code. A fresh headless CTest run passed 87/87; the subsequent headless build was up to date and succeeded after Windows SDK access was granted. Debug inventory remains 88 tests. G0/G1 gaps and the latest 69-case interaction report (9 passed, 60 not_verified) remain open. Findings include duplicated production/core Gain implementations, the missing editor application boundary, Gain-specific UI, incomplete SDK/runtime integration, and main-Gate quality/evidence coverage gaps. A proposed architecture regression is to complete an old run after opening another project with the same revision and verify that neither its success nor failure updates the new project; this is currently a static risk, not a newly executed failing test. Existing desktop acceptance work remains required.





Last updated: 2026-10-19 (Asia/Shanghai)





## Current state





- Current Gate: **G0 in progress**; prior G1 desktop evidence remains scoped to the old core loop. BLD-01/02/03, LOOP-01, full UI-01 and remaining UI-02 are still missing, so no platform Gate is claimed.


- Current source tree hash and Git revision: `build/reports/environment.json`; this checkout has Git metadata and is based on `98fe518926cbca77c24b70245ed616bf9e9c7e16` with local changes.


- Environment: `build/reports/environment.json`; Computer Use found the running Debug AudioAlgo Editor window, but screenshot state capture timed out twice. No live pointer action or DPI change was made; current interaction evidence remains `not_verified`.


- Editor launch convenience: `scripts/run_debug.ps1` and `scripts/run_release.ps1` call the shared `scripts/build_and_run_editor.ps1`. `Get-Command cmake` returned two executables; the script now selects the first PATH result. Escalated execution completed Debug and Release configure/build; Debug started and remains running as PID 42524. Release was built, but its process exited because this app disallows multiple instances while Debug remained open. A preflight now reports this case instead of claiming success. PowerShell parser checks pass; full results and the one-instance constraint are recorded in `build/reports/loops/NI0006/run-editor-scripts-20261018.md`. Environment inventory was refreshed at `build/reports/loops/NI0006/environment-run-editor-scripts-fix.log`. Desktop screenshot capture for GUI interaction still timed out.





## Active editor interaction repair (2026-10-18)





- NI0007 (user-reported follow-up): root causes were an identity-only canvas transform with no middle-button gesture; Delete handling only recognized Escape and depended on an unfocused canvas; library drop used the last hover event rather than the release point, and palette drag began after button drag-state processing; all pins were painted with one fixed color. Added panning translation shared by drawing/hit testing, Delete/Backspace routed through existing topology history, release-point drop retargeting and click suppression, and compatible endpoint/candidate-pin preview colors (incompatible insertions show the rejection color). Evidence and commands: `build/reports/loops/NI0007/loop.md`.


- NI0007 verification: latest source rebuilt and linked the Release editor; Release UI-00 smoke passed 1/1. Debug CTest passed 84/84 and headless CTest passed 83/83 after rebuilding test targets; focused NED-06/10/11 checks passed 15/15. Debug editor relink was blocked because the running Debug executable is locked, so its editor binary is not the latest source. No native window was available through Computer Use for drag, key, pan, pin-color, or DPI evidence; those interactions remain `not_verified`.


- NI0007 fail-closed verifier: **67/67** required subcases registered; **9 passed, 0 failed, 0 skipped, 0 not_run, 58 not_verified**; overall `failed` because current desktop evidence is absent. The verifier writes a timestamped report under `build/reports/editor_interaction/`; invocation logs are preserved under `build/reports/loops/NI0007/`. Environment inventory is `build/reports/environment.json`.


- NI0008 (user screenshot follow-up): the library-button `mouseUp` was cancelling `LibraryDropSession` before JUCE delivered `GraphCanvas::itemDropped`. Since the target edge and preview live in that session, the drop callback rejected delivery, so the preview disappeared and the old connection stayed intact. Source release now marks the session without clearing it; `EditorComponent::dragOperationEnded` performs cleanup after the drop callback (or after an outside-canvas release). The insertion transaction already replaces the exact old edge with source→inserted-node and inserted-node→destination atomically. Evidence: `build/reports/loops/NI0008/loop.md`.


- NI0008 regression: source-release→drop-dispatch→operation-cleanup lifecycle check passed; focused NED-07.d/NED-10.a/NED-10.h passed **3/3**; cumulative Debug CTest **84/84** and headless CTest **83/83** passed after rebuilding. The editor target in `build/windows-release` compiled and linked, and its registered UI-00 launch smoke passed **1/1**. Computer Use found no targetable native app windows, so a live palette-to-wire release and visual confirmation remain `not_verified`.


- NI0008 fail-closed verifier: **67/67** required subcases registered; **9 passed, 0 failed, 0 skipped, 0 not_run, 58 not_verified**. It exits `1` because desktop evidence is missing; the timestamped report is under `build/reports/editor_interaction/`. Environment inventory: `build/reports/environment.json`.


- NI0009 (user screenshot follow-up): both `EditorComponent` and `MainWindow` requested a fixed 1440×900 logical-pixel size. At the screenshot's display scaling, that height exceeds the work area, pushing the native title bar/tool row off-screen. The initial size is now 1280×800 and is clamped to the primary display's JUCE `userArea` with a 24 px margin; the component and window share the same default dimensions. NED-02.c verifies 1280×800 layout visibility and work-area fit. Debug CTest **84/84**, headless **83/83**, and the rebuilt editor's UI-00 smoke **1/1** pass. Latest interaction verifier remains **67/67 registered, 9 passed, 0 failed, 58 not_verified** because there is no targetable native app window; its timestamped report is under `build/reports/editor_interaction/`. Evidence: `build/reports/loops/NI0009/loop.md`.


- NI0010 (latest pin, insertion, and order feedback): pin hit testing now checks the painted socket area outside the node body before treating the body as an occluder; this fixes output drags missed on the protruding half of a socket. During a compatible wire drag the destination input pin turns green. During compatible insertion preview, the old edge is hidden and two normal-color preview edges connect through the candidate node; both original endpoint pins and the candidate's compatible pins turn green. Drop delivery no longer depends on a source-button mouse-up flag; the active drag session plus project epoch/revision guard remains authoritative, and JUCE's drag-operation end clears the session. Dragging one processing node onto an adjacent processing node atomically rewires their linear order and swaps their canvas positions; branched or incompatible topology is rejected. Regressions cover the exposed socket hit, insertion dispatch ordering, swap topology, algorithm order, history, and branch rejection.


- NI0010 verification: Debug CTest **86/86**, headless CTest **85/85**, Release editor build, and Release UI-00 launch smoke **1/1** passed. The running Debug process holds its executable, so the latest Debug editor relink returned LNK1168; the current source is compiled and linked at `build/windows-release/bin/AudioAlgo Editor.exe`. A parallel Release test-target attempt hit MSVC C1041 on Catch2's shared PDB; the independent Debug and headless test builds both completed. The fail-closed interaction verifier still reports **67/67 required cases registered, 9 passed, 0 failed, 0 skipped, 0 not_run, 58 not_verified**; the latest timestamped report is under `build/reports/editor_interaction/`.


- NI0010 desktop status: `sky.list_apps` returned the running Debug editor window, but both screenshot capture attempts timed out (`FrameArrived timed out`, then `window capture timed out`). The release smoke test verifies startup and exit only; real pin drags, insertion preview/commit, node-order drag, and DPI evidence remain `not_verified`.


- NI0011 (current undo/delete follow-up): Ctrl+Z was missing because the canvas only handled Escape/Delete/Backspace and the parent editor had no undo shortcut; keyboard focus events bubble through JUCE's parent chain, so the editor now routes Ctrl+Z to its topology history and Ctrl+Y/Ctrl+Shift+Z to redo. `delete_node` previously removed every incident edge; it now tries a validated upstream-to-downstream bypass only for a block node with exactly one incoming and one outgoing edge. Branches, incompatible ports, cycles, and nodes with missing/extra edges keep the normal delete behavior. Undo restores the original node and edges; redo reapplies the bypass as one history entry. Evidence: `build/reports/loops/NI0011/loop.md`.


- NI0011 verification: the new NED-09.e deletion/history regression and NED-11.g keyboard mapping regression each showed the expected pre-fix failure, then passed. Debug CTest **88/88**, headless CTest **87/87**, Release editor linked, and Release UI-00 launch smoke **1/1** passed. Desktop Ctrl+Z/redo input and delete/reconnect visuals remain `not_verified` because this session's native window screenshot capture has timed out twice.


- Implementation and acceptance entry points: [node interaction plan](plan_editor_node_interaction.md), [NED-01–14 acceptance](testing_editor_node_interaction.md), [Agent TDD/Loop Engineering workflow and startup prompt](agent_loop_editor_node_interaction.md).


- Status: **NI0000 complete; NI0001–NI0004 model/component slices recorded green; NI0005 NED-10.a–h insertion checks and NED-11.a–g topology-history checks pass in pure/component tests; NI0006 contains NED-01.a–c UTF-8/path/tooltip checks, NED-02.a–d geometry/layout checks, NED-06.d connected-node drag-preview cable fix, NED-13 persistence checks, and NED-14 RunService/CLI/oracle checks. NI0010 adds pin/insertion feedback and adjacent Node order swap. NI0011 adds keyboard undo/redo routing and safe processing-node deletion bypass. Latest cumulative Debug 88/88 and headless 87/87; verifier registration is 69/69, with 60 required desktop evidence records still `not_verified`.** This is not专项 completion: real GUI operations, screenshots, and DPI acceptance remain required.


- Current findings: library-drop insertion carries the exact hovered edge, renders a preview, preserves sibling branches, and offers explicit port choices when ambiguous. Port connection/disconnection and Add/Connect/Disconnect/Insert/Delete use shared model actions; the toolbar exposes New, Open, Undo, and Redo. New/Open clear history, connection and drag state, and old file chooser completion context. The editor was rebuilt and linked with these paths, and the current UI-00 launch smoke passes; it remains launch-only evidence. None of the actual drag, menu, undo/redo, New/Open, or DPI interactions has fresh desktop evidence. Output recreation selects the new unique sink in the model and RunService accepts the reconnected graph; its GUI lifecycle evidence remains open.


- Latest NI0006 result: NED-01.a verifies UTF-8 project/descriptor text and special punctuation (7 assertions); .b verifies Chinese paths with spaces across save/reopen, RunService/CLI, unchanged source bytes, and matching input hash (18 assertions); .c verifies full long tooltip text round-trips through JUCE conversion (4 assertions). NED-02.c checks shared layout at logical 1100×700 and 1440×900 (34 assertions); .d checks nine input/output ports' anchors, labels, and hit targets (102 assertions). NED-06.d now also verifies that during connected Gain drag preview, both incident cable endpoints follow the preview geometry while the project node position and revision stay unchanged; the focused case passes **1,056 assertions**. The attached reproduction screenshot is preserved as `build/reports/loops/NI0006/repro-connected-node-drag.png` (SHA-256 `A0D49A6918910527C577F2D58F0F9E874B61D6C16E197A0D4B7AFD2208374475`). NED-13 persistence and NED-14 RunService/CLI oracle checks still pass. Latest cumulative Debug CTest **84/84**, headless **83/83**, including a fresh editor link and UI-00 smoke. The screenshot is diagnostic evidence; current live GUI and DPI acceptance remain `not_verified`.


- NED-01/02 implementation details: `EditorText.h` centralizes UTF-8 conversions and complete tooltip text; Chinese project and WAV path tests preserve file bytes and input hash. `EditorComponentLayout.h` is now the shared pure layout used by `EditorComponent::resized()` and the 1100×700/1440×900 component test. NED-02.d verifies hit testing at every fixture anchor and label center. No display-scale setting was changed and no 100/150/200% physical desktop run was completed.


- NI0005 history: NED-10.a carries the exact hovered edge through library-drop preview and commits atomically. NED-10.c reuses an unconnected canvas node and preserves ID/parameters with one revision. NED-10.d rejects unsupported insertion; NED-10.e chooses the nearest edge within <=8 px with stable ties and respects occlusion. NED-10.f waits for an explicit port pair; NED-10.g allows an inserted node's extra required input to remain open while RunService rejects with `input_required`; NED-10.h rejects stale sessions, missing/duplicate targets, and candidate validation failures without mutation. NED-11.a covers Add/Connect/Disconnect/Insert/Delete; .b proves one undo restores the old edge and removes a newly created node; .c restores an existing node's identity, parameters, and original position; .d verifies monotonic revisions while preserving later parameter/layout/name changes; .e verifies failed commands preserve model/history and new work clears redo; .f verifies cleared history and pending gesture state for project replacement. The six NED-11 cases pass **147 assertions**. Debug editor links and UI-00 launch smoke passes. Manual drag/choice/history screenshots, saved-project JSON, and DPI run remain `not_verified`.


- NED-10.b adds a branched File→Gain graph with two outgoing branches and asserts exact preservation of both sibling edges while one selected edge is replaced. The test passed immediately on the NED-10.a generalized candidate-copy command (8 assertions), so no implementation change or fabricated Red was introduced. Cumulative Debug CTest is now **58/58**, headless **57/57**. Branch selection still needs desktop evidence.


- Current Catch2 inventory registers all **69/69** NED subcases. The latest fail-closed verifier reports **69/69 registered; 9 passed, 0 failed, 0 skipped, 0 not_run, 60 not_verified** and exits 1 because desktop evidence is absent. Its latest timestamped JSON report is under `build/reports/editor_interaction/`; historical NI0006 log remains at `build/reports/loops/NI0006/verifier-final-drag-preview-refresh.log`.


- Historical UTF-8 literal repair remains present. It does not establish this session's rendered text evidence, and historical L0012 branch creation is not evidence of library drag-and-drop or serial insertion.


- Rebuilt baseline: Debug CTest 17/17, headless CTest 16/16. NI0000 regression: Debug 21/21, headless 20/20. NI0001 regression: Debug 23/23, headless 22/22. NI0002 regression: Debug 31/31, headless 30/30. NI0003 cumulative after NED-05.c: Debug 34/34, headless 33/33; after NED-05.d: Debug 35/35, headless 34/34; after NED-05.e: Debug 36/36, headless 35/35; after NED-06.a: Debug 37/37, headless 36/36; after NED-06.b: Debug 38/38, headless 37/37; after NED-06.c: Debug 39/39, headless 38/38; after NED-06.d: Debug 40/40, headless 39/39; after NED-06.e: Debug 41/41, headless 40/40; after NED-09.a: Debug 42/42, headless 41/41; after NED-09.b: Debug 43/43, headless 42/42; after NED-09.c: Debug 44/44, headless 43/43; after NED-09.d: Debug 45/45, headless 44/44. NI0004 after NED-07.a–c: Debug 48/48, headless 47/47; after NED-07.d: Debug 49/49, headless 48/48; after NED-07.e: Debug 50/50, headless 49/49; after NED-07.f: Debug 51/51, headless 50/50; after NED-08.a: Debug 52/52, headless 51/51; after NED-08.b: Debug 53/53, headless 52/52; after NED-08.c: Debug 54/54, headless 53/53; after NED-08.d: Debug 55/55, headless 54/54; after NED-08.e: Debug **56/56**, headless **55/55**. Debug CTest passed, but its UI-00 smoke used the previously linked executable because the updated editor relink was blocked (LNK1168); the headless build/test exits were 0.


- The manifest enumerates all **69** NED subcase entries and binds them to `doc/testing_editor_node_interaction.md` by SHA-256. Current Catch2 inventory registers NED-01.a–c, NED-02.a–d, NED-03.a–e, NED-04.a–c, NED-05.a–e, NED-06.a–e, NED-07.a–f, NED-08.a–e, NED-09.a–e, NED-10.a–h, NED-11.a–g, NED-12.a–d, NED-13.a–d, and NED-14.a–e. This registration does not satisfy the separate desktop evidence requirement.


- A controlled temporary CMake/CTest failure probe returned nonzero and was cleaned up; its log is preserved under `build/reports/loops/NI0000/`.


- The editor process launched, but `get_window_state` timed out twice through the computer-use helper. No screenshot was captured, no DPI operation was performed, and no desktop result is claimed.


- `scripts/verify_editor_interaction.py` and `tests/editor_interaction_manifest.json` now exist. NED-03.a–e, NED-04.a/b model checks, NED-04.c RunService/CLI rejection, and NED-05.a–e pure creation/selection/type checks are registered and pass where no desktop proof is required. NED-05.d exercises a registered processing descriptor through the same library-entry projection consumed by the dynamic GUI button list; its required real desktop interaction remains `not_verified`. NED-06.a–e pure hit testing follows paint order, prevents selecting covered edges, gives sockets priority over node movement, and shares a logical coordinate transform; NED-06.d confirms layout dirty/revision/output invariants. NED-07.a–f implement processing-node drag, source resource selection from the preserved drop point, click/drag threshold, click suppression, exit/cancel cleanup, stale-project rejection, and source path/revision commit checks. NED-07.f adds a regression around the existing project epoch guard; native chooser project-switch/view-close interaction remains `not_verified`. NED-08.a checks shared exact-port command creation for click and drag and wires the output-port drag path to the same commit method; actual input release on desktop is still `not_verified`. NED-08.b tests the logical pointer state consumed by connection preview painting; actual pointer-following is still `not_verified`. NED-08.c tests invalid-release cleanup and Escape state cancellation without model mutation; actual Escape/release behavior remains `not_verified`. NED-08.d verifies the existing edit validator rejects a duplicate required input with a concrete diagnostic and leaves project state unchanged; GUI prompt evidence remains `not_verified`. NED-08.e implements exact-edge disconnect on the Delete action, increments revision once, preserves the other edge, and permits reconnect in pure checks. Main.cpp compiled; editor linking was blocked twice by LNK1168 because an active editor process held the target executable. The stale UI-00 pass is not treated as current GUI evidence. NED-09.a–d deletion/recreation, unique Source/Sink capability, save/reopen and RunService checks are implemented and pass. Real overlap/edge/port/move/DPI/output lifecycle interactions remain `not_verified`. The manifest and verifier were corrected to include NED-09.d, bringing the acceptance total from the erroneous 66 to 67. Dirty/history and GUI diagnostic fields for NED-04 remain unverified; remaining NED subcase registrations and product tests are incomplete, not skipped.


- Next sequence: when a native app window is available through Computer Use, capture fresh GUI text, drag/drop, hit/connection (including a connected-node move), save/reopen, and DPI 100/150/200 evidence at both required logical sizes, then run the full desktop NED-14 GUI/CLI correlation. Keep the 58 missing desktop records fail-closed until captured. After evidence, rerun cumulative Debug/headless suites and verifier.


- Preserve existing platform G0/G1 gaps below. NED completion does not imply those gaps or G2–G6 are complete.





## Completed and verified slices





- L0000–L0007: unchanged from previous report (gate verifier, JUCE host, Gain oracle, graph contract, WAV IO, schema-v1 project files, real JUCE graph routing, shared RunService/CLI). Evidence under `build/reports/loops/L0000/` … `L0007/`.


- L0008: planar buffer valid-interval metadata + structured diagnostics (CORE-01) — present in both suites (Debug 17, headless 16).


- L0009: editor UTF-8 mojibake fixed. JUCE `String(const char*)` is ASCII-only; all non-ASCII literals in `apps/editor/Main.cpp` now go through a `utf8_literal` (`String::fromUTF8`) helper. Before/after screenshots: `build/reports/loops/L0009/`.


- L0010: editor Run verified on the real desktop (OS-input automation): background render creates `runs/run-*/` artifacts, status shows run id + frames + SHA, waveform renders; editor output SHA equals the CLI oracle SHA (`37eae941ad4d…`). Evidence: `build/reports/loops/L0010/`.


- L0011: editor Play output verified with the real audio device: button toggles to Pause, status "Playing rendered output". Evidence: `build/reports/loops/L0011/`.


- L0012: full interaction chain verified: library add Gain → drag away from overlap → click-connect File.out → new Gain.in (fork) → slider mutation (−55.4 dB live on canvas) → Run re-render → waveform amplitude change + new deterministic SHA (`3b04fe51c9dd…`, run id `0d658a519a5f4edf9d316f5a662c4ed7`). Unconnected-input Run rejection shows the specific diagnostic `input_required: Required input is not connected`. Evidence: `build/reports/loops/L0012/`.


- NI0000: current Debug/headless baselines rebuilt; NED-12.a–d verifier checks registered; a real CMake/CTest failure probe was captured; missing registrations and desktop evidence make the overall verifier correctly exit 1. Evidence: `build/reports/loops/NI0000/`.


- NI0001 (partial): `EditorGeometry.h` now supplies shared title, summary, port text, socket, and node bounds. Canvas painting, exact-port hit testing, and connection anchors consume this geometry. NED-02.a/b produced behavior Red against the extracted legacy rectangles, then passed after the layout fix. Desktop text/DPI evidence is still `not_verified`. Evidence: `build/reports/loops/NI0001/`.


- UI-00: `AudioAlgo Editor.exe --smoke-test` registered in CTest (`UI-00_editor_binary_launches_and_quits_smoke`) and passing.





## Actual commands





- Build debug editor/tests: `cmake --build --preset windows-debug --parallel 4`


- NI0000–NI0003 evidence: `build/reports/loops/NI0000/`, `NI0001/`, `NI0002/`, and `NI0003/`; latest Debug 42/42 and headless 41/41.


- Current Debug verifier: `py -3.13 scripts/verify_editor_interaction.py` (expected exit 1 while required product subcases and desktop evidence are missing)


- Run cumulative Debug suite: `ctest --test-dir build/windows-debug -C Debug --output-on-failure --no-tests=error`


- Run cumulative headless suite: `ctest --test-dir build/windows-headless -C Debug --output-on-failure --no-tests=error`


- Gate verifier: `E:\0qyx\res\miniconda\python.exe scripts\verify_gate.py --gate G1 --build-dir build\windows-debug` (currently exits 1 by design: missing BLD-01/02/03, LOOP-01, UI-01, UI-02)


- CLI demo: `build\windows-debug\bin\audio_cli.exe run examples\gain_demo\gain_demo.aaproj build\runs\gain-demo-view 127`


- Desktop automation helpers used for GUI evidence: `build\reports\loops\L0011\play_verify.ps1`, `build\reports\loops\L0012\ui01_interact3.ps1`, `build\reports\loops\L0012\ui01_finish.ps1`





## Unverified and blocked acceptance





- NED-01.a–c now have UTF-8/path/tooltip code and IO checks; NED-02.a–d have geometry/layout checks. Their rendered-glyph, GUI path chooser, visible tooltip, and real DPI evidence are still `not_verified`. Desktop records for NED-03 through NED-14 are also `not_verified` where the manifest requires them. NED-03.a–e, NED-04.a/b model/file checks, NED-04.c RunService/CLI rejection, NED-05.a–e pure checks, NED-06.a–e geometry/layout checks, NED-07.a–f pure checks, NED-08.a–e pure checks, NED-09.a–d model/run checks, NED-10.a–h model/geometry checks, NED-11.a–f history checks, NED-13.a–d persistence/service checks, and NED-14.a–e RunService/CLI numerical checks pass where applicable. GUI diagnostics, real drag/drop, menu choice, history buttons, New/Open, save/reopen, and DPI evidence remain `not_verified`; NED-14 same-snapshot GUI/CLI numerical evidence remains outstanding.


- The user-provided screenshot now gives a concrete drag-preview reproduction: both cables remain at the Gain's old y-position while its preview moves. It is recorded under NI0006 but lacks this run's application/source/DPI metadata and is not acceptance evidence. Computer Use reports no native app window for a live retest; text, drag/hit/connection, save/reopen, and DPI 100/150/200 evidence therefore remain `not_verified`. Do not infer these from prior L0009/L0012 screenshots.


- G1 remaining: UI-01 needs File source/Output added from the library via the native file chooser, and GUI save/reopen interaction (Save to staged path untested; Open requires dialog navigation). UI-02 remaining: wrong-connection diagnostic message and missing-file diagnostic message via desktop evidence (code paths exist; unconnected-input diagnostic already captured).


- G0 remaining: BLD-01/02/03 (editor-off/python-off/headless build configs as registered checks) and LOOP-01 are absent from the CTest inventory; the gate verifier reports them missing.


- G2–G6 behavior (fixed-frame adapter, history/migrations, Python runner, SpeexDSP, beamforming, quality data, release packaging) remains unimplemented.





## Lessons (desktop automation)





- Processes launched from the agent shell are killed when the shell job closes; launch GUI apps via `Invoke-CimMethod Win32_Process Create` so they survive.


- The agent PowerShell is DPI-unaware by default: captures and `SetCursorPos` share one virtualized 1536x864 space on this 1920x1080/125% desktop, but `SetProcessDPIAware()` makes both physical and is preferred.


- The user's active desktop can move/close/steal focus from automated windows: keep launch→locate→click→capture in one command, locate buttons by pixel scan instead of layout math, and never click blindly after an unexpected screenshot.





## 历史 Next Red（已被顶部唯一当前摘要替代）





- **当前接续：RF0006/M15 参数编辑里程碑累计验收；下一实现 Red 为 AR-10.e/f。** T11已核实，T12～T14已完成AR-10.c/d/g的行为Red、同目标Green和独立音频/报告oracle，详细证据和候选映射见本文顶部与对应loop。当前快照全套结果以 `build/reports/loops/RF0006/M15-parameter-milestone/summary.json` 实际生成结果为准；T10/T11累计数字保留为历史，不能替代本轮验证。桌面交互未经用户明确恢复不重试，AR-05.b及AR-10.h/AR-11桌面项继续not_verified。


- **R2待补的外部证据**：AR-05.b要求编辑器实际运行输出满足独立数值oracle并提供run id、帧数、SHA截图。本轮启动编辑器成功但Computer Use窗口截图连续两次超时，不能据此判通过；原始尝试 `build/reports/loops/RF0006/T09-delayed-run-notification/desktop-capture-attempt.json`。恢复桌面截图能力后完成该证据并复跑R2验证器。


- 平台 Gate 与 NED 桌面缺项不变：NI0007–NI0009 的平移/删除/插入/节点顺序、UI-01/UI-02/UI-04/UI-07、


  DPI 100/150/200%、真实设备播放与 BLD-01/02/03 构型检查仍为 `not_verified`。
