<#
.SYNOPSIS
  Builds tools\xex-dump\dump-xex-image.cpp and runs it on game\extracted\default.xex and
  TMNT_r.xex, writing game\analysis\default.image.bin and tmnt_r.image.bin for
  tools\find-missing-functions.py. Development tool: not part of the game or launcher build.
#>
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
$Src = Join-Path $PSScriptRoot 'xex-dump'
$Out = Join-Path $Root 'game\analysis'
New-Item -ItemType Directory -Force $Out | Out-Null

$VsRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community'
$VsDevCmd = Join-Path $VsRoot 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path $VsDevCmd)) { throw "Visual Studio not found at $VsRoot" }
$env:PATH = "$env:PATH;$(Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer')"
$vsEnv = cmd.exe /c "`"$VsDevCmd`" -arch=x64 -host_arch=x64 -no_logo >nul && set"
foreach ($line in $vsEnv) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] } }
$LlvmBin = Join-Path $VsRoot 'VC\Tools\Llvm\x64\bin'
$Clang = Join-Path $LlvmBin 'clang.exe'
$ClangXX = Join-Path $LlvmBin 'clang++.exe'

$Exe = Join-Path $Out 'dump-xex-image.exe'
$Stamp = Join-Path $Out '.dump-xex-image.stamp'
$Inputs = @(Get-Item (Join-Path $Src 'dump-xex-image.cpp')) + (Get-ChildItem $Src -Filter *.c -Recurse) +
    (Get-ChildItem $Src -Filter *.h -Recurse) + (Get-ChildItem $Src -Filter *.hpp -Recurse)
$fingerprint = ($Inputs | Sort-Object FullName | ForEach-Object { "$($_.FullName)=$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)" }) -join "`n"
$built = (Test-Path $Exe) -and (Test-Path $Stamp) -and ((Get-Content -LiteralPath $Stamp -Raw) -eq $fingerprint)
if (-not $built) {
    Write-Host 'Building dump-xex-image.exe...'
    Push-Location $Out
    try {
        & $Clang -std=c11 -O2 -w -I $Src -I (Join-Path $Src 'mspack') -c (Join-Path $Src 'rijndael-alg-fst.c') -o rijndael.obj
        if ($LASTEXITCODE) { throw 'rijndael-alg-fst.c failed to compile' }
        & $Clang -std=c11 -O2 -w -I (Join-Path $Src 'mspack') -c (Join-Path $Src 'mspack\lzxd.c') -o lzxd.obj
        if ($LASTEXITCODE) { throw 'lzxd.c failed to compile' }
        & $ClangXX -std=c++20 -O2 -w -I $Src -I (Join-Path $Src 'mspack') -c (Join-Path $Src 'dump-xex-image.cpp') -o main.obj
        if ($LASTEXITCODE) { throw 'dump-xex-image.cpp failed to compile' }
        & $ClangXX main.obj rijndael.obj lzxd.obj -o $Exe
        if ($LASTEXITCODE) { throw 'link failed' }
    } finally { Pop-Location }
    Set-Content -LiteralPath $Stamp -Value $fingerprint -NoNewline
}

foreach ($name in @('default.xex', 'TMNT_r.xex')) {
    $xex = Join-Path $Root "game\extracted\$name"
    if (-not (Test-Path $xex)) { throw "Missing $xex (extract the XBLA package into game\extracted first)" }
}
& $Exe (Join-Path $Root 'game\extracted\default.xex') (Join-Path $Out 'default.image.bin')
if ($LASTEXITCODE) { throw 'dump-xex-image failed on default.xex' }
& $Exe (Join-Path $Root 'game\extracted\TMNT_r.xex') (Join-Path $Out 'tmnt_r.image.bin')
if ($LASTEXITCODE) { throw 'dump-xex-image failed on TMNT_r.xex' }
Write-Host "Wrote $Out\default.image.bin and $Out\tmnt_r.image.bin"
