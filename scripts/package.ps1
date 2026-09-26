# 生成可分发的便携版(全部产物在工程 dist/ 内,零系统安装)
param(
    [string]$QtBin = "D:\Users\qt\6.8.3\mingw_64\bin",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "dist\Mswrite"

$exe = Join-Path (Join-Path $root $BuildDir) "Mswrite.exe"
if (-not (Test-Path $exe)) {
    Write-Error "请先完成编译:$exe 不存在"
}

# 更新同一个发布目录,保留 MSWriteData、用户文档和 WebView2 缓存。
# 清理旧构建应独立核对,不能在打包时整目录删除。
New-Item -ItemType Directory -Path $out -Force | Out-Null

# 1. 主程序与 WebView2 加载器
Copy-Item $exe $out -Force
Copy-Item "$root\third_party\webview2\WebView2Loader.dll" $out -Force

# 2. 部署 Qt 运行库(windeployqt)
$env:PATH = "$QtBin;$env:PATH"
& "$QtBin\windeployqt.exe" --release --no-translations --compiler-runtime --verbose 0 --qmldir "$root\resources\pdf" "$out\Mswrite.exe"
if ($LASTEXITCODE -ne 0) { Write-Error "windeployqt 失败" }

# 3. 页面资源(编辑器内核/主题)
$webOut = Join-Path $out "resources\web"
New-Item -ItemType Directory -Path $webOut -Force | Out-Null
Copy-Item "$root\resources\web\*" $webOut -Recurse -Force

# Install bundled skills once; later updates preserve the user's edits.
$skillsOut=Join-Path $out 'skills'
foreach($skill in Get-ChildItem -LiteralPath "$root\resources\skills" -Directory) {
    $target=Join-Path $skillsOut $skill.Name
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    $skillFile=Join-Path $target 'SKILL.md'
    if(-not(Test-Path -LiteralPath $skillFile)) { Copy-Item -LiteralPath (Join-Path $skill.FullName 'SKILL.md') -Destination $skillFile }
}

Write-Output ""
Write-Output "打包完成: $out"
Write-Output "双击 $out\Mswrite.exe 即可运行(无需安装;WebView2 运行时用系统自带的)"
