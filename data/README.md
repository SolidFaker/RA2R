# RA2R 数据文件目录

本目录存放**不入库**的本地数据文件（经 `tools/fetch_references.ps1` 获取）。

## global mix database.dat（不随仓库分发）

XCC 官方社区名库（RA2 段 14,386 个文件名 → MIX CRC 映射）。

- 来源：XCC Utilities 包（Olaf van der Spek，https://xhp.xwis.net/utilities/XCC_Utilities.exe）
- 格式：TD/RA/TS/RA2 四段，每段 `[i32 count][name\0][desc\0]×count`
- 用途：mixbrowser / assetcheck 的条目名解析（工具自动搜索本目录与 `third_party/reference/`）
- 获取：`powershell -File tools/fetch_references.ps1`（自动下载 XCC Utilities 并解出本文件）

> 该文件是社区整理的 RA2 资产文件名列表（事实数据）。重新分发遵循 XCC Utilities
> 的 GPLv3 许可，故不与 MIT 代码同库分发；本项目代码本身为 MIT。
