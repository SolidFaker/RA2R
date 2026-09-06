# 规范缓存层设计（M1 交付物 · 设计稿）

> 状态：设计稿（M2 渲染引擎动工前实现）。本文定义"规范缓存（canonical cache）"的
> 磁盘/内存格式、生成策略与 API 草案。目标：M2 引擎启动时**只读缓存**，不再逐帧解包。

---

## 1. 设计目标

| 目标 | 说明 |
|---|---|
| 一次转换，多次消费 | MIX 解包 + 解码 + 规范化只做一次；M2 起直接 mmap/流式读取 |
| 启动时间可预期 | 首启全量构建缓存（可并行、可增量），之后按缓存键跳过 |
| 可失效 | 缓存键 = 源文件标识 + 格式版本；源变更自动重建 |
| 内存友好 | 图集/网格采用紧凑布局；地图 cooked 支持分块流式加载 |
| 可移植 | 全部小端序、显式对齐，不依赖平台结构体内存布局 |

---

## 2. 缓存根与目录布局

```
<工程>/cache/                      ← 不入库（.gitignore）
├── index.bin                      ← 全局索引（缓存键 → 文件路径/偏移）
├── shp/
│   └── <name>/<crc32>/atlas.png   ← SHP 图集（RGBA）
│                /frames.bin       ← 帧元数据（见 §4）
├── vxl/
│   └── <name>/<crc32>/mesh.bin    ← 体素网格（见 §5）
│                /pal.bin          ← 768B 调色板副本
├── ini/
│   └── <set>/<crc32>/rules.bin    ← 编译规则库（见 §6）
├── map/
│   └── <name>/<crc32>/header.bin  ← 地图头（尺寸/剧场/索引）
│                /chunk_0000.bin   ← 分块格子数据（流式加载）
└── audio/
    └── <name>/<crc32>/pcm16.bin   ← 解码后 16bit PCM（可选，量大时按需生成）
```

- 缓存键 = `CRC32(源文件字节) + 格式 schema 版本号`，写入 `index.bin`。
- 图集用 PNG（磁盘紧凑）；运行时纹理由 SDL/GPU 直接加载。

---

## 3. 缓存键与失效

```
cache_key = SHA 前 8 字节？ → 不需要：CRC32(源字节) 即可（本地工具级）
key 记录: {u32 source_crc, u16 schema_ver, u16 flags, u64 cached_at}
```

- `schema_ver` 按格式模块硬编码（如 SHP=3），解析器改动时 +1 → 旧缓存整体失效。
- 资产位于 MIX 内时，源字节 = MIX 解出的条目字节（含嵌套路径的条目 CRC）。
- 增量构建：`index.bin` 比对后仅重建变更项；构建可多线程（资产相互独立）。

---

## 4. SHP → RGBA 图集 + 帧元数据

### 4.1 图集打包策略

- 逐帧解码索引图（RLE）→ 调色板映射 RGBA。
- **打包**：按帧尺寸降序装箱（shelf 算法），帧间 1px 透明 padding（防采样渗色）。
- 单 SHP 超 4096² → 拆多张图集（`atlas_0.png`, `atlas_1.png`...），元数据记录归属。
- 调色板选择：记录构建时采用的调色板名与 CRC（与自动识别结果一并存于元数据）；
  运行时换剧场换盘 = 重建缓存（键含调色板 CRC）。
- **remap 通道**（单位换色）：图集额外存一版"索引图"（`atlas_idx.png`，每像素=原始调色板索引），
  运行时换色 = 查 remap 表逐像素重映射。成本：图集体积 ×2，收益：换色零重建。

### 4.2 帧元数据（frames.bin）

```
header: {u32 magic 'SHPF', u16 ver, u16 frame_count, u16 width, u16 height}
每帧: {u16 atlas_id, u16 x, u16 y, u16 w, u16 h,     ← 图集内位置
       i16 hotspot_x, i16 hotspot_y,                 ← 中心锚点（RA2 SHP 无，恒 0；留扩展）
       u16 fps_hint}
```

- 内存态 = 上述数组 + 纹理句柄；`DrawFrame(n)` = 查表取 UV 矩形。

---

## 5. VXL → 网格

### 5.1 网格化（构建期）

- 体素网格 → **可见面提取**：对每个非空体素的 6 个邻面，邻接为空/越界才输出四边形。
- 顶点位置 = 体素坐标（构建期单位：1 体素 = 1 单位，y 轴向上同 RA2 世界）。
- 每面记录：4 顶点 + 调色板索引 + 法线索引（RA2 244 条法线表查表得方向）。
- 同 section 内按索引合并 → `mesh.bin`：

```
header: {u32 magic 'VXLM', u16 ver, u16 section_count}
每 section:
  {name[16], u32 vert_count, u32 quad_count, float[12] bind_pose}
  顶点: {i16 x,y,z, u8 pad, u8 color_idx, u8 normal_idx, u8 pad2} × vert_count
  面:   {u32 a,b,c,d} × quad_count         ← 索引网格（顶点共享去重）
```

