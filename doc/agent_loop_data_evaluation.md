---
title: 数据与独立评测线 Agent TDD 与 Loop Engineering 规程
status: ready-for-implementation
updated: 2026-10-18
---

# 数据与独立评测线 Agent 执行规程

## 1. 启动入口与授权

先按根AGENTS读三份主文档，了解G0～G6/AR/NED要求；再读[数据线开发方案](plan_data_evaluation.md)、[测试及通过准则](testing_data_evaluation.md)、本规程与[本线状态](development_status_data_evaluation.md)。按需要只读重构方案和最新共享状态，以确认接口/产物事实，不接管R0～R6任务。

当前人类任务明确是并行的数据与评测线，实施范围优先于根入口中主线排程。后续代码任务按D0～D6自主开发/测试/自验证/自迭代，常规技术选择及阶段推进不重复确认。遵守工具权限，不自动提交/推送/发布/购买，不覆盖用户文件或其他Agent改动。

只写本线允许目录，维护本线状态和独立报告，不编辑共享AGENTS、根CMake/presets、主API、AR/NED/verifier、主状态或主验收文档。方案指出的主线问题作为只读集成约束；不能为了修本线测试顺手修改主线。新根构建/运行接入需求记录integration说明，留给主线按授权实施。

本规程的Loop Engineering是有限行为迭代，不要求后台无限循环、定时任务或默认派生子Agent。本次文档阶段没有产品测试通过结论，启动后重新建立事实。

## 2. 恢复与基线

1. 检查Git/工作区、主线正在修改的文件和本线所有权；已有Git则使用其状态加工作树内容hash，不创建提交，无Git不自行初始化。
2. 检查本线目录/代码/测试/接口是否真实存在，未实现标planned/not_run。不要把已有verify_run_audio.py或原算法测试当作本线通用指标实现。
3. 定位实际CMake/MSVC/Python3和锁定依赖/cache；本线使用独立configure/build/PDB/temp/venv，禁用共享自动下载或修改全局环境。若依赖获取需要工具审批按实际流程执行。
4. 创建本线source/env/profile/data manifest及测试inventory；本线已有实现则执行最相关测试，没有则从D0基础设施及DE-02.b起步。
5. 读取旧DE报告和下一条Red，对本线修改影响范围重跑；主线持续变化时只记录所消费的固定产物hash，不要求其全仓源hash永远不变。
6. 不启动或关闭主线编辑器，不复用其输出目录。需要真实算法输出可只读消费现存完整run；不完整或必需字段缺失时要求显式sidecar并诊断，不写入原run。

## 3. 第一条产品Red

固定DE-02.b：构造声明513帧、单通道float32但实际512帧的载荷与manifest；请求validate必须返回结构化`payload_size_mismatch`一类诊断、CLI非零、没有dataset_complete或evaluation_complete。相同RMS、相同峰值不能改变拒绝结果。

接口不存在时首次可记compile_red；接口出现后必须看到语义断言失败并保存日志。先实现最小可测入口，错误字节数未被检查时出现正确失败，再加最小校验；不要通过刻意破坏已正确产品制造Red。若现有能力直接通过，记录事实，用受控独立mutation/故障fixture证明测试敏感度并恢复。

与其并行的基础设施Red仅用本线临时CTest/pytest fixture：失败、零测试、skip、timeout/缺注册应传到验证器非零。合成结果只能验证基础设施，不能记为产品数据/算法通过。

## 4. 每次循环

### Define

选一个DE字母子用例，明确输入、dtype/布局、时间/窗口、固定公式、许可/资格、expected、容差和失败条件。引用主测试ID只作映射，不自动宣称主Gate完成。优先扩展已有测试；不为目录命名、颜色或排版写镜像测试。

小向量先手算。例如peak/RMS用`[-1,-0.5,0,0.5,1]`；SNR/SI-SDR用四点正交向量；ERLE用residual=echo/4；频率投影用已知幅度sin/cos。期望常量写明推导，严禁调用生产函数计算expected。

### Red

先写并运行测试，保存参数数组、cwd、退出码、完整日志和关键断言。区分工具/环境、文件契约、公式、数据资格、阈值判定和外部证据缺口；缺编译器不是数值Red，无授权数据不是算法质量失败。

### Minimal implementation

