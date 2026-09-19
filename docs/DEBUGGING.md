# 调试经验总结（M0–M4 实战沉淀）

> 记录开发期间踩过的坑与验证手段，供后续复用。按主题分类，每条含"现象 → 根因 → 手法/教训"。
> 格式层的事实结论同步落在 `docs/formats/*.md`；本文件记**怎么被骗、怎么查出来**。

## 1. 崩溃类（Heisenbug 与堆破坏）

### 1.1 崩溃只在直接运行出现、gdb 下消失
- **现象**：mixbrowser 调色板下拉框闪退；gdb 批处理跑完全程正常；`-D_GLIBCXX_DEBUG` 构建也正常。
- **根因**：堆越界写（`SDL_UpdateTexture` 向尺寸不符的纹理写数据等），gdb/调试容器改变堆布局后越界"隐身"。
- **手法**：
  1. **从构造上消灭嫌疑**：尺寸感知纹理上传（尺寸不符即重建+数据长度校验）、预览脏标记（减少高频驱动交互）。
  2. 排查工具可用性：WinLibs 工具链**不带 libsanitizer**（`cannot read spec file 'libsanitizer.spec'`）；Dr. Memory 在 Win11 上 `failed to create process`。
  3. 保留**未处理异常过滤器**（打印 `ExceptionCode/Address/模块偏移`）→ `addr2line` 直接定位行号。

### 1.2 帧内状态替换导致的悬垂引用（UAF）
- **现象**：打开一个 mix 后再打开另一个崩溃。
- **根因**：`draw_list_pane` 顶部持有 `Level& lv = cur(a)`，按钮处理器中 `a = std::move(fresh)` 整体替换 App → 旧 Level 销毁 → 后续表格仍用 `lv`。
- **手法**：
  1. 按钮区**不持有跨操作的引用**；表格区在操作后**重新获取**。
  2. 会用指针时（`Session* s`），在替换类操作后**立即重绑**（`s = &cur(a)`）。
  3. 架构上优先"替换最小单元"（会话对象）而非"整体替换应用"。
  4. 回归测试要**精确复现触发路径**（帧内确定性触发 `open_mix_at`），合成点击未必可靠（见 §3）。

## 2. 加密/编码类

### 2.1 CRC 变体
- **现象**：C++ 按名查找资产全部失败，Python 暴力匹配却命中。
- **根因**：Python `zlib.crc32(data, 0)` 是**标准 CRC-32（init/xorout 0xFFFFFFFF）**，初版 C++ 写成了裸 init-0 变体。
- **手法**：实现后先过**锚点验证**（`crc("LOCAL MIX DATABASE.DAT" 混淆后) == 0x366E051F`）。

### 2.2 LZO1X 的 canonical 语义（三连坑）
- 首字节**窥视不消耗**（仅 `>17` 时 `*ip++`）；
- `match_done` 尾部扩展字节来自**输入流**（不是匹配位置）；
- 匹配拷贝允许与**写位置重叠**（逐字节前进，别用区间切片）；
- 调试手法：用 Python 写一个**可工作的参考实现**（`tools/debug_mappack.py`），逐步对比 C++ 轨迹（ip/op/t）。

### 2.3 地图瓦片记录流跨块连续
- 每块解压长度（8192）不是 11 的倍数，**必须拼接全部块后再解析**；逐块解析导致对齐错位（3643 单元格 → 732）。

### 2.4 变体格式按「结构锚点」判别，不迷信跨代文档
- **CSF**：XCC 源码的 `'STR '/'STRW'` 明文 UTF-16 是 Dune2000 变体；RA2 用 `'RTS '/'WRTS'` + **正文逐字节取反**（`~c & 0xFF`）。判别法：先按魔数猜，再用「走 N 条完整条目后偏移是否精确对齐下一条目」验证假设；Python 多假设暴力对齐（走 200 条目）比逐字节盯十六进制快得多。
- **AUD 头 12 字节 vs 14 字节**：TD 文档说 14 字节头，实测 RA2 是 12 字节。判别锚点：`12 + size == 文件大小` 且偏移 12 处恰好是 `[fsize=0x0200][dsize=0x0800][magic=0xDEAF]` 的合法块头。用 Python 走完整块链到文件尾做终验，而不是只看前 8 字节。
- **WAV 不是 PCM**：RA2 语言包 459 个 WAV 全是 fmt=17（IMA ADPCM，bits=4，blockAlign=512，samples/block=1017）。fmt 块解析偏移错一位（pos+10/pos+22）会得到 0/20 的假阴性——先 dump 完整 fmt 字段再下结论。

## 3. GUI 自测类

### 3.1 合成鼠标点击的四个坑（按发现顺序）
1. `SDL_EVENT_MOUSE_BUTTON_DOWN` 的 **`down` 字段必须为 true**（零初始化会变成"抬起"）；
2. ImGui 点击检测要求**按下与抬起跨帧**（同帧 down+up 不算点击）；
3. **ImGui SDL3 后端每帧从 SDL 重新同步真实鼠标**（位置+按键），SDL 事件注入会被覆盖——需在后端 NewFrame 之后、`ImGui::NewFrame()` 之前直驱 `io.MousePos`/`AddMouseButtonEvent`；
4. **活跃桌面上真实光标仍在覆盖位置**（用户正常使用电脑时跑测试）——自测注入天然不可靠。
- **结论**：GUI 逻辑验证优先用**程序化路径**（直接调用 activate/select 等），合成点击只作补充；真实交互交给用户确认。

### 3.2 ImGui 标签栏的选中状态陷阱
- `BeginTabItem` 对选中标签**每帧返回 true**；`SetSelected` 的切换**滞后一帧**；`IsItemClicked` 对标签项**不可靠**（点击走独立焦点队列）。
- **解法**：需要程序化切换的场景**自绘标签行**（普通 SmallButton），状态完全由应用掌控。

### 3.3 SDL3 API 差异备忘
- `SDL_RenderReadPixels` 返回 `SDL_Surface*`（不再接受目标缓冲）；
- `SDL_ConvertSurfaceFormat` 已改名 **`SDL_ConvertSurface`**；
- `SDL_ShowOpenFileDialog` 过滤器 pattern 是**扩展名列表**（`"mix"`，无通配符；校验只允许 `[a-zA-Z0-9_.-;]` 与单独的 `"*"`）——非法 pattern **静默回调 NULL**，不弹窗不报错。

### 3.4 隐式根窗口收缩：预览面板被挤没
- **现象**：`--shot` 截图右半全空，预览面板只有 ~188px 宽或不可见；日志 `GetWindowPos()` 显示面板在 (514,152)，但像素只画到 x≈458。
- **根因**：帧循环里没有 `ImGui::Begin()`，所有 `BeginChild` 挂在 ImGui **隐式根窗口**上；隐式窗口按内容自动收缩到 ~430px，`BeginChild(0,0)` 的"填满剩余"拿到的是收缩后的宽度。
- **手法**：无头排查截图别只看"有没有彩色像素"——用 Python 做**逐列非背景像素分布**找 UI 实际渲染的 x 范围（发现范围 ≈ 列表宽度即锁定布局问题）。修复：帧开头 `SetNextWindowPos/Size(主视口)` + `Begin("main", NoDecoration|NoMove|NoResize|NoSavedSettings)` 全屏窗口包裹整个 UI。

### 3.5 斜投影体素渲染：画家算法不够，直接上逐像素 z-buffer
- **现象**（三次迭代）：①固定屏幕菱形近似 → 旋转时像独立方块；②写死面顺序（背→顶→前）→ 斜投影下"前后"随 yaw 变，某些角度远面盖近面（不该渲染的面被渲染）；③面中心深度排序后仍有相邻面发丝裂缝与个别面消失。
- **根因**：斜投影不是正投影，画家算法按体素/面中心深度排序在相邻面共享棱、等深平局时必然有错序角落；**任何**排序启发式都只能缓解。
- **解法**：**逐像素深度缓冲**（XCC Mixer `image_z` 同款；OpenRA 用 GPU 深度）。面是平面、深度 w2 是屏幕坐标的线性函数 → 每三角形解平面方程 `d = A·x + B·y + C`，光栅时逐像素 z-test。配合 0.35px 边容差（防发丝裂缝，z-test 保证正确覆盖）。之后无需任何画家排序，任意 yaw/pitch 遮挡都精确。

### 3.6 格式判别陷阱：flags 位域是垃圾，偏移字段才可信
- **现象**：悬崖瓦片按 flags&1 判别扩展区 → 读垃圾尺寸报"extra data truncated"；普通道路瓦片 flags 恰好也置位 → 误读扩展。
- **根因**：RA2 TMP 帧头的 flags 位域（has_extra_data 等）实测恒为 **0xCD 未初始化填充**——写瓦片的工具根本没填这个字段；同文件里 height/terrain/ramp 却是真实值。
- **手法**：格式文档声称有 flags 时先怀疑"谁写了它"——用偏移字段（`extra_ofs/extra_z_ofs`）+ 尺寸合理性（0 < w,h ≤ 512 且落在帧跨度内）做判别；帧偏移 0 = 空帧哨兵；帧跨度由相邻非零偏移界定。

