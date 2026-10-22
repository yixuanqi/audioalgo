# 原生输入资源快照与重放

RunService在创建处理器/prepare前，把当前G1 WAV源与descriptor.input_resource=true的文件参数复制到新运行目录的resources/<序号>/<原文件名>。复制内容hash与捕获前版本一致，并在捕获结束前检查原始版本；随后渲染只读取保存副本。原文件在执行期改变或删除不再使已固定的请求改用新内容。捕获期间版本不一致、复制失败或保存副本被修改会返回定位诊断，不发布成功标记。原输入和编辑模型不由服务覆盖。

每次运行保留project.aaproj（原逻辑引用、实际默认参数）、replay.aaproj（引用当前运行目录内保存副本的相对路径）、WAV、run.json和最后的complete.json。resources条目的path表示原始位置，sha256表示固定版本；snapshot_path为保存副本相对路径，snapshot_sha256与其实际内容一致。原始位置后来不存在不代表保存副本丢失。run.json另记replay_project及其SHA。

在保持相同插件/构建环境时，可以用新输出目录重放，例如：

```powershell
build/windows-debug/bin/audio_cli.exe run build/runs/first/replay.aaproj build/runs/replayed 127
```

T21实际内置CLI演示位于build/reports/loops/RF0006/T21-ar16a-native-resource-snapshot/cli-replay-proof/first/replay.aaproj。测试仅删除其自有克隆输入/工程，随后真实CLI重放，仓库原始示例保持原hash。另有测试插件在prepare时替换/删除自有WAV与系数文件、修改调用方模型，四份原生/重放产物以独立解析公式核验，最大误差0。

本切片仍使用现有单WAV源/所选sink执行路径。多源/命名多端口计划、版本/环境完整冻结、排队/取消、临时输出发布与失败诊断保留、Python模块快照等按AR14～18继续开发。资源复制不是这些未实现能力的替代证明；没有恢复暂停中的桌面操作。
