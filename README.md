# PixelSandbox

**Noita 材料模拟＋泰拉瑞亚探索建造。** Demo 目标：草原建家 → 洞穴采矿 → 制作与改杖 → 建造战场 → 史莱姆王。

当前已实现阶段 A/B 与材料实验场：保留石墙的破坏、拾取、修补、保存闭环；新增粉末、液体、气体、有限寿命火焰，以及加速度、稳定液面、冲击、燃烧供氧、冷热转化、黏附、黏液与固体松动/再固定，以及配置接触/区域反应、遮挡爆炸与可保存的连锁引爆，并接入有限电能脉冲和导电，以及可运动、碰撞、局部擦除和保存的像素刚体。刚体与网格共用接触反应、供氧、燃烧和冷热转化，转成流体或气体后回到网格；爆炸可破坏、加热和推动刚体，原版 TNT 支持可保存的引信与聚合引爆；导电刚体可与网格连通，脉冲随实际材料移动、分裂或熔化。刚体运动沿通路逐格排开流动液体，按通路顺序转移各层材料及其状态，并参与双向动态压力与阻力、基于密度的静水浮力近似。采用 C++20、单线程、60 Hz；引擎和依赖版本未变。人物、完整建造条件、背包、制作、战斗与世界生成尚未实现。

## 材料实验场操作

实验场画布占满窗口，世界等比居中显示；窗口比例与世界不同时保留留边，不拉伸材料像素。右上角“工具 / 数据”标签切换浮动面板，按 **Tab** 或点击“收起 / 展开”隐藏或显示当前面板；点击标签也能重新展开。工具页包含材料、画笔、单步、保存读取和暂停等操作，数据页显示模拟统计与材料数量。面板覆盖区域不接受画笔输入，收起后可编辑其下方世界。

1. 主界面选择“进入材料实验场”。当前默认是 **液体实验场**：世界 256×128、原点 `(-64,0)`，只预置石质容器、水和油。左槽观察落水，中槽初始水在油上方，观察密度分层，右槽留空用于手动画液体。
2. 默认画笔为水。左键拖动画材料，右键擦除；半径 1～8。工具页可切换材料、暂停、单步、保存读取或重置；重置恢复液体实验场。完整材料定义仍保留，旧混合实验场只用于原生与显式加载的回归夹具。
3. 普通原版液体启用局部交换、下落/横向累计与概率密度交换，不再运行连续压力投影或整行出口导流。水、油、血液、酒精、酸、熔岩和熔融金属等共 21 项使用新路径；砂、气体、黏液、胶水及静态材料仍沿用既有路径。
4. 暂停后画笔操作只排队，点击“单步”才执行。左槽可先观察下落，再在槽壁下部擦除缺口观察局部排水；中槽用于看重水与轻油交换，不要求每帧立即按密度排好。
5. 加热、冷却、冷热转换、腐蚀、导电等关联接口保留，可自行绘制相应材料实验。本轮没有预置燃烧、爆炸、电路和盐水反应样本；旧坐标教程不再对应默认场景。
6. 保存的是已完成逻辑步。格式 15 保存液体下落累计、横向累计与运动方向；旧档补零，原有材料与状态保留，但恢复后的普通液体按新规则运动，不承诺延续旧压力模型的轨迹。
7. 当前刚体仍能排开液体并使用已有浮力近似。普通液体不再提供旧连续模型的双向动态压力/隐式阻力；原版完整受力、更新调度、随机流和高速粒子路径尚未恢复。本实现是原版风格的普通液体分支，不是完整 Noita 物理复刻。

材料 ID/密度/分类/原版标签及纹理参考本机 Noita，继承属性和项目参数分别保存在 `game/assets/prototype_noita/source-materials.json`。已确认的燃烧/反应分支按本地证据接入；更新调度、热传播和火焰时长仍包含项目规则，未声称恢复完整原版算法。重新导入会重写这 60 项预置定义，调整后应保留自己的修改；清单中另行追加的自建材料路径会保留：