### 3.7 等距包围盒：极值角不能只用对角两格
- **现象**：地图渲染"看起来正常"但右半边被裁掉（85×91 图只出 2580 宽，实为 5280）；方形图上 (W-1,0) 格完全画不进去。
- **根因**：`map_bounds` 用 (0,H-1) 与 (W-1,H-1) 两角求 x 范围——(W-1,H-1) 的 px = (W-H)·30，方形图恒 0；而真正的右极值是 **(W-1,0)** 的 +W·30。菱形的四个极值角由**四个不同格子**决定，不能只取对角。
- **手法**：变换类代码写完先做**方形图边界检验**（W==H 时对称性最容易被对角线公式掩盖）；发现"渲染结果看着像那么回事"时用 `(W+H)·30` 快速心算期望宽度对照。

### 3.8 地形瓦片整体左偏 30px：帧原点语义与公式对不上
- **现象**：菱形瓦片右边少一截、左边多出来（缩放适配视角下约"几个屏幕像素"）；官方地图渲染整体错位但内部自洽，计数类回归（格数/对象数）发现不了。
- **根因**：瓦片帧的像素区是**以菱形包围盒为原点**的（无扩展时 bounds_x=0，菱形中心在帧内 (30,15)），而放置公式 `top_x = px - 30 - (30 - bounds_x)` 把"菱形中心在帧内 (30-bounds_x, 15-bounds_y)"又减了一次 30，实际多移了 30px。正确公式：`top_x = px - 30 + bounds_x`（扩展区负 bounds 与菱形中心偏移恰好抵消）。
- **手法**：像素级验证：先渲染一个已知地图，逐列扫描内容包围盒与 `cell_to_pixel` 心算的四极值对照（左顶点 x=0、右顶点 x=bw-1、顶顶点 y=oy-15 等）；改前后两次渲染做逐像素位移直方图（主峰 = 30）确认单一平移量。

### 3.9 剧场瓦片集 SetName 搜索误中前排集合
- **现象**：算法生成图的"湖泊"实际是水崖瓦片（WCliff01），斜坡全错选成泥路坡（DRSLPE）；按名字包含匹配（"Water"/"Slope"）永远命中**排在前面的集合**。
- **根因**：地形 INI 中 `WaterCliffs`/`DirtRoads Slopes` 等集合排在目标集合（`Water`/`Slope Set Pieces`）之前，且名字包含目标关键词。
- **手法**：搜索先做**精确名匹配**（"Water"、"Slope Set"），回退路径加**排除词**（Cliff/Cave、Road）；并打印"搜索到的集合名 + 首个文件名"验证（`tileset.name_for(base)`），不要只信命中与否。

### 3.10 GUI 类别与渲染 kind 编号错位
- **现象**：stage 里"载具"类别左键放置完全无效，"步兵"类别画出载具。
- **根因**：GUI 类别顺序（建筑/步兵/载具）与渲染 kind（0=建筑 1=载具 2=步兵）编号不同，点击路径直接把类别号当 kind 用；无头测试里硬编码的 kind 是对的，所以自检全绿。
- **手法**：类别→kind 转换收口成一张表（`kCatKind`），**GUI 路径与无头路径共用同一语义**，避免两套编号各自正确。

### 3.11 高度单位猜 6px 实为 15px：悬崖全带错位 + 帧头元数据未读
- **现象**：官方图悬崖带大片黑楔/凸出；算法图高度过渡断裂；"每个瓦片错位几个像素"的观感。
- **根因**：两级问题。① 高度换算用了估的 6px/级（真实 **15px/级 = 半瓦高**）；② TMP 帧头
  offset 40..42 的 `height/terrain/ramp` 元数据一直没读——RA2 的高度衔接（悬崖面向上扩展、
  斜坡坡向码 1..8、坡长档 = 扩展 px÷15）全部由这仨字节 + cell.height 驱动，靠美术猜方向必然错。
- **手法（正向测量，不反向猜现象）**：写探针聚合"23 张地图 × 逐格"：
  ① 悬崖底格（扩展 60px ↔ 邻居 +4 级）⇒ 15px/级，24/24 样本一致；② 斜坡格按 ramp 码分桶
  统计 8 邻居高差符号 ⇒ 坡向表；③ 帧头 x/y 与 (u−v)·30 公式恒等 ⇒ subtile 定位无隐藏偏移。
  语义表落在 docs/formats/tmp.md §4。修复 = isometric.h `kHeightLevelPx=15` +
  stage 按 ramp 元数据选帧（`assign_slopes`）+ 对象层高度跟随。
- **教训**：资产自带元数据（帧头字段）优先于任何"实测反推"；先 dump 元数据分布再写渲染逻辑。

### 3.12 pixel_to_cell 整数截断 + 方格近似：点击落格系统性偏移
- **现象**：stage 点击放置的对象相对所见瓦片偏移可达半格（左半图尤甚），观感如"瓦片错位"。
- **根因**：`px/(w/2)` 对负数向零截断（地图左半 cx 全错 1）；且方格近似不含菱形判定
  （菱形边缘错最多半格）。画布内瓦片/对象其实全部精确（bbox 实测 [0, bw-1] 吻合）。
- **手法**：精确反算——令 A=px/(w/2)+py/(h/2)=2·cx、B=py/(h/2)−px/(w/2)=2·cy，
  菱形在 (A,B) 空间是边长 2 正方形，就近取整即得格。"渲染错位"类报告先分清
  **画布内**还是**交互路径**，别混在渲染里查。

### 3.14 RA2 地图是砖墙矩形不是菱形；点阵行不对折（连环三坑）
- **现象**：官方图"少渲染下半部分"→ 修边界后"只有 41 行链"；玩家肉眼计数 vs 代码统计长期对不上。
- **根因链**：① 旧代码 `my = rx+ry−(W+1)` 用 `H`（逻辑行）做边界，而反对角线行跨度 ≈ **2H**
  （`Size` 的 H 是逻辑行，1 逻辑行 = 2 反对角线）→ 下半图整段裁掉；② 误把行对折 `/2` + 发明
  "奇偶伴生数据"过滤 → 半数瓦片被丢（可见链 41 = 82/2）；③ 正解 = **全部记录为真实瓦片**，
  行不折半：`py=(rx+ry−min)·15`，`px=(rx−ry−min)·30`（奇偶相位天然由 px 给出）。
- **手法**：玩家的一句"82 实际是 164"胜过所有代码推断——**几何约定类问题优先信官方编辑器
  （FinalAlert2）的显示与玩家实测**，代码统计（格数/bbox/覆盖）在错误几何下会自洽地"验证"错误。
  砖墙几何详见 formats/map.md §2.7。
- **顺带**：NEWURBAN 剧场地形盘是 **ISOUBN.PAL**（曾错复用 ISOURB.PAL，整体偏蓝；单位盘
  UNITUBN.PAL 同理独立存在）。

### 3.15 建筑精灵锚点 = 原版规则"最顶上尖的那一格"（顶格中心 + 帧画布位置）
- **现象**：建筑偏移"超过建筑本身"（锚角/锚边方案都会差半个精灵 + 半个地基；2×2 约 45px+）。
  玩家 FA2 目测先后报"最上角/最底角"——角点观感与中心锚点差不到半格，肉眼容易各执一词。
- **根因**：OpenRA 全链：`CenterPosition = CenterOfCell(topLeft) + CenterOffset`，
  `CenterOffset = (CenterOfCell(W,H) − CenterOfCell(1,1))/2` = **地基中心**（顶左格中心与
  右下格中心的中点，1×1 即格中心）；`SpriteRenderable.ScreenPosition = pos − sprite.Size/2`
  → 精灵**以中心**对齐；ra2 序列 Offset（≈1px 量级）可忽略；SHP 帧在画布内的 (x,y)
  不参与放置（只影响损伤帧切换时的内部偏移）。
- **教训**：锚点语义不要从"美术里哪个点像锚点"反推（底板前脸/楔形都能自圆其说），
  **直接读参考实现的渲染链**（CenterPosition → SpriteRenderable → SpriteRenderer.DrawSprite），
  逐像素验证精灵中心 = 计算锚点（GAPOWR 2×2 实测中心差 0.5px）。
- **再续（原版 ≠ OpenRA）**：原版规则（ra2diy 实测）：**"最顶上尖的那一格是建筑的中心
  也是本体的位置"**——本体位置 = 地基最顶格（地图存储的顶格 (cx,cy)），锚点 = 该格中心；
  OpenRA 的"地基中心"（CenterOffset 公式）是其近似，多格建筑差 (fw−1)·30+…。
  最终公式：`bx = 顶格中心x + frame.x − 画布W/2`（y 同理，−height·15）。
  **结论：先问"原版怎么规定"，再参考实现**；OpenRA 与 FA2/原版可能各自为政。
