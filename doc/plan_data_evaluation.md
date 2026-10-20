---
title: 数据与独立评测线详细开发方案
status: ready-for-implementation
updated: 2026-10-18
---

# 数据与独立评测线详细开发方案

## 1. 任务与实施边界

本线由“固定测试数据与独立判断依据”及“独立评测模块”组成，支持现有[主验收准则](testing_audio_pipeline_editor.md)F01～F08、DSP/IO/GRAPH、QUAL-01～03和[架构重构验收](testing_architecture_refactor.md)。配套依次阅读[本方案](plan_data_evaluation.md)、[测试通过准则](testing_data_evaluation.md)、[Agent执行规程](agent_loop_data_evaluation.md)。文中的新增目录、接口、脚本和命令均是待实现契约，不代表已经存在。

用户已要求规划一条可与R0～R6重构并行的独立开发线。后续按本线范围进行常规代码/测试/本地验证时无需逐阶段确认，遵守实际工具权限，不自动提交/推送/发布/购买，不覆盖用户输入或不相关改动。本次规划文档本身不代表代码实现。

交付目标：

1. 版本化且可重跑的数据生成/导入配方，固定seed、格式、时序、有效区间、参考分量、许可与内容hash。
2. 无JUCE/NodeSDK/项目模型依赖的C++评测库，支持基础统计、严格数值比较、SNR、SI-SDR、ERLE、频率投影与SIR。
3. 独立命令行工具，将固定输入、外部算法输出和明确评测profile转为可追溯的通过/失败报告。
4. 可验证数据、指标和判定器的独立测试，包括错误格式、错误对齐、退化输出及证据造假拒绝。
5. 主线可读取的数据包、库导出和报告契约，不要求主线当前接口先稳定。

本线不实现Gain/Speex/DAS处理算法、不接管主运行框架、不修改画布、不开发STFT/波形展示或桌面自动化。实际算法输出由主线或另一个原库直调任务提供；本线只读消费。真实数据来源与听评缺失时不得替代为仿真通过。

## 2. 当前事实和并行所有权

2026-10-18规划时，重构聊天“实施架构 R0～R6 重构”正在R0推进，已出现RunIdentity/RunReception和AR验证器等改动。已有`scripts/verify_run_audio.py`用于独立Gain检查；应阅读其格式和oracle，但不修改该共享脚本或把它当通用评测能力。当前未发现独立audio_evaluation模块及本线工具目录，后续启动仍须重新检查。

### 2.1 本线可新增或修改

| 位置 | 用途 |
| --- | --- |
| libs/audio_evaluation/ | 独立C++20指标内核、公开头文件、自有CMake片段 |
| tools/data_evaluation/ | 独立CMake工程、Python生成/文件桥接/评测CLI、schemas、锁定依赖、测试、开发说明 |
| tests/fixtures/data_evaluation/ | 小型手算向量、版本化配方、许可清单；大音频只保留配方 |
| benchmark/suites/data_evaluation/ | 场景profile和数据要求、冻结阈值与公式版本 |
| doc/plan_data_evaluation.md等本线文档 | 设计、验收、规程和本线状态 |
| build/data_evaluation/ | 本线构建、venv、生成数据、消费测试的独立输出 |
| build/reports/data_evaluation/ | 本线阶段/循环/运行报告和证据 |

### 2.2 由重构主线负责，本线不改

根AGENTS/CMakeLists/CMakePresets、现有dependencies锁、主AR/NED清单及验证器、audio_core/audio_algorithms/NodeSDK/project/runtime/run_api/backend/application/view、apps/editor/CLI、共享`scripts/`、`doc/development_status.md`及主/重构/节点专项验收文档。

本线状态写入[独立状态文档](development_status_data_evaluation.md)，主线将来按实际接入结果汇总。禁止因主线产物变化回滚其修改、锁住整个仓库或复用其构建/PDB/venv/临时文件。无明确用户授权不向其他聊天发消息；需要接入时提供可审查的集成说明和产物，由已授权主线操作共享文件。

如果目录已被其他任务占用，先只读核对并缩小本线到未冲突文件；明确范围冲突且无法独立推进时才报告缺口，不自行覆盖。检查Git/文件hash记录本线变更，不能把另一Agent持续写入导致的全仓hash变化当作本线产品回归。

## 3. 技术与目录划分