- HVA：动画数据很小，**运行时直接读原 HVA 字节**（不转缓存）；每 section 的 bind_pose
  记录到 mesh 头以便绑定。渲染 = 骨骼式变换：`world = HVA矩阵(frame,section) × 体素坐标`。

### 5.2 运行时

- GPU 顶点缓冲一次上传；动画帧变换走实例化/顶点着色器（uniform 传 12 float）。
- 阴影：法线点积光方向，M2 与引擎光照统一。

---

## 6. INI → 编译规则库

### 6.1 两层表示

1. **原样层**：`IniFile`（现 M1 实现）——文本 → 节/键值，供调试与增量编辑。
2. **编译层（rules.bin）**：类型化结构序列化：

```
header: {u32 magic 'RULC', u16 ver, u32 section_count, u32 entry_count}
节表: {name_offset u32, kind u8, entry_range u32[2]} ...
实体表: 按 kind 的紧凑结构数组（Building/Vehicle/Infantry/Weapon/Projectile/Warhead/...）
字符串池: NUL 分隔 UTF-8（全部名字/枚举值去重后集中存放）
```

- 数值字段用**整数定标**（如 Speed=float→i16×256；Range=格子→i16），避免浮点漂移与体积。
- 布尔/枚举 → u8 位域。
- 未知键：保留在"扩展池"（键名 + 原始值文本），**前向兼容**——新引擎版本不认识旧键时不丢数据。

### 6.2 确定性要求

- 序列化顺序 = INI 出现顺序（同源同输出，字节级稳定）。
- 规则库版本随引擎语义版本走；`[AudioVisual]` 等全局节同样编译。

---

## 7. 地图 → cooked 格式（流式加载）

- 现 M1 `MapFile` 解析结果直接序列化，不做二次压缩（LZO 解包成本换磁盘）：

```
header.bin: {u32 magic 'MAPC', u16 ver, i32 w,h, theater[32],
             u32 chunk_bits, u32 chunk_count, u32 cell_count,
             u32 overlay_present, u32 waypoint_count, ...}
chunk_N.bin: 固定 N×N 格子块（如 32×32）：
  每格: {u16 tile, u16 extra, u8 subtile, u8 height, u8 extra2, u8 overlay, u8 overlay_data}
  ← 11B 地图记录 + overlay 归并，32 字节对齐填充
```

- **流式加载**：仅加载视口覆盖的 chunk（+1 圈预取）；迷雾/地形破坏回写改脏块。
- 战役脚本/触发：M3 再定（另出 `triggers.bin` 草案，不阻塞 M2）。

---

## 8. CacheManager API 草案（引擎侧）

```cpp
namespace ra2r::assets {

class CacheManager {
public:
    // 打开缓存根；mode: ReadOnly（M2 运行）/ ReadWrite（构建工具）
    bool open(const std::filesystem::path& root, Mode mode, std::string* error);

    // 取 SHP 图集：命中返回句柄，未命中（ReadWrite）触发构建
    const ShpAtlas* shp_atlas(const AssetRef& ref);   // AssetRef{mix 路径, 条目 id, 名字}
    const VxlMesh*  vxl_mesh(const AssetRef& ref);
    const RulesDb*  rules(const std::string& set);     // 如 "RULESMD"
    MapCooked*      map_cooked(const std::string& name);

    // 工具侧
    BuildStats build_all(const std::vector<std::filesystem::path>& mix_paths,
                         size_t threads);
};

} // namespace ra2r::assets
```

- `AssetRef` 解析路径：目录 MIX → 条目（含嵌套）；名字解析复用 M1 名称库。
- 构建工具 = M2 起始阶段的小工具（`tools/cachebuild`），复用 assetcheck 的遍历框架。

---

## 9. 容量估算（YR 全量）

| 项 | 估算 |
|---|---|
| SHP 图集（RGBA + 索引图双份） | ~600–900 MB（PNG 压缩后 ~150–300 MB） |
| VXL 网格 | 单位 ~30 个 section × 几 KB → 总量 < 20 MB |
| 规则库 | < 2 MB |
| 地图 cooked（全量预构建） | < 50 MB |
| 音频 PCM16（可选） | ~200 MB（默认不构建，运行时流式解） |

结论：**默认构建"图集 + 网格 + 规则库 + 地图"**（磁盘 ~300 MB 级），音频按需。

---

## 10. 与 M2 的接口约定（提前锁定）

1. 纹理坐标一律**左上原点**（与 SHP 帧一致），M2 渲染器做翻转适配。
2. 颜色空间：调色板 6bit×4 → 8bit，sRGB 直用；光照在 sRGB 近似下计算（M2 先不搞线性空间）。
3. 体素网格 y 轴向上；等距投影在渲染器统一处理（缓存不做投影烘焙，保留 3D 信息）。
4. 所有 bin 文件头带 magic + ver；解析器见 magic 不识别即报"缓存版本过旧"。