- **续（逐建筑残差 ≤3 格的真相）**：建筑 SHP 多为"帧错位摆放在大画布里"的多帧画布——
  探针实测 CrctBrd 各建筑帧中心相对画布中心的偏移达 ±70px（CASEAT01 (0,−70)、
  CAMEX02 (+43,+41)、CAEGYP02 (0,+42)…）。OpenRA 在 `DrawSprite` 里把 quad 画在
  `location + sprite.Offset`，ShpTS 的 `Offset = (x + (w−W)/2, y + (h−H)/2)` =
  **帧中心 − 画布中心** ⇒ 美术中心 = 锚点 + (帧中心 − 画布中心)。
  修复 = `bx = 锚x + frame.x − 画布W/2`（步兵同语义）。教训：**逐建筑不同的偏移
  必来自资产自带元数据**（帧在画布内的位置），别在全局公式里找。

### 3.18 建造=BuildupTime 时长（YR .06→54 帧）；步兵 Walk/Idle 原版速率 3 帧/动画帧
- **现象**：① 建筑在建动画被按工期摊帧（GAPOWR 25 帧被 400 帧工期拖成 27 秒），
  且动画播完后停在 make 帧直到"工期"结束（原版没有这段）；② SHP 步兵行走动画
  快 4.5 倍（旧 `clock·2/3` ≈ 1.5 帧/逻辑帧），站立时完全没有 Idle1/Idle2 动作。
- **权威语义（ModEnc + YR 游戏 INI + 参考实现三路交叉）**：
  1. **BuildupTime**：`[General] BuildupTime` = 建筑建造/展开动画运行的平均分钟数；
     **YR rulesmd 实测 = .06 → 3.6s = 54 逻辑帧 @15Hz**（ModEnc 默认档 .05，以
     游戏 INI 为准）。放置后按该时长播完一遍**即完工**（OpenRA yr 每个 `^Building`
     的 `WithMakeAnimation` 也是"动画播完才可用"，无额外长工期）；**无 Buildup
     美术的建筑原版不进入建造状态**（即放即完成；ModEnc：无 Buildup 者不可出售）。
     Buildup SHP = 前半建造帧 + 后半同数阴影帧（阴影索引 1，垫底绘制）。
  2. **步兵播放速率**（ModEnc"Infantry Animation Sequences"hardcoded playback rates，
     单位 = 逻辑帧/动画帧）：**Guard/Ready = 0（静态首帧）、Walk = 3、
     Idle1/Idle2 = 3（RA2/YR；TS/FS 为 1）、Prone = 6、FireUp/Down/Crawl/Up/
     Die* = 1、Cheer = 3、Panic = 4**。`WalkRate/IdleRate` 是载具/飞行器
     （体素）的每类型覆盖，步兵不吃这两个键。
  3. **Idle 动作调度**：`[General] IdleActionFrequency`（**YR = .15 分钟**）= 平均
     间隔，每次动作后取 **0.5~2×** 的随机等待（ModEnc）；动作本体 = Idle1/Idle2
     随机二选一。序列第 4 参数（`Idle1=56,15,0,S`）= 该动画所绘朝向/播完所指方向
     （N/NE/E/SE/S/SW/W/NW 与引擎朝向帧序同序）。
  4. 朝向公式仍是原版原式（chronoshift `facing.cpp` 反编译：`((dir+16)&255)/32`；
     游戏罗盘 DirType 0 = N = 屏幕右上、顺时针增大 → 与引擎 dir 0..7 一致；
     SHP 帧号 = `Start + 朝向·Stride`）。ModEnc 的步兵"逆时针排帧"提示是作者存疑
     备注（该页自注"someone please check"），不采信。
- **实现**：
  - stage `onsite_ticks()` 读 `[General] BuildupTime` 折算逻辑帧（YR 54），作为
    放置/展开的 `build_total`；无 Buildup → 0 = 即放即完成（`under_construction=false`）。
    渲染层 Buildup 帧 = `build_ticks·建造段帧数/build_total`，播完即完工显示 idle 帧。
  - sim：步兵静止且无移动时按 `IdleActionFrequency` 调度 Idle1/Idle2（每单位独立
    LCG 流，确定性；忙期 `kIdleAnimBusyTicks=78` = 最长 idle 26 帧·速率 3；
    移动打断并重掷等待）；`idle_kind/idle_start` 随 `PlacedObject` 给渲染层，
    `visual_hash` 计入 idle 相位（每 3 帧一档 → 重绘节流不吞动画）。
  - 渲染：Walk 相位 = `(anim_clock/3) % Length`；Idle 相位 = `(anim_clock −
    idle_start)/3`（`< Length·3` 时播 Idle 段，否则回 Guard 帧）。
- **根因二（用户实测"没变化"的真凶）：artmd 缓存只存了 Image 表**。
  `ObjectRenderCache` 用 `art_ready` 跳过重解析，但**没有保存解析结果本体**——
  `render_objects` 第二帧起局部 `art` 是空 IniFile：`Buildup=`（以及本次新加的
  `Walk/Idle1/Idle2`）全部查不到 → 建筑建造期停在 make 帧（外观像"受损/半成品"，
  即用户反馈的"建造动画错误"）、步兵永远 Guard（"移动/idle 无动画"）。
  修复 = 缓存 `shared_ptr<core::IniFile> art_file`，`art_ready` 时复用本体
  （见 object_layer.cpp 头部注释）。教训：**缓存派生数据必须把源数据一起缓存**；
  且探针/自检若**不走缓存路径**（`cache=nullptr`），这类"第二帧才失效"的 bug
  永远测不出来——`probe_infanim` 已补上 cache 预热路径回归（首帧后再逐帧验证）。
- **验证**：① `tools/probe/probe_infanim.cpp`（`render_objects` 单对象 → 与 GI.SHP 各帧逐像素
  比对识别实际帧号；**带 ObjectRenderCache 预热**走 stage 同款缓存路径）：dir=0 行走
  t=0..20 帧号 = 8,8,8,9,9,9,…,13,13,13,8（每 3 帧一档）；Idle1 t=0..44 帧 56..70、
  t=45 起回 Guard 帧 0；Idle2 从帧 71 起。
  ② `tools/probe/probe_idle.cpp`（SimWorld 直接驱动）：首次触发 t=176（等待 ∈ [67,337] ✓）、
  忙期 78 帧、之后重掷等待，双跑触发序列逐项一致。③ 回归：simbuild 240 帧
  `GAPOWR under=0 ticks=54/54 hp=256 电力+4550`、遭遇战 4800 帧 Player 5/5（$4700）
  ——双跑哈希一致（movement 自 §3.19 起为 8 向，simattack 基线随之更新）。
- **教训**：① 播放速率查 ModEnc 的 hardcoded rates 表（单位是**逻辑帧**；OpenRA
  导入器的 100/120ms 是它自己的 tick 换算，直接套会错 ~1.5 倍）；② 建造时长与
  帧数解耦：**时长 = BuildupTime，帧数只决定动画帧疏密**；③ idle 是表现但需要
  时间基准——放 sim 用确定性 LCG，渲染层只做相位截断；④ 参考实现的"近似"
  （.05 vs .06、3s vs 3.6s）要回游戏 INI 定夺。

### 3.22 Linux 构建与运行（Windows 之外的第一个平台）- **背景**：引擎原为 MinGW/Windows 开发；移植 Linux（Arch，g++ 16.2）暴露一批
  "Windows 掩盖的坑"。修完 `ctest` 51/51 全绿（含资产/渲染用例，0 SKIP），
  stage 自检输出与 Windows **逐字节一致**（dttd simbuild 240 帧 SHA256 相同）。
- **改动（按坑归类）**：
  1. **CMake 平台守卫**：`tools/CMakeLists.txt` 的 `-municode`（wmain 入口）、
     `-mwindows`（GUI 子系统）、`-static`（MinGW 运行时）与 SDL3.dll 拷贝全部
     收进 `if(MINGW)`（非 Windows 置空）。源码里 `wmain`/`main` 分支本就有
     `#ifdef _WIN32` 守卫，无需改。
  2. **子目录顺序**：`third_party` 必须先于 `engine`——`ra2r_ui` 依赖
     `RA2R_IMGUI_DIR`（在 third_party 里 `CACHE INTERNAL` 定义）。旧顺序靠
     本地构建缓存掩盖，干净构建（Linux）直接报 "No SOURCES given to ra2r_ui"。
  3. **显式头文件**：`lzo1x.h`/`voxel_raster.h` 补 `<cstddef>`、`map_file.cpp`/
     `sim_world.cpp` 补 `<climits>`——GCC 16 不再间接包含（MinGW 侧恰好有别的
     头带进来）。
  4. **同名遮蔽**：`map_file.cpp` 局部 `min_s` 与成员函数 `min_s()` 同名，GCC
     报 "invalid use of non-static member function"（MSVC/MinGW 不报）→ 改 `min_s_v`。
  5. **游戏目录发现跨平台**：
     · 判定"像不像游戏目录"从 `exists(dir/"ra2md.mix")`（Windows 大小写不敏感）
       改为**遍历目录逐项 `iequals`**（Linux 上文件可能是 `RA2MD.MIX`）；
     · 新增 `RA2R_GAME_DIR` 环境变量（Linux 无注册表的主要入口）；
     · 非 Windows 用 `/proc/self/exe` 解析 exe 路径（对应 GetModuleFileNameW）；
     · 候选路径逐级**大小写不敏感解析**（`yuri/` 目录也能命中 `Yuri` 候选；
       `resolve_ci` 第一版有个 bug：该级已存在时忘了写回 `out`，小写目录仍失败）；
     · `name_db.cpp` 的 XCC 名库查找同样补 `/proc/self/exe` 分支。
  6. **测试基建**：`tests/test_util.h` 原来硬编码 `I:/ai/RA2R/Yuri` → 改用引擎
     `find_game_dir()`（与工具同一条发现链），Linux 下资产/渲染用例不再 SKIP。
