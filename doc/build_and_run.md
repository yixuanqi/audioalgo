# 一键编译并运行

在仓库根目录双击 `build_and_run.cmd`，自动配置 CMake、增量编译并启动 **Debug** 编辑器。运行入口不受当前目录影响，仓库路径包含空格也可使用。

需要已有 CMake（PATH 中可用）、仓库 preset 指定的 Visual Studio C++ 工具链和 Python；依赖与配置仍由 `CMakePresets.json` 管理。脚本不会安装工具或修改系统设置。首次配置可能需要获取锁定的第三方依赖。

如果编辑器已经打开，先保存工程并关闭，再双击脚本。脚本在编译前检测已有进程，避免链接时遇到文件占用。编译成功后启动新程序；失败时保留命令窗口显示错误，不启动旧程序。

在终端中可选择配置或并行数量：

```powershell
.\build_and_run.cmd
.\build_and_run.cmd -Configuration Release
.\build_and_run.cmd -Parallel 8
.\build_and_run.cmd -BuildOnly
```

`-BuildOnly` 只配置和编译，不打开窗口。输出位于 `build/windows-debug/bin/AudioAlgo Editor.exe` 或 `build/windows-release/bin/AudioAlgo Editor.exe`。Debug 与 Release 的节点/依赖开关以各自 preset 为准。

现有 `scripts/run_debug.ps1`、`scripts/run_release.ps1` 仍可使用；它们与根目录入口共用 `scripts/build_and_run_editor.ps1`。根目录入口仅为本次 PowerShell 进程允许执行该脚本，不更改系统执行策略。自动化调用如需失败时不等待按键，可设置环境变量 `AUDIOALGO_NO_PAUSE=1`；成功返回 0，失败返回非零。
