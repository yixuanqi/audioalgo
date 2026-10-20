---
title: 数据与独立评测线测试及通过准则
status: ready-for-implementation
updated: 2026-10-18
---

# 数据与独立评测线测试及通过准则

## 1. 验收边界

按[开发方案](plan_data_evaluation.md)和[Agent规程](agent_loop_data_evaluation.md)实施。本文定义DE-01～DE-16，每个a/b/c/d均为必需，共64个子用例；文档定义不代表测试已存在或执行。

阶段累计：D0=DE-01/02；D1新增DE-03/04；D2新增DE-05/06；D3新增DE-07/08/09/10；D4新增DE-11/12/13；D5新增DE-14；D6新增DE-15/16。父ID需全部字母子用例与映射证据通过。D5外部受限可先开发D6，正式完整验收不能省略D5。

状态passed/failed/not_run/not_verified/blocked/skipped/timeout分开；必需非passed阻止正式阶段通过。工具自测、仿真子集、真实数据资格、真实算法质量分别报告，不能互相替代。公式/配置/profile/data版本和源码/二进制必须关联。

原[主验收](testing_audio_pipeline_editor.md)的全部质量/格式/时序阈值继续生效；本线不修改共享主文档、AR或NED映射。不因被测输出不达标改变窗口、配方或门槛。

## 2. Oracle与数值规则

- 手算小向量的整数计数/帧/通道/区间精确相等，peak/RMS绝对误差≤1e-12；double dB结果误差≤1e-9 dB。输入为可精确表达的float数，不引入量化期望歧义。
- 一般float32数据的C++指标对独立double标量参考绝对误差≤1e-6 dB；非dB统计满足`abs(error)<=1e-10+1e-9*abs(expected)`。近退化/病态结果先按契约分类，不通过放宽容差比较。
- Gain/直通/ChannelSwap逐样本遵循`2e-6+1e-5*abs(expected)`，PCM16误差≤1LSB、无回绕，shape/通道/起点精确。
- 固定频率投影在解析可辨识向量上的幅度绝对误差≤1e-6；SIR以独立分量能量验证。病态基/不足样本不能返回高置信分量。
- 正式质量阈值按主验收直接比较，包含边界以原定义为准，不用上述实现数值容差将低于门槛的值放行。
- 一般测试默认10秒，文件/CLI集成60秒；F08和整套质量数据可独立标慢测并记录预算，不能因超时自动skip。pytest先collect，CTest先inventory，零测试非零。

所有oracle使用手算常量、独立标量公式或与生成器不同的解析实现。不得导入生产评测函数生成expected，不能以相同总RMS代替逐样本/通道比较，不能用DUT合成天然真值。

## 3. D0：边界、构建与数据契约

### DE-01 独立开发与可执行基线

- a：本线独立CMake configure/build/CTest成功，不获取JUCE、不引入NodeSDK/project/runtime/View、不打开设备；C++公开头文件消费者只用标准库可编译。
- b：本线Python明确解释器、锁定依赖、测试collect非零，使用本线venv/build/cache，不修改共享环境、根构建或主线文件；记录实际变更清单。
- c：本线源/依赖/profile/data/binary清单可重建；主线无关文件变化不改变本线产品hash，外部产物变化会被其内容hash捕获。
- d：临时独立fixture中的断言失败/空inventory确实使本线执行器非零，保存Red日志并清除探针；正常集合无永久故意失败。

### DE-02 Manifest、文件与只读校验

- a：合法dataset/asset manifest包含schema、dtype/layout、rate、frames、channels稳定ID、role、时域/区间、hash/recipe/许可；缺字段、重复ID、非法枚举或不支持schema诊断具体。
- b：声明513帧实际512帧、字节数不整帧、错误hash、缺载荷均失败，不能依靠整体RMS放行；不发布dataset_complete。此项是首条产品Red。
- c：PCM16/float32 WAV及extensible对应子格式按声明解码；未知格式、截断/伪大chunk、非法align、padding错误、重复关键chunk与NaN/Inf均明确拒绝，不越界分配。
- d：新目录原子发布、失败/中断不留成功标记；不覆盖已存在生成目录/用户原音频；读前后hash不变，中文空格路径和相对资源可用。

## 4. D1：固定数据与可复现性

