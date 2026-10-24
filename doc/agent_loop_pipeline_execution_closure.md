# Pipeline闭环修复：Agent TDD执行规程

2026-10-24。本文指导后续Agent实施，当前任务只完成文档。配合[修改方案](plan_pipeline_execution_closure.md)、[测试通过准则](testing_pipeline_execution_closure.md)，并继续遵守AGENTS及原工作台TDD规程。

## 1. 恢复入口和首片

先按[AGENTS](../AGENTS.md)阅读适用文档，再读本补充三文档、[唯一当前摘要](development_status.md)及最新原始证据。保留当前工作区已有未提交/未跟踪改动；不得reset、覆盖其他工作或自动提交。

当前已知事实：

1. NA0030的118/118、Python OFF54/54、纯库6/6仅属于所记快照，不覆盖新增卷根保存和可见日志验收。
2. NA0031只读探针已证明当前MSVC处理扩展卷根的parent_path丢分隔符；它不是产品保存Green，未写D盘。证据`build/reports/loops/NA0031-root-path-diagnosis/read-only-result.json/log`。
3. 混合执行、Python stdout/stderr采集及成功运行provenance已经存在。缺的是用户可见日志、失败/中间事件和完整身份闭环，不能先重写调度器。
4. 原生v1独立插件已存在；新日志扩展与应用内C++构建激活尚未完成。模板类型/脚本/PC编号是待实现，先检查实际源码。

**首片只做PC-01：卷根保存路径Red→修复→直接回归。** 先扩展真实路径/文件服务测试，断言生产计算的父目录保留卷根分隔符，确认与用户截图同根因；不先修改实现，不以探针输出代替生产行为。若需先暴露最小可测职责，compile_red单列，随后必须取得behavior_red。

首片完成马上推进PC-02生产Print闭环，接PC-03/04日志身份/失败/预算，再定向核对PC-05混合计算，然后PC-06 SDK日志兼容、PC-07 C++工作台及PC-08版本携带。各片可按实际依赖拆分，不一次实现所有组件；既有正确行为复用，不重新制造Red。

## 2. 每片执行卡

在一个未占用的`build/reports/loops/`目录中写几行：

```text
用户行为 / PC具体子项 / 原NA或AR/NED/UPN关联
已有产品路径与测试 / 独立期望 / 首条失败断言
最小产品修改 / 目标构型 / 必要直接回归及扩测原因
独立观察面 / 桌面或资源限制 / 下一片
```

先查现有目录，选择新前缀，不覆盖NA0031或其他证据。一个实际测试/运行可以关联多个要求，不为相同事实建立多份重复验证器。PC映射与原68项并存，不能把部分新增通过升级成整个原NA通过。

## 3. Red → Green → Refactor → Regression → Evidence

**Red**：先写失败测试并执行真实生产路径；记录精确测试名/构型/命令/source/test/EXE身份及原始失败。区分路径、状态、UI输入、日志、调度、ABI、环境层。零收集、权限拒绝、编译环境错误不能代替目标Red。异步用barrier、受控完成序列和注入时钟，不能拉长等待来掩盖挂起。

**Green**：只修满足该行为的最小生产代码，不加算法名/Print特判。UI不执行import/编译/目录扫描/进程等待；纯算法与计划不依赖JUCE/GUI/工作台。根路径修复保留长路径与安全检查；日志接收遵守完整身份；C++ ABI旧结构和入口不破坏。写完运行同一Red用例，核对真实文件或数值。

**Refactor**：Green后消除必要重复，保持职责和接口最小。若契约真的需要改变，先写ADR及独立验收依据再改测试；不能把当前错误行为写成新期望。不要顺带做无关平台重构。

**Regression**：依据测试文档第10节，只执行直接受影响集合，Debug增量优先。公共接口/构建/SDK变化才补相关消费者。已有匹配源码且未受影响的数值/SDK证据写明复用理由；不每片重复整个矩阵。新增失败先修，不以新功能优先隐藏回退。

**Evidence**：至少两个观察面：生产组件/真实worker的输入与状态断言，加独立读取实际`.aadraft`/锁/日志/WAV/NPY/DLL/EXE。检查日志不能冒充正式Run日志；生成包/库条目不能冒充执行新算法；GUI与CLI不能只彼此比较。记录source/测试/宿主/SDK/包/资源hash和实际容差。

当同根因连续三次无进展，停止重复相同尝试，缩小复现、核对上游接口与Windows/ABI/生命周期假设，写明调整再继续。不要放宽预算或换场景绕过真实缺陷。

## 4. 可用命令与执行边界

确认本机preset和工具仍存在。当前可用Python为`E:/0qyx/res/miniconda/python.exe`，不要误用PATH中的旧Python。下列参数为已有入口；新增PC测试必须实际注册后再筛选：

```powershell
cmake --build --preset windows-debug --target audio_node_authoring_tests audioalgo_tests AudioAlgoEditor --parallel 4
ctest --test-dir build/windows-debug -C Debug --show-only=json-v1 --no-tests=error
```