- **验证**：① Linux `cmake -G Ninja` 干净构建 0 error；② `ctest` 51/51、
  `--gtest_filter` 过滤正常、SKIPPED=0；③ 交叉确定性：dttd simbuild 240 帧
  `.sim.bmp` 与 Windows 版 **SHA256 完全一致**；④ 大小写场景：`yuri` 目录 +
  `RA2MD.MIX` + `--map DTTD.YRM` 全部正常，输出哈希与标准布局一致。
- **教训**：① "Windows 上能编" 不等于可移植——`-municode/-mwindows`、大小写
  不敏感文件系统、隐式头包含是三处典型 Windows 特权；② **干净构建**是移植
  第一道体检（子目录顺序/缓存问题只有它暴露）；③ 路径大小写解析要逐级做，
  且"已存在"分支别忘写回结果（本次 `resolve_ci` 首版就栽在这）。

### 3.23 Linux 中文字体（mixbrowser / stage 的 CJK 支持）
- **现象**：Linux 上 GUI 全是"豆腐块"（缺中文字形）。原因：mixbrowser 自己
  维护字体列表（只有 `C:/Windows/Fonts/*`），stage 走 `ui::setup_cjk_font`
  但候选表也以 Windows 为主；Linux 发行版字体路径千差万别。
- **实现（engine/src/ui/ui.cpp `setup_cjk_font`）**：三级候选——
  1. Windows 系统字体（黑体/雅黑/宋体/等线/楷体，行为不变）；
  2. 常见 Linux 发行版绝对路径（Arch `noto-cjk`、Debian `opentype/noto`、
     Fedora `google-noto-cjk`、思源、文泉驿、文鼎）；
  3. **fontconfig 兜底**：`fc-match -f '%{file}' "sans-serif:lang=zh-cn"` 等
     查询（命令行调用，不引入链接依赖）；fontconfig 也无结果时**递归扫描**
     `/usr/share/fonts` 等目录，按文件名关键字（cjk/han/hei/wqy/noto/…）挑。
  字形范围从 `GetGlyphRangesChineseSimplifiedCommon`（常用 2500 字，生僻字
  缺字）换成 **`GetGlyphRangesChineseFull`**（全量 CJK）。mixbrowser 不再自带
  字体表，直接调用引擎 `ra2r::ui::setup_cjk_font`（与 stage 同一条链）。
- **验证**（Linux 无显示器，用 ImGui 图集探针 `font_check`）：加载
  `NotoSansCJK-Regular.ttc`，图集 2048×4096，**8 个测试汉字（中文游戏建造
  影像）字形全部存在且图集内有实际墨迹**；stage/mixbrowser 无头启动日志
  `cjk_font=1 font=NotoSansCJK-Regular.ttc`。
- **教训**：① 字体发现不要写死路径——Linux 上 fontconfig 是标准答案，
  绝对路径只作快速命中；② 字体"加载成功"不等于"有字形"，验证要看**图集里
  有没有墨迹**（`FindGlyphNoFallback` + alpha 扫描）；③ 工具间共享字体逻辑
  （mixbrowser 抄了一份 Windows-only 列表 → 收编到 `ui::setup_cjk_font`）。

### 3.21 测试基建（GoogleTest）与它抓出的三个真 bug
- **引入**：GoogleTest v1.15.2 vendored（`third_party/googletest`，`RA2R_BUILD_TESTS=ON`
  时编译）+ `tests/`（51 用例 / 18 套件，`ctest` 或 `build/tests/ra2r_tests.exe`）。
  纯逻辑用例不依赖素材；资产/渲染用例在游戏目录缺失时 `GTEST_SKIP`
  （`RA2R_GAME_DIR` 可覆盖）。
- **抓出的 bug（均已修，engine 侧）**：
  1. **到达终点 frac 未清零**：`advance_segment` 在路径耗尽时把余量 `rem` 留在
     `frac` 上（`next` 已置回当前格，位置显示正确但 `unit_moving()` 恒真）→
     到达后走路动画停不下来、`visual_hash` 抖动。修复：终点分支 `frac = 0`。
  2. **空矿石格栅越界**：`find_nearest_ore` 不检查 `ore.size()`，无矿区地图/测试
     场景下按 `y*w+x` 越界读（坏内存/崩溃）。修复：尺寸不符直接"无矿"。
  3. **`SimWeapon` 默认值 = 活武器（25 伤害 / 射程 1）**：没配武器的建筑
     （`configure_building(id, dir, {})` / 地图装载）也拿到"默认手枪"→
     防御建筑误开火、射程外单位被挨打。修复：默认 `damage=0, range=0`（无武器），
     有武器必须显式注入 `Primary=`（stage/rules 路径本就显式）。
- **用例组织**：`test_core`（INI/等距/LUT/寻路）、`test_sim`（移动/建造/经济/防御/
  确定性）、`test_skirmish`（展开/科技树/阵营色）、`test_assets`（文件与规则解析）、
  `test_render`（步兵朝向块/走序列速率/idle/Buildup/体素，带帧识别工具）。
- **手法**：渲染类用例复用"**把美术帧叠到渲染结果上做逐像素识别**"（
  `test_render.cpp` 的 `ShpRef::identify`：参考帧可见包围盒 + 探针像素粗筛 +
  全量比对，注意对齐偏移可为负、画布要够大）。数值期望**以引擎实现语义为准**
  （如 INI `get` 取第一条、调色板 6 位×4、索引 1 = 阴影半透明），发现"期望与
  实现不符"时先读实现判断谁对——本次三例都是**实现对、测试错**，只有三例
  （frac/矿石/默认武器）是实现错。
- **验证**：`ctest` 51/51 通过（~1.5s）；stage 回归（simbuild/simattack/遭遇战/
  演示）双跑哈希一致，遭遇战 $4700 基线不变；simattack 660 帧基线变为
  `(51,155) frac=0 moving=0`（到达清零修复后的正确状态）。

### 3.20 遭遇战自建建筑无动画 + 建筑选中/血条/防御攻击
- **现象**：① 遭遇战里基地车展开、玩家自造的建筑**没有任何动画**（电厂天线/工厂
  门都不动，炮塔也不转）；② 建筑能点选但没有选中反馈与血条，防御建筑完全不能
  攻击。
- **根因**：① `SimBuilding.has_anim`（配件动画时钟开关）只在地图装载后由 stage
  统一标记一次；`spawn_building`（展开/建造/AI 建造）路径没有这一步 → 新建筑的
  `anim_clock` 永不推进，`draw_building_anims` 直接跳过。② 模拟层压根没有"建筑
  武器/目标/炮塔朝向"的字段与逻辑——防御建筑只是静态美术。
- **实现**：
  1. `stage adopt_building(a, id, dir)`：统一收编新落成建筑——`configure_building`
     注入朝向与 rulesmd `Primary=` 武器（`SimWeapon`），并按与地图装载相同的
     判据标记 `has_anim`（ActiveAnim/ActiveAnimTwo/Three/SpecialAnim/体素炮塔）。
     展开（玩家/AI）保留基地车朝向；建造放置与 AI 建造走同一条路径。
  2. `SimBuilding.weapon/target/cooldown/acquire_clock/turret_dir`：tick 中每帧
     （a）清失效目标；（b）无目标每 15 帧扫射程内最近**敌单位**（固定序遍历、
     确定性；`Neutral/Special` 陈设不自动开火）；（c）炮塔按 `dir_toward` 每帧
     转 ≤16/256 圈；（d）射程内且 |偏差|≤16 且 ROF 冷却结束 → 扣血 + 冷却
     （单位死亡沿用既有死亡整批结算）。`issue_build_attack/stop_build_attack`
     为手动接口；`turret_dir` 参与 `visual_hash`（转向即重绘）。
  3. 渲染：SHP 炮塔面向帧与体素 yaw 改用 `b.turret_dir`（地图建筑初值 = 地图
     朝向）；建筑血条 `draw_building_hp_bars`（受损常显/选中必显，画在本体帧高
     之上）；选中建筑画地基格绿框；侧栏显示防御武器并提供"停止攻击"。
