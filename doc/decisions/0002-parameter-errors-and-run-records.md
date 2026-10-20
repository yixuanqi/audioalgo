# 参数诊断与运行参数记录（2026-10-20）

JUCE Slider默认文本解析只取数字前缀，非法文本会静默变成0，并在范围内成为合法参数命令。面板在Slider归一化前完整解析文本并调用共享descriptor数值校验；失败返回原值，不发命令，显示字段诊断。controller/runtime继续独立校验，控件禁用不是信任边界。

NodeInspector从Main.cpp提取，供生产宿主及真实组件测试复用。接受参数命令后无通知同步已有控件，不在当前控件回调内重建/销毁它。Reset默认值来自descriptor，经原命令路径；Undo/Redo保留仍存在的选中节点并刷新Inspector。

ParameterDescriptor增加尾部 `input_resource`（默认false），只有path字段可声明。普通path可能表示输出位置，不能一律读文件。文件存在性/解析/hash检查属于运行服务，不把I/O放入controller或消息线程；编辑中允许空/待重定位路径，执行时明确拒绝且定位节点/参数。内置File source和六参数测试节点的系数资源显式声明此语义。

RunService保存含实际默认值的 `project.aaproj`，run.json关联其hash及资源node/parameter/path/hash。tag等元数据保留在运行快照，不伪装为DSP效果。对声明的输入资源运行前后核对hash；这仍不是R5所要求的内容副本冻结，资源在处理期间短暂改动后恢复等强快照场景仍待R5，不提前标AR-16通过。运行目录失败guard和最后发布complete.json规则保留。

桌面交互按用户指示继续暂停；组件输入和独立文件oracle不替代AR-10.h、AR-05.b或NED桌面证据。

后续AR-10.e采用`visible_if`/`enabled_if`单个类型化相等条件，无表达式解释器。注册时检查所引用描述与值；View按当前参数或默认值刷新可见/可用状态。未知`opaque_parameters`显示只读JSON与未支持原因，不提供修改或默认恢复动作；controller历史与工程保存继续保留原结构。