实现最小正确的生产路径。C++纯库不依赖JUCE/NodeSDK/主AudioBuffer；Python只负责配方/文件/编排，指标核心不维护第二套生产回退。控制载荷大小、逐通道/窗口，不隐式重采样/去均值/归一化/最优对齐。不因一个公式失败改变eps/profile/质量门槛。

### Green

重跑同一目标断言，检查真实执行非零数量且无skip。实际读取生成文件、样本数、元数据/hash及输入只读性；读取评测报告的逐项值/单位/有效性/原因，不能只看退出0或report文件存在。

### Refactor与有限回归

Green后去掉重复、提取共用schema/I/O/诊断及稳定数值累积；重跑当前模块和消费者。阶段完成跑累计DE与本线CLI/consumer集合。通过且无新改动/失败/疑虑时继续下一行为，不反复跑完整F08或整套数据浪费资源。

### 第二观察面

| 开发项 | 首观察面 | 独立观察面 |
| --- | --- | --- |
| 数据生成 | 元数据/hash/长度 | 手算点、独立标量/解析公式及分量关系 |
| WAV/PCM | 端点/截断/错误格式 | 独立fixture/解码及量化参考，不调用主writer生成期望 |
| 指标 | 手算常量 | 不同实现的标量参考、性质和退化测试 |
| 频率投影 | 整周期已知分量 | 非整周期同时拟合与独立参考 |
| profile判定 | 边界值测试 | 漏阈值/错窗口/全零等对抗用例，拒绝伪通过 |
| 只读接入 | manifest/complete/run核验 | 主线文件清单前后hash一致，独立数学oracle |
| 独立库 | target/header审查 | 安装后独立消费者，无源码路径或JUCE |
| 数据资格 | 来源/许可/通道清单 | 实际文件/同步/几何/参考分量与采集记录 |

第二观察可由同一Agent执行，必须真实读产物和判断依据，不能复述实现。人工授权数据审查与算法听评分别记录，不用截图/表格填pass代替事实。

### Evidence与继续

保存本线源码/依赖/binary/公式/profile/data/hash和命令结果，更新本线状态及下一条Red，直接继续。阶段缺外部数据时保留未通过并推进后续独立代码，不以“是否继续”结束已授权工作。

## 5. 阶段行动顺序

| 阶段 | 开发重点 | 必需新增ID |
| --- | --- | --- |
| D0 | 本线构建/测试、manifest、解码/严格载荷校验、最小失败传播 | DE-01～02 |
| D1 | F01/F02/F03/F08固定recipe、独立oracle和有界生成 | DE-03～04 |
| D2 | 统计/计数、严格比较、channel/window/latency | DE-05～06 |
| D3 | SNR/SI-SDR/ERLE/RMS增益、分量投影/SIR | DE-07～10 |
| D4 | F04/F05/F06数据和质量profile判断能力 | DE-11～13 |
| D5 | 实际授权语音和真实同步阵列导入/资格 | DE-14 |
| D6 | 完整CLI/verifier/报告、主线只读接入、安装consumer交付 | DE-15～16 |

每阶段不等于一个巨型改动；循环ID按DE0000-01等分配，创建前检查现存目录。D5缺实际数据时仍可实现工具和D6，但完整状态保持not_verified。固定输入的手算控制输出只证明评测器，不把它当新算法。

## 6. 运行入口（全部待实现）

规划时尚未交付以下本线CMake、包、CLI和verifier。启动前检测，先实现后执行；缺文件记not_run，不把下列示例当历史成功日志。当前Python可用路径应实际检查，解释器与venv创建工具使用独立环境并记录。

```powershell
cmake -S tools/data_evaluation -B build/data_evaluation/native-debug -G "Visual Studio 18 2026" -A x64
cmake --build build/data_evaluation/native-debug --config Debug --parallel 4
ctest --test-dir build/data_evaluation/native-debug -C Debug --show-only=json-v1 --no-tests=error
ctest --test-dir build/data_evaluation/native-debug -C Debug --output-on-failure --no-tests=error
& build/data_evaluation/venv/Scripts/python.exe -m pytest tools/data_evaluation/tests/python --collect-only -q
& build/data_evaluation/venv/Scripts/python.exe -m pytest tools/data_evaluation/tests/python --junitxml=build/reports/data_evaluation/pytest.xml
```