- **验证**：`tools/probe/probe_bdef.cpp`：射程内手动指定 → 炮塔转到目标方向
  （dir 字节 96）并按 ROF 扣血（256→176→136→96）；射程外 → 只转向不扣血；
  自动索敌生效（hp 256→56）而 `Neutral` 建筑 target 恒 -1；`stop_build_attack`
  后恢复索敌。`bmpdiff` 对比修前/修后遭遇战 4800 帧整图：差异 9318px 且 bbox
  恰好覆盖两基地建筑带（[677..2479]×[954..1088]）= 补上的配件动画，其余
  逐字节一致；simbuild/simattack/遭遇战/演示双跑哈希一致（$4700 基线不变）。
- **教训**：① "只在地图装载路径做的一次性初始化"是典型漏点——凡是能被
  spawn 出来的实体，都要走同一条"收编"路径；② 表现层缺动画先查**时钟是否推进**
  （`has_anim`/`anim_clock`），再查美术解析；③ 给建筑加武器要顺手处理"中立陈设
  自动开火"这类语义（Neutral/Special 不索敌）与炮塔朝向的状态机。

### 3.19 移动"走格子"感：砖墙格 4 邻接无法走直线 → 8 向 A* + 转角平滑
- **现象**：单位移动有明显"一格一格"感——直行也在左右摆动（阶梯状）。
- **根因**：砖墙格（= 地图格的同构重标号，屏幕位置 `(60c+30(r&1), 15r)`）上，
  4 邻接步 `(0,±1)` 的屏幕向量**随行奇偶在 ±(30,15) 之间交替**，而笔直的 45°
  线需要 `(0,±1)` 与 `(±1,±1)` 交替；4 邻接根本表达不出一条直线斜向路径。
- **OpenRA 参照**（`PathSearch.cs` / `Move.cs`）：
  1. A* 用**8 向邻接 + octile 对角距离启发**（对角代价 ×√2），路径可走直线；
  2. `Move`/`MoveFirstHalf` 在**世界坐标**（WPos/lepton）里推进：`progress +=
     速度`、`WPos.Lerp(from,to,progress,Distance)` 逐帧连续、跨步余量 carryover、
     朝向按进度插值；转角处 SmallTurn→**椭圆弧**连接两段（`shouldArc`），
     大转弯才原地转身——即"直线段精确 + 转角切弧"。
  3. `Locomotor` 按地形速度给每格移动代价；位置从不吸附到格心。
- **本引擎修法**（保持砖墙格与现有地基/阻挡/矿石数据不变）：
  1. **砖墙格 8 邻接 = 地图 8 向单步**（`engine/sim/pathfind.cpp`）：
     · 地图对角步（√2，代价 14）：`(±1,0)` 屏幕水平 60px、`(0,±2)` 屏幕垂直 30px；
     · 地图轴向步（1 格，代价 10）：`(0,±1)` 与按行奇偶二选一的对角步——
       `n = X−Y−min_d = 2col+e`，`e = (row+min_s+min_d)&1`：e=1 → `+X=(1,1) −Y=(1,−1)`，
       e=0 → `+X=(0,1) −Y=(0,−1)`（**曾把 −X/−Y 两条对角写反**，探针立刻暴露：
       屏幕水平目标走出 17px 偏差 + 40px/帧跳变）。
  2. **恒屏幕速率**（`advance_segment`）：每步按屏幕长度折算 frac 增量——
     45° 步 33.5px → `speed`；水平 60px → `speed·143/256`；垂直 30px →
     `speed·286/256`（对应 OpenRA 按世界距离推进的匀速观感）。
  3. **渲染转角平滑**（`stage append_sim_objects`，仅表现层、确定性整数）：
     二次 B 样条，控制点 = 上一格/当前格/下一格/再下一格的**格心**，缺失的邻居
     按直线外推（`prev==cur` → 2P−N；路径尽头 → 2N−P）→ 直线段零偏差、路径
     端点精确落在格心、只在真转弯处切角（切角量 ≤ 步长/8，随 frac 的二次式，
     中点用 2× 坐标避免舍入抖动）。`SimUnit.prev_col/prev_row` 存上一格。
- **验证**：`tools/probe/probe_move.cpp`（逐步推进打印屏幕位置）：
  四个地图主轴目标（屏幕右/下/右下/左下）**直线偏差 ≤0.45px、切角 0.00px**、
  步长恒定 ≈8.8px/帧；斜向目标（含转角）最大切角 ≈9px、步长无跳变。
  `bmpdiff` 对比修前/修后 simattack 660 帧整图：**仅 173 像素（50×29 框 =
  那台在动的矿车）不同**，其余逐字节一致；simbuild/simattack/遭遇战/演示
  双跑哈希一致。
- **后续实测反馈三修**（用户试玩后）：
  1. **朝向判据必须归一化**：`kDirV` 的 8 个屏幕向量长度不等（45° 33.5px、
     水平 60px、垂直 30px），未归一化时 45° 步的点积输给水平步
     （1800 > 1125）→ 斜向移动被判成"朝右"：**VXL 车头只有水平/竖直**、
     **步兵走序列放的是水平朝向帧**。修复 = `kDirU[8]`（×256 单位向量；
     45°=(229,±114)），`dir_of`/`dir_toward` 共用（`dir_toward` 的老实现
     同样中招——斜向目标时单位面向也是错的）。
  2. **重下令不再复位到格心**：移动中（`frac>0`）再下移动令时，旧实现把
     `frac` 归零 + 重设 `next` → 位置瞬间弹回格心（反复右键"跳格"）。
     修复 = 从**当前段终点**续接：保留 `col/next/frac/prev`，只替换剩余路径
     （`set_move_target`/`set_move_target_near` 同规；目标不可达也先走完当前步）。
  3. **跨帧残留 frac 造成每格一次抖动**：`frac += inc` 后可能 ≥256 而留到下一帧
     才结算，渲染位置于是越过格心再被拉回（实测单帧 15~18px 摆幅）。修复 =
     **同帧结算跨格**，且余量按新旧段长度换算（`frac` 是段内百分比，同余量 =
     同屏幕距离：`335/600/300` 基准）。
  4. **步兵序列朝向块序是逆时针（左右反）**：`Sequence=` 的 8 个朝向块按
     **屏幕逆时针**排布——块 f 画的是方向 `(7−f)`（我们的 dir 索引口径），
     与炮塔/载具（顺时针）相反。→ 渲染帧号 = `Start + (7−dir)&7 · Stride`
     （4 参数字母的 idle 段同理换算块号）。判据（`tools/probe/probe_facing_arrow.cpp`）：
     把每帧与"该帧应画方向"的箭头叠画，GI/CONS 两套 SHP 全部 8 帧的
     **枪口/身体朝向与箭头一致**；交叉验证：GI `Idle1=56,15,0,S`/`Idle2=71,15,0,E`
     的固定帧用像素比对最接近 guard 块 3（S=down-left）/5（E=down-right）✓
     与 `(7−dir)` 口径一致（dir(S)=4→块 3；dir(E)=2→块 5）。ModEnc 该页
     "infantries have a counter-clockwise ordering" 的存疑备注其实是对的。
  `tools/probe/probe_movecheck.cpp` 验证：8 个地图方向步的 `dir` 全部命中期望
  （0 错）；移动中连续 4 次重下令，位置单调、单帧位移 ≈8.9px（45° 曼哈顿
  12px）无复位；probe_move 直线偏差复测不变；`probe_infanim` 8 个方向的
  朝向块（dir→块 (7−dir)）全部命中。
- **教训**：① 方向判据用**单位向量**（长短不一的向量做点积 = 隐式加权）；
  ② "格内百分比"这种相对量跨段/跨基准时必须换算绝对长度；③ 移动类 bug 的
  量化指标：单帧位移方差 + 直线偏差 + 朝向命中表，三张表一把梭；
  ④ **不同美术类别的朝向块序可能不同**（步兵 SHP 逆时针、炮塔/载具顺时针），
  不能把一个约定套到所有资产——判"哪一帧画哪个方向"要用**把方向参考叠到美术上**
  的对照法，而不是查文档或猜。

### 3.17 体素炮塔对齐四修：原点锚去 HVA、基准=包围盒中心、偏移=像素、比例 0.35355
- **现象**：盖特机炮/巨炮/爱国者 VXL 炮塔与 SHP 底座错位（盖特炮管组件漂在底座
  上方；巨炮偶/奇行错位程度差 30px；盖特/爱国者对比原版需"左移几像素、上移几像素"）。