### DE-03 F01/F02与解析期望

- a：F01覆盖0/1/479/480/481/511/512/513/16037帧、16/48kHz、1kHz/0.25幅度及静音/脉冲；全部元数据与独立手算点/公式正确，零长容器或消费者接受/诊断明确。
- b：F02双/四通道互异频率/符号/脉冲，稳定ID与索引明确；通道互换/丢失/复制及正负号错误均被独立oracle捕获。
- c：Gain各-60/-6/0/+6/+12dB、直通和ChannelSwap期望按实际解码/量化后的输入计算，不调用DUT；长度、有效区间和只读性精确。
- d：锁定同环境/版本/seed两次生成规范样本和文件hash一致；换recipe/seed会产生正确的新身份/记录，不能覆盖旧版本。跨平台差异声明适用基线并数学复核。

### DE-04 F03/F08及有界生成

- a：F03固定seed噪声和随机host分割表可重跑，表覆盖全部帧且无重叠/空洞，包含1/127/256/480/512/1024及不整帧尾部。
- b：恒等/已知整数延迟/计数oracle独立于被测FIFO与调度器，不产生“被测输出等于自身”的假期望；丢/重一个样本或错误偏移会失败。
- c：F08 48kHz四通道60秒及10分钟float文件帧数/通道/有效区间/hash正确；分块生成与验证不保留整段副本，磁盘错误不发布成功。
- d：F08完整生成+验证的增量工作内存≤64MiB（相同进程/已加载依赖为基线，含解码/生成/校验缓冲），并记录总峰值、分块大小与耗时；该工具预算不能替代UI的PERF-02。

## 5. D2：统计、比较与时间契约

### DE-05 Peak/RMS/计数

- a：`[-1,-0.5,0,0.5,1]`的peak=1、RMS=sqrt(0.5)、越界数0；分通道统计、单样本、静音按公式正确。
- b：±1不计越界，±1.25分别计入；full_scale_hits与写前削波/输出饱和端点概念分开，不能从PCM16输出端点反推历史削波。
- c：空向量、NaN/Inf、非法窗口/长度明确diagnostic/undefined；非静音输入的全零输出标退化，不能拿到质量通过；静音专用测试按peak/finite单独判定。
- d：任意固定分块与整段统计在规定容差内一致，输入不变；大文件流式累积有界且不把float当double精度期望，数值误差与内存分开报告。

### DE-06 严格比较、通道与对齐

- a：先精确检查采样率、长度、稳定channel_ids/角色/时钟和窗口，再逐样本容差；相同RMS但交换通道、漏一帧或反相均失败。
- b：显式mapping可同步重排通道/几何引用；缺ID、重复ID、错误端口角色拒绝；禁止默认选择最优通道、自动mono或平均掩盖错误。
- c：已声明整数延迟按逻辑坐标只补偿一次，raw/aligned区分；未知延迟、错误clock、重复补偿、越界/不覆盖固定窗口拒绝，不互相关寻优。
- d：质量profile预设交集/预热排除及尾部区间有精确计数，报告被排除帧；逐样本恒等/Gain长度不符不能靠裁剪通过，输入/产物不重写。

## 6. D3：参考指标与独立数值证明

### DE-07 Clean-reference SNR与RMS增益

- a：正交向量`s=[1,-1,1,-1]`、`n=[1,1,-1,-1]`，y=s+n时SNR=0dB；y/2时SNR=10log10(2)，证明SNR并非增益不变；预设常量/手算独立核验。
- b：RMS增益对0.5/1/2倍幅度分别为-6.020599913279624/0/+6.020599913279624dB，无归一化或去均值；有DC向量按raw公式计算。
- c：非零参考的完全相同输出SNR为positive_infinity；静音参考undefined；全零输出按退化保护失败，错误形状/非有限输入不计算。
- d：有限SNR/增益及按语句中位对独立标量参考，有限/无穷提升和inf-inf有明确kind；阈值边界不以实现容差放宽。

### DE-08 SI-SDR

