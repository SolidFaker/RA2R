#!/bin/bash
# cppcheck 静态分析（engine/src；排除 third_party 与 vendored 头）。
# CI 门禁：发现 warning/performance/portability 问题即退出码非 0。
# 用法：在仓库根目录执行 tools/ci/cppcheck.sh（脚本自行定位仓库根）
set -u
cd "$(dirname "$0")/../.." || exit 1
cppcheck --std=c++20 --enable=warning,performance,portability --inline-suppr \
    --suppress=missingIncludeSystem --suppress=unusedFunction \
    --suppress='*:*third_party/*' \
    --error-exitcode=1 -I engine/include engine/src 2>&1 | tee /tmp/cppcheck.log | grep -vE '^Checking|^[0-9]+/[0-9]+ files'
status=${PIPESTATUS[0]}
if [ "$status" -ne 0 ]; then
    echo "=== cppcheck 告警 ==="
    grep -E '^\S+\.(cpp|h):[0-9]+:[0-9]+:' /tmp/cppcheck.log | sed "s|$(pwd)/||" | sort -u
    echo "cppcheck 失败（退出码 $status）"
    exit 1
fi
echo "cppcheck 通过（engine/src，零告警）"
