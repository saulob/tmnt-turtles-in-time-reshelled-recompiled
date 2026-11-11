# Health verification for tools/worker.ps1: reads what a deploy must serve and checks /health. Dot-sourced; runs nothing.

function Get-ExpectedWorkerBuild([string]$RepoRoot) {
    $source = Get-Content -LiteralPath (Join-Path $RepoRoot 'server\tmnt-lobby\src\protocol.js') -Raw
    if ($source -notmatch "WORKER_BUILD\s*=\s*'([^']+)'") { throw 'server\tmnt-lobby\src\protocol.js has no WORKER_BUILD' }
    return $Matches[1]
}

function Get-WorkerHealthUrl([string]$RepoRoot) {
    $header = Get-Content -LiteralPath (Join-Path $RepoRoot 'src\online\online_service.h') -Raw
    if ($header -notmatch 'kDefaultServiceUrl\[\]\s*=\s*"wss://([^/"]+)/?"') { throw 'src\online\online_service.h has no kDefaultServiceUrl' }
    return "https://$($Matches[1])/health"
}

# Returns @{ Ok; Build; Attempts; Reason }. Fetch takes the URL and returns the parsed /health object, or throws.
function Test-WorkerHealth([string]$Url, [string]$ExpectedBuild, [scriptblock]$Fetch, [int]$MaxAttempts = 10, [int]$DelayMs = 1000) {
    $reason = 'no attempt was made'
    $build = $null
    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        try {
            $health = & $Fetch $Url
            $build = if ($health -and $health.PSObject.Properties['worker_build']) { $health.worker_build } else { $null }
            if ($build -isnot [string] -or [string]::IsNullOrWhiteSpace($build)) {
                $build = $null
                $reason = 'the response has no worker_build'
            } elseif ($build -ceq $ExpectedBuild) {
                return @{ Ok = $true; Build = $build; Attempts = $attempt; Reason = '' }
            } else {
                $reason = "the Worker still serves build $build"
            }
        } catch {
            $reason = "the request failed: $($_.Exception.Message)"
        }
        if ($attempt -lt $MaxAttempts -and $DelayMs -gt 0) { Start-Sleep -Milliseconds $DelayMs }
    }
    return @{ Ok = $false; Build = $build; Attempts = $MaxAttempts; Reason = $reason }
}

function Get-WorkerHealth([string]$Url) {
    Invoke-RestMethod -Uri $Url -Method Get -TimeoutSec 10 -ErrorAction Stop
}
