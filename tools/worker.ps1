# Deploys the TMNT Worker (server\tmnt-lobby) with Wrangler. Wrangler's own configuration decides where it goes.
#   .\tools\worker.ps1            npx wrangler deploy
#   .\tools\worker.ps1 -DryRun    npx wrangler deploy --dry-run (bundles, uploads nothing)
param([switch]$DryRun)

$ErrorActionPreference = 'Stop'

function Fail([string]$message) {
    [Console]::Error.WriteLine("worker: $message")
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$workerDir = Join-Path $repoRoot 'server\tmnt-lobby'

if (-not (Test-Path -LiteralPath $workerDir -PathType Container)) { Fail "missing folder $workerDir" }
if (-not (Test-Path -LiteralPath (Join-Path $workerDir 'wrangler.toml') -PathType Leaf)) {
    Fail "missing $workerDir\wrangler.toml, the Wrangler configuration"
}
if (-not (Get-Command npx -ErrorAction SilentlyContinue)) {
    Fail 'npx not found; install Node.js (https://nodejs.org) and open a new terminal'
}

$wrangler = @('wrangler', 'deploy')
if ($DryRun) { $wrangler += '--dry-run' }

Push-Location -LiteralPath $workerDir
try {
    & npx @wrangler
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -ne 0) {
    [Console]::Error.WriteLine("worker: wrangler exited with code $code")
    exit $code
}
if ($DryRun) {
    Write-Host 'Worker bundle checked (dry run, nothing uploaded).'
    exit 0
}
Write-Host 'Worker deployed.'

. (Join-Path $PSScriptRoot 'worker-health.ps1')
$expectedBuild = Get-ExpectedWorkerBuild $repoRoot
$healthUrl = Get-WorkerHealthUrl $repoRoot
$health = Test-WorkerHealth $healthUrl $expectedBuild { param($url) Get-WorkerHealth $url }
if (-not $health.Ok) {
    [Console]::Error.WriteLine('Health check: FAILED')
    [Console]::Error.WriteLine("Expected worker build: $expectedBuild")
    [Console]::Error.WriteLine("Production worker build: $(if ($health.Build) { $health.Build } else { '(none)' })")
    [Console]::Error.WriteLine("worker: $($health.Reason) after $($health.Attempts) attempts at $healthUrl; the deploy is not retried, run .	ools\worker.ps1 again if it should be")
    exit 1
}
Write-Host 'Health check: OK'
Write-Host "Worker build: $($health.Build)"
