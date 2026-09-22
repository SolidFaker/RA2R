#pragma once
// RA2R — 共享地图场景渲染（mapview/stage 复用同一套地形+装饰管线）
//
// 抽取自 mapview/stage 的重复实现（编码准则：渲染路径只维护一份）：
//   - scene_cells：MapFile → 场景格视图（瓦片/子瓦片/高度/存在性）
//   - scene_overlay_names：RULESMD [OverlayTypes] 覆盖物名表（含类型 0 沙袋）
//   - build_scene_decor：装饰列表（树木/岩石 + 覆盖物，类型→硬编码艺术表 + 帧规则）
//   - render_scene：地形绘制（瓦片解码/缓存/光照）+ 装饰层一次完成
// 对象层由调用方用 render_objects 追加（mapview/stage 各自的对象列表/缩放不同）。
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ra2r/assets/map_file.h"
#include "ra2r/assets/theater.h"
#include "ra2r/assets/tileset.h"
#include "ra2r/cache/cache_manager.h"
#include "ra2r/render/isometric.h"
#include "ra2r/render/overlay_layer.h"
#include "ra2r/render/palette_lut.h"

namespace ra2r::render {

// 场景格（MapFile::MapCell / StageCell 的统一视图）
struct SceneCell {
    uint16_t tile_id = 0;
    uint8_t subtile = 0;
    uint8_t height = 0;
    bool present = false;
};

// 从 MapFile 提取场景格（transpose 交换 cx/cy，诊断对照用）
std::vector<SceneCell> scene_cells(const assets::MapFile& map, bool transpose);

// 覆盖物类型 → RULESMD 名（含类型 0 = 沙袋围墙、Image= 覆盖）
std::map<int, std::string> scene_overlay_names(const FileLoader& load);

// 构建装饰列表：树木/岩石（[Terrain]）+ 覆盖物（类型硬编码艺术表 + 帧规则）。
// 产出 MapDecorObject.art = 最终文件名（已含剧场后缀/墙前缀）。
// transpose = 交换装饰的 cx/cy（与地形转置配合）。
void build_scene_decor(const assets::MapFile& map, const assets::TheaterConfig& cfg,
                       const FileLoader& load, bool transpose,
                       std::vector<MapDecorObject>& decor);

// 一次完成地形 + 装饰渲染到画布（调用方提供已建好的装饰列表与画布空间）。
// cells = 场景网格行主序 [cy*cell_w+cx]（transpose 在 scene_cells 提取时完成）。
// light_fn 可选：每格光照等级 0..24（mapview 迷雾/光照演示用），nullptr = 恒 0。
// 返回画布尺寸与原点（bw/bh/ox/oy）；drawn_out = 实际绘制的地形格数。
void render_scene(const std::vector<SceneCell>& cells, int cell_w, int cell_h,
                  const assets::TerrainTileset& tileset, const PaletteLut& terrain_lut,
                  const PaletteLut& resource_lut, const PaletteLut& unit_lut,
                  const std::vector<MapDecorObject>& decor, bool no_decor,
                  const FileLoader& load, cache::CacheManager* cache,
                  const std::function<int(int, int)>* light_fn, const IsometricGrid& grid,
                  int& bw, int& bh, int& ox, int& oy, std::vector<uint8_t>& canvas,
                   int* drawn_out);
// 该地形瓦片是否是"会遮挡对象"的一类（原版语义）：
//   ① 格高 > 0（高台/抬升地形）→ 参与排序，可压住其后对象；
//   ② 带扩展面、ramp == 0（不是坡面）、且位于高度断崖边（邻居高度不同）
//      → 悬崖面（cliff 底线瓦片格高 0 也带整面向上的面，即此类）。
// **坡面（ramp!=0）与平地永远不是遮挡物**——它们整批先画、垫在所有对象之下
//（路缘/斜坡 art 高出格 1~15px，若参与排序会盖住走在路上的单位）。
bool terrain_tile_is_occluder(const SceneCell& cell, const std::vector<SceneCell>& cells,
                              int cell_w, int cell_h, int c, int r,
                              const assets::TerrainTileset& tileset, const FileLoader& load);

// 单块地形瓦片绘制（含解码缓存/高度抬升/光照）。返回是否实际画了像素。
// 供"悬崖/地形高度遮挡"补画复用（见 redraw_terrain_front）。
bool draw_terrain_cell(const SceneCell& cell, int cx, int cy,
                       const assets::TerrainTileset& tileset, const PaletteLut& terrain_lut,
                       const FileLoader& load, cache::CacheManager* cache,
                       const IsometricGrid& grid, int bw, int bh, int ox, int oy, int light_level,
                       std::vector<uint8_t>& canvas);

// 悬崖/地形高度遮挡（参考原版/OpenRA 的画家序：地形按行压在对象上）：
// 在**对象画完之后**，补画它"前方"（base row > 对象前沿行 = cy+fh−1）的高地形
// 瓦片——悬崖面最高 ~76px ≈ 5 行，故取行窗口 [front+1, front+6]、列窗口
// [cx−2, cx+fw+1]。平地瓦片不重叠、补画无害；只有带高度/扩展区的瓦片才画。
void redraw_terrain_front(const std::vector<SceneCell>& cells, int cell_w, int cell_h,
                          const assets::TerrainTileset& tileset, const PaletteLut& terrain_lut,
                          const FileLoader& load, cache::CacheManager* cache,
                          const IsometricGrid& grid, int bw, int bh, int ox, int oy,
                          int obj_cx, int obj_cy, int fw, int fh, int obj_height,
                          std::vector<uint8_t>& canvas);

} // namespace ra2r::render
