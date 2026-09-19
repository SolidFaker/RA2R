#!/bin/bash
# 对 engine/（src+include，排除 third_party）跑 clang-tidy，汇总检查类别。
# CI 门禁：--warnings-as-errors 下必须零告警。
# 用法：在仓库根目录执行 tools/ci/tidy.sh（脚本自行定位仓库根；需 build-ct/compile_commands.json）
set -u
cd "$(dirname "$0")/../.." || exit 1

CHECKS='-*,bugprone-*,clang-analyzer-*,performance-*,portability-*'

# 需要排除的检查（噪声/误报/与本项目约定冲突）；旧版 clang-tidy 不认识的就跳过，
# 避免 "unknown check" 直接报错（该检查在旧版本来也不存在）。
KNOWN_CHECKS="$(clang-tidy --list-checks -checks='*' 2>/dev/null | sed 's/^[[:space:]]*//' || true)"
excl() {
    if [ -n "$KNOWN_CHECKS" ] && ! grep -qx "$1" <<<"$KNOWN_CHECKS"; then
        return
    fi
    CHECKS+=',-'"$1"
}
excl bugprone-easily-swappable-parameters                  # 参数顺序误用噪声过大
excl bugprone-narrowing-conversions                        # 与 -Wconversion 重复
excl bugprone-reserved-identifier                          # 与 SDL/系统头交互误报
excl bugprone-signed-char-misuse                           # 字节流有意用 char
excl portability-avoid-pragma-once                         # 本项目统一 #pragma once
excl performance-enum-size                                 # 枚举底层类型不值得改
excl bugprone-implicit-widening-of-multiplication-result   # 尺寸运算噪声大
excl bugprone-unchecked-string-to-number-conversion        # 地图解析有意用 atoi
excl clang-analyzer-optin.performance.Padding              # 性能提示，非安全性

: > /tmp/tidy.log
while IFS= read -r f; do
    clang-tidy -p build-ct --checks="$CHECKS" --quiet \
        --header-filter="$(pwd)/engine/(include|src)/" \
        --warnings-as-errors='bugprone-*,clang-analyzer-*,performance-*' \
        "$f" >> /tmp/tidy.log 2>&1
done < <(find engine/src -name '*.cpp' | sort)
if grep -qE 'warning:|error:' /tmp/tidy.log; then
    echo "=== clang-tidy 告警/错误 ==="
    grep -E 'warning:|error:' /tmp/tidy.log | sed "s|$(pwd)/||" | sort -u
    exit 1
fi
echo "clang-tidy 通过（engine/，零告警）"
