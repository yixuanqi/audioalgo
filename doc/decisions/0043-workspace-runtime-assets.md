# ADR0043 — Python 工作区冻结运行资产

日期：2026-10-27。状态：accepted；以 NWS0006 的真实资源 Red/Green 验证，完整 NWS/W2 尚未通过。

ADR0040 的运行内容包括 src/include 普通文件及显式资源。原 Python checker 拒绝非 .py 运行文件，因此合法工作区不能完成 Check。只复制源码或只核作者 hash 不能保证执行的资源是 Ready 所冻结的版本。

在 ADR0042 包 manifest 增加可选 workspace_files 对象：键为封装后的相对路径，值为 {role:"source"|"resource",bytes:非负整数,sha256:小写64位hex}。node.json 已由 manifest 中的精确 workspace_node_json 保留，不另列；src/ 映射 workspace_src/，include/ 与资源保持相对路径。源码旁数据可按 __file__ 相邻定位，显式根资源可由 __file__ 的工程根定位。包不依赖作者目录。

宿主冻结、已冻结包验证、runner 再冻结和执行结果回收均使用该清单，逐字节/hash验证所有运行文件。缺文件、额外运行文件、路径越界、链接/reparse、声明 hash 或 bytes 不符拒绝；快照元数据 snapshot.json 只属于内部记录。没有 workspace_files 的历史目录包与普通类插件继续使用原 .py+manifest 清单，不补猜资源。

ADR0040 作者源码128文件/8MiB减16KiB、资源64文件/64MiB保持；运行包源码128文件/8MiB，资源另外按64文件/64MiB计数，不能将普通插件改成192源码文件。所有资源内容纳入既有不可变包hash、Ready与精确版本身份；协议1MiB和其他预算不变。新测试覆盖普通插件129源码拒绝/128恢复、目录128源码与64资源同时通过、资源64MiB及各上限增加1拒绝。

包保留路径可能与 manifest.json/snapshot.json 或映射后的 workspace_src 路径冲突时明确拒绝，不覆盖资源；该命名冲突的完整支持仍挂账。极长清单/大声明与极限源码组合的封装开销仍须另验，不能凭单一边界测试宣布完整预算通过。冻结夹具、oracle和期望不变。
