#pragma once
// RA2R — 从规则 INI 推导资产名称库（游戏 MIX 内无名字库时的名称来源）
#include <cstdint>
#include <string>
#include <vector>

#include "ra2r/assets/mix_file.h"

namespace ra2r::assets {

// 从规则 INI 文本提取候选资产名（节名 + 资源引用键值，如 Image=/Cameo=/Voxel=）
std::vector<std::string> collect_names_from_ini(const uint8_t* data, size_t size);

// 扫描 mix 内的 rules/art/ai 系 INI，把命中条目的 id→名称 填入 mix 名称库。
// 这是 RA2/YR 资产树（local.mix / localmd.mix 等）的主要命名来源，
// 与游戏本身的"Image=XXX → XXX.SHP/VXL"命名规则一致。
void enrich_names(MixFile& mix);

// 加载 XCC "global mix database.dat"（官方社区名库，TD/RA/TS/RA2 四段，
// 每段 [i32 count][name\0][desc\0]×count）。返回成功加载的名字数（仅 RA2 段计入）。
// 数据文件获取：tools/fetch_references.ps1（XCC Utilities 包内，GPL 数据随包分发，
// 不入本仓库，工具在 third_party/reference/ 下查找）。
size_t load_global_names_from_xcc(MixFile& mix, const uint8_t* data, size_t size);

// 在常见位置查找 XCC 名库并加载（CWD、third_party/reference/）。找到返回 true。
bool try_load_xcc_database(MixFile& mix, std::string* loaded_from = nullptr);

} // namespace ra2r::assets
