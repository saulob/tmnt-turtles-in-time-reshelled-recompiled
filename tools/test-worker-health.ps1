# Tests the deploy health verification of tools/worker.ps1 with mock fetchers. Deploys nothing, contacts nothing.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'worker-health.ps1')

$failures = [System.Collections.Generic.List[string]]::new()
function Expect([bool]$condition, [string]$message) { if (-not $condition) { $failures.Add($message) } }

$expected = Get-ExpectedWorkerBuild $root
$old = '2000-01-01-lobby0'
$state = @{ Calls = 0; Replies = @() }
$fetch = {
    param($url)
    $reply = $state.Replies[[Math]::Min($state.Calls, $state.Replies.Count - 1)]
    $state.Calls++
    if ($reply -is [scriptblock]) { return & $reply }
    return $reply
}
function Run([object[]]$Replies, [int]$Attempts = 10) {
    $state.Calls = 0
    $state.Replies = $Replies
    $result = Test-WorkerHealth 'https://example.invalid/health' $expected $fetch $Attempts 0
    $result.Calls = $state.Calls
    return $result
}
$good = [pscustomobject]@{ status = 'ok'; worker_build = $expected }

$r = Run @($good)
Expect ($r.Ok -and $r.Calls -eq 1 -and $r.Build -eq $expected) 'a matching build must succeed on the first attempt'

$r = Run @([pscustomobject]@{ worker_build = $old }, [pscustomobject]@{ worker_build = $old }, $good)
Expect ($r.Ok -and $r.Calls -eq 3) 'a stale build followed by the matching build must succeed'

$r = Run @([pscustomobject]@{ worker_build = $old })
Expect (-not $r.Ok -and $r.Calls -eq 10 -and $r.Build -eq $old -and $r.Reason -match 'still serves') 'a build that never matches must fail after 10 attempts and name the stale build'

$r = Run @([pscustomobject]@{ status = 'ok' })
Expect (-not $r.Ok -and $r.Calls -eq 10 -and $r.Reason -match 'no worker_build') 'a missing worker_build must fail'

foreach ($bad in @($null, 'not json', 42, [pscustomobject]@{ worker_build = '' }, [pscustomobject]@{ worker_build = 7 }, [pscustomobject]@{ worker_build = $expected.ToUpper() })) {
    $r = Run @($bad) 3
    Expect (-not $r.Ok -and $r.Calls -eq 3) "an invalid or non-matching response ($bad) must be retried and then fail"
}

$r = Run @({ throw 'connection refused' })
Expect (-not $r.Ok -and $r.Calls -eq 10 -and $r.Reason -match 'request failed') 'a request that always fails must retry and then fail'

$r = Run @({ throw 'timeout' }, { throw 'timeout' }, $good)
Expect ($r.Ok -and $r.Calls -eq 3) 'a temporary request failure followed by the matching build must succeed'

$source = Get-Content -LiteralPath (Join-Path $root 'server\tmnt-lobby\src\protocol.js') -Raw
Expect ($source -match "WORKER_BUILD = '$([regex]::Escape($expected))'") 'the expected build must come from WORKER_BUILD in protocol.js'
Expect ((Get-WorkerHealthUrl $root) -match '^https://[^/]+/health$') 'the health URL must come from kDefaultServiceUrl'

$script = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'worker.ps1') -Raw
foreach ($needed in @("worker-health.ps1", 'Get-ExpectedWorkerBuild', 'Get-WorkerHealthUrl', 'Test-WorkerHealth', 'Health check: OK', 'Health check: FAILED', 'Expected worker build', 'Production worker build', 'if (-not $health.Ok)', 'exit 1')) {
    Expect ($script.Contains($needed)) "worker.ps1 no longer contains $needed"
}
Expect ($script -notmatch '20\d\d-\d\d-\d\d-lobby\d') 'worker.ps1 must not hardcode a worker build'
Expect ($script.IndexOf('Test-WorkerHealth') -gt $script.IndexOf('exit $code')) 'the health check must run after a successful deploy only'
Expect ($script.IndexOf("if (`$DryRun) {`r`n    Write-Host 'Worker bundle") -gt 0 -or $script.Contains("Worker bundle checked")) 'a dry run must stay a dry run'

if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "  FAIL $_" -ForegroundColor Red }
    exit 1
}
Write-Host 'worker health tests passed'
exit 0