Python包应安装到本线环境，不能依赖主线sys.path或工作目录偶然可见性。模块入口契约如下，参数可在实现时一致调整并更新文档：

```powershell
& build/data_evaluation/venv/Scripts/python.exe -m audioalgo_eval generate --recipe tests/fixtures/data_evaluation/recipes/f01.json --output-dir build/data_evaluation/datasets/f01-new
& build/data_evaluation/venv/Scripts/python.exe -m audioalgo_eval validate --dataset build/data_evaluation/datasets/f01-new/manifest.json
& build/data_evaluation/venv/Scripts/python.exe -m audioalgo_eval evaluate --request build/data_evaluation/requests/gain.json --metrics-executable build/data_evaluation/native-debug/bin/audio_eval_metrics.exe --output-dir build/reports/data_evaluation/gain-new
& build/data_evaluation/venv/Scripts/python.exe -m audioalgo_eval verify --stage D0 --build-dir build/data_evaluation/native-debug --config Debug
```

独立工程可依据本机可用工具链选等价生成器，不改根presets。Release、安装导出、consumer同样使用本线独立目录，配置和命令数组入报告。不得同时启动主线使用的同一build并行写PDB。

## 7. 证据和失败调整

本线状态文档保持简短、可恢复：当前D/DE循环、已实现/已验证/未验证、实际失败与原始日志、数据资格、下一条Red及重跑命令。循环目录`build/reports/data_evaluation/loops/DE0000-xx/`保存loop.md、red/green/regression日志、JUnit/CTest、源/binary/data/profile清单；汇总按独立时间戳保存，不覆盖失败。

每份报告至少记录实际命令数组、cwd、UTC开始/结束、exit、通过/失败/skip/timeout数、环境、本线产品hash及消费产物hash。人工数据资格缺证据明确not_verified；工具自测使用scope=evaluator_self_test，仿真scope=synthetic_subset，外部真实算法scope=external_algorithm_result。

同根因连续三次没有进展，缩小为一个通道/短向量/单窗口/单文件，检查dtype、PCM缩放、10/20log、dot投影、eps单位、起点/延迟符号及窗口边界；核对原公式/主验收，写本线ADR后换诊断继续。不得自动更新golden、随输出改threshold或删除异常条目。

真正缺权限、数据或输入且没有可行独立工作才交接阻塞。使用实际工具流程申请必要权限，不能以历史脚本或其他工具绕过审批。对真实F04/F07资源不足应继续完成可独立的库、CLI、profile判定器、安装和集成文档。

## 8. 最终交付与启动提示词

交付独立算法评测库/CLI、F01～F08 recipe和具备资格的数据清单、formula/profile/schema与锁定依赖、手算/独立参考测试、完整逐项报告、主线只读集成说明。完整数据线通过与工具子集通过、真实算法QUAL通过分开；不修改主线Gate状态或宣称桌面/设备验收完成。

> 在当前audioalgo仓库独立实施数据与评测线。先读AGENTS要求的三份主文档，再读doc/plan_data_evaluation.md、doc/testing_data_evaluation.md、doc/agent_loop_data_evaluation.md和doc/development_status_data_evaluation.md，核对实际源码与并行所有权。只修改本线audio_evaluation、tools/data_evaluation、专属fixture/suite/文档及独立build/report目录，不改共享构建、主API、AR/NED清单或主进度。按D0～D6持续开发，首条产品Red为DE-02.b：声明513帧但实际512帧的载荷必须被拒绝且无成功标记；随后完成固定数据/独立oracle、纯C++统计和参考指标、严格通道/窗口/延迟、场景profiles、真实数据资格、独立CLI与只读run接入。每次一个DE字母子用例，执行Define→真实Red→最小实现→Green→Refactor→累计回归→独立核验→证据记录。公式与原质量门槛固定，不偷偷归一化/对齐/裁剪，不用DUT生成expected，不删断言、不跳过必需项、不伪造数据许可、golden或质量通过。三次同根因无进展则最小化重现并核对数学/文件/时间契约后继续。维护本线状态与DE循环报告，工具自测、仿真、真实数据资格和实际算法质量分开；缺F04/F07等外部条件保留not_verified并继续独立工作。常规开发与阶段推进无需确认，遵守实际工具权限，不自动提交/推送/发布/购买、不覆盖用户音频；向主线交付库、数据、文件契约与真实报告，由主线接入共享工程。