- a：上述正交向量y=s+n及y/2的SI-SDR均0dB；s+0.5n为6.020599913279624dB，projection alpha与残差可独立复核。
- b：非零纯缩放参考为positive_infinity，正交估计为negative_infinity；小残差采用数值稳定计算，不因相减能量抵消变成假无穷。
- c：静音/近零参考与全零估计按固定eps/退化策略处理，JSON不含NaN/Infinity数值；默认zero_mean=false、公式版本/profile有记录。
- d：C++对独立Python标量/手算向量满足容差，不调用生产指标做expected；错误的alpha、10/20log、隐式去均值或改幅能被回归捕获。

### DE-09 ERLE

- a：echo非零，residual=echo/4，ERLE=12.041199826559248dB；residual=echo为0dB，能量比和eps单位明确。
- b：echo能量≤eps时undefined，残差为0时采用固定eps均方下限并标记，不能通过改变epsilon或窗口抬分；非有限样本拒绝。
- c：只在manifest固定远端单讲区间正式判断，双讲不能由整体ERLE替代near-end SI-SDR；错误角色/采样率/起点明确拒绝。
- d：5～15秒和路径切换15～20秒整数边界精确，逐窗口曲线与整段报告一致；少一个必需区间或选择最佳片段不得通过。

### DE-10 固定频率投影与SIR

- a：独立1kHz/4kHz正弦混合重建分量幅度/能量正确；N=48000的A=0.2、B=0.1基准SIR=6.020599913279624dB，逐分量对手算oracle。
- b：非整周期窗口以所有目标/干扰sin/cos列同时最小二乘，避免假设基正交；独立标量/高精度参考一致，病态/不足样本诊断。
- c：固定参考麦克风、target/interference独立分量及同一配置/状态/窗口计算SIR提升；不自动挑麦克风、方向或频点，缺可靠分量参考不能作正式质量通过。
- d：频率/方向/通道错误、分量近零/未辨识与漂移有明确诊断；simulated F06的投影SIR不能冒充real F07 SIR。

## 7. D4：场景配方与质量判定器

本阶段验证数据和判定器，受控输出是测试夹具，报告须标evaluator_self_test，不登记成真实算法质量结果。

### DE-11 F04降噪场景

- a：支持三条各≥10秒授权语音的导入/混音recipe与0/10dB配置；数值自测可使用明确标注test_only的解析clean向量验证，目标SNR对独立能量公式误差≤0.01dB，clean/noise/mixture关系对量化容差成立。实际三条语音资格在D5核验，解析向量不标为真实语音或QUAL通过。
- b：clean活动区间与0.5秒预热来自固定输入recipe，算法输出不能改变区间；混合后不归一化，原始clean/hash不变。
- c：QUAL-01全部阈值逐项编码：0dB场景SNR中位提升≥2dB；SI-SDR中位变化≥0dB且单条≥-1dB；clean RMS增益[-3,+3]dB；静音peak≤1e-5。边界/略低边界、全零/NaN退化各有真实拒绝测试。
- d：真实授权数据/听评未提供时正式suite为not_verified，即使合成语音形信号和判定器自测通过；正式结果需要实际算法输出与原验收全部证据。

### DE-12 F05回声/AEC场景

- a：16kHz/20秒、FIR+延迟及独立near-end/render生成capture，far-only/near-only/double-talk/10秒路径切换分场景保存；scalar卷积及线性分量关系独立正确，角色/clock/起点明确。
- b：完整支持范围/路径能量/激励与主文档128ms约束一致；5秒预热、5～15秒及15～20秒窗口固定，边界样本精确。
- c：QUAL-02全部门槛正确判定：静态ERLE≥10dB、近端RMS[-3,+3]dB、双讲SI-SDR相对capture变化≥-1dB、路径切换后ERLE≥6dB、长度/有效区间/延迟/finite正确；少一项不通过。
- d：把所有输出置零虽然可使ERLE升高，仍被near-end/degenerate保护拒绝；参考角色互换、时间错移、错误路径切换及无成对参考均不能通过。

### DE-13 F06解析阵列场景

- a：四麦坐标`((i-1.5)*0.035,0,0)`m、343m/s、48kHz，target `(0,1,0)` 1kHz，interference `(sin60°,cos60°,0)` 4kHz；相位延迟由解析式生成，代码不调用Pyroomacoustics/DUT。
- b：方向正负号、固定首尾排除窗口和预设第二方向明确；独立期望响应可捕获错误方向/坐标单位，不能只比两个文件hash不同。
- c：同步重排音频和几何引用等价，只改音频的错误被诊断或数值回归捕获；稳定channel_id与index分开。
- d：QUAL-03仿真部分目标增益[-1,+1]dB、干扰SIR提升≥6dB、重排等价/方向改变判断均按原准则；报告标synthetic_subset，F07真实部分缺失不许正式全过。

