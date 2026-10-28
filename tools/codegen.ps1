<#
.SYNOPSIS
  Downloads the pinned ReXGlue SDK, checks game\extracted\default.xex and TMNT_r.xex and
  generates the recompiled C++ sources into generated\default and generated	mnt_r.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$sdkVersion = '0.10.0'
$sdkZipSha256 = 'AD4485DA853FBE932E9F5DBB643F9B5FE85DC73775EBBAD4DF6879FE6DF0C11B'
$xexSha256 = @{
    'default.xex' = '5FF5B37E120B1F2C3057D78BFC8C8AD6885C3FA576AECB3026E23E6044C5C3DE'
    'TMNT_r.xex'  = 'C6C19BD1D3DE0C0AF3651DE201E8CAC47D9C380C03060734F6F649E83EE89DD9'
}

$sdkDir = Join-Path $root 'tools\rexglue'
$rexglue = Join-Path $sdkDir 'win-amd64\bin\rexglue.exe'
$gameDir = Join-Path $root 'game\extracted'
$log = Join-Path $root 'generated\codegen.log'

if (-not (Test-Path $rexglue)) {
    $zip = Join-Path $sdkDir "rexglue-sdk-$sdkVersion-win-amd64.zip"
    if (-not (Test-Path $zip)) {
        New-Item -ItemType Directory -Force $sdkDir | Out-Null
        $url = "https://github.com/rexglue/rexglue-sdk/releases/download/v$sdkVersion/rexglue-sdk-$sdkVersion-win-amd64.zip"
        Write-Host "Downloading ReXGlue SDK $sdkVersion..."
        Invoke-WebRequest -Uri $url -OutFile $zip
    }
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $sdkZipSha256) { throw "$zip is not the official ReXGlue $sdkVersion release" }
    Expand-Archive -Path $zip -DestinationPath $sdkDir -Force
}
$installed = "$(& $rexglue --version)".Trim()
if ($installed -ne $sdkVersion) { throw "tools\rexglue has ReXGlue $installed; expected $sdkVersion (delete tools\rexglue to re-download)" }

foreach ($name in $xexSha256.Keys) {
    $xex = Join-Path $gameDir $name
    if (-not (Test-Path $xex)) { throw "Missing $xex (extract the XBLA package into game\extracted first)" }
    $hash = (Get-FileHash $xex -Algorithm SHA256).Hash
    if ($hash -ne $xexSha256[$name]) { throw "Unsupported $name (SHA-256 $hash). Expected $($xexSha256[$name]) (Turtles in Time Re-Shelled XBLA 0.0.1.3)." }
}
Write-Host "default.xex and TMNT_r.xex are the supported version."

New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
Push-Location $root
try {
    # rexglue logs to stderr; in Windows PowerShell 5.1 that would trip 'Stop', so judge by exit code.
    $ErrorActionPreference = 'Continue'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $rexglue codegen tmnt_turtles_in_time_reshelled_manifest.toml 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log
    $exitCode = $LASTEXITCODE
    Write-Host ("codegen exit code {0} after {1:N1} s (log: {2})" -f $exitCode, $timer.Elapsed.TotalSeconds, $log)
    if ($exitCode -ne 0) { throw "codegen failed ($exitCode)" }
} finally { Pop-Location; $ErrorActionPreference = 'Stop' }
