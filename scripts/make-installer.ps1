# 一键生成安装包:刷新 dist → 准备干净暂存目录 → Inno Setup 编译
# 用法: powershell -ExecutionPolicy Bypass -File scripts\make-installer.ps1
param(
    [string]$QtBin   = "D:\Users\qt\6.8.3\mingw_64\bin",
    [string]$BuildDir = "build",
    [string]$Iscc    = "D:\Applications\innosetup\Inno Setup 6\ISCC.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# 1) 刷新便携发布目录(exe + windeployqt + resources/skills)
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "package.ps1") -QtBin $QtBin -BuildDir $BuildDir
if ($LASTEXITCODE -ne 0) { throw "package.ps1 失败" }

# 2) 暂存目录:整目录复制,剔除运行期产物(用户数据/缓存/日志/临时导出)
$src   = Join-Path $root "dist\Mswrite"
$stage = Join-Path $root "dist\_stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
robocopy $src $stage /E /XD webview-data export-tmp MSWriteData /XF mswrite.log /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy 失败:$LASTEXITCODE" }

$files = (Get-ChildItem $stage -Recurse -File | Measure-Object).Count
$size  = "{0:N1} MB" -f ((Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
Write-Output "暂存目录就绪:$stage($files 个文件,$size)"

# 3) 编译安装包
if (-not (Test-Path $Iscc)) { throw "找不到 ISCC.exe:$Iscc" }
& $Iscc (Join-Path $root "installer\Mswrite.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC 编译失败:$LASTEXITCODE" }

$setup = Get-ChildItem (Join-Path $root "dist") -Filter "*-setup.exe" |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Output ""
Write-Output ("安装包完成:{0}({1:N1} MB)" -f $setup.FullName, ($setup.Length / 1MB))
