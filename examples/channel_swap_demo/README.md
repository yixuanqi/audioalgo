# ChannelSwap 示例与扩展接入

`example.cpp_channel_swap@1.0.0` 使用一个命名输入 `audio_in` 和输出 `audio_out`，v1接受16/48kHz双声道，拒绝单/四通道。`enabled=true`交换两个通道位置的样本，false恒等；保持长度、有效区间与位置channel ID。算法无延迟、无尾部、无跨块状态；参数用于下一次离线渲染。无效格式、参数或非有限样本给具体诊断。

```powershell
build/windows-debug/bin/audio_cli.exe run examples/channel_swap_demo/channel_swap_demo.aaproj build/runs/channel-swap-demo 127
```

输出目录必须是新路径。输入为本地解析公式生成的513帧双通道PCM16，配方与SHA见demo_manifest.json；已有音频不会被生成脚本覆盖。独立oracle按PCM16/32768解码，输出左右互换，不调用被测算法；禁用参数时输出恒等。

纯调用包含 `audio_algorithms/ChannelSwap.h`，链接安装导出的 `AudioAlgo::algorithms`；prepare强类型配置与格式，process只借用两个输入/输出channel view，输入输出存储必须分离，调用结束不得保留view。reset后必须重新prepare；同类实例不共享配置/状态。`apply_channel_swap`为拥有buffer的便捷接口，复用同一内核。没有SDK、工程或JUCE依赖。

节点适配位于 `plugins/examples/ChannelSwapNode.*`；描述与工厂通过register_node成对注册，默认目录在共享组合函数register_builtin_nodes装配，因此GUI/CLI获得相同插件。接入只新增算法、描述/adapter、构建/组合注册、测试和示例；通用画布、表单、编译器、调度器无需算法分支。参数label/help/default来自描述，摘要使用同一元数据。

接入验收暴露了已有的通用计划错误定位缺陷，已用原固定帧/采样率/缺工厂fixture独立修复。原始冻结记录与失败保留；R4范围比较使用明确标注的“框架修复完成、插件未注册”新基线，再只恢复组合注册验证。该修复没有新增算法分支，不能把最初基线说成完全未改。

这是一份真实原生扩展示例，不宣称低延迟设备链、动态DLL热卸载、Python或R5多端口/FIFO能力。桌面操作按用户指示暂停；组件/CLI产物不替代AR-12.d所需的真实桌面证据。
