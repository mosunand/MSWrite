# Keep the vendored runtime safe even when its ignored dist files are refreshed.
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskPatches = @(
    @{
        Path = 'resources/web/vditor/dist/index.js'
        Unsafe = 'return Function("\"use strict\";return (".concat(text, ")"))();'
        Safe = 'return JSON.parse(text);'
    },
    @{
        Path = 'resources/web/vditor/dist/index.min.js'
        Unsafe = 'o=function(e){return Function(''"use strict";return (''.concat(e,")"))()}'
        Safe = 'o=function(e){return JSON.parse(e)}'
    },
    @{
        Path = 'resources/web/vditor/dist/method.min.js'
        Unsafe = 'i=function(e){return Function(''"use strict";return (''.concat(e,")"))()}'
        Safe = 'i=function(e){return JSON.parse(e)}'
    }
)
foreach ($taskPatch in $taskPatches) {
    $taskPath = Join-Path $taskRoot $taskPatch.Path
    $taskSource = [IO.File]::ReadAllText($taskPath)
    if ($taskSource.Contains($taskPatch.Unsafe)) {
        $taskSource = $taskSource.Replace($taskPatch.Unsafe, $taskPatch.Safe)
        [IO.File]::WriteAllText($taskPath, $taskSource, [Text.UTF8Encoding]::new($false))
    } elseif (-not $taskSource.Contains($taskPatch.Safe)) {
        throw "Unknown diagram parser in $taskPath; review it before building a release."
    }
}