```powershell
python tools/import_noita_materials.py --source 'E:/SteamLibrary/steamapps/common/Noita'
```

## 石墙操作

1. 主界面选择“进入石墙测试”。测试世界为 192×96 材料单元，原点 `(-64,0)`；石墙跨越 `x=0`、`x=64`、`y=64` 的 64×64 区块边界，初始共 7168 单位石材。
2. 在石墙上**单击左键**，挖掉半径 4 单元的局部圆形；允许留下半块和孔洞。金色小方块代表按区块聚合的掉落包。
3. 点击“拾取全部”，把测试世界中的掉落转入最小石材容器；本轮不检查距离，也没有完整背包界面。
4. **单击右键**，补齐选中 8×8 格内的空气。填几格扣几单位，完整格为 64 单位；资源不足或格子越界时整次失败，不部分修改。移动鼠标可看到格子边框。
5. 点击“保存”，等待结果行显示 `Saved tick ... / ... ms`。返回主界面，再点击“读取存档”；世界以暂停状态打开，形状、掉落、持有量、种子和 tick 按快照恢复，点击“继续世界”后可继续操作。

界面同时显示操作结果、逻辑 tick、世界/掉落/持有石材与守恒总量。暂停时不接收世界修改命令，允许保存和读取；返回主界面不自动保存。读取失败显示具体原因，保留当前世界。

正常存档位于 `%APPDATA%/Godot/app_userdata/PixelSandbox/saves/stone_wall.psw`；烟雾测试按进程单独使用 `smoke/stone_wall-<pid>.psw`，不会覆盖正常存档或其他测试进程。当前写入格式 15，保存世界边界、网格比例、稳定内容 ID 映射、实际材料形状及逐单元温度、寿命、燃料、燃烧、创建 tick、二维速度、运动余量、Q24 液体流量余量、Q16 低速动量补数、燃烧强度字节、烧损计数、焦化颜色、待点燃转换标记、累计 HP 损伤、有符号气体计数、最近电访问 tick、松动标记、再固定计数、旧作者黏附计时、脱落冷却、源附着方向/释放交接、剩余引信与带电显示步数，以及电能脉冲的位置、前一位置、能量、方向、分叉门槛／次数、点燃参数和模式，新增普通液体浮点运动累计与方向，并保存环境冷热门槛；两处地址分别保存所属刚体 ID（0 表示网格，非零表示局部坐标），燃料使用 uint32，保留 90000/1000000 等源值；另保存刚体 ID、局部材料形状/状态（包括正在燃烧的温度、寿命和燃料）、位置、角度、速度、睡眠和下个 ID，Box2D 接触缓存重建；检查校验和和石材账目。格式 1～14 可迁移，保留已有字段，缺少的新字段补零；旧映射保留索引并追加新材料；旧档没有的燃烧状态补零，后续使用当前规则，不承诺旧规则的燃烧时长。未知版本或缺失定义拒绝读取并保留当前世界。同步写临时文件，成功后替换；尚未实现压缩、多存档槽或断电恢复。实验场与石墙共用这个存档槽，后一次成功保存会替换前一次。

石材外观暂用本机 Noita 素材：`E:/SteamLibrary/steamapps/common/Noita/data/data.wak` 中的 `data/materials_gfx/rock.png`，复制到 `game/assets/prototype_noita/`，原安装目录保持只读。来源及 SHA-256 记录在同目录 `source.json`；这是临时原型素材，单独列入 `THIRD_PARTY_NOTICES.md`。复现导入可执行 `python tools/import_noita_stone.py --source 'E:/SteamLibrary/steamapps/common/Noita'`。外观由 GodotHost 采样并提亮，材料形状与资源数量仍由原生世界决定。

## 文档入口

