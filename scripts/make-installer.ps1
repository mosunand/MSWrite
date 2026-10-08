# 一键生成安装包:检查源码 → 从构建/源码组装新目录 → 检查凭据 → Inno Setup 编译
# 用法: powershell -ExecutionPolicy Bypass -File scripts\make-installer.ps1
param(
    [string]$QtBin   = "D:\Users\qt\6.8.3\mingw_64\bin",
    [string]$BuildDir = "build",
    [string]$Iscc    = "D:\Applications\innosetup\Inno Setup 6\ISCC.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# 1) Audit source and assemble a fresh stage directly from build/source inputs.
# Never use dist/Mswrite or another installation as an installer source.
foreach ($sourceDir in @('src','resources','scripts','installer')) {
    & (Join-Path $PSScriptRoot 'assert-release-clean.ps1') -Path (Join-Path $root $sourceDir)
}
$stage = Join-Path $root ("build\installer-stage-" + [Guid]::NewGuid().ToString('N'))
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "package.ps1") -QtBin $QtBin -BuildDir $BuildDir -OutputDir $stage
if ($LASTEXITCODE -ne 0) { throw "package.ps1 失败" }

# 2) Refuse private runtime files and literal credentials before compression.
& (Join-Path $PSScriptRoot 'assert-release-clean.ps1') -Path $stage -Release

$files = (Get-ChildItem $stage -Recurse -File | Measure-Object).Count
$size  = "{0:N1} MB" -f ((Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
Write-Output "暂存目录就绪:$stage($files 个文件,$size)"

# 3) 编译安装包
if (-not (Test-Path $Iscc)) { throw "找不到 ISCC.exe:$Iscc" }
& $Iscc /Qp "/DStageDir=$stage" (Join-Path $root "installer\Mswrite.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC 编译失败:$LASTEXITCODE" }

$setup = Get-Item -LiteralPath (Join-Path $root 'dist\setup.exe')
Write-Output ""
Write-Output ("安装包完成:{0}({1:N1} MB)" -f $setup.FullName, ($setup.Length / 1MB))

# 保留便携版及已有数据/备份。每次构建使用独立暂存目录,不递归删除目录。
Write-Output "验证用暂存目录已保留:$stage；已有便携版未修改"