- **最终对齐规则（ModEnc 语义 + 真值标定，stage `draw_bld_turret_voxel`）**：
  1. **基准点 = 炮塔 VXL 的包围盒中心 (x,y)**（含 HVA 帧 0 的旋转+平移），
     不是模型原点。这些美术的原点常贴在模型一侧（YAGGUN 主体中心在模型 x=+12、
     FLAKTUR +8.5、SAM +1.8），拿原点当锚会把炮塔整体推右 6~9px；包围盒中心
     同时是炮塔的旋转中心（旋转时自转不漂移）。z 以模型原点（地面）为准。
  2. **偏移 = TurretAnimX/Y（像素）**，相对**建筑精灵锚点**（顶格顶顶点 =
     底座 SHP 画布中心）。ModEnc 权威："default ... dead-center (0,0,0)"、
     "positive values move the turret downward"。**不是**勒普顿、**不是**地基
     几何中心——旧标定"勒普顿+地基中心"两错相抵只在巨炮（2×2）碰巧接近，
     1×1（爱国者）因此偏 15px。
  3. **比例 = 0.35355**（见下）不与载具滑杆联动。
  4. 逐建筑 nudge 表删除（旧 YAGGUN nudge 实为补偿根因）。
- **依据（三路交叉）**：底座/建成帧美术的金色炮塔座与接触点（GTGCAN 实测座心
  (−1.2,+21.1)，包围盒中心规则预测 (+3.0,+21.1)、旧规则 (+? ,27.2)）；四建筑
  手绘 MK 接触点误差包围盒中心规则 |总|≈18px vs 原点规则 ≈29px；用户原版对照
  反馈方向一致（盖特/爱国者需左上）。
- **比例 0.35355**：ModEnc Voxel：**RA2 一格 = 42.4264 体素**（1 体素 = 6.03397
  勒普顿）→ 每体素沿格轴 60/42.4264 = 1.4142px；本投影 `ex=(2s,−s)` →
  s = 30/42.4264 = 0.35355。勿用 0.5（放大 41%：GTGCAN 炮塔罩 105px 占满
  113px 平台，原版约占 2/3）。MK 手绘宽度亦印证（YAGGUN 43px 对模型 @0.354=45px；
  若按 det=1/18 缩到 0.236 则仅 30px → det 不参与渲染比例，只用于 HVA 平移换算）。
- **原引擎缺陷排查**：`rasterize_voxel_parts` 的 origin 锚曾用 `secs[0].proj`
  （含该节 HVA 平移）——YAGGUN 第 0 节是炮管（t=(6.2,9.1,26.7) 体素），整模随
  首节漂移 (+15.4,−25.2)px@scale0.5；修复 = 用单位 HVA 重建节投影再投影原点。
  载具 `anchor_*` 语义未动。
- **手法**：① 判锚点污染看"同光栅 HVA 开/关 origin 差"；② 判基准看模型原点是否
  贴在包围盒一侧（量化 body center）；③ 判单位/参照看手绘 MK 接触点与四建筑
  一致性（不能用 F1——F1 奖励"墨迹≈轮廓面积"，对绝对比例/锚点不敏感）。
- **教训**：① 体素模型原点只是 HVA/载具用的坐标原点，建筑炮塔的对齐基准要另定
  （包围盒中心）；② 手绘 MK 只作粗标定（±5px、炮管仰角会带偏 F1），但"15px 级
  系统性偏移"用接触点（金色座/接触线）一量即现。

### 3.16 建筑炮塔 TurretAnimX/Y 是勒普顿不是像素；真值标定三件套
- **现象**：巨炮/盖特炮塔与底座错位且反复修不对；爱国者 (0,0) 却正确。
- **根因链**：① 基准点用顶格中心（2×2 巨炮差 +30,+7.5）→ 应为地基中心（fw×fh 格中心均值）；
  ② TurretAnimX/Y 按像素 1:1 套用（ModEnc 讹传）→ 实为勒普顿（X·60/256、Y·30/256 换算；
  28 勒普顿≈3px，按像素套即多沉 25px）；③ ZAdjust 是深度遮挡修正非 y 位移。
- **手法**：真值标定 = <建筑>MK.SHP 建成帧（官方成品含炮塔）——差分（真值 − 底座帧0）
  得炮塔区 bbox，其中心 − 底座美术中心 = 实测偏移；三建筑实测（GTGCAN ≈(−4,2)、
  YAGGUN ≈(−4,−8)、NASAM (0,0)）与勒普顿换算全部吻合、与像素解释全部矛盾 → 定案。
  注意 MK 为手绘 SHP，纵横比与体素渲染不同，只可粗标定（±5px 级），细对齐靠语义修正。
- **另**：OpenRA VoxelLoader 顶点用原始索引（忽略 limb Bounds/Scale），与原版引擎
  （包围盒空间，底盘/炮塔按 Bounds 咬合）不同——OpenRA 靠 yaml 手调 Turret Offset 补偿；
  两者实现路线不同，照抄 OpenRA 会踩坑。

### 3.15 VXL 节包围盒是模型空间真坐标：忽略则炮塔埋进底盘
- **现象**：坦克只见底盘不见炮塔/炮管；建筑炮塔（巨炮 GTGCAN/盖特 YAGGUN）不渲染或与底座错位。
- **根因**：VXL 每节的 min/max 是**模型空间包围盒**（体素索引 i 的模型坐标 = min + (i+0.5)·(max−min)/size）。
  底盘/炮塔/炮管三件 VXL 同一坐标系按包围盒对位（实测 HTNK：底盘 z 0.2..11.4、炮塔 z 11.7..20、
  炮管 x 11.8..30.7 —— 不用任何装配偏移就天生咬合）。旧光栅把**索引**当模型坐标投影，三件全叠原点
  → 炮塔埋进底盘内部不可见。
- **手法**：把包围盒映射烘焙进投影基向量（每轴步长 = span/size，min+0.5·step 并入平移），
  project() 仍吃索引、底盘观感不变（步长 ≈ 1）；建筑炮塔命名 = <Image>TUR.VXL（GTGCAN）或
  <Image>.VXL 本体即炮塔（YAGGUN 单 VXL 三节，HVA 平移定位双炮管），固定 2.0px/体素
  （54 体素 ≈ 2 格宽实测），锚点 = 主体节基座中心对顶格中心。
- **教训**：资产自描述的坐标数据（包围盒/HVA 矩阵）优先于任何手工装配公式——先探针 dump 结构再写合成。

### 3.13 帧缓存序列化头长不一致：所有缓存瓦片整体右移（+4/+7px）
- **现象**：3×3 纯平图逐行比对——每帧行内容统一右移 7px（左缘缺口 + 右缘越界被画布
  裁掉）；大图上因位移全局一致、接缝自洽，仅"对象偏左/右列被裁"可见，极难察觉。
  此前 schema 1 就有 +4px 版本（`4+2+5*4+4` 多算一个字段），M2 全部缓存渲染均带 4px 位移。
- **根因**：`serialize_frame` 头长算式（30/33）与 `deserialize_frame` 的 26 不一致——
  像素区被 memcpy 到偏移 30/33，读取却从 26 开始，**每行行首多读 4/7 个字节**。
- **手法**：① 序列化头长提取为 serialize/deserialize **共用常量**（本次直接 `head=26`
  + 注释声明布局）；② 修序列化必升 schema 并清缓存；③ 最小实验（`--flat 3x3` 纯平图
  + Python 理论覆盖掩码逐像素 diff）是"整体位移类"缺陷的唯一可靠检出手段——
  大图上全局位移在统计上不可见（bbox 都会被内容多样性骗过）。
- **教训**：cache-miss 与 cache-hit 两条渲染路径**都要**逐像素回归（本轮加进无头自检基线）。


## 4. 构建/工具链类

- 无管理员工具链：WinLibs MinGW（免安装）+ pip CMake + SDL3 mingw 预编译包，全部放 `I:\tools\`（不入库）。
- 网络：`github.com` 直连不可达；`git`/`curl -x socks5h://127.0.0.1:10808` 可用；WinINET（winget/IWR）不可用。
- **静态链接运行时**（`-static`）+ SDL3.dll 构建后拷贝 → 双击可运行（依赖清单只剩系统 DLL + SDL3）。
- `-municode` + `wmain`：中文路径/文件名的必配（`std::filesystem` 从窄字符串构造在 GBK 环境会抛异常）。
- `-mwindows`：GUI 工具双击无控制台窗口。
- `-D_GLIBCXX_DEBUG` 需**全局一致**（engine 与工具 ABI 匹配），单目标开启会链接失败。
- PowerShell `Set-Content` 默认 ANSI 编码会**毁掉 UTF-8 源文件**——改文件用编辑工具或显式 `-Encoding utf8`。

## 5. 流程类经验

- **Heisenbug 优先"构造性加固"**：与其无限追幽灵，不如消灭所有潜在越界源（尺寸校验、防御钳制）+ 保留崩溃过滤器等下次现形。
- **锚点验证**：每个编解码器落地后，先用公开锚点/真实文件数学自洽（帧表偏移、body_size 等式、mod 11、单元格总数公式）验证，再谈渲染。
- **参考实现对照**：格式逆向时维护一个独立语言（Python）的可工作实现，逐步 diff 轨迹。
- **回归套件**：每个修复都补一个确定性自测（--test-*），并跑全套件（combo/reopen/tabs/basic）。

## 6. 资产渲染类（SHP / VXL / 剧场）

> 总原则：**先按内容/元数据自判，再谈公式**。美术文件自带"谁是什么"的信息
> （帧像素构成、帧段边界、包围盒、名字里的剧场代号），比任何跨代文档都可靠。