| 要了解什么 | 阅读位置 |
|---|---|
| Demo 玩法、建造破坏、采集制作 | [Demo 内容方案](docs/Demo内容方案.md) |
| 当前代码边界、长期架构、存档与性能安排 | [技术框架与性能](docs/技术框架与性能.md) |
| 阶段 A/B 与材料实验场实际验证、证据位置与已知问题 | [工程验证](docs/工程验证.md) |
| Noita 原版世界及五个系统专题 | [世界基础系统](docs/Noita原版/世界基础系统.md) |

项目与资料统一在 `D:/Godot/PixelSandBox`。后续文档维护在 `docs/`，原版分析在 `docs/Noita原版/`，历史稿在 `docs/历史版本/`。目录职责说明留在相应模块，许可证留在项目根目录与依赖目录。

## 运行与开发

双击根目录的 [打开编辑器.cmd](打开编辑器.cmd)，直接使用已有的优化开发扩展打开锁定版本的 Godot 编辑器，不重复构建或预先启动无界面编辑器导入；资源由可见编辑器增量扫描。缺少开发 DLL 时自动构建。按 **F5** 运行，在主界面选择“进入材料实验场”查看效果。启动器会从 PATH、PowerShell 7 常见安装位置及本机 Codex 运行时查找 `pwsh.exe`，无需手动设置它的 PATH；找不到时提示安装。启动失败时窗口保留错误信息。

修改 C++ 后须先保存工作并关闭本项目编辑器及游戏，再运行 `.\打开编辑器.cmd -Rebuild`，或先执行 `build` 再双击启动器；快速启动不会自动检查源码是否比 DLL 更新。日常调整场景、纹理和材料资源无需重编 C++，可保持编辑器打开并反复按 F5 运行。

在 PowerShell 中执行：

```powershell
cd 'D:/Godot/PixelSandBox'
pwsh -NoProfile -File tools/project.ps1 bootstrap   # 获取并校验锁定依赖；首次需要网络
pwsh -NoProfile -File tools/project.ps1 build       # 优化开发版原生库、Debug 功能扩展、测试
pwsh -NoProfile -File tools/project.ps1 test        # 构建、编辑器导入、CTest
pwsh -NoProfile -File tools/project.ps1 core-test   # 核心独立编译/测试，不启动 Godot
pwsh -NoProfile -File tools/project.ps1 benchmark -Configuration Release # 固定材料负载；输出模拟耗时分位数
pwsh -NoProfile -File tools/project.ps1 run         # 构建并启动游戏
pwsh -NoProfile -File tools/project.ps1 editor      # 使用已有开发 DLL 打开编辑器；缺少时构建
pwsh -NoProfile -File tools/project.ps1 editor -Rebuild # 增量构建后打开编辑器
pwsh -NoProfile -File tools/project.ps1 export      # Release 导出至 dist/windows
pwsh -NoProfile -File tools/project.ps1 verify      # 运行导出程序的集成检查
pwsh -NoProfile -File tools/project.ps1 verify -MaterialSoak # GUI 闭环后持续模拟，检查连续 200 步完全稳定
pwsh -NoProfile -File tools/project.ps1 all         # 依赖、测试、导出与导出检查
```

`build/test/configure -Configuration Release` 选择 Release；`-Clean` 先清理目标再重编。默认 `windows-debug` 使用原生 `RelWithDebInfo`（MSVC `/O2` 与调试符号），仍链接 `template_debug` 并加载 `pixel_host.debug.dll`，适合编辑器中看效果。`core-only` 保留纯 Debug；需要未优化的原生逐行调试时，可在开发终端执行 `cmake --preset windows-debug -DCMAKE_BUILD_TYPE=Debug` 后构建，下一次普通脚本会恢复优化开发配置。`export` 另构建分发用 Release 扩展。停止游戏与编辑器后再编译，不启用原生热重载。

