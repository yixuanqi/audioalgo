---
title: 自定义节点工作台 Agent TDD 执行规程
status: ready-for-implementation
updated: 2026-10-23
---

# 自定义节点工作台 Agent TDD 执行规程

## 1. 入口、任务边界和授权

先按根[AGENTS](../AGENTS.md)读取原平台、重构和相关节点交互文档，再依次读取[工作台方案](plan_node_authoring_workbench.md)、[工作台测试准则](testing_node_authoring_workbench.md)和本文。读[development_status顶部](development_status.md)和直接证据，核对实际源码，不把计划类名/命令当已有接口。

本次用户只要求把方案落地为MD和启动提示词，当前不实施产品。后续Agent收到“开始自定义节点工作台”等启动指令后，按S0～S5持续实施，自验证、自迭代，常规接口/局部重构/分片推进无需反复确认；不因完成面板、脚手架或一个示例就结束。用户新指令优先，实际工具权限和既有真实桌面暂停保持有效；不自动提交、推送、上传、购买、安装依赖或改系统DPI。

本专项与既有P3/数据评测的需求并存；启动本专项后当前主线为NA，不顺带同时推进无关算法重构。保留原P3借用/通道ID等恢复入口，NA完成或用户另行指定时再接续。原AR/G/NED/UPN缺项继续挂账，不能用NA或组件证据冲掉。

## 2. 恢复检查（只做必要范围）

1. 检查cwd、适用AGENTS、Git/未跟踪文件和用户改动；本方案编写时已有画布自由拖动、启动脚本、命令行打开工程等工作树修改。不得重置或覆盖，不自动提交。
2. 查`NativePlugins`、`PythonTask`、runner、NodeLibrary/Inspector/controller/格式IO、现有模板与测试是否仍符合方案现状。先复用真实实现，不另写一套只供预览的执行器。
3. 对当前将动的文件与二进制检查最近记录/hash；产品未变且已有有效结果就记录复用，不重跑全部R0/SDK消费者/变异/故障探针。纯文档仅做一致性、链接、ID和hash绑定，不编译产品。
4. 验证本次需要的工具版本/路径。当前机器曾使用`E:/0qyx/res/miniconda/python.exe`、Visual Studio/CMake presets，恢复时实际检查；Python OFF/无工具链是明确能力状态，不能调用默认旧Python或自动联网补环境。
5. 创建或接续`build/reports/loops/NAxxxx/`（先查占用，禁止覆盖），写本片几行卡：用户行为、独立期望、第一Red、最小产品路径、精确子项、执行集合、第二观察面和外部缺项。

## 3. 第一条Red与首片可用出口

**第一条产品Red为NA-01.a：现有编辑器没有“新建自定义节点→正文和定义保存为草稿→关闭重开”的用户流程。**

优先用实际EditorComponent/宿主装配的组件测试，寻找待新增的新建入口（可约定component ID为`authoring.new`，这是待实施名称）。当前应因入口/流程缺失出现真实行为断言失败；不可只测一个与生产无关的假DraftStore。利用已存在的依赖注入避免打开设备，组件事件不是OS桌面证据。

首片必须做到：点击生产按钮→打开真正面板→输入代码/声明→保存至测试自有目录→关闭重开内容一致；同时项目节点/边/参数/布局、计算revision、dirty、历史、当前运行结果与活动目录均不改变。关闭不隐式检查/应用。该片只创建最小草稿服务、面板和文件事务，不先搭建完整多语言IDE/热卸载架构。

若缺接口先compile_red，接口可编译后必须获得并保留behavior_red，再最小实现至Green。若实际已有正确能力则核对并复用，不人为破坏代码制造Red。文件权限/依赖下载/构建错误是环境失败，不能冒充目标行为Red。

S0片后立即推进S1：用户写一个真实Python模板→检查/试跑→原子应用→不重启出现节点库→File/新节点/Output运行并读音频对独立oracle。该出口需要的最小版本锁/格式支持随真实消费者交付，随后扩完整更新/便携能力，不能长期只做文档服务/CLI/空窗口。

## 4. 每片TDD循环

### Define

固定一个可验收行为和NA字母子项，先查现有测试，优先扩展有意义的边界，避免同义/镜像测试。定义独立oracle/状态事件次序/拒绝前后不变量、工具与格式限制。涉及契约变化先写简短ADR和独立依据，再同步测试，不为当前错误输出改期望。

