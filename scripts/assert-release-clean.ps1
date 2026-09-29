# Fail closed without ever printing credential values or matching source lines.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [switch]$Release,
    [string]$KnownConfigFile
)
$ErrorActionPreference = 'Stop'
$scanRoot = (Resolve-Path -LiteralPath $Path).Path
$knownSecrets = @()
if ($KnownConfigFile) {
    $config = Get-Content -LiteralPath $KnownConfigFile -Raw | ConvertFrom-Json
    $knownSecrets = @($config.providers | ForEach-Object { $_.apiKey } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
}
$rules = @(
    @{Name='credential-prefix'; Pattern='\b(?:sk-[A-Za-z0-9_-]{16,}|gh[pousr]_[A-Za-z0-9_]{20,}|AIza[A-Za-z0-9_-]{30,})\b'},
    @{Name='jwt'; Pattern='\beyJ[A-Za-z0-9_-]{15,}\.[A-Za-z0-9_-]{15,}\.[A-Za-z0-9_-]{15,}\b'},
    # TLS libraries contain standalone PEM labels. A real key also has a
    # Base64 body and matching closing label; accept raw or JSON-escaped lines.
    @{Name='private-key'; Pattern='-----BEGIN (?<kind>(?:RSA |EC |OPENSSH |ENCRYPTED )?)PRIVATE KEY-----(?:\s|\\r|\\n)+[A-Za-z0-9+/=]{32,}(?:[\sA-Za-z0-9+/=]|\\r|\\n)*-----END \k<kind>PRIVATE KEY-----'},
    @{Name='credential-value'; Pattern='(?i)["''](?:[A-Z_]*(?:API_?KEY|ACCESS_?TOKEN|REFRESH_?TOKEN|AUTH_?TOKEN|CLIENT_?SECRET)|authorization)["'']\s*[:=]\s*["''](?<secret>[^"''\r\n]{8,})["'']'}
)
$issues = [System.Collections.Generic.List[string]]::new()
$files = @(Get-ChildItem -LiteralPath $scanRoot -Recurse -File -Force)
foreach ($file in $files) {
    $relative = $file.FullName.Substring($scanRoot.Length).TrimStart('\','/')
    if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        $issues.Add("$relative : linked file is not a release input")
        continue
    }
    if ($Release -and ($relative -match '(?i)(^|[\\/])(MSWriteData|webview-data|export-tmp|\.ms-agent|\.cc-switch|\.codex)([\\/]|$)' -or
        $file.Name -match '(?i)^(ai-providers.*\.json|providers\.json|ai-session.*\.json|auth\.json|credentials.*|\.env(?:\..*)?|mswrite\.log.*|ai-doctor\.txt|ai-smoke\.txt)$')) {
        $issues.Add("$relative : private runtime file")
    }
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    # Both encodings are needed for source/config files and Windows executables.
    foreach ($encoding in @([Text.Encoding]::UTF8, [Text.Encoding]::Unicode)) {
        $text = $encoding.GetString($bytes)
        foreach ($secret in $knownSecrets) {
            if ($text.Contains($secret)) { $issues.Add("$relative : saved credential match"); break }
        }
        foreach ($rule in $rules) {
            foreach ($match in [regex]::Matches($text, $rule.Pattern)) {
                if ($rule.Name -eq 'credential-value' -and $match.Groups['secret'].Value -match '^(test-only|test-key|fake-key|mock-key|dummy-key|example|placeholder|<[^>]+>)') { continue }
                $issues.Add("$relative : $($rule.Name)")
                break
            }
        }
    }
}
if ($issues.Count) {
    $issues | Select-Object -Unique | ForEach-Object { Write-Output $_ }
    throw 'Credential audit failed. Values have not been printed. Do not distribute these files.'
}
Write-Output "Credential audit passed: $($files.Count) files; no values printed."
