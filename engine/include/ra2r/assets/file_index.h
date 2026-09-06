#pragma once
// RA2R — 全 MIX 树名称索引（M2 从 tools/mapview 提入引擎的共用模块）
//
// 游戏目录下所有 .mix（含嵌套）扁平化为 mix 池；每个已命名条目
// 建立 大写名 → (池序号, 条目 id) 映射。名字来源：各 mix 本地名库
// （enrich_names）+ XCC 官方名库（try_load_xcc_database）。
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "ra2r/assets/mix_file.h"

namespace ra2r::assets {

class FileIndex {
public:
    // 扫描目录下全部 .mix 并递归建索引；error 输出失败原因
    bool build(const std::filesystem::path& dir, std::string* error = nullptr);

    // 按名字读条目（含嵌套，大小写不敏感）；失败返回空
    bool read(const std::string& name, std::vector<uint8_t>& out) const;

    // mix 扁平池（顶层在前，嵌套追加）与名字映射（键为大写）
    std::vector<MixFile> mixes;
    std::map<std::string, std::pair<size_t, uint32_t>> by_name;
};

} // namespace ra2r::assets
