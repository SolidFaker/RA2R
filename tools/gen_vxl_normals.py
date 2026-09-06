#!/usr/bin/env python3
"""从 docs/formats/vxl.md 的 RA2 法线表章节生成 engine/src/assets/vxl_normals.inc。
表为社区公开数据（来源：OpenRA VoxelNormalsPalette.cs，原始出处 sleipnirstuff 论坛 t=8048）。
"""
import os
import re

DOC = os.path.join(os.path.dirname(__file__), "..", "docs", "formats", "vxl.md")
OUT = os.path.join(os.path.dirname(__file__), "..", "engine", "src", "assets", "vxl_normals.inc")

def main():
    text = open(DOC, encoding="utf-8").read()
    # RA2 表（4.1 与 4.2 之间）
    start = text.index("### 4.1")
    end = text.index("### 4.2")
    seg = text[start:end]
    pat = re.compile(r"^\s*(\d+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s*$", re.M)
    ra2 = []
    for m in pat.finditer(seg):
        idx = int(m.group(1))
        if idx > 243:
            break
        ra2.append((float(m.group(2)), float(m.group(3)), float(m.group(4))))
    assert len(ra2) == 244, len(ra2)
    # TS 表（4.2 与 ## 5 之间）
    seg2 = text[end:text.index("## 5")]
    ts = []
    for m in pat.finditer(seg2):
        ts.append((float(m.group(2)), float(m.group(3)), float(m.group(4))))
    assert len(ts) == 36, len(ts)
    with open(OUT, "w") as f:
        f.write("// 自动生成：tools/gen_vxl_normals.py（数据源 docs/formats/vxl.md §4.1/§4.2，\n")
        f.write("// 社区公开法线表，出处 OpenRA VoxelNormalsPalette.cs / sleipnirstuff t=8048）。\n")
        f.write("namespace {\n")
        f.write("constexpr float kVxlNormalsRA2[244][3] = {\n")
        for x, y, z in ra2:
            f.write("    {%ff, %ff, %ff},\n" % (x, y, z))
        f.write("};\n")
        f.write("constexpr float kVxlNormalsTS[36][3] = {\n")
        for x, y, z in ts:
            f.write("    {%ff, %ff, %ff},\n" % (x, y, z))
        f.write("};\n")
        f.write("} // namespace\n")
    print("generated", OUT, "ra2:", len(ra2), "ts:", len(ts))


if __name__ == "__main__":
    main()
