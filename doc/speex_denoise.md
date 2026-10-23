# Speex Denoise 当前可用路径

Debug preset已启用真实 SpeexDSP 1.2.1。`dependencies/versions.json` 的URL/SHA-512锁定原始源码；CMake优先复用已校验下载缓存，采用浮点 SmallFT 构建，独立算法库无 JUCE/项目模型依赖。安装算法组件时包含该静态依赖及BSD许可；安装包变量 `AudioAlgo_SPEEXDSP_ENABLED` 描述可用性。Node SDK安装组件仍为纯C头文件，不包含Speex/JUCE。

打开 `examples/speex_denoise_demo/speex_denoise.aaproj`，或从节点库添加 Speex Denoise，连接 File.audio → audio_in / audio_out → Output.audio。参数 Noise suppression 是整数 −60..0 dB（默认−20）。修改参数用于下一次离线运行；没有运行中动态配置、任意flush或无限尾部承诺。

```powershell
cmake --preset windows-debug
cmake --build build/windows-debug --config Debug --target AudioAlgoEditor audio_cli --parallel 4
build/windows-debug/bin/audio_cli.exe run examples/speex_denoise_demo/speex_denoise.aaproj build/runs/my-denoise 127
ctest --test-dir build/windows-debug -C Debug -R '^(ALG-|UPN_)|AR-15.[bd]' --output-on-failure
```

输入/输出为float32 planar，支持16/48kHz、1/2/4通道。每通道独立原库实例，以480帧处理；int16转换是乘32768、四舍五入（半值远离零）、饱和至[−32768,32767]，输出除32768。当前配置中480帧在48kHz为10ms、16kHz为30ms，后者是明确锁定且经过直调数值验证的profile；不宣称所有帧长可用。

算法原生分析/合成重叠延迟480样本，FIFO再引入479样本。运行服务向JUCE声明总延迟959，末端只裁剪一次并补零完成所需末帧；output.wav保持原样本数，有延迟时另保存output.raw.wav。run.json记录实际原始/对齐长度、offset、原始有效起点、两个文件hash及节点版本。AGC、VAD、dereverb关闭；VAD读取并确认原库默认关闭，不调用即使关闭也会打印警告的SET_VAD。

已执行原库直调 → 独立core → Node/backend → RunService/CLI，包含静音、脉冲、固定seed噪声，16/48kHz×1/2/4通道×host1/127/512，以及两实例、重复prepare/reset、非法参数/格式/NaN/短块和真实组件控件调参。默认演示是合成音/噪声，不能替代F04授权语音、质量指标或听评。桌面仍暂停。AR-19.a的已实现精确测试与剩余契约同列，剩余项使其保持not_verified；整个P2/R5/R6尚未完成。
