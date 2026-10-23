# 原生算法延迟与单次对齐产物

2026-10-22。AR-15.d真实Red：37样本延迟分支与直通分支合流，输出首脉冲0.125而oracle为0.25；raw_frames513而应550，offset0而应37。

NodeExecutionCapabilities声明已知整数latency_frames（默认0，nullopt为未知）。纯计划在构造实例前拒绝未知/超范围延迟，通用wrapper向JUCE报告算法延迟加固定帧缓冲F-1。算法本身必须实现其声明的延迟，不由宿主伪造算法状态。JUCE负责合流对齐，区域输出一次裁剪总延迟。

RunService捕获非零延迟时实际处理的原始流，发布output.raw.wav与对齐output.wav；manifest记录raw/aligned帧数、对齐offset、raw有效起点及两个hash。无延迟时两项引用同一output.wav，避免额外大缓冲。原始文件的有效区间由报告描述，不能把前导延迟算成有效信号。

已验证真实JUCE分支脉冲、未知延迟零工厂，以及RunService实际两份WAV逐样本与长度/hash归属。该静态已知延迟契约不表示实现动态延迟、NeedMoreInput、算法有限尾音或未知尾部flush。