```text
libs/audio_evaluation/
  include/audio_evaluation/     # spans、metric结果、统计/比较/参考指标
  src/
  CMakeLists.txt
tools/data_evaluation/
  CMakeLists.txt                # 独立configure，不include根工程
  requirements.lock            # 本线Python工具/测试依赖锁
  dependency_manifest.json     # 编译、解释器、第三方版本/hash/许可
  python/audioalgo_eval/       # generate/import/validate/evaluate/report
  cli/                         # 纯C++指标命令行桥接
  schemas/                     # dataset/profile/report的JSON schema
  tests/cpp/                   # Catch2/CTest纯测试
  tests/python/                # pytest数据/文件/CLI/报告测试
  tests/integration/           # C++/独立参考/文件消费
  README.md
tests/fixtures/data_evaluation/
  recipes/
  analytic_vectors/
  provenance/
benchmark/suites/data_evaluation/
  native_gain.json
  denoise.json
  aec.json
  beamforming.json
doc/development_status_data_evaluation.md
```

库内仅C++标准库/公开span视图，不包含AudioBuffer、JUCE、NodeInstance或项目路径。接收float样本视图，使用double累积；多通道由调用方显式逐通道评测，不隐式平均。轻量MetricResult/EvaluationWindow是本模块数据契约，不建立另一套项目图或拥有整段音频的基础类型。

Python负责数据配方、许可/manifest、WAV解码与报告编排；评测数值以C++库为唯一生产实现。Python中的手算/标量/高精度参考只存在测试路径，不作为遇错时替代C++的生产回退。通过独立指标可执行程序和规范化临时样本文件通信，首版无需pybind11、嵌入Python或修改主CLI。

桥接文件先用明确的float32 little-endian planar载荷及sidecar，严格校验`4*channels*frames`字节数、布局和hash。CLI参数/profile明确窗口与选定通道，指标结果输出标准JSON；文件桥接属于本线独立实现，不复用主算法执行器生成期望。批处理/直接库消费可后续优化，但不能改变公式。

Python解释器/测试依赖先实际检测并锁定，本线venv位于独立build目录。可优先用标准库生成与解码基础数据；需要NumPy等时必须锁版本及PRNG，不依赖共享全局环境。C++测试采用仓库已锁定Catch2版本，可读取现有锁/合法缓存，但使用本线独立构建目录，不改根锁或主线build输出。

## 4. 数据包与文件契约

### 4.1 Manifest最小字段

- schema_version、dataset_id、recipe_version、生成器代码hash、生成环境、固定seed与PRNG版本。
- 每项资源的稳定ID、role、相对路径、容器/PCM格式、采样率、frame_count、channel_ids及角色、文件SHA-256与解码后规范样本hash。
- logical_start_frame、clock_domain、有效半开区间[start,end)、声明延迟、预热与评测窗口、末尾策略；单位为整数样本，秒仅作可读派生。
- clean/noise/mixture、render/echo/near_end/capture、target/interference/mixture的分量关系及具体生成参数；几何坐标/声速/方向约定。
- license/provenance、原始文件hash、处理链、使用/再分发限制、适用基线与是否具备正式质量资格。

同一锁定平台/环境/recipe/seed重新生成应得相同规范样本及文件hash，容器不写易变时间字段。跨平台libm/浮点差异不承诺天然同hash：单独声明基线并保留数学容差，禁止把任意hash都视为合格。

manifest最后发布；生成使用新目录及临时文件，失败不生成dataset_complete标记，不覆盖已有目录。原始用户文件只读导入；大音频写独立build目录，源码只保存小fixture/配方/清单。基于内容hash缓存生成物，不在每个测试分支重新随机生成。

### 4.2 WAV与幅值

支持主方案要求的PCM16及IEEE float32 WAV，16/48kHz与1/2/4通道；验证RIFF/chunk长度、padding、frame完整性、格式和资源预算。多通道WAVE_FORMAT_EXTENSIBLE的PCM16/float32子格式需明确处理或诊断；不能误解码为普通PCM。解析恶意长度、截断、重复关键chunk、非法block_align和非有限float必须失败。

PCM16解码以整数/32768转换；生成侧量化、饱和、舍入规则固定在配方中并测试端点。与真实算法结果比较时oracle使用实际解码输入，而非量化前理想正弦。PCM16原样回编码及饱和计数独立验证，不照搬生产WAV writer作为期望。

