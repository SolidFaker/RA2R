# RA2R 开源参考获取脚本
# 用途：克隆/下载只供本地查阅的开源参考（GPL 等许可代码，不入库）。
# 目标目录 third_party/reference/（已在 .gitignore 中）。
# 需要本地 SOCKS5 代理（127.0.0.1:10808）访问 GitHub；如网络环境不同请修改 $proxy。

$ErrorActionPreference = 'Stop'
$proxy = 'socks5h://127.0.0.1:10808'
$dest = Join-Path $PSScriptRoot '..\third_party\reference'
New-Item -ItemType Directory -Force -Path $dest | Out-Null

# 1. 代码参考仓库（git clone，浅克隆）
$repos = @(
  @{ name = 'chronoshift';   url = 'https://github.com/TheAssemblyArmada/Chronoshift' },
  @{ name = 'yuris-revenge'; url = 'https://github.com/cookgreen/Yuris-Revenge' },
  @{ name = 'ra2-remixer';   url = 'https://github.com/rust-alert/ra2-remixer' }
)
foreach ($r in $repos) {
  $target = Join-Path $dest $r.name
  if (Test-Path $target) {
    Write-Host "[跳过] $($r.name) 已存在"
    continue
  }
  Write-Host "[克隆] $($r.name) ..."
  git -c http.proxy=$proxy clone --depth 1 -q $r.url $target
}

# 2. 单文件参考（curl 拉取）
$files = @(
  @{ name = 'LZOCompression.cs'; url = 'https://raw.githubusercontent.com/OpenRA/OpenRA/bleed/OpenRA.Mods.Cnc/FileFormats/LZOCompression.cs' },
  @{ name = 'SDL_dialog.c';      url = 'https://raw.githubusercontent.com/libsdl-org/SDL/release-3.4.14/src/dialog/SDL_dialog.c' },
  @{ name = 'SDL_dialog_utils.c'; url = 'https://raw.githubusercontent.com/libsdl-org/SDL/release-3.4.14/src/dialog/SDL_dialog_utils.c' }
)
foreach ($f in $files) {
  $target = Join-Path $dest $f.name
  if (Test-Path $target) {
    Write-Host "[跳过] $($f.name) 已存在"
    continue
  }
  Write-Host "[下载] $($f.name) ..."
  curl.exe -x $proxy -L -sS -o $target $f.url
}

# 3. XCC 官方名库（global mix database.dat，随 XCC Utilities 分发的社区名库）
#    大幅提升 mixbrowser/assetcheck 的条目名覆盖率（工具自动查找本目录）。
$db = Join-Path $dest 'global mix database.dat'
if (-not (Test-Path $db)) {
  Write-Host "[下载] XCC Utilities（内含官方名库）..."
  $tmp = Join-Path $env:TEMP 'XCC_Utilities.exe'
  curl.exe -x $proxy -L -sS -o $tmp 'https://xhp.xwis.net/utilities/XCC_Utilities.exe'
  $extract = Join-Path $env:TEMP 'xccutil_extract'
  New-Item -ItemType Directory -Force -Path $extract | Out-Null
  & 7z x $tmp "-o$extract" -y | Out-Null
  Copy-Item (Join-Path $extract 'global mix database.dat') $db -ErrorAction Stop
  Write-Host "名库已保存: $db"
} else {
  Write-Host "[跳过] 名库已存在"
}

Write-Host "完成。参考目录: $dest"
