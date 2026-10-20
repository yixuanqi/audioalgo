# 数据与评测线状态

文档更新：2026-10-18。

- 当前范围：独立规划，D0～D6尚未开始代码实施；未声明DE或平台测试通过。
- 入口：[开发方案](plan_data_evaluation.md)、[测试准则](testing_data_evaluation.md)、[Agent规程](agent_loop_data_evaluation.md)。
- 并行所有权：本线只修改audio_evaluation、本线工具/fixture/suite/文档及独立build/report目录；不修改主线AGENTS、构建、核心API、AR/NED清单或公共进度文档。
- 当前事实：重构主线正在R0推进；共享scripts/verify_run_audio.py仅为现有Gain独立检查，非本线通用评测实现。本线计划接口/命令均须启动时重新核实。
- 首条产品Red：DE-02.b，manifest声明513帧但float32载荷只有512帧，必须结构化拒绝且不得发布dataset_complete。
- 基线/环境：后续Agent先检测本线目录、工具/锁定依赖及源文件hash，建立独立构建和测试inventory；不复用主线构建/PDB/venv。
- 外部缺项：F04授权语音、F07真实同步阵列及参考分量尚未由本线获取/核验，正式数据资格与质量均not_verified。
- 下一步：从D0的最小校验器和失败传播开始，按DE0000起的本线循环保存Red/Green/回归/独立证据；D5受外部条件限制时继续D6独立工作。
- 本次仅文档检查；详细记录保存在build/reports/data_evaluation/planning/，不属于产品验收。
