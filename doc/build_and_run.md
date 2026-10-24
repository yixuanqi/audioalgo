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

`-BuildOnly` 只配置和编译，不打开窗口。构建输出位于 `build/windows-debug/bin/AudioAlgo Editor.exe` 或 `build/windows-release/bin/AudioAlgo Editor.exe`。Debug 与 Release 的节点/依赖开关以各自 preset 为准。

正常启动时，脚本把运行必需文件复制到 `%LOCALAPPDATA%/Programs/AudioAlgo/<Debug或Release>/<内容hash>/`，校验字节与最终 EXE 的 Windows 完整性标签，再启动并检查新进程。复制使用新文件继承目的目录权限，不复制开发工作区的 Low 标签，也不修改 ACL。构建输出可能仍带 Low 标签，直接打开该 EXE 不能替代正常运行副本。

可指定自己的正常运行目录（应位于构建目录之外）：

```powershell
.\build_and_run.cmd -Configuration Release -RunDirectory 'D:\My Apps\AudioAlgo'
```

启动器自身或所选目录仍为 Low Integrity 时会停止并解释原因。请从正常用户的资源管理器或终端运行，无需管理员。已有副本被修改或不完整时不覆盖，需选择另一个运行目录。旧版本按内容分目录保留，脚本不会自动清理用户文件。

已经打开的旧实例不会自动升级；先复制或保存未保存源码、正常关闭，再使用一键入口。脚本不会强杀旧窗口。自定义节点文件和内部目录的选择见 [使用说明](custom_node_files.md)。

现有 `scripts/run_debug.ps1`、`scripts/run_release.ps1` 仍可使用；它们与根目录入口共用 `scripts/build_and_run_editor.ps1`。根目录入口仅为本次 PowerShell 进程允许执行该脚本，不更改系统执行策略。自动化调用如需失败时不等待按键，可设置环境变量 `AUDIOALGO_NO_PAUSE=1`；成功返回 0，失败返回非零。
