#!/usr/bin/env python3
"""用 BBP 公式独立计算 π 的前 8336 个十六进制位，
生成 engine/src/crypto/pi_tables.inc（Blowfish 的 P 数组与 S 盒）。

8336 = 18*8 (P) + 1024*8 (S)。输出与规范首值交叉验证：
  P[0] == 0x243F6A88, S[0][0] == 0xD1310BA6
"""
import os

TOTAL_HEX = 18 * 8 + 1024 * 8          # 8336
MARGIN = 64                            # BBP 截断误差余量
OUT = os.path.join(os.path.dirname(__file__), "..", "engine", "src", "crypto", "pi_tables.inc")


def pi_hex_digits(count):
    """返回 pi 的前 count 个十六进制数字（字符串），BBP 整数算法。"""
    n = count + MARGIN
    # floor(16^n * pi) 的整数逼近（BBP 级数，分子先乘幂再做整除）：
    #   pi = sum_k 16^-k * (4/(8k+1) - 2/(8k+4) - 1/(8k+5) - 1/(8k+6))
    total = 0
    for k in range(n + 1):
        p = 4 * (n - k)
        total += ((4 << p) // (8 * k + 1) - (2 << p) // (8 * k + 4)
                  - (1 << p) // (8 * k + 5) - (1 << p) // (8 * k + 6))
    digits = []
    x = total
    for _ in range(n):
        digits.append("0123456789ABCDEF"[x >> (4 * (n - 1)) & 0xF])
        x = (x << 4) & ((1 << (4 * n)) - 1)
    return "".join(digits[:count])


def main():
    digits = pi_hex_digits(TOTAL_HEX + 8)
    # 前 8 个十六进制位是 "243F6A88"（整数部分 3 由 "3.243F6A88..." 表示；
    # BBP 直接给出小数部分，故 digits[0:8] == "243F6A88"）
    assert digits[:8] == "243F6A88", digits[:8]

    def words(hexstr):
        return [int(hexstr[i:i + 8], 16) for i in range(0, len(hexstr), 8)]

    p = words(digits[: 18 * 8])
    s = [words(digits[18 * 8 + i * 2048: 18 * 8 + (i + 1) * 2048]) for i in range(4)]
    assert p[0] == 0x243F6A88 and s[0][0] == 0xD1310BA6

    with open(OUT, "w") as f:
        f.write("// 自动生成：tools/gen_pi_tables.py（BBP 公式计算 π 的十六进制位）。\n")
        f.write("// P 数组与 S 盒 = π 前 8336 个十六进制位，勿手改。\n")
        f.write("namespace {\n")
        f.write("constexpr uint32_t kPiP[18] = {\n")
        for i in range(0, 18, 4):
            f.write("    " + ", ".join("0x%08X" % v for v in p[i:i + 4]) + ",\n")
        f.write("};\n")
        f.write("constexpr uint32_t kPiS[4][256] = {\n")
        for box in s:
            f.write("    {\n")
            for i in range(0, 256, 4):
                f.write("        " + ", ".join("0x%08X" % v for v in box[i:i + 4]) + ",\n")
            f.write("    },\n")
        f.write("};\n")
        f.write("} // namespace\n")
    print("generated", OUT)


if __name__ == "__main__":
    main()