固定种子原生基准支持真实实验场 `lab`。2026-10-28 切换优化构建时的 56 材料/148 条反应、预热 10 步后测量 120 步，同一实验场的未优化 Debug 与优化开发版中位数为 48.861/7.778 ms，P95 为 56.544/8.691 ms；最终完整快照字节一致。默认编辑器入口已使用后者。附着修改前的 60 材料/186 条反应同窗口为中位数 7.980 ms、P95 9.313 ms，详见[本轮验证](docs/工程验证.md#新增冷热电弧爆炸与腐蚀2026-10-28)。密集液体压力仍有性能限制，原生耗时不包含 Godot 绘制，不能直接当作游戏帧率；范围见[工程验证](docs/工程验证.md#编辑器开发构建的性能2026-10-28)。

脚本自动初始化已安装的 MSVC 环境；CMake、CTest、Ninja、Git、Python 需在 PATH。在已初始化的 MSVC 开发终端也可直接使用 `cmake --preset windows-debug`、`cmake --build --preset windows-debug`、`ctest --preset windows-debug`。

可直接运行 `dist/windows/PixelSandbox.exe`；分发时保留整个 `dist/windows`，包括 `.pck`、扩展 DLL 和许可证。完整证据与首次导入问题见[工程验证](docs/工程验证.md)。

## 固定版本

| 项目 | 当前锁定值 |
|---|---|
| Godot / 导出模板 | 标准版 4.7.2-stable，Windows x64，单精度，Compatibility |
| godot-cpp | 10.0.0-stable，提交 `507ed9d840c01a3c5b2a39af8bb4000bfac30bf5` |
| 目标 API | Godot 4.7 |
| 刚体库 | Box2D v3.1.1，提交 `8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3`，独立静态库 |
| MSVC / Windows SDK | tools 14.44.35207，编译器 19.44.35227.0 / SDK 10.0.26100.0 |
| 构建工具 | CMake/CTest 4.0.1、Ninja 1.13.2、Python 3.13.2 |
| 项目代码 / CRT | C++20，Debug 与 Release 均使用静态 `/MT` |

以 `tools/versions.json`、`tools/artifact-hashes.json` 和构建配置为准。代码职责及接入点见[技术框架与性能](docs/技术框架与性能.md)。

## 目录与产物

| 目录 | 用途 |
|---|---|
| `native/` | 原生运行时、玩法模块与 Godot 接入。 |
| `game/` | Godot 工程、场景、内容资源与表现资产。 |
| `docs/` | 玩法、技术、工程验证与原版参考。 |
| `tests/`、`benchmarks/` | 测试代码、固定负载与后续性能记录。 |
| `tools/`、`third_party/` | 统一入口、版本锁与可恢复依赖。 |
| `build/`、`game/bin/`、`game/.godot/`、`dist/` | 构建、扩展、引擎缓存与导出产物，均被 Git 忽略。 |

`third_party/godot-cpp` 与 `third_party/box2d` 是被忽略的锁定工作副本，保留本地改动；`tools/_cache` 保存工具与下载缓存。依赖获取和校验由 `bootstrap` 负责，发布许可见 `THIRD_PARTY_NOTICES.md`。当前材料实验场及导出检查不代表大世界负载性能。刚体由独立 `pixel_physics` 接入 Box2D，`pixel_runtime` 仍仅依赖标准库，原有锁定版本未更改。完整原版材料复刻仍在推进，56 项参数和当前反应表不代表覆盖全部 Noita 材料、反应或私有物理算法。

## Git 版本管理

默认分支为 `main`。仓库保存源码、工程资源、文档、测试、构建入口和依赖锁；构建目录、导出程序、Godot 缓存、工具下载缓存及第三方源码工作副本由 `.gitignore` 排除。新检出后按上面的固定工具链要求执行 `tools/project.ps1 bootstrap` 恢复依赖，再构建。

原型素材及来源说明保留在工程中，GitHub 仓库使用私有可见性。日常修改后使用 `git status` 检查，提交到 `main`，再执行 `git push` 同步。
