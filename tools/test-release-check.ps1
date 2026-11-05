<#
.SYNOPSIS
  Checks that package-release.ps1 -VerifyOnly accepts a fresh extraction and refuses player data, logs, game files,
  symbols, updater leftovers, debugging tools and development markers. Scratch folders only; nothing is built,
  launched or published.
#>
$ErrorActionPreference = 'Stop'
$ReleaseCheck = Join-Path $PSScriptRoot 'package-release.ps1'
$StageRoot = Join-Path ([IO.Path]::GetTempPath()) 'tmnt_release_check'
$Cases = [ordered]@{
    'fresh extraction'   = $null
    'empty user_data'    = 'user_data\'
    'scores'             = 'user_data\584109D2\live\stats.toml'
    'achievements'       = 'user_data\achievements\584109D2.toml'
    'settings'           = 'user_data\settings.toml'
    'cache'              = 'cache\shaders\a.bin'
    'migration leftover' = 'user_data.migrating\'
    'log'                = 'logs\tmnt_turtles_in_time_reshelled_001.log'
    'game file'          = 'game\default.xex'
    'updater pdb'        = 'trg-updater.pdb'
    'debug file'         = 'trg-updater.debug'
    'updater workspace'  = 'trg-update-1234-5678\job.txt'
    'updater log'        = 'trg-updater.log'
    'editor backup'      = 'README.txt~'
    'settings leftover'  = 'settings.toml.invalid'
    'disc image'         = 'game.iso'
    'renderdoc'          = 'renderdoc.dll'
    'vulkan layer'       = 'VkLayer_test.json'
    'vulkan loader'      = 'vulkan-1.dll'
    'Xbox package'       = 'rexruntime.dll'
    'Xbox executable'    = 'rexgpu-xenos.dll'
    'DEBUG_SAULO marker' = 'README.txt'
    'repository path'    = 'tmnt_turtles_in_time_reshelled.toml'
    'user profile path'  = 'tmnt_turtles_in_time_reshelled.toml'
    'other user profile' = 'tmnt_turtles_in_time_reshelled.toml'
    'repos path'         = 'tmnt_turtles_in_time_reshelled.toml'
    'msys2 path'         = 'tmnt_turtles_in_time_reshelled.toml'
    'UTF-16 user path'   = 'tmnt_turtles_in_time_reshelled.toml'
    'UTF-16 marker'      = 'README.txt'
    'Tracy client'       = 'TracyClient.dll'
    'Python cache'       = 'assets\launcher\x.pyc'
    'mixed-case xex'     = 'Default.XEX'
    'nested xex'         = 'assets\launcher\module.xex'
    'nested save'        = 'user_data\x.sav'
}
$Content = @{
    'Xbox package'       = 'LIVE'
    'Xbox executable'    = 'XEX2'
    'DEBUG_SAULO marker' = 'DEBUG_SAULO'
    'repository path'    = "cache_root = '$(Split-Path $PSScriptRoot -Parent)'"
    'user profile path'  = "cache_root = '$env:USERPROFILE'"
    'other user profile' = "cache_root = 'D:\Users\Somebody\Documents'"
    'repos path'         = "cache_root = 'E:\VisualStudio\repos\x'"
    'msys2 path'         = "cache_root = 'C:\msys64\clang64'"
}
$Bytes = @{
    'UTF-16 user path' = [Text.Encoding]::Unicode.GetBytes("cache_root = 'F:\Users\Somebody\Games'")
    'UTF-16 marker'    = [Text.Encoding]::Unicode.GetBytes('DEBUG_SAULO')
}
$failures = 0
try {
    foreach ($case in $Cases.Keys) {
        $stage = Join-Path $StageRoot ($case -replace '\W', '_')
        New-Item -ItemType Directory -Force (Join-Path $stage 'licenses') | Out-Null
        foreach ($leaf in 'tmnt_turtles_in_time_reshelled.exe', 'tmnt_turtles_in_time_reshelled_TMNT_r.dll', 'trg-updater.exe', 'rexruntime.dll', 'rexgpu-xenos.dll', 'tmnt_turtles_in_time_reshelled.toml', 'README.txt', 'LICENSE',
            'licenses\ReXGlue-LICENSE.txt', 'licenses\THIRD-PARTY-LICENSES.txt', 'assets\launcher\header-art.png', 'assets\launcher\sponsor-heart.png') {
            $file = Join-Path $stage $leaf
            New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
            Set-Content -LiteralPath $file -Value 'placeholder'
        }
        $extra = $Cases[$case]
        if ($extra) {
            $target = Join-Path $stage $extra
            if ($extra.EndsWith('\')) {
                New-Item -ItemType Directory -Force $target | Out-Null
            } else {
                New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
                if ($Bytes.Contains($case)) {
                    [IO.File]::WriteAllBytes($target, $Bytes[$case])
                } else {
                    $text = if ($Content.Contains($case)) { $Content[$case] } else { 'placeholder' }
                    Set-Content -LiteralPath $target -Value $text -NoNewline
                }
            }
        }
        $global:LASTEXITCODE = 0
        & $ReleaseCheck -VerifyOnly $stage *> $null
        $accepted = $LASTEXITCODE -eq 0
        if ($accepted -ne ($null -eq $extra)) {
            Write-Host "  FAIL $case"
            $failures++
        }
    }
} finally {
    Remove-Item -LiteralPath $StageRoot -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "$($Cases.Count) release check cases, $failures failures"
if ($failures) { throw 'Release check tests failed' }
$global:LASTEXITCODE = 0
