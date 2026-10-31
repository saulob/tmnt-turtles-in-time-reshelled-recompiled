<#
.SYNOPSIS
  Checks that Reset-RuntimeSource (tools/rexruntime-patch.ps1) makes the patch series apply
  exactly once regardless of what a previous build or validation left in the SDK source tree.
  Works on a scratch clone of the already-fetched SDK commit (no network); nothing is built.
#>
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rexruntime-patch.ps1')
$Baseline = Read-RuntimeBaseline
$Root = Split-Path $PSScriptRoot -Parent
$Sdk = Join-Path $Root 'out\rexglue-sdk-src'
if (-not (Test-Path (Join-Path $Sdk '.git'))) { throw "Missing $Sdk (run tools\codegen.ps1 or tools\build-patched-rexruntime.ps1 first)" }

$Src = Join-Path ([IO.Path]::GetTempPath()) 'tmnt_rexruntime_source_reset_test'
Remove-Item -LiteralPath $Src -Recurse -Force -ErrorAction SilentlyContinue
Invoke-Native 'git clone' { git clone --quiet $Sdk $Src }
$commit = "$(git -C $Src rev-parse HEAD)".Trim()
if ($commit -ne $Baseline.SDK_COMMIT) { throw "scratch clone is at $commit; expected $($Baseline.SDK_COMMIT)" }

$failures = 0
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { $script:failures++; Write-Host "  FAIL $Message" -ForegroundColor Red }
}
function Get-NormalizedText([string]$Text) { $Text.Replace("`r`n", "`n") }

function Apply-AllPatches {
    foreach ($patch in (Get-RuntimePatchFiles)) {
        & git -C $Src apply --check $patch.FullName
        if ($LASTEXITCODE) { throw "$($patch.Name) does not apply" }
        Invoke-Native "git apply $($patch.Name)" { git -C $Src apply $patch.FullName }
    }
}

$NewFile1 = 'include/rex/graphics/d3d12/aa838_stage_fix.h'
$NewFile2 = 'src/graphics/d3d12/aa838_stage_fix.cpp'
# A file 0080 modifies but does not create, to prove the reset restores untouched baseline content too.
$BaselineTouchedFile = 'include/rex/graphics/flags.h'
$baselineFlagsH = Get-NormalizedText (((git -C $Src show "HEAD:$BaselineTouchedFile") -join "`n") + "`n")

Write-Host 'pristine baseline'
Check (-not (Test-Path (Join-Path $Src $NewFile1))) "$NewFile1 exists before any patch"
Check (-not (Test-Path (Join-Path $Src $NewFile2))) "$NewFile2 exists before any patch"

Write-Host 'apply current patches once'
Apply-AllPatches
Check (Test-Path (Join-Path $Src $NewFile1)) "$NewFile1 missing after applying patches"
Check (Test-Path (Join-Path $Src $NewFile2)) "$NewFile2 missing after applying patches"

Write-Host 'leave patched source in place, as an interrupted or previously validated build would'
# A stray `git add` (exactly what a prior diff/validation left behind) must not survive the reset.
Invoke-Native 'git add -N' { git -C $Src add -N $NewFile1 $NewFile2 }

Write-Host 'trigger rebuild again: Reset-RuntimeSource restores the baseline'
Reset-RuntimeSource $Src
Check (-not (Test-Path (Join-Path $Src $NewFile1))) "$NewFile1 survived Reset-RuntimeSource"
Check (-not (Test-Path (Join-Path $Src $NewFile2))) "$NewFile2 survived Reset-RuntimeSource"
Check (-not (git -C $Src status --short)) 'git status is not clean right after Reset-RuntimeSource'
$restoredFlagsH = Get-NormalizedText (Get-Content -LiteralPath (Join-Path $Src $BaselineTouchedFile) -Raw)
Check ($restoredFlagsH -eq $baselineFlagsH) "$BaselineTouchedFile does not match the pinned baseline after reset"

Write-Host 'current patches apply exactly once without conflict, from every old patch to the new ones'
Apply-AllPatches
Check (Test-Path (Join-Path $Src $NewFile1)) "$NewFile1 missing after reapplying patches"
Check (Test-Path (Join-Path $Src $NewFile2)) "$NewFile2 missing after reapplying patches"
$oldDiagnosticFiles = @('src/graphics/d3d12/debug_saulo_flicker.cpp', 'include/rex/graphics/d3d12/debug_saulo_flicker.h',
                        'src/ui/debug_saulo_flicker_live.cpp', 'include/rex/ui/debug_saulo_flicker_live.h')
foreach ($file in $oldDiagnosticFiles) {
    Check (-not (Test-Path (Join-Path $Src $file))) "removed patch's file $file is back"
}

Write-Host 'a patch applying to a source tree that already has it is refused, not silently skipped'
$patch0080 = (Get-RuntimePatchFiles | Where-Object { $_.Name -like '0080-*' }).FullName
$appliedTwice = $true
try { & git -C $Src apply --check $patch0080 2>$null; $appliedTwice = ($LASTEXITCODE -eq 0) }
catch { $appliedTwice = $false }
Check (-not $appliedTwice) '0080 should no longer apply a second time on top of itself'

Write-Host '0081 still applies after 0080'
Check (Test-Path (Join-Path $Src 'src/graphics/pipeline/shader/translator.cpp')) 'translator.cpp missing (0081 target)'

Remove-Item -LiteralPath $Src -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "$(if ($failures) { 'FAILED' } else { 'PASSED' }), $failures failure(s)"
if ($failures) { exit 1 }