### 6.1 SHP 阴影帧被画成蓝色：阴影索引是 1，不是 XCC 说的 4
- **现象**：建筑/动画的影子是**深蓝色**，科技油井的动画帧下方出现蓝色团块。
- **根因**：阴影帧的非零像素**只有调色板索引 1**；`UNITURB.PAL` 索引 1 = (0,0,49) 深蓝，直接取色就是蓝影。XCC 文档的"索引 4 = 阴影"是 TD/RA1 时代惯例，不适用于 RA2/TS。
- **实测**：扫 1200 个 SHP / 20589 帧，单色帧 6363 个，其中 **6292 个（1025 文件）是索引 1**，其余索引各只 1 帧。OpenRA 引擎 `PaletteFromFile` 的 `ShadowIndex=1` 把该索引重映射为 `140u << 24` = **ARGB(140,0,0,0)** 半透明黑。
- **手法**：调色板 LUT 里把索引 1 映射成半透明黑，blit 端**按源 alpha 混合**（原 blit 只看调用方 alpha，源 alpha 被丢弃 → 仍画成不透明黑）。三条 blit 路径（object_layer/overlay_layer/stage）都要改。

### 6.2 建筑动画"空闲与受损同时播放、叠在一起"
- **现象**：光棱塔/磁暴塔的棱镜/线圈出现**两套动画叠加**；受损时反而什么都不画。
- **根因**：动画 SHP 是 **`[空闲 L][受损 L][阴影 L][阴影受损 L]`**——受损变体在**第 2 段**、阴影在**后半 n/2**。旧实现按 `[动画][阴影][受损][受损阴影]` 理解（受损取 `2L`、阴影取 `fi+L`），于是把**受损帧当阴影画**（看起来像两套动画叠加）。
- **旁证**：`artmd` 的 `ActiveAnimDamaged=<名>_AD` 指向的文件（NATSLA_AD / GAPRIS_BD / CAOILD_AD）**在原版数据里根本不存在**，受损动画一直是同文件第 2 段。
- **手法**：新增 `assets::shp_layout()` 按帧内容自判：后半全为"纯索引 1 帧或空帧"才有阴影段；四段/两段用**循环接缝**区分（空闲段自闭环时 `L=n/4` 的接缝更小）。实测 NATSLA_A 0.37 vs 0.84 → L=10、CAOILD_F 0.53 vs 0.32 → L=16（两段）、GAREFNOR 1.13 vs 1.06 → L=20。`_AD` 文件存在才用它，否则回退同文件受损段。

### 6.3 科技油井"动画贴图下方有奇怪的图形"
- **现象**：油井下方出现彩色乱图 + 摇臂重影。
- **根因**（两处索引错位）：① 本体 `CAOILD.SHP` n=8，阴影起点是 **n/2=4**，旧代码写死 `fi+3` → 把第 4 帧（**建造脚手架彩色帧**）当影子画；② 动画 `CAOILD_A.SHP` n=128，阴影在 64 起，旧代码取 `fi+32` = **受损摇臂**。
- **教训**：**帧数不是 6 的建筑照样有阴影段**（8 帧 = 4 状态 + 4 阴影），"阴影帧 = 帧 0/1/2 各 +3"只对 6 帧本体成立；一律用 `shp_shadow_start()`（后半全阴影/空 → n/2）。

### 6.4 `SpecialAnim` 是动作动画，待机不该静态叠加
- **现象**：光棱塔待机时棱镜"多了一层"。
- **根因**：`SpecialAnim`（GAPRIS_A 充能、NATSLA_B 放电、GADEPT_A 吊臂）是**开火/动作**动画——artmd 的 `IsAnimDelayedFire=yes` / `DelayedFireDelay=28` 是直接证据；旧实现按"常驻配件"静态画帧 0。
- **手法**：待机不画 SpecialAnim（动作播放归战斗系统）；判断依据从 artmd 键读，不靠观感猜。

### 6.5 HVA：矩阵是**列主序**、平移单位是 **1/16 体素**
- **现象**：盖特机炮的枪管变成"白色条状物乱飞/闪烁"，且闪烁频率与炮塔抖动一致。
- **根因**：① 文件里 12 个 float 是**列主序**（每 4 个一列：3 旋转 + 1 平移），按行主序读得到的是转置（逆旋转）；② 平移量单位是 **1/16 体素**，原值直接用会把部件甩出几十到几百体素（YAGGUN 枪管 tz=479.6 → 应为 30）。
- **实测**：`YTNKTUR` tz=256.7/16 = 16.0，而 `YTNK` 车体高 15 体素（炮塔正好落在车体顶）；全库 139 个非零平移 section 按 1/16 全部落在模型尺寸内。
- **手法**：解析时转置（对齐 OpenRA `HvaReader` 的 `ids={0,4,8,12,1,5,9,13,2,6,10,14}`），平移统一 `×1/16`；改完先跑"单模渲染"看部件是否落在模型内。

### 6.6 体素炮塔：先查"是不是漏了部件"，再用游戏自己的美术标定锚点
- **现象**：盖特机炮炮塔相对底座偏右约 7px；巨炮只有一团黑方块、没有炮管。
- **根因（两层）**：
  1. **漏部件**：一个建筑/单位由多个 VXL 拼装（`<Image>TUR.VXL` 炮塔、`<Image>BARL.VXL` 炮管，见 formats/vxl.md §1.0.1）。巨炮的炮管是独立的 `GTGCANBARL.VXL`（x 21.5–74.8，53 体素长），只画 `TurretAnim` 就只剩炮塔主体；坦克同理只画了车体。
  2. **锚点**：`YAGGUN` 主体包围盒中心在模型 x=+12（原点贴在主体一侧），用模型原点当锚点整体偏右——但**这正是原版语义**：`TurretAnimX/Y` 就是相对建筑锚点的像素偏移，模型原点即炮塔枢轴。
- **最终规则（OpenRA 机制 + 原版 INI 数值）**：
  - **机制照 OpenRA**：多部件合成一张光栅（共享 z-buffer）→ 以**模型原点**对齐 `pxOrigin`（OpenRA `ModelRenderable` 的 `DrawSprite(sprite, pxOrigin - 0.5*Size)`，Sprite.Offset = 模型屏幕 AABB 中心，净效果 = 模型原点落在 pxOrigin）；偏移是**屏幕空间平移**。
  - **数值照游戏自己的 INI**：`rulesmd` 的 `TurretAnimX/TurretAnimY`（盖特 0,15；巨炮 3,28；四管 0,2；防空 0,0），相对**建筑精灵锚点**（画布中心）——不读 OpenRA 的 yaml。
- **被否掉的几种锚点**（都试过，附实测）：
  - 整模 AABB 中心：炮管朝向一变摆 **±27px**（GTGCAN dir0→dir128 从 +27 到 −28）→ 旋转漂移。
  - 主体"座锚点"（主体包围盒中心 + 主体最低 z）落在底座黄铜/红漆"炮塔座"上：与建成帧美术 F1 只有 0.03~0.08，巨炮炮塔被拉到平台一侧。
  - 只按 `TurretAnimY` 而不含模型自身高度：盖特机炮下沉 13.6px（模型 z 从 22.6 起）。
- **标定手法（可复用）**：`<建筑>MK.SHP` 完成帧 = "底座 + 炮塔"的正确相对位置，**炮塔轮廓 = MK 帧像素 ≠ 底座帧像素**的区域；把体素整模叠上去暴力搜"尺度+偏移"使轮廓 F1 最大（`tools/probe/probe_turret_align.cpp`）。实测 YAGGUN 最优尺度 0.35（F1 0.678）、GTGCAN 0.50（F1 0.800）；该 F1 最优解的 x 比 INI 规则偏左约 14px——原因是美术里炮管带仰角、且完成帧本身是手绘近似，故**最终以 INI 数值为准，美术只作交叉验证**。
- **教训**：① 对齐/缺件先问"这个单位到底由几个文件组成"（全库 14 个 `*BARL.VXL`）；② 锚点必须选**朝向无关**的模型点；③ 机制可以照参考实现，**数值一定要回到游戏自己的 INI**——参考实现的 yaml 是它自己调出来的，未必等于原版。

### 6.7 VXL 内嵌调色板是 8-bit，别再 ×4
- **现象**：体素模型整体过曝、炮管发白、暗部丢层次。
- **根因**：`.PAL` 是 6-bit（0–63，需 ×4），但 **VXL 主头内嵌的 768 字节是 8-bit（0–255）**。实测 `YAGGUN.VXL` 索引 15/20/13 = 255/195/87，而 `UNITURB.PAL` 同索引 = 63/48/21。
- **手法**：体素渲染直接取内嵌值（最多乘光照系数）；同类问题先 dump 两种来源的同索引值对比，别凭"都是调色板"就套同一套换算。

