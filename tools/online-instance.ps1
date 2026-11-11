<#
.SYNOPSIS
  Starts one isolated test instance of the game for online testing on a single PC: its own user folder (settings,
  online identity, friend code, saves, caches), its own log file and its own gameplay UDP port.
.DESCRIPTION
  Instance "a" uses build\online-instances\a\user_data and build\online-instances\a\logs, instance "b" the same with "b",
  and so on; four instances (a, b, c, d) can run at once. The first start copies the display and graphics settings of the
  main user folder (-CopyFrom) and gives the instance its own online name (Test-a, Test-b, or -OnlineName), Online play
  Off and no Connection test result; later starts keep the instance's own files. The online identity is never copied, so
  every instance makes its own friend code on first use and signs in to the online service as its own player.
  Every start writes a new log named after the instance (tmnt_turtles_in_time_reshelled_a_001.log, _002, ...) inside
  that instance's logs folder, and detailed logging is on. The gameplay socket takes the first free of UDP 7001-7004, then
  any free port, so instances need no port arguments; they find each other through the online service.
  Runs the launcher; Online play is switched on from the instance's own Online page. -PrepareOnly sets the folders up and
  prints the same summary without starting the game. Every instance uses the production online service built into the
  game; -Lobby is a diagnostic override for one start and is never needed.
.EXAMPLE
  tools\online-instance.ps1 -Name a     # terminal 1
  tools\online-instance.ps1 -Name b     # terminal 2
  tools\online-instance.ps1 -Name c     # terminal 3
  tools\online-instance.ps1 -Name d     # terminal 4
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9-]{1,16}$')][string]$Name,
    [string]$OnlineName = '',
    [string]$CopyFrom = '',
    [string]$Executable = '',
    [string]$GameData = '',
    [string]$InstanceRoot = '',
    [string]$Lobby = '',
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$BuildDir = Join-Path $Root 'build'
if (-not $Executable) { $Executable = Join-Path $BuildDir 'tmnt_turtles_in_time_reshelled.exe' }
if (-not $GameData) { $GameData = Join-Path $Root 'game\extracted' }
if (-not $CopyFrom) { $CopyFrom = Join-Path $BuildDir 'user_data' }
$DefaultInstanceRoot = Join-Path $BuildDir 'online-instances'
if (-not $InstanceRoot) { $InstanceRoot = $DefaultInstanceRoot }
elseif (-not [IO.Path]::IsPathRooted($InstanceRoot)) { $InstanceRoot = Join-Path $DefaultInstanceRoot $InstanceRoot }
$InstanceRoot = [IO.Path]::GetFullPath($InstanceRoot)
$insideRepo = $InstanceRoot.StartsWith($Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
$insideBuild = $InstanceRoot.StartsWith($BuildDir.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
if ($insideRepo -and -not $insideBuild) { throw "InstanceRoot must be outside the repository or under build\: $InstanceRoot" }
$label = $Name.ToLowerInvariant()
if (-not $OnlineName) { $OnlineName = "Test-$label" }
$OnlineName = ($OnlineName -replace '[^A-Za-z0-9_-]', '')
if ($OnlineName.Length -gt 16) { $OnlineName = $OnlineName.Substring(0, 16) }
if (-not $OnlineName) { $OnlineName = 'Player' }

$user = Join-Path (Join-Path $InstanceRoot $label) 'user_data'
$logs = Join-Path (Join-Path $InstanceRoot $label) 'logs'
$settingsFile = Join-Path $user 'settings.toml'

# Window placement, the online choices and the logging choice are the instance's own; everything else is copied once.
$OwnKeys = '^\s*tmnt_(launcher_(x|y|width|height|maximized|fullscreen)|online_(enabled|name|ping_display|connection_test))\s*=|^\s*detailed_logging\s*='
$NameKey = 'tmnt_online_name'

function Set-SettingLine([string[]]$Lines, [string]$Key, [string]$Value) {
    @($Lines | Where-Object { $_ -notmatch ('^\s*' + [regex]::Escape($Key) + '\s*=') }) + "$Key = $Value"
}

function Next-LogFile([string]$Folder, [string]$Label) {
    $prefix = "tmnt_turtles_in_time_reshelled_${Label}_"
    $last = 0
    foreach ($file in Get-ChildItem -LiteralPath $Folder -Filter "$prefix*.log" -ErrorAction SilentlyContinue) {
        if ($file.BaseName.Substring($prefix.Length) -match '^\d{1,9}$') { $last = [Math]::Max($last, [int]$Matches[0]) }
    }
    Join-Path $Folder ('{0}{1:000}.log' -f $prefix, ($last + 1))
}

$fresh = -not (Test-Path -LiteralPath $user)
if ($fresh) {
    New-Item -ItemType Directory -Force $user | Out-Null
    $lines = @('# Turtles in Time Re-Shelled: online test instance settings.')
    $source = Join-Path $CopyFrom 'settings.toml'
    if (Test-Path -LiteralPath $source) {
        $lines = @(Get-Content -LiteralPath $source | Where-Object { $_ -notmatch $OwnKeys })
    }
    $lines = Set-SettingLine $lines $NameKey ('"' + $OnlineName + '"')
    Set-Content -LiteralPath $settingsFile -Value $lines -Encoding utf8
} elseif (-not (Select-String -LiteralPath $settingsFile -Pattern ('^\s*' + $NameKey + '\s*=') -Quiet)) {
    Add-Content -LiteralPath $settingsFile -Value "$NameKey = `"$OnlineName`"" -Encoding utf8
}
New-Item -ItemType Directory -Force $logs | Out-Null
$logFile = Next-LogFile $logs $label

$service = if ($Lobby) { "-Lobby $Lobby" } else { 'production' }
Write-Host "Online test instance $($label.ToUpperInvariant())$(if ($fresh) { ' (new)' })"
Write-Host "  instance:       $label"
Write-Host "  user folder:    $user"
Write-Host "  settings:       $settingsFile"
Write-Host "  identity:       $(Join-Path $user 'online_identity.ini') (made on first use; this instance has its own friend code)"
Write-Host "  logs folder:    $logs"
Write-Host "  log file:       $logFile"
Write-Host "  gameplay port:  first free of UDP 7001-7004, then any free port"
Write-Host "  online service: $service"

if ($PrepareOnly) { return }

if (-not (Test-Path -LiteralPath $Executable)) { throw "Not built yet: $Executable" }
if ($Lobby) { $env:TMNT_LOBBY = $Lobby } else { Remove-Item Env:TMNT_LOBBY -ErrorAction SilentlyContinue }
$gameArgs = @("--game_data_root=`"$GameData`"", "--user_data_root=`"$user`"", "--log_file=`"$logFile`"")
Start-Process -FilePath $Executable -ArgumentList $gameArgs -WorkingDirectory (Split-Path -Parent $Executable)