记录命令时先选新目录；用实际发现的精确测试名替换筛选条件，先检查测试数量非零。JUnit使用绝对路径，避免`--test-dir`把相对输出放入嵌套目录。例如首片的目标纯草稿集合：

```powershell
$loopDirectory = Join-Path (Get-Location).Path 'build/reports/loops/PC01-root-save-NEW'
& 'E:/0qyx/res/miniconda/python.exe' scripts/record_command.py --output (Join-Path $loopDirectory 'red') --artifact build/windows-debug/bin/audio_node_authoring_tests.exe -- ctest --test-dir build/windows-debug -C Debug -R '^NA-01\.' --output-on-failure --no-tests=error --output-junit (Join-Path $loopDirectory 'red.xml')
```

`NEW`必须替换为未占用后缀，此命令只是已有NA-01集合的开发示例，不能据此声称已经执行新增PC测试。必要时把新增精确名称加入筛选，并检查真实inventory与JUnit。产品改变后构建需完成再测试，不并发修改/重链同一二进制后继续引用旧hash。

涉及SDK与绝对路径的CMake参数整项引用，例如`'-DPython3_EXECUTABLE=E:/0qyx/res/miniconda/python.exe'`。不能只看exit0：核对CMAKE_HOME_DIRECTORY、构型/开关及实际目标，避免错配成第三方依赖工程。不要调用尚不存在的专项验收脚本声称已过。

当前权限不足的构建/外部消费者按实际工具审批处理并记录；不修改ACL、不默认管理员运行作为产品修复。卷根实写、仓库外SDK消费者需合法测试位置；不能写用户既有文件，不能把未执行记通过。

## 5. 无回退清单

- 画布：缩放连线、端口命中、右键四向框选、多选Inspector、负坐标拖动、节点库拖放、插入/换序、批量删除及一次撤销。
- 文档：Save/SaveAs/Open/Recover/Cancel/Discard、外部修改保护、恢复绑定、500ms防抖、源码焦点撤销、配置会话降级和正常运行副本部署。
- 运行：File/Gain/Output、ChannelSwap/Mixer、多源/所选输出、混合状态/固定帧/已知延迟/有限尾部/EOS、资源与代码冻结、运行身份门禁和历史结果归属。
- 扩展：既有C ABI v1 DLL、SDK独立消费、模块租约、Python OFF与纯库依赖边界；新增用户节点不需要重编宿主。
- 验收：不删断言、不改golden、不降容差/预算、不跳必需项；原68个NA与原平台要求全部保留。

根据实际改动选择相关项执行并给出依据，不要求每片机械执行整张清单；稳定/最终出口仍完成全部累计必需验收。

## 6. 交接和完成条件

每片代码、测试、示例、精确映射、原始命令/JUnit及独立产物核验、简短`loop.md/summary.json`同片更新到工作区；同步`doc/development_status.md`，写明用户新增可用能力、实际已过范围、已知失败、未验证项和下一条Red。Git提交/推送/发布不自动执行。

区分“代码实现”“自动化开发子集”“正式桌面”“整个专项/平台”。缺桌面不冻结独立实现；当前不得重启/关闭用户窗口、改变系统DPI或以组件渲染伪造桌面证据。C++probe与自有子进程可按权限独立测试，正式重启验收待桌面恢复授权。

PC-01～08全部必需条款、受影响旧契约和正式外部证据未闭合前，不报告本补充专项完整通过。不得以完成方案、一个Print、一个CLI样例或窗口骨架结束既定开发；此补充完成后继续原NA未完范围。

## 7. 后续Agent启动提示词

在当前audioalgo工作区继续代码实现，先遵守AGENTS.md，阅读development_status唯一当前摘要，以及doc/plan_pipeline_execution_closure.md、doc/testing_pipeline_execution_closure.md、doc/agent_loop_pipeline_execution_closure.md和适用原工作台/节点交互约束。保留所有未提交改动，先从PC-01卷根保存的真实生产Red开始：NA0031只读探针已确认扩展路径parent_path丢卷根分隔符，但产品尚未修复；不能归因于D盘没权限或用换子目录绕过。按Red→定位层次→最小实现→同目标Green→直接回归→独立实际产物→精确映射/状态记录推进；随后完成PC-02编写Print→检查/Apply→节点库生产拖放→Canvas Run→本次日志可见，再做日志身份/失败/预算、两方向C++/Python混合数值、兼容C++日志SDK与应用内独立构建/probe/PendingRestart及版本携带。复用RunService/MixedExecution，不重写已过调度、不写Print特判；保留v1 DLL兼容、不可变版本和精确锁，新增节点不重编宿主。保留缩放连线、右键多选、负坐标、拖放/历史/参数/保存/旧运行身份；不删断言、不放宽阈值或预算、不跳必需项。Debug增量为主，公共边界变化才扩相关构型及独立SDK消费者，已有未受影响同源证据说明复用。当前真实桌面仍暂停，不操作用户窗口，缺项如实not_verified并继续独立工作；不自动提交、推送、发布、安装依赖或修改权限，不把历史118项/组件子集当作新需求或整个平台完成。每片维护真实报告和下一条Red，直到已定范围闭合。
