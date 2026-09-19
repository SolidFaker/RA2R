// RA2R — 剧场配置表实现（接口见 theater.h）
#include "ra2r/assets/theater.h"

#include <algorithm>
#include <cctype>

namespace ra2r::assets {

TheaterConfig theater_config(const std::string& theater) {
    std::string u = theater;
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (u == "SNOW") return {"sno", "SNOWMD.INI", "ISOSNO.PAL", "UNITSNO.PAL"};
    if (u == "URBAN") return {"urb", "URBANMD.INI", "ISOURB.PAL", "UNITURB.PAL"};
    if (u == "NEWURBAN") return {"ubn", "URBANNMD.INI", "ISOUBN.PAL", "UNITUBN.PAL"};
    if (u == "DESERT") return {"des", "DESERTMD.INI", "ISODES.PAL", "UNITDES.PAL"};
    if (u == "LUNAR") return {"lun", "LUNARMD.INI", "ISOLUN.PAL", "UNITLUN.PAL"};
    return {"tem", "TEMPERATMD.INI", "ISOTEM.PAL", "UNITTEM.PAL"};
}

char theater_code(const std::string& theater) {
    std::string u = theater;
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (u == "SNOW") return 'A';
    if (u == "URBAN") return 'U';
    if (u == "DESERT") return 'D';
    if (u == "LUNAR") return 'L';
    if (u == "NEWURBAN") return 'N';
    return 'T'; // TEMPERATE（及未知）
}

} // namespace ra2r::assets