### Red

先加入生产路径回归并运行最小目标，保存命令数组/cwd/退出码/完整失败日志和源码清单。区分UI输入、文档/身份、模板/声明、检查进程、工厂/目录、持久化、runtime/ABI与环境层。异步用barrier、可控时钟和注入完成序列稳定重现；进程崩溃/取消用真实自有进程，不在用户宿主里注入故障。

禁止删断言、放宽阈值、生成并批准自己的golden、把必需项改可选、把skip/not_verified变成pass。门禁可以用合成失败输入，产品验收必须实际执行产品。

### Implement → Green

修改最少生产路径，使同一个Red用例通过；UI不执行算法/import/编译，pure库不引入JUCE或项目依赖，新增算法不改通用画布/调度器算法分支。先复用runner/SDK/内容快照/取消/身份；需要扩展公共契约时按方向检查消费者。

草稿写入、Ready、Apply分别绑定generation和内容hash。检查失败/取消不改当前可用版本；Active包不可写；不能以覆盖重复type_id代替版本解析。图升级通过controller一次事务，旧任务使用不可变旧版本。C++保持独立SDK消费，不把生成源码加入宿主编译目标。

Green读取真实输出和独立期望；语法/编译通过不等于算法正确，正确的失败恢复也要执行随后的合法任务。保存/重开读取新文件，不能仅检查存在。不得只比较GUI/CLI两个同源结果。

### Refactor → 直接回归

移除本片重复和临时桥接，不顺带扩展尚未验收能力。记录影响矩阵：通常Debug目标＋直接回归；schema/catalog/ABI/构建变化才扩editor/Python/runtime OFF消费者和一个独立SDK消费。完整矩阵集中稳定出口/最终Release。通过后无新改动/疑点不重复跑同一套。

任何受影响交互回归先修复，不能以“工作台新功能优先”为由隐藏。尤其保留NI0012缩放/右键框选/Inspector多选、NI0014负坐标拖动、原拖线/插入/换序、批量删除/历史、参数编辑、保存/所选输出和旧运行身份。

### 第二观察面 → Evidence → 下一片

组件/纯服务之外，再读实际文件、WAV/NPY、目录/EXE hash、独立子进程或外部SDK消费者；桌面可用且已获相应授权时执行真实输入和DPI证据。不能用组件渲染替代OS桌面，也不因桌面暂停而冻结独立实现。

更新精确manifest映射、切片报告、development_status唯一摘要和下一条Red。日志追加不覆盖；代码/测试/精确映射同片提交到工作区，Git提交仍需用户另行要求。若同根因三轮无进展，缩小重现、查锁定上游和生命周期假设、记录调整后继续，不重复盲改阈值。

## 5. 阶段必需项绑定

manifest从S0就列全68项，未实现的保持not_run；每项设唯一最早正式验收阶段，之后累计保留。阶段映射如下，不因缺桌面临时移到更晚阶段。NA-16.a的完整inventory绑定在S5闭合，S0只交付manifest和当前已实现映射，不虚注册68个空测试。

| 阶段 | 最早必需的NA子项 |
| --- | --- |
| S0 | NA-01.a～d；NA-02.a/b；NA-14.a/b/c；NA-16.b/c/d |
| S1 | NA-03.a/d；NA-04.a～d；NA-05.a/b/d；NA-07.a/b；NA-08.a～d；NA-09.a；NA-11.a/b；NA-15.a/b/c；NA-17.a |
| S2 | NA-02.c/d；NA-03.b/c；NA-05.c；NA-06.a～d；NA-14.d |
| S3 | NA-07.c/d；NA-09.b/c/d；NA-10.a～d；NA-11.c/d；NA-17.b |
| S4 | NA-12.a～d；NA-13.a～d；NA-17.c |
| S5 | NA-15.d；NA-16.a；NA-17.d |

NA-15.a/b/c按实际产品影响和有效证据绑定当前已支持行为；不是要求在S1伪造旧平台所有未完成项通过。阶段只能在全部累计必需项通过后passed；S1桌面NA-17.a或S2 DPI暂停时阶段保持非通过，但允许继续S2～S4不依赖桌面的工作，准确展示“实现/自动化子集/桌面缺项”。

