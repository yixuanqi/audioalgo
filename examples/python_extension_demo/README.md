# 独立 Python 扩展示例

需要启用 Python 的宿主、Python 3.11+ 与锁定的 NumPy 环境。开发环境见 `dependencies/` 与运行报告；示例使用现有 `python_stateful_demo/input.wav`。插件完整目录是 `examples/plugins/python_user_transform/`，系数资源属于工程目录。

从仓库根运行（Windows PowerShell）：

```powershell
$env:AUDIOALGO_PYTHON_EXECUTABLE='E:/0qyx/res/miniconda/python.exe'
& 'build/windows-debug/bin/audio_cli.exe' --plugin-dir examples/plugins/python_user_transform --list-nodes
& 'build/windows-debug/bin/audio_cli.exe' --plugin-dir examples/plugins/python_user_transform run examples/python_extension_demo/extension.aaproj build/runs/my-extension-001 127
$env:AUDIOALGO_PLUGIN_PATH=(Resolve-Path examples/plugins/python_user_transform).Path
& 'build/windows-debug/bin/AudioAlgo Editor.exe'
```

每次运行使用新的输出目录。编辑器启动后使用 Open 打开 examples/python_extension_demo/extension.aaproj；当前编辑器没有 --project 或 --plugin-dir 命令行入口。编辑器命令仅为使用说明；当前真实桌面验收暂停。参数控件的组件验收不能替代桌面操作、试听或设备验收。

部署时将 manifest.json 和 user_transform.py 一起复制到新插件目录，配置 `--plugin-dir` 后重启同一个宿主；不编译或链接宿主。工程内资源使用相对工程文件的路径：`coefficient.txt` 默认内容为 0.5。部署在仓库外时同时复制输入音频并修改 File 的 path；不要依赖示例的仓库相对路径。

六类参数：enabled(bool) 选择处理/旁路；repeat(int) 为 1～4；scale(float) 为 0～2；polarity(enum) 为 negative/positive；tag(string) 只记录标识；coefficient(path) 读取有限浮点系数。运行准备阶段复制系数到资源快照，插件只读取 context 中绑定的副本。冻结后原件替换/删除不会改变本次结果。

独立公式：`g=(enabled ? repeat*scale*polarity*coefficient : 1)*VERSION_GAIN`，polarity 为 ±1；`main=float32(x*float32(g))`，`side=-main`。Mixer 以 1 与 0.5 加权两个输出，结果为 main+0.5*side；float32 物化边界显式舍入。默认 g=-0.25，最终输出为 -0.125*x。main/side 每块同时返回，不为观察分支额外执行实例。extension.json 记录调用次数、tag、实际系数路径与增益。

更新：关闭宿主，修改插件代码（示例可将 VERSION_GAIN 从 1.0 改为 2.0），重启原 EXE，再运行到新目录。新进程/代码 hash/输出变化；旧成功目录仍保存自己的代码、资源、参数、身份与输出 hash。不要覆盖旧结果，也不在运行时重载模块。坏系数返回 coefficient_invalid，定位 python/coefficient；CLI 非零、无成功目录，修正后可重新运行。

验证入口：`scripts/verify_python_extension.py --command-record <P3-06组件命令记录> --output <新报告路径> --cli <audio_cli.exe>`。它直接读取真实 RIFF/NPY、快照及清单，用独立公式核对，不导入被测插件；同时实际验证 CLI 更新与错误恢复。原始命令/JUnit 与本机证据见 `build/reports/loops/P3-host-integration/P3-06/`。

后续宿主版本验证时，用 `--host-build-record <对应构建记录>` 明确绑定该宿主；原报告仍归属原EXE/hash。Windows 的 Context.run_directory 和模块 __file__ 可能已是扩展文件系统路径，可直接交给 pathlib/NumPy 使用，不要重复拼接路径前缀。runner 已验证深目录下超过260字符的载荷、代码快照和异常后恢复。

限制：首版离线 PCM 任务，Python 不进入 UI/设备回调；此示例不声明实时、任意依赖自动安装、安全沙箱或进程内原生崩溃隔离。固定帧/延迟/尾部混合支持按后续时序切片逐项验收；未知能力仍拒绝。正式干净禁网交付、完整 Release、桌面/设备及授权质量数据尚未通过。
