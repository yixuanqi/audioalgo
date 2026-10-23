# Python Stateful 混合链

`mixed.aaproj`：File → C++ Gain(+6.020599913279624 dB) → Python Stateful → C++ Gain(−6.020599913279624 dB) → Output。1025帧双通道解析输入，Python跨块累加位置；main = float32(float32(input × scale) + float32(position × step))，side = −main。默认scale=.75、step=.00001。Clip节点使用同一公式，整段只调用一次。

在仓库根执行（使用本机已验证Python3，替换解释器路径）：

```powershell
& <Python3> scripts/verify_python_mixed.py --cli build/windows-debug/bin/audio_cli.exe --output build/runs/<全新目录>
build/windows-debug/bin/audio_cli.exe --plugin-dir examples/python_stateful_demo/plugin run examples/python_stateful_demo/mixed.aaproj build/runs/<另一全新目录> 127
```

首条命令只在input.wav不存在时生成固定输入；已有内容不同时拒绝覆盖。独立stdlib RIFF/float32 oracle读取1/127/512三种host的真实WAV、run/complete、任务依赖/物化端口、代码/NPY hash及prepare/call计数。发布仅由整次RunService完成，内部Python结果不是工程成功标记。

GUI启动前设置`AUDIOALGO_PLUGIN_PATH`为该plugin目录，重启后打开同一工程。真实参数组件通过controller修改scale，再由后台worker运行同链；桌面操作/设备验收仍暂停。Python导入/启动/执行只在worker，消息线程直接执行离线图会返回offline_thread_invalid。

运行在任何任务开始前复制全部相关插件和宿主runner源码，固定解释器选择与hash、实际默认参数和资源；同包两个实例使用同一快照。原脚本随后变化不会改变本次运行，下一请求新PID使用新代码。固定副本被改写明确拒绝。`run.json`的`frozen_python_packages`及`task_artifacts`相对运行目录记录，含实际环境、PID及逐文件hash。

重放可将整个成功目录移动到新位置，然后仅以记录的固定插件目录启动同一CLI：

```powershell
build/windows-debug/bin/audio_cli.exe --plugin-dir <迁移运行目录>/<frozen_python_packages中的相对目录> run <迁移运行目录>/replay.aaproj build/runs/<全新重放目录> 127
```

多个插件根重复`--plugin-dir`。无需发现原插件目录；工程资源来自相对副本。环境/本地原生扩展兼容及最终离线安装仍须独立验收。

当前混合PCM边界要求16/48kHz、1/2/4通道、长度/通道ID/有效区间不变，原生区域为无额外延迟/尾部的可变块处理器；未支持组合明确拒绝。原生专用路径继续提供已验证固定帧/时序能力。错误/取消完整组合按P3-05继续验收，不能用本示例成功代替完整P3/G3。
