# Speex Denoise → Python → Output

离线示例使用固定合成音频，不代替授权语音质量验收。需要 Python ON、SpeexDSP ON、声明的 Python 3/NumPy 环境。

从仓库根运行，输出目录每次新建：

```powershell
$env:AUDIOALGO_PYTHON_EXECUTABLE='E:/0qyx/res/miniconda/python.exe'
& 'build/windows-debug/bin/audio_cli.exe' --plugin-dir examples/python_stateful_demo/plugin run examples/speex_python_demo/speex_python.aaproj build/runs/my-speex-python-001 127
```

编辑器使用 `AUDIOALGO_PLUGIN_PATH` 指定 `examples/python_stateful_demo/plugin` 的绝对路径，启动现有编辑器后从 Open 打开本工程。桌面仍暂停，未将组件验收当作真实桌面操作。

Speex 使用锁定1.2.1原库，480固定帧及480算法延迟，FIFO适配479帧，原生区域raw offset合计959。运行保存每个原生输出的raw/aligned WAV和hash，进入Python前仅裁剪一次已知区域延迟；最终Output的逻辑offset为0，32037帧保持完整。Python连续持有一个Stateful实例，main=float32(float32(denoised*0.75)+float32(index*0.00001))，side=-main。默认选main进入Output。

已验证host 1/127/256/480/512/1024，组件/CLI各自产物对锁定Speex原库直调+独立float32公式；证据在 `build/reports/loops/P3-mixed-timeline/known-delay/`。`scripts/verify_python_timeline.py --kind speex --command-record <该次命令记录> --output <新报告> ` 读取真实WAV/NPY与代码/资源hash，不导入被测Python插件。

未知延迟和无界/未知尾部仍前置拒绝；有限尾部和逐输出独立有效区间按后续切片验收，不由该示例自动取得通过。F04授权语音质量、听评、真实桌面/设备与最终Release/禁网交付仍未通过。