### 6.8 NewTheater 剧场代号：类型名第 2 字母是**雪**（'A'）
- **现象**：温和地图里建筑底座**覆雪**（GAPOWR 白底、YAGGUN 白底座）；巨炮整块底座缺失。
- **根因**：`NewTheater=yes` 的建筑 SHP 名第 2 字母是剧场代号：**T=温和、A=雪、U=城市、D=沙漠、L=月球、N=新城市**，找不到则回退 **G=通用**。而 rulesmd 类型名（GAPOWR/YAGGUN/NATSLA/CAOILD…）第 2 字母恰好是 **A**——"按原名加载"= 加载雪地美术。巨炮 `GTGCAN` 的底座只有 `GAGCAN`(雪)/`GGGCAN`(通用)，原名 `GTGCAN.SHP` 不存在。
- **实测**：GAPOWR(A) 底座覆雪 vs GGPOWR(G) 橄榄绿；GASAND(A) 雪白沙袋 vs GTSAND(T) 橄榄绿；CAOILD(A) 塔身积雪 vs CTOILD(T) 无雪；GAWALL(A) 白墙基 vs GTWALL(T) 绿墙基。全库 SHP 名第 2 字母只有 A/D/G/L/N/T/U 高频。
- **手法**：解析链 = **换剧场代号 → 换 G → 原名**（代号优先！）；本体、`*_A` 动画、`*BB` 底座、`*MK` 建造、围墙/沙袋/栅栏共用同一张表。**注意与扩展名代号是两套**：扩展名用 `tem/sno/urb/des/lun/ubn`。

### 6.9 低桥多格件的"件中格"：等屏步进 + 短对角限制
- **现象**：atwar.yrm 混凝土桥缺一段（露出水面）；相邻同类型桥件互相"抢中格"。
- **根因**：桥件 = 3 格沿屏幕对角排列（origin→middle→tail，data 0/1/2）。旧实现用"±1 内第一个同类型 data=1 邻格"，相邻两件同类型时会命中**邻件的中格**，把自己画到邻件位置，自身 3 格空出。
- **手法**：判据改成"**屏幕坐标等步进的 3 格**"，且步进必须限制为**短对角 ±(30,±15)**——地图坐标因隔行奇偶会交替记成 `(0,+1)/(+1,+1)`，屏幕投影上才是直线；水平 `(±60,0)` 或长对角 `(±90,±15)` 都是"借邻件格子凑的假直线"。
- **教训**：等距几何里"看起来是直线"要**先换算到屏幕坐标再判等**，别在地图格坐标里比。

### 6.10 主循环不让出 CPU → SDL 音频线程饿死
- **现象**：mixbrowser 里 AUD 播放慢到 ~1/60 速度；BIK 有画面无声音。
- **根因**：主循环无帧率上限、连续 `SDL_UpdateTexture` + D3D11 present 占满 CPU，SDL 的 WASAPI 音频线程拿不到时间片。
- **手法**：每帧补足到 **16ms**（`SDL_Delay(16 - elapsed)`）。验证口径：AUD 队列按实时速率消耗（100% 实时）、BIK 音频缓冲稳定在 ~0.81s。
- **教训**："音频慢放"优先怀疑**主线程饥饿**，而不是重采样参数。

### 6.11 swscale 目标行距必须对齐（否则堆破坏）
- **现象**：宽度非 16 倍数的视频（如 140）转换后随机崩溃/花屏。
- **根因**：FFmpeg 8.x 的 bilinear 路径会按**对齐宽度**写行尾，目标缓冲按 `w*4` 分配就越界。
- **手法**：目标行距 `vstride = (w*4 + 31) & ~31`，缓冲按 `vstride*h` 分配，转换时把 `dst[0]=vstride` 传进 `sws_scale`；对外用 `video_stride()` 暴露真实行距。

### 6.12 FFmpeg 动态绑定：符号归属库别搞错 + packet 必须 unref
- **现象**：`SDL_LoadFunction` 绑定全部失败/运行期崩溃。
- **根因**：`av_packet_alloc/free/unref` 在 **libavcodec**，不在 libavutil；加载顺序也必须 avutil → swresample → swscale → avcodec → avformat。
- **手法**：`BIND(库, 符号)` 宏把库名写进绑定表（编译期就能看出归属）；`av_read_frame` 得到的 packet 每条路径都要 `av_packet_unref`（含错误分支）。

### 6.13 "解码失败"其实是 UI 没置标志位
- **现象**：mixbrowser 的 PCX 预览永远显示"解码失败"，但日志显示解码成功。
- **根因**：`select_entry` 解码成功分支**忘了置 `s.pcx_ok = true`**，渲染分支按旧标志位走失败提示。
- **教训**：状态机型 bug 先查"成功分支是否把状态写全"，别急着怀疑解码器；给每个 kind 都加一条"解码成功 → 断言标志位"的自检。

### 6.14 探针方法论（本轮反复见效的三件套）
1. **多模态读图**：把渲染结果裁图/放大直接看（`read_image`），比逐像素数字更快定位"形状/对齐/颜色"类问题；导出用 BMP→PNG（PowerShell `System.Drawing` 即可）。
2. **全库聚合统计**：单例可能骗人（一个 SHP 的布局是特例），把**全库同类文件**扫一遍看分布（第 2 字母直方图、单色帧索引直方图、139 个 HVA 平移量）才能定规则。
3. **几何自洽校验**：判定出的参数要能用**另一份数据**验证（炮塔 z=30 对炮塔节 z 范围 22.6–39.7；`YTNKTUR` tz/16=16 对车体高 15）。只靠"看起来对"的修复最容易留下 ±15px 级误差。

---

## 7. 遭遇战流程类（M4 实战沉淀）

### 7.1 单位从 vector 里 erase 后，引用/下标立刻失效（又一次 UAF 变体）
`deploy_mcv()` 内部会 `remove_unit()`（erase 该 MCV），调用方若在此之后还读
`a.sim.units[i].type` / `u.col`，读到的是**移位后的邻居**——日志里出现过
"`DOG 展开 → GACNST @(41,55)`"（其实展开的是 AMCV，狗只是被前移补位）。
规则：**调用可能改动容器的函数前，把需要的字段拷成值**（`const std::string utype = u.type;`），
展开/击杀/生产完成这类"消耗自身"的操作全部按此写。

### 7.2 Buildup 动画只有前半是建造帧
artmd `Buildup=` 的 SHP 与建筑本体同构：**前半建造帧 + 后半同数阴影帧**
（GACNSTMK 58 = 29 + 29）。按 `进度 × 总帧数` 取帧会在中段取到阴影帧（画面突然变空）。
正确做法：先 `shp_shadow_start()` 求建造段长度，再 `floor(进度 × 建造段帧数)`。
展开时长同理 = 建造段帧数 / 15fps（GACNST ≈ 1.9s），不是总帧数。

### 7.3 阵营色 remap 段是 8 位值，不能走调色盘的 ×4 路径
`.PAL` 是 6 位（显示时 ×4），但 `[Colors]` 的 H,S,V 生成的 16 色 ramp 是**8 位 RGB**。
若把 ramp 填进 `PaletteLut::build` 的 6 位路径再 ×4，颜色会过曝成白块。
`build(pal, remap16)` 里对索引 16..31 直接取 8 位值；VXL 内嵌盘同理（索引同段 16..31）。

### 7.4 按名字前缀挑阵营建筑会挑错
苏军兵营是 `NAHAND`（不是 NAPILE）、尤里兵营是 `YABRCK`——"GAPOWR→NAPOWR"式前缀替换只在
部分建筑上成立。而且 `[BuildingTypes]` 里盟军的 `GAPOWR` 也写着 `Owner=…Russians…`，
按 Owner 过滤仍可能先命中盟军件。可靠做法：**按角色（power/refinery/barracks/weapon）
扫描 `[BuildingTypes]`，候选必须通过同一套科技树校验**（前置 `GACNST` 不满足时
`NAPOWR` 自然胜出）。

### 7.5 空 `Owner=` 与"没有 rulesmd 节"的建筑要显式排除
`[BuildingTypes]` 里混着大量装饰/民用件（`CATIME`、`CASYDN01`…），它们**在 artmd 有节、
rulesmd 没有**；按 `get()` 取默认值会得到 `TechLevel=0 / Owner 空 / Cost=300`，
于是"全阵营可造"。判定第一关必须是 `rules().has_section(name)`，第二关 `Owner=` 非空且含国家。

### 7.6 地图 waypoint 的奇偶性不保证
`[Waypoints]` 的 `(rx−ry)` 可能与 IsoMapPack5 点阵的奇偶不一致（battle1 的 wp1 是奇数），
原版与 OpenRA 都直接 `/2` 截断——**不要自作聪明取偶**，否则出生点会整体偏一格。

### 7.7 自检脚本的时序要按"动画时长"排，不是按"命令发出时刻"
排队/展开在动画未结束时会被正确拒绝（`需要建造厂`/`缺少前置：POWER`）。
脚本里 `queue(GAREFN)` 必须排在电厂**完工**之后（电厂工期 = cost/2 帧），否则日志里的
失败看起来像 bug，其实是科技树在正常工作。