float可超过±1，评测报告越界数量而不自动归一化/削波。`out_of_range_sample_count`定义为sample<-1或sample>1，±1不计；`full_scale_hit_count`可单列，不能声称仅凭PCM16输出端点就知道历史削波数量。实际写入削波须有写前样本/量化策略/原报告作为证据。

## 5. 固定数据交付

| 数据 | 本线交付 | 独立核验 |
| --- | --- | --- |
| F01 | 主文档长度矩阵、16/48kHz、1kHz/0.25正弦与静音/脉冲/边界输入 | 手算点、独立float64公式、原输入hash；零长可在载荷中表示，容器/消费者接受或诊断明确 |
| F02 | 2/4通道互异频率/符号/脉冲，稳定channel_ids | 明确的置换/直通oracle可捕获串道，不能只比混合RMS |
| F03 | 固定seed噪声、host块分割表、480/512串联测试输入及时间索引 | 不运行被测FIFO生成期望，恒等/已知延迟/计数序列独立构造 |
| F04 | 授权clean语音、稳态noise、0/10dB mixture、clean活动区间 | 至少3条各≥10秒，混音前按固定clean区间标定，混合后不归一化；许可不足时仅有生成/导入工具，正式质量数据缺项 |
| F05 | 16kHz/20秒render、因果FIR/显式延迟、echo、near_end、capture、四类场景及10秒路径切换 | scalar卷积参考、角色/clock/起点、FIR与延迟总支持范围匹配128ms约束；保留near/echo参考 |
| F06 | 48kHz四麦、间距0.035m、343m/s、1kHz broadside target、4kHz/60°干扰及独立分量 | 解析相位延迟；不调用Pyroomacoustics；固定角度/方向符号与首尾区间 |
| F07 | 真实同步阵列数据导入、几何/方向/通道/分量/采集与许可记录 | 不推测同步/几何/参考分量；证据不足仅探索性，不能补齐QUAL-03 |
| F08 | 48kHz四通道60秒/10分钟float文件，固定解析recipe | 分块生成/校验，不在内存同时复制全部音频；长度精确、源文件不变 |

F05的FIR和纯延迟组合需落在128ms支持范围内，不以超出滤波能力的数据改变既定AEC验收难度。F06建议固定60秒，去掉首尾各1秒后使用整秒评测窗口，方向符号写recipe；它是独立解析数据，不能据此声称真实波束质量已过。

三类配方的固定计算约定：

- F04：在冻结clean活动窗口上，noise系数`a=sqrt(E(clean)/(10^(snr_db/10)*E(noise)))`，mixture=clean+a*noise；clean预处理增益/活动检测规则先冻结且与DUT无关。正式质量优先保存float32分量，PCM16派生物单列量化信息；不能混音后根据当前峰值重新缩放。
- F05：`echo[n]=sum(h_scene(n)[k]*render[n-delay-k])`，负索引补0；场景在第10秒按输出样本索引切换FIR，保留render历史，near_end独立生成/导入，capture=echo+near_end。每个场景的单讲/双讲区间和能量由manifest固定，不从残差反推。
- F06：单位向量u从阵列原点指向声源，`tau_i=-dot(r_i,u)/c`，`x_i(t)=s(t-tau_i)`；多源按各自解析延迟叠加。第二预设方向用`(sin30°,cos30°,0)`，主频保持1kHz。理想DAS方向响应可由`H(f)=sum(exp(j*2*pi*f*dot(r_i,u_source-u_steer)/c))/M`独立计算；具体库的离散滤波/延迟处理仍需按原ALG直调与频率质量验收，不以理想连续响应替代真实实现验证。

## 6. 指标的固定数学契约

公式输入均为已显式对齐且同一固定窗口中的向量。逐通道计算，double累计，N>0且全部样本有限。正式profile使用原物理幅值，默认zero_mean=false，不自动去均值/归一化/互相关寻优；若未来需不同定义，版本化ADR与独立验收后另建profile，不静默替换。

令E(v)=sum(v²)/N，eps=1e-20（线性均方幅度单位）；eps为退化检测/明确ERLE下限策略，不随被测输出选择。所有边界行为先测试并写入formula_version。