## 8. D5：真实数据与资格

### DE-14 数据许可、真实参考和可用性

- a：F04 clean每条来源/作者/许可/允许用途/原始hash/处理链明确；至少三条各10秒，未知许可或时长不足拒绝正式资格，不自动下载/发布未授权资源。
- b：F07明确真实同步四通道录音、几何/坐标/方向/采样率/稳定通道、采集与同步证明、target-alone/interference-alone/mixture及有效区间；合成录音、缺几何/同步/参考不具正式资格。
- c：数据导入只读、大小/shape/内容hash/处理链可复核，转换结果使用新ID和清单；机器路径变化不改变内容身份，缺源文件不扫描整机同名替换。
- d：完整数据包满足主验收所需资格，真实F07数值门槛在外部算法结果到位后验证（SIR提升≥3dB、target RMS[-3,+3]dB）；没有实际结果时数值质量not_verified，不把数据资格passed等同算法质量passed。

DE-14.d的本线必需事实是完整合格数据及正确拒绝缺实际结果；真正QUAL数值通过仍由实际外部结果与主验收共同判定。D5无可靠真实数据时保持未完成，不能只用缺失诊断测试让D5通过。

## 9. D6：CLI、证据与交付

### DE-15 验证器/报告/CLI

- a：显式manifest精确覆盖全部64个子用例和阶段，核对本文hash、inventory与实际逐用例JUnit/CTest结果；零测试/缺注册/未运行/skip/timeout/崩溃均非零。
- b：CLI退出码、JSON状态、每条阈值/实测/窗口/单位/hash与正式/子集scope一致；格式错误、阈值失败、缺数据/证据分别非零，完整成功才有evaluation_complete。
- c：伪造/过期/缺输入hash、缺原始日志或文件、错run_id/complete字段、漏一条质量门槛均被拒绝；不能仅自填passed JSON或从父ID推导子用例。
- d：时间戳新目录保留实际参数数组/cwd/环境/源码与binary/profile/data/hash、逐项状态和原始日志；失败不覆盖成功，正负无穷/undefined合法序列化且聚合不删失败样本。

### DE-16 主线只读消费与独立交付

- a：只读导入当前Gain run.json/complete.json/output.wav，经显式request/sidecar核对hash/帧/通道/区间，对实际解码输入用独立Gain oracle验证；主线未提供字段明确要求补sidecar，不猜测。
- b：评测前后主线run目录和原音频文件清单/hash不变，所有缓存/转换/报告写本线新目录；主线同时构建/写其他run不影响本次冻结引用。
- c：安装导出评测库后，独立消费者编译运行基础/参考指标且无源码绝对路径/JUCE依赖；独立PythonCLI、schemas、profiles、recipe和锁定依赖可按README重跑。
- d：在本线独立Release构建/环境执行全部累计必需测试，交付mainline integration说明、产品/工具/真实质量状态矩阵及剩余缺项；不因工具通过宣称G4/AR-19或全平台通过。

## 10. 注册、证据与完成状态

后续待实现：`tools/data_evaluation/acceptance_manifest.json`、本线verifier、CLI与schemas。manifest为每个DE字母子用例列出精确测试名、runner、stage、required、数据/证据要求、映射主ID；不能存预设pass。单子用例可映射多个runner/人工数据资格审查，全部事实完成才通过。

报告路径`build/reports/data_evaluation/<timestamp>/`，循环`build/reports/data_evaluation/loops/DE0000-xx/`，输入输出只写`build/data_evaluation/`独立目录。本线源码清单只含实际自有产品/依赖/验收，主线产物按只读内容hash记录；共享主文档hash变化需要明确评估profile是否受影响，不自动改期望或覆盖主线清单。

工具自测可使用合成inventory/受控输出，但正式产品或质量报告必须执行真实文件/评测与核验。锁定输入下通过期望分数测试不代表算法改善；真实语音/阵列、听评与实际算法输出缺失均需如实保留。
