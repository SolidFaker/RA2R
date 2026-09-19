# RA2/YR 遭遇战流程规格（出生点 / 基地车展开 / 科技树 / 阵营色）

> 调研文档（清洁室复刻用）。**数值全部取自游戏自身数据文件**（rulesmd.ini、artmd.ini、
> 地图 INI）；**机制**参照 OpenRA 引擎代码与 mod 规则表（`mods/yr/rules/world.yaml` 的
> `MPStartUnits` / `SpawnMPUnits`），但不采用其 yaml 里的数值——数值以 INI 为准。
> 实现：`engine/sim/skirmish.*`（流程）、`engine/assets/rules_db.*`（键位）、
> `tools/stage`（UI 与粘合）。验收脚本见 §7。

---

## 0. 信息来源

| 来源 | 用途 |
|---|---|
| OpenRA `OpenRA.Mods.YR/UtilityCommands/ImportRA2MapCommand.cs` `ReadWaypoints()` | 地图 waypoint → 格坐标公式 |
| OpenRA `mods/yr/rules/world.yaml` `MPStartUnits@*` | 开局兵力**机制**（BaseActor + SupportActors + 半径） |
| 本机 `rulesmd.ini` / `artmd.ini` | 全部数值（Owner/Prerequisite/TechLevel/DeploysInto/Buildup/Colors…） |
| 本机 `battle1.yrm` | 实测样本（50×100 点阵、2 出生点、全平地形） |
| [ModEnc Colors](https://modenc2.markjfox.net/index.php?title=Colors) / [Remap](https://modenc2.markjfox.net/index.php?title=Remap) | 阵营色 16 色 remap 段语义 |

---

## 1. 出生点（`[Waypoints]`）

地图 INI 的 `[Waypoints]` 节：`0=41066`、`1=62042`，值 = `ry·1000 + rx`（地图**原始**坐标，
与 IsoMapPack5 记录同一坐标系）。引擎格换算（OpenRA `ReadWaypoints` 同式）：

```
rx = pos % 1000,  ry = pos / 1000
col = (rx − ry + W − 1) / 2      // 整数除法（C 语义，向零截断）
row = rx + ry − W − 1            // W = [Map] Size 的宽（不是点阵宽）
```

与 `MapFile` 的归一化公式等价：`col=(rx−ry−min_d)/2`、`row=rx+ry−min_s`，
其中 **`min_d = −(W−1)`、`min_s = W+1`**（battle1：W=50 → min_d=−49、min_s=51，实测吻合）。

实测（battle1.yrm，`Size=0,0,50,50`）：

| waypoint | 值 | rx | ry | 引擎格 |
|---|---|---|---|---|
| 0 | 41066 | 66 | 41 | (37,56) |
| 1 | 62042 | 42 | 62 | (14,53) |

> 注意：waypoint 的 `rx−ry` **奇偶性不保证**与 IsoMapPack5 点阵一致（wp1 为奇数 29），
> 原版与 OpenRA 都直接整除截断——不必"取偶"。

---

## 2. 基地车与展开（`DeploysInto` / `ConstructionYard` / `Buildup`）

| 类型 | Owner | `DeploysInto` | 美术 | 说明 |
|---|---|---|---|---|
| `AMCV` | British,French,Germans,Americans,Alliance | `GACNST` | `Image=MCV` → **MCV.VXL**（AMCV.VXL 不存在） | `DeploySound=PlaceBuilding` |
| `SMCV` | Russians,Confederation,Africans,Arabs | `NACNST` | `SMCV.VXL` | 同上 |
| `PCV` | YuriCountry | `YACNST` | `PCV.VXL` | 同上 |

建造厂（`GACNST`/`NACNST`/`YACNST`）：

- `ConstructionYard=yes`、`Factory=BuildingType`、`TechLevel=-1`（**不可建造**，只能由基地车展开）；
- `UndeploysInto=AMCV`（原版支持再打包，本项目留待 M6）；
- artmd：`Foundation=4x4`、`Buildup=GACNSTMK`（`FreeBuildup=true`）。

**展开落点**：地基以基地车所在格为中心 → 4×4 时左上 = 车格 `(−1,−1)`，车格落在 4 个中心格之一
（中心被占时按固定邻序试近旁落点，全部失败则不展开）。

**展开/建造动画 = artmd `Buildup=`**（实测帧数）：

| 建筑 | Buildup | 总帧 | 建造段 | 阴影段 |
|---|---|---|---|---|
| GACNST | GACNSTMK | 58 | 29 | 29 |
| NACNST | NACNSTMK | 62 | 31 | 31 |
| YACNST | YACNSTMK | 54 | 27 | 27 |
| GAPOWR | GAPOWRMK | 50 | 25 | 25 |
| GAREFN | GAREFNMK | 50 | 25 | 25 |

Buildup SHP 与**建筑本体同构**：前半为建造帧、后半为同数阴影帧（判据同 `assets::shp_shadow_start`）。
播放帧 = `floor(进度 × 建造段帧数)`；展开时长 = 建造段帧数 / 15fps ≈ 1.9s（原版观感）。

---

## 3. 开局兵力（OpenRA `MPStartUnits` 机制）

`BaseActor`（基地车）落在出生点，`SupportActors` 在以基地车为心的
**InnerSupportRadius=3 .. OuterSupportRadius=5** 环带内散布（确定性随机，不与已放格重叠）。
下表与 `mods/yr/rules/world.yaml` 逐项一致（单位名 = rulesmd 类型名）：

| 档位 | 盟军（Side=GDI） | 苏军（Side=Nod） | 尤里（Side=ThirdSide） |
|---|---|---|---|
| `none` | AMCV | SMCV | PCV |
| `light` | +DOG,E1,E1 | +DOG,E2,E2,E2 | +BRUTE,INIT,INIT,INIT |
| `medium` | +DOG,E1,E1,E1,MTNK,ENGINEER | +DOG,E2,E2,E2,E2,HTNK,ENGINEER | +BRUTE,INIT×4,HTNK,ENGINEER |
| `heavy` | +DOG,E1,E1,E1,E1,MTNK,MTNK,FV,ENGINEER | +DOG,E2×5,HTNK,HTNK,HTK,ENGINEER | +BRUTE,INIT×5,HTNK,HTNK,HTK,ENGINEER |

阵营 → 档位行的选择依据是**国家节的 `Side=`**（`[Sides]`：GDI/Nod/ThirdSide），不是名字前缀。
可选国家 = `[Countries]` 中 `Multiplay=yes` 的 10 个（盟 5 / 苏 4 / 尤 1）。

---

## 4. 科技树（`Owner` / `Prerequisite` / `TechLevel` / `ConstructionYard`）

建筑可造判定（顺序即失败提示顺序）：

1. 该类型在 rulesmd 有节（`[BuildingTypes]` 里只有 artmd 的装饰/民用件排除）；
2. `Owner=` 含玩家国家名（**空 Owner= 视为不可建造**，如中立建筑）；
3. `TechLevel= −1` 永不建造；`> 科技等级` 不可造；
4. 拥有任一 `ConstructionYard=yes` 的**已建成**建筑；
5. `Prerequisite=` 逗号列表**全部**满足；其中"组名"查 `[General] Prerequisite<组名>=`：

```
PrerequisitePower      = GAPOWR,NAPOWR,NANRCT,YAPOWR
PrerequisiteProc       = GAREFN,NAREFN,YAREFN      （+ PrerequisiteProcAlternate=SMIN）
PrerequisiteRadar      = GAAIRC,NARADR,AMRADR,NAPSIS
PrerequisiteFactory    = GAWEAP,NAWEAP,YAWEAP
PrerequisiteBarracks   = NAHAND,GAPILE,YABRCK
PrerequisiteTech       = GATECH,NATECH,YATECH
```

实测（battle1.yrm，盟军 $10000，科技等级 10）：

| 步骤 | 结果 |
|---|---|
| 展开 GACNST 后 | 可造 **仅 GAPOWR**（GAREFN/GAPILE 需 `POWER`、GAWEAP 需 `PROC`…） |
| +GAPOWR | GAREFN、GAPILE 解锁 |
| +GAREFN +GAPILE | GAWEAP（`PROC,GAPILE,GACNST`）解锁 |
| +GAWEAP | GADEPT 解锁；GATECH 仍缺 `RADAR`（需 GAAIRC/AMRADR…） |

苏军同链条（AI 脚本实测）：NACNST → NAPOWR → NAREFN → NAHAND → NAWEAP。
**不要按名字前缀硬套**：苏军兵营是 `NAHAND`（不是 NAPILE）、尤里是 `YABRCK`——
按角色在 `[BuildingTypes]` 里找并通过同一套校验，才能自然挑对阵营件。

---

## 5. 阵营色（`[Colors]` + Remap）

`[Colors]`：`Gold=43,239,255` 即 `H,S,V`（各 0..255，Westwood 域）。
ModEnc 语义：**H 恒定；V 是该色最大亮度；越暗越饱和**（"Saturation curves through color
space as the value component changes so that darker colors become more saturated"）。

Remap 段 = 单位调色盘 **索引 16..31**（`UNITTEM/UNITSNO/UNITURB/UNITDES/UNITLUN.PAL`，
原版为红渐变占位；OpenRA `PlayerColorPalette.RemapIndex: 16..31` 同段）。
VXL **内嵌调色板同样是这 16 个索引**，故体素与 SHP 共用同一条 ramp。

ramp 生成（16 档，`i=0` 最暗 → `i=15` 最亮）：

```
t     = (i+1)/16
V_i   = V · t
S_i   = S + (255 − S) · (1 − t)      // 越暗越饱和
rgb_i = HSV→RGB(H, S_i, V_i)          // 8 位；不再按 .PAL 的 6 位 ×4
```

实测（`house_color_ramp`）：

| 颜色 | H,S,V | 首档（idx16） | 末档（idx31） |
|---|---|---|---|
| Gold | 43,239,255 | `#101000` | `#fdff10` |
| DarkRed | 0,230,255 | `#100000` | `#ff1919` |
| DarkBlue | 153,214,212 | `#00060d` | `#226cd4` |

国家默认色 = 国家节 `Color=`（盟军全 Gold、苏军/尤里 DarkRed）；遭遇战里玩家可另选。

---

## 6. 建造队列（遭遇战运营）

原版流程：侧边栏点选 → 排队并扣款 → 进度条 → 就绪闪烁 → 玩家在地图上落点 →
建筑播放 `Buildup` 动画后可用。

| 项 | 原版 | 本项目（M4 阶段） |
|---|---|---|
| 工期 | `[General] BuildSpeed=.7` × Cost（帧） | `cost/2` 逻辑帧（近似，待 M4 校准） |
| 扣款 | 排队时扣除 | 排队时扣除（`queue_build`） |
| 取消退款 | `[General] RefundPercent=50%` | 50%（`cancel_build`） |
| 队列 | 每工厂/类别独立队列，可并行 | 每 House 单队列（多工厂并行待 M4） |
| 落点 | 地基合法性（含地形/坡度/邻近） | 引擎格矩形 + 阻挡表（`can_place`） |

---

## 7. 无头验收

```powershell
# 遭遇战全流程（玩家展开 → 电厂 → 矿场 → 兵营 → 重工；AI 同步运营）
build\tools\stage.exe --gamedir <游戏目录> --map battle1.yrm --skirmish `
  --country Americans --color DarkBlue --ocountry Russians --ocolor DarkRed `
  --skclass 3 --test --shot build\sk.bmp --simsteps 4800
```

实测输出（2026-09，battle1.yrm）：

```
[stage] 遭遇战开始：玩家 Americans/DarkBlue class=2 7 单位 @(37,56)；对手 Russians/DarkRed 8 单位 @(14,53)
[stage] AMCV(Player) 展开 → GACNST @(37,56) 动画 29 帧 ok
[stage] AI 展开 SMCV → NACNST @(14,53)
[stage] 放置 GAPOWR @(34,53) / GAREFN @(36,52) / GAPILE @(33,55) / GAWEAP @(40,51)
sim: 遭遇战 Player 建筑 4/5 完成 单位 9 资金 $4700 电力 +140
sim: 遭遇战 Opponent 建筑 5/5 完成 单位 7 资金 $10000 电力 +65
```