| 指标 | 定义/用途 |
| --- | --- |
| peak/RMS | max(abs(x))、sqrt(E(x))；按通道及窗口记录 |
| 越界计数 | 精确计数x<-1或x>1；另报非有限样本并拒绝正式评测 |
| 严格比较 | 每样本满足原容差；格式/通道/窗口先精确检查，任何不符直接失败 |
| clean-reference SNR | 10log10(E(s)/E(y-s))；无额外幅值拟合 |
| SI-SDR | alpha=dot(y,s)/dot(s,s)，10log10(E(alpha*s)/E(y-alpha*s))；标量投影只属于该指标定义，不修改音频 |
| ERLE | 10log10(E(echo)/max(E(residual),eps))，只对预定义远端单讲区间正式使用 |
| RMS增益 | 20log10(RMS(y)/RMS(s))，用于近端/clean/target保真与幅度退化保护 |
| 频率分量/SIR | 在同一固定窗口对预设sin/cos基同时做最小二乘投影，按重建分量能量计算target/interference及相对参考麦克风的提升 |

SI-SDR公式出处为[Le Roux等原始论文第2.3节](https://arxiv.org/html/1811.02508#S2.SS3)。本项目zero_mean、窗口和eps是显式工程约定，不宣称论文要求这些默认值。不采用可拟合长FIR的其他SDR定义替换SI-SDR。

### 6.1 退化值与聚合

参考能量≤eps时SNR/SI-SDR/RMS比值为undefined，不能算作质量通过。有效参考且精确零残差的SNR/SI-SDR可为positive_infinity；有限误差下零投影可为negative_infinity。JSON以`value:null,value_kind:positive_infinity/negative_infinity/undefined`表达，不写非法NaN/Infinity数字。

非静音参考上的全零/近零输出单列degenerate_output并使质量失败；静音测试按peak/finite条件单独判断。不得让eps把全静音结果变成高分。提升指标在inf-inf等不可定义情况返回indeterminate，不用0替代或删除该样本。正式质量所有必需条目先通过有效性保护，才计算中位/单条阈值。

以每语句/场景先计算能量比，再按主验收汇总。中位数偶数项取中间两项算术平均；每通道单列，无隐式把最好通道/最好窗口作为结果。不能把先平均dB与先求能量比混用。profile固定输入、窗口、聚合和边界策略并参与hash。

### 6.2 对齐与有效区间

使用整数logical_start_frame、显式declared_latency_frames及半开有效区间；定义物理文件索引i的逻辑坐标为logical_start_frame+i-declared_latency_frames。已对齐产物声明latency=0并标alignment_applied，禁止重复裁剪。比较前检查共同clock_domain和采样率，相同物理通道按稳定ID显式映射；禁止偷偷重采样、截短到最小长度或忽略剩余数据。

逐样本Gain/直通/ChannelSwap要求精确长度/起点/窗口；质量profile可预先声明固定交集或排除预热，需满足窗口完全覆盖并报告所有排除帧。未知延迟/时钟、窗口越界或重复延迟补偿返回诊断，不能自动寻找最佳对齐。raw和aligned产物保持只读，不重写原始文件。

## 7. 质量profile与主线接入

profiles将QUAL-01/02/03原阈值逐条编码并绑定主验收内容hash；不通过本线另定更宽松阈值。F04保留0.5秒预热，F05固定5～15秒/路径切换15～20秒，F06/F07按recipe固定区间。F04活动区间从原clean按预先冻结规则确定，不能从被测输出挑选。

数值测试可注入手算输出验证阈值边界/拒绝能力，但报告须标`evaluator_self_test`。真实算法评测标`external_algorithm_result`，必须关联实际运行及完整输出hash。生成参考数据和通过判定器测试，不意味着任何降噪/AEC/波束算法通过。

正式质量还需要原验收中的真实听评、授权语音、F07分量和设备等证据；缺失时整体not_verified。允许另报`synthetic_subset`或`numeric_subset`，不能把子集作为完整suite passed。

### 7.1 读文件接入，不修改主工程

首版接收独立evaluation_request.json，其中包含dataset/profile/baseline/output引用、明确格式/窗口/延迟、project/request/run/revision可选关联及所有文件hash。支持只读导入当前run.json+complete.json+output.wav，并核对run_id/输出hash/完整标记；旧schema不具备的元数据由显式sidecar提供，不推测，不修改生产manifest。

所有解码缓存、临时载荷、评测report和完成标记写本线新输出目录。禁止向主线run目录写验证JSON，避免与现有verify_run_audio.py及主line快照相互污染。跨线接入由主线将来修改自己的组合/build/验收映射，本线只提供headers、export target、schemas、样例request与契约测试。

### 7.2 报告与退出码

report包含evaluator版本/代码hash、公式/profile/data/hash、环境、输入/输出/run引用、通道/窗口/对齐排除、逐指标值与有效性、逐断言预期/实测/容差/状态、聚合、诊断和原始命令。状态passed/failed/not_run/not_verified/blocked/skipped/timeout分开；必需非passed阻止正式suite通过。

独立CLI退出码建议固定0成功、2格式/契约错误、3评测执行错误、4质量阈值未达、6环境/必需数据或证据缺失。内部JSON记录更精确状态。输出report采用临时文件原子替换；`evaluation_complete.json`只在完整scope通过时最后发布。失败/未验证保留report和日志但没有成功标记；子集通过只能写带scope的子集报告。

## 8. 开发阶段与首条Red

阶段集合及每条具体验收以[测试准则](testing_data_evaluation.md)为准，全部必需且累计。

| 阶段/循环建议 | 开发交付 | 首条Red |
| --- | --- | --- |
| D0 / DE0000 | 独立构建/测试、schema/manifest、严格文件校验、最小失败传播 | DE-02.b：manifest声明513帧而载荷只有512帧，必须拒绝且不得发布成功 |
| D1 / DE0001 | F01/F02/F03/F08配方与可复现生成、独立oracle | 通道被交换/尾部少一帧不能由相同总体RMS掩盖 |
| D2 / DE0002 | peak/RMS/计数、严格比较、格式与对齐规则 | ±1被误计削波或未知延迟被静默对齐必须失败 |
| D3 / DE0003 | SNR/SI-SDR/ERLE、RMS增益、频率投影/SIR | 手算正交向量分数错误、全零输出或inf-inf被算通过 |
| D4 / DE0004 | F04/F05/F06场景配方、固定profile与判定器 | 混音SNR不符、AEC窗口偏移或错误方向仍通过必须被捕获 |
| D5 / DE0005 | 授权clean语音/真实阵列导入与资格检查 | 无许可/同步/几何/参考分量的数据不得标为正式质量数据 |
| D6 / DE0006 | 独立CLI、run只读接入、报告/verifier、库安装消费和集成交付 | 原始run内容被写入、伪complete/漏测试/旧数据hash仍通过必须失败 |

D5真实数据不足允许先推进D6，不改变D5未完成；D6正式数据线验收仍检查全部累计必需项。环境受限与算法未提供导致的外部质量结果缺失单独记录，不伪装成本线工具实现错误或整个平台已通过。

## 9. TDD与证据

每次一个DE字母子用例：契约/手算例→真实Red→最小实现→同一目标Green→重构→有限累计回归→独立观察→证据与下一条Red。指标公式至少有手算精确向量、独立标量参考、性质/边界三类观察；不能直接调用待测函数生成expected。生成器与oracle不得共享同一生成计算实现。

同一根因三次无进展则缩小输入、核对数学定义/采样尺度/端点/时间单位，写本线ADR后换检查策略。禁止改原门槛、选择更有利窗口、重归一化、删失败条目或自动批准golden。公式若确需改变先记录独立依据、profile新版本及受影响要求，不修改共享验收文件。

本线manifest/verifier、CLI、示例命令均待实现；精确要求见[执行规程](agent_loop_data_evaluation.md)。证据以本线源码/依赖/profile/data/binary的明确hash为主，同时记录所消费主线产物的run/hash，不因无关主线文件变化废弃全部本线证据，也不能复用受本线改动影响的旧证据。

## 10. 完成定义与主线交接

- 工具实现完成：D0～D4及D6中不依赖外部数据的代码/测试/示例/命令/独立消费者全部实际通过，报告清楚标为工具子集。
- 数据与评测线完整完成：D0～D6全部DE必需子用例通过，F04/F07所需正式数据与许可/同步/参考证据齐备；被测算法尚未提供时明确不能宣称其质量通过。
- 平台算法质量通过：在本线工具基础上，实际算法产物与完整QUAL听评/真实数据等要求都通过，由主线按G/AR验收汇总。
- 交接提供可重跑数据recipe、独立库与CLI、固定profile、实际报告、输入/结果schemas及迁移说明；主线决定何时接入root CMake/runtime/evaluation任务，本线不占用共享核心文件。
