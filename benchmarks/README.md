`material_benchmark.cpp` 为固定种子的原生材料性能基准；`generate_material_catalog.py` 从项目实际标量材料资源生成仅供基准编译的定义，运行时使用项目反应解析器与完整反应表。通过 `tools/project.ps1 benchmark -Configuration Release` 构建运行，默认编辑器 Debug 功能预置已采用带符号的优化原生构建；纯 Debug 可用 CMake 构建类型覆盖作诊断对比。

也可执行 `build/windows-release/pixel_material_benchmark.exe game build/benchmark-Release 120 water`：参数依次为内容目录、可选快照目录、测量步数（至少 30，默认 120）、可选单一场景（`water`、`steam`、`mixed`、`thermal`、`wide_pool`、`lab`，省略则全部）。`lab` 直接使用进入材料实验场时的真实初始布置和反应，不清空材料或替换规则。各场景预热 10 步；不同测量窗口不能直接比较分位数。CSV 除完整步耗时、访问/移动数外，还报告平均压力迭代数、子步数、寻路访问数，以及液体建图、投影、输运的平均毫秒数；不包含 Godot 渲染耗时。

负载、实际结果和未验证范围集中维护在 [工程验证](../docs/工程验证.md#液体压力与动量2026-10-28)，不将原生耗时等同于图形帧率。