S1和S3提供可运行Python示例与独立音频核验，S4提供C++实例及同EXE更新证明；S5做当前Release正式累计，不把其工作提前到每个helper。第三方编辑器/LSP、关闭自动激活、同图同type多版本、原生热卸载和原生正式执行隔离不在本版必需范围，不以实现它们拖延已定出口。

## 6. 可重跑命令与待建能力

现有入口在确认存在及构型后可使用：

```powershell
cmake --build --preset windows-debug --target audioalgo_tests AudioAlgoEditor --parallel 4
ctest --test-dir build/windows-debug -C Debug --show-only=json-v1 --no-tests=error
& 'E:/0qyx/res/miniconda/python.exe' scripts/record_command.py --output build/reports/loops/NA0001/red -- ctest --test-dir build/windows-debug -C Debug -R '^NA-01.a$' --output-on-failure --no-tests=error
```

其中`NA-01.a`须先真实注册并构建；未注册时no-tests非零是门禁失败，不是产品Red。新测试可以注册独立pure/component目标，target名字按实际模块确定，不要求所有测试继续塞进大可执行文件。源文件/命令路径带空格时使用结构化参数数组，正文和多行描述写文件，不插值执行源码。

以下都是待建接口，先按测试文档实现才使用：

```powershell
& 'E:/0qyx/res/miniconda/python.exe' scripts/verify_node_authoring.py --stage S1 --build-dir build/windows-debug --config Debug --development-subset
& 'E:/0qyx/res/miniconda/python.exe' scripts/verify_node_authoring.py --stage S5 --build-dir build/windows-release --config Release --evidence <本次证据文件>
```

开发子集不能给整个阶段passed；正式入口缺必需桌面/环境证据必须非零。新manifest/验证适配入口复用现有记录、JUnit和hash策略，不复制一套更宽松的通过规则。测试发现系统Temp不可写时，可为测试进程设置工作区自有TEMP/TMP并只重跑受影响项，记录原失败和环境修正，不修改产品绕过权限。

## 7. 报告和收口

每片`loop.md/summary.json`至少包含目标与NA子项、当前原改动/本片差异、第一真实Red、同目标Green、直接回归、构型、source/host/test/SDK/environment/package/source/audio hash、完整命令/JUnit/退出码、独立观察、限制与下一条Red。开发/正式scope与全平台状态分开。`tests/node_authoring_manifest.json`逐项登记精确测试名、shared evidence、stage、required、layer、runner和证据要求；不手填passed取代执行。

交接时说明当前用户实际能做什么、入口/示例、自动化哪些通过、桌面/构型/质量哪些未验证、可恢复命令。新版本必须真正运行新代码，旧工程/旧结果必须仍可归属旧版本。旧源码/日志/音频不能被覆盖以制造“升级成功”。

本专项完成条件为测试文档68个NA子项及受影响旧合同全部通过，当前Release/真实桌面证据齐全；未满足则持续可独立工作或准确交接外部缺项，不宣称完成。不因需要恢复旧P3工作而提前将本专项标为passed。

## 8. 后续Agent启动提示词

> 在当前audioalgo工作区开始实现“自定义节点工作台”。先按AGENTS.md读取原平台/重构/交互约束，再依次读doc/plan_node_authoring_workbench.md、doc/testing_node_authoring_workbench.md、doc/agent_loop_node_authoring_workbench.md及development_status唯一摘要，保留所有已有改动。从NA-01.a的真实生产按钮→编辑面板→草稿保存重开行为Red开始，按S0～S5持续TDD开发；先交付Python编写→检查/隔离试跑→原子应用→节点库刷新→画布运行，再完善精确版本/工程迁移和C++独立SDK构建/probe/保存重启激活。关闭面板默认保存草稿，失败保留旧可用版本，不热卸载DLL，不重编宿主来新增用户节点。一次一个可验收行为，Red→最小实现→同目标Green→直接回归→独立产物验证→证据和下一片；已有正确行为不造假Red，不删断言/放宽阈值/跳过必需项。保留缩放连线、右键多选、负坐标自由拖动、端口/历史/参数、保存兼容与旧运行身份等现有能力；NA共68个字母子项精确映射，原AR/G/NED/UPN缺项保留。Debug增量为主，公共契约变化才扩消费者，完整Release集中出口。更新状态与build/reports，真实桌面仍暂停则准确not_verified并继续独立工作；不得自动提交/推送/发布/安装依赖或覆盖用户文件，不把面板骨架、单个示例或组件子集称为专项/平台完成。
