<#
.SYNOPSIS
  The headless parity gate for Windows and Linux: every check that needs neither the game, the launcher, a window nor a
  build. Prints one PASS, FAIL or NOT RUN line per check and a summary; exits 1 when a check fails.

.DESCRIPTION
  Checks the ReXGlue patch manifest against the patch files, the names and the documentation, that both platform
  series apply to the pinned SDK source, and the audio client teardown of patch 0086 (a threaded regression test and a
  syntax check of the patched source, compile only); the title music level, Skip intro and the host keys (standalone test
  programs); the Windows and Linux packaging scripts, the AppImage inputs (desktop file,
  icons, licenses, FUSE-fallback runtime pins), the glibc baseline logic, the release check, the player-facing features
  on both platforms, the updater's asset names against the packaging scripts, and the repository for temporary markers,
  local paths, stale claims, credentials, game data and player data. Nothing is linked into the game, launched or
  published.

  The fixture AppDir of tools/test-appimage-release.sh needs Linux: it runs in the Ubuntu 20.04 WSL distribution named by
  TMNT_LINUX_DISTRO, or the first one that has clang-20. TMNT_APPIMAGE_TOOLS (a Linux path) adds the fixture AppImage.

  -SdkRepo (or REXGLUE_SDK_REPO) is a clone of the official ReXGlue repository with its submodules; by default it is
  out\rexglue-sdk-src, which tools\build-patched-rexruntime.ps1 creates. Without one the checks that need the SDK source
  are NOT RUN. -CppTests <build folder> additionally builds the tmnt_runtime_tests target there (a configured build) and
  runs every test program except those in $WindowedTests, which open a window; without it that check is NOT RUN.

  -Release is the final gate: the temporary DEBUG_SAULO diagnostics listed in $TemporaryMarkers fail it. Without it they
  are reported and only the exact listed files, up to the listed counts, are accepted.

.EXAMPLE
  .\tools\test.ps1
  .\tools\test.ps1 -Release
  .\tools\test.ps1 -SdkRepo S:\path\to\rexglue-sdk
#>
[CmdletBinding()]
param(
    [string]$SdkRepo = $env:REXGLUE_SDK_REPO,
    [string]$CppTests,
    [switch]$Release
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
if (-not $SdkRepo) { $SdkRepo = Join-Path $Root 'out\rexglue-sdk-src' }
$SdkHelp = "$SdkRepo is missing or incomplete: run tools\build-patched-rexruntime.ps1 (tools\build.ps1 does when needed), or pass -SdkRepo or set REXGLUE_SDK_REPO"
$PatchDir = Join-Path $Root 'patches\rexglue-sdk'
$Results = [System.Collections.Generic.List[object]]::new()
$PowerShellHost = (Get-Process -Id $PID).Path

function Join-Root([string]$Relative) { Join-Path $Root $Relative }

function Invoke-Check([string]$Name, [scriptblock]$Body) {
    Write-Host "== $Name"
    try {
        $problems = @(& $Body | Where-Object { $_ })
    } catch {
        $problems = @("exception: $($_.Exception.Message)")
    }
    if ($problems.Count -and $problems[0] -like 'NOT RUN:*') {
        $Results.Add([pscustomobject]@{ Name = $Name; Status = 'NOT RUN'; Detail = $problems[0].Substring(8).Trim() })
        Write-Host "   NOT RUN: $($problems[0].Substring(8).Trim())" -ForegroundColor Yellow
    } elseif ($problems.Count) {
        $Results.Add([pscustomobject]@{ Name = $Name; Status = 'FAIL'; Detail = "$($problems.Count) problem(s)" })
        foreach ($problem in $problems) { Write-Host "   FAIL: $problem" -ForegroundColor Red }
    } else {
        $Results.Add([pscustomobject]@{ Name = $Name; Status = 'PASS'; Detail = '' })
    }
}

function Read-Text([string]$Relative) { [IO.File]::ReadAllText((Join-Root $Relative)) }

$GitAvailable = [bool](Get-Command git -ErrorAction SilentlyContinue)
$ProjectFiles = @()
if ($GitAvailable) {
    $ProjectFiles = @(& git -C $Root ls-files --cached --others --exclude-standard | Where-Object { $_ })
}
$TextExtensions = '.cpp', '.h', '.hpp', '.c', '.ps1', '.sh', '.py', '.md', '.txt', '.toml', '.json', '.cmake', '.patch', '.desktop', '.env', '.rc', '.manifest', '.yml', '.inc'

# ---- ReXGlue patches -------------------------------------------------------------------------------------------------

$PatchPattern = '^(?<number>[0-9]{4})-[a-z0-9]+(-[a-z0-9]+)*(\.(?<platform>windows|linux))?\.patch$'
$ParityPattern = '^(shared|d3d12-specific|vulkan-specific|windows-specific|linux-specific|vulkan-candidate|pair:[0-9]{4})$'

function Read-PatchManifest {
    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath (Join-Path $PatchDir 'patches.txt')) {
        if (-not $line.Trim() -or $line.TrimStart().StartsWith('#')) { continue }
        $fields = @($line.Trim() -split '\s+')
        $rows.Add([pscustomobject]@{ Line = $line; Fields = $fields })
    }
    return $rows
}

Invoke-Check 'patch manifest: patches.txt matches the patch files' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $rows = Read-PatchManifest
    $listed = @()
    foreach ($row in $rows) {
        if ($row.Fields.Count -ne 5) { $problems.Add("not five columns: $($row.Line.Trim())"); continue }
        $file, $platforms, $targets, $origin, $parity = $row.Fields
        $listed += $file
        if ($file -notmatch $PatchPattern) { $problems.Add("$file is not named NNNN-name[.windows|.linux].patch"); continue }
        $expected = switch ($Matches['platform']) { 'windows' { 'windows' } 'linux' { 'linux' } default { 'windows,linux' } }
        if ($platforms -ne $expected) { $problems.Add("${file}: platforms '$platforms' do not match its name (expected '$expected')") }
        foreach ($target in $targets -split ',') {
            if ($target -notin 'rexruntime', 'rexgpu-xenos', 'rexglue') { $problems.Add("${file}: unknown target '$target'") }
        }
        if ($origin -notin 'reference', 'imported', 'tmnt') { $problems.Add("${file}: unknown origin '$origin'") }
        if ($parity -notmatch $ParityPattern) { $problems.Add("${file}: unknown parity '$parity'") }
        elseif ($expected -eq 'windows,linux' -and $parity -ne 'shared') { $problems.Add("$file applies on both platforms, so its parity must be 'shared'") }
        elseif ($expected -ne 'windows,linux' -and $parity -eq 'shared') { $problems.Add("$file is for one platform, so its parity cannot be 'shared'") }
    }
    $files = @(Get-ChildItem -LiteralPath $PatchDir -Filter '*.patch' -File | ForEach-Object Name)
    foreach ($name in $files) { if ($name -notin $listed) { $problems.Add("patches.txt does not list $name") } }
    foreach ($name in $listed) { if ($name -notin $files) { $problems.Add("patches.txt lists $name, which does not exist") } }
    $sorted = [string[]]@($listed)
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    if (($sorted -join ',') -ne ($listed -join ',')) { $problems.Add('patches.txt is not in name order') }
    $numbers = @($listed | ForEach-Object { $_.Substring(0, 4) })
    foreach ($group in $numbers | Group-Object | Where-Object Count -gt 1) { $problems.Add("number $($group.Name) is used more than once") }
    $problems
}

Invoke-Check 'patch parity: every platform-specific patch is accounted for on the other platform' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $byNumber = @{}
    foreach ($row in Read-PatchManifest) { if ($row.Fields.Count -eq 5) { $byNumber[$row.Fields[0].Substring(0, 4)] = $row.Fields } }
    foreach ($fields in $byNumber.Values) {
        $file, $platforms, $targets, $origin, $parity = $fields
        if ($parity -notmatch '^pair:(?<other>[0-9]{4})$') { continue }
        $other = $Matches['other']
        if (-not $byNumber.ContainsKey($other)) { $problems.Add("${file}: pair $other does not exist"); continue }
        $otherFields = $byNumber[$other]
        if ($otherFields[1] -eq $platforms) { $problems.Add("${file}: pair $other is for the same platform ($platforms)") }
        if ($otherFields[4] -notmatch '^pair:' -and $otherFields[4] -ne 'shared') { $problems.Add("${file}: pair $other is not a counterpart ($($otherFields[4]))") }
    }
    $candidates = @($byNumber.Values | Where-Object { $_[4] -eq 'vulkan-candidate' } | ForEach-Object { $_[0].Substring(0, 4) })
    if ($candidates.Count) { Write-Host "   deferred Vulkan candidates (documented, not ported): $($candidates -join ', ')" }
    $problems
}

Invoke-Check 'patch docs: docs/rexglue-runtime-patch.md covers every patch with its platform' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $doc = Read-Text 'docs\rexglue-runtime-patch.md'
    foreach ($row in Read-PatchManifest) {
        $file = $row.Fields[0]
        $number = $file.Substring(0, 4)
        if ($doc -notmatch "(?m)^\| \[$number-") { $problems.Add("$number has no row in the patch table") ; continue }
        $tableRow = ($doc -split "`n" | Where-Object { $_ -match "^\| \[$number-" } | Select-Object -First 1)
        if ($tableRow -notmatch [regex]::Escape($file)) { $problems.Add("$number's row links another file than $file") }
        $platform = switch -Regex ($file) { '\.windows\.patch$' { 'Windows' } '\.linux\.patch$' { 'Linux' } default { 'both|All' } }
        if ($tableRow -notmatch "\| ($platform) \|") { $problems.Add("$number's row does not state the platform $platform") }
    }
    if ($doc -match 'toolsexruntime|patchesexglue|[a-z]exgpu-xenos\.profdata') { $problems.Add('a path in the patch docs lost its backslashes') }
    $problems
}

$AudioPatch = '0086-audio-unregister-waits-for-callback.patch'
$HostPlatform = if ($IsLinux) { 'linux' } else { 'windows' }
$PatchSeries = @{}
$PatchedSdk = $null

Invoke-Check 'ReXGlue patch stack: the Windows and Linux series apply to the pinned SDK source in order' {
    $baseline = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $PatchDir 'baseline.env')) { if ($line -match '^([A-Z0-9_]+)=(.*)$') { $baseline[$Matches[1]] = $Matches[2] } }
    if ($baseline['SDK_COMMIT'] -notmatch '^[0-9a-f]{40}$') { return 'baseline.env has no SDK_COMMIT' }
    if (-not $GitAvailable) { return 'NOT RUN: git is not available' }
    if (-not (Test-Path -LiteralPath $SdkRepo)) { return "NOT RUN: $SdkHelp" }
    & git -C $SdkRepo cat-file -e "$($baseline['SDK_COMMIT'])^{commit}" 2>$null
    if ($LASTEXITCODE) { return "NOT RUN: $SdkRepo does not hold commit $($baseline['SDK_COMMIT']). $SdkHelp" }
    if (-not (Get-Command tar -ErrorAction SilentlyContinue)) { return 'NOT RUN: tar is not available' }
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($platform in 'windows', 'linux') {
        $work = Join-Path ([IO.Path]::GetTempPath()) "tmnt_patch_apply_$platform"
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
        New-Item -ItemType Directory $work | Out-Null
        $keep = $false
        try {
            $archive = Join-Path $work 'sdk.tar'
            & git -C $SdkRepo archive --format=tar -o $archive $baseline['SDK_COMMIT']
            $tree = Join-Path $work 'tree'
            New-Item -ItemType Directory $tree | Out-Null
            # Relative: MSYS tar reads the drive colon of an absolute path as a remote host.
            Push-Location $work
            try { & tar -xf 'sdk.tar' -C 'tree' 2>&1 | Out-Null; $extracted = $LASTEXITCODE } finally { Pop-Location }
            if ($extracted) { $problems.Add("tar could not extract the SDK source for the $platform series"); continue }
            & git -C $tree init -q
            $other = if ($platform -eq 'windows') { 'linux' } else { 'windows' }
            $names = [string[]]@(Get-ChildItem -LiteralPath $PatchDir -Filter '*.patch' -File | Where-Object { $_.Name -notlike "*.$other.patch" } | ForEach-Object Name)
            [Array]::Sort($names, [StringComparer]::Ordinal)
            $series = @{ Applied = 0; Failed = $null; Count = $names.Count }
            foreach ($name in $names) {
                & git -C $tree apply --whitespace=nowarn (Join-Path $PatchDir $name) 2>&1 | Out-Null
                if ($LASTEXITCODE) { $series.Failed = $name; break }
                $series.Applied++
            }
            $PatchSeries[$platform] = $series
            if ($series.Failed -and $series.Failed -ne $AudioPatch) { $problems.Add("patch sequence: $($series.Failed) does not apply on the $platform series") }
            elseif ($series.Failed) { $problems.Add("$AudioPatch does not apply on the $platform series after the patches before it") }
            Write-Host "   $platform series: $($series.Applied) of $($names.Count) patches"
            if ($platform -eq $HostPlatform -and -not $series.Failed) {
                $script:PatchedSdk = $tree
                $keep = $true
            }
        } finally {
            if (-not $keep) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }
    $problems
}

Invoke-Check 'Audio unregister patch: 0086 is listed once, in order, and applies after the patches before it' {
    $problems = [System.Collections.Generic.List[string]]::new()
    if (-not (Test-Path -LiteralPath (Join-Path $PatchDir $AudioPatch))) { $problems.Add("$AudioPatch does not exist") }
    $listed = @(Read-PatchManifest | ForEach-Object { $_.Fields[0] })
    $rows = @($listed | Where-Object { $_ -eq $AudioPatch })
    if ($rows.Count -ne 1) { $problems.Add("patches.txt lists $AudioPatch $($rows.Count) times, expected once") }
    $at = [Array]::IndexOf([string[]]$listed, $AudioPatch)
    if ($at -ge 1 -and $listed[$at - 1] -notlike '0085-*') { $problems.Add("patches.txt lists $($listed[$at - 1]) right before $AudioPatch, expected 0085") }
    if ($problems.Count) { return $problems }
    if (-not $PatchSeries.Count) { return 'NOT RUN: the patch stack check did not run' }
    foreach ($platform in $PatchSeries.Keys) {
        $failed = $PatchSeries[$platform].Failed
        if ($failed -eq $AudioPatch) { $problems.Add("$AudioPatch does not apply on the $platform series") }
        elseif ($failed) { $problems.Add("$AudioPatch was not reached on the $platform series: $failed failed first") }
    }
    $problems
}

Invoke-Check 'patch scripts: both builders name patches the same way and read nothing but *.patch' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $ps = Read-Text 'tools\rexruntime-patch.ps1'
    $sh = Read-Text 'tools\build-patched-rexruntime.sh'
    if ($ps -notmatch [regex]::Escape("(\.(?<platform>windows|linux))?\.patch`$")) { $problems.Add('rexruntime-patch.ps1 does not enforce the patch name pattern') }
    if ($sh -notmatch [regex]::Escape('(\.(windows|linux))?\.patch$')) { $problems.Add('build-patched-rexruntime.sh does not enforce the patch name pattern') }
    if ($sh -notmatch '\*\.windows\.patch\) ;;') { $problems.Add('build-patched-rexruntime.sh does not leave the Windows patches out') }
    if ($ps -notmatch "-Filter '\*\.patch'") { $problems.Add('rexruntime-patch.ps1 reads other files than *.patch') }
    foreach ($needed in 'sdk_commit', 'inputs_sha256', 'patches') {
        if ($ps -notmatch $needed) { $problems.Add("rexruntime-patch.ps1 has no $needed in its stamp logic") }
        if ($sh -notmatch $needed) { $problems.Add("build-patched-rexruntime.sh has no $needed in its stamp logic") }
    }
    $problems
}

Invoke-Check 'runtime fingerprint: pinned baseline and official hashes are complete' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $env = Read-Text 'patches\rexglue-sdk\baseline.env'
    foreach ($key in 'SDK_VERSION', 'SDK_TAG', 'SDK_COMMIT', 'SDK_REPO') {
        if ($env -notmatch "(?m)^$key=\S+") { $problems.Add("baseline.env has no $key") }
    }
    foreach ($m in [regex]::Matches($env, '(?m)^(OFFICIAL_[A-Z_0-9]+_SHA256)=(.*)$')) {
        if ($m.Groups[2].Value -notmatch '^[0-9a-f]{64}$') { $problems.Add("$($m.Groups[1].Value) is not a SHA-256") }
    }
    $version = [regex]::Match($env, '(?m)^SDK_VERSION=(\S+)').Groups[1].Value
    $linuxScript = Read-Text 'tools\package-linux.sh'
    if ($linuxScript -notmatch "(?m)^SDK_VERSION=$([regex]::Escape($version))\s*$") { $problems.Add("package-linux.sh does not pin SDK $version") }
    $problems
}

# ---- Version, settings, sources --------------------------------------------------------------------------------------

Invoke-Check 'version: one project version, used by the executable, the AppImage name and the tests' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $cmake = Read-Text 'CMakeLists.txt'
    $m = [regex]::Match($cmake, '(?m)^\s*project\(\s*tmnt_turtles_in_time_reshelled\s+VERSION\s+(\d+\.\d+\.\d+)')
    if (-not $m.Success) { return 'CMakeLists.txt has no project VERSION' }
    $version = $m.Groups[1].Value
    $linux = Read-Text 'tools\package-linux.sh'
    if ($linux -notmatch [regex]::Escape('project\([[:space:]]*tmnt_turtles_in_time_reshelled[[:space:]]+VERSION')) { $problems.Add('package-linux.sh does not read the project version from CMakeLists.txt') }
    $release = Read-Text 'tools\package-release.ps1'
    if ($release -notmatch 'CMakeLists\.txt') { $problems.Add('package-release.ps1 does not read the project version from CMakeLists.txt') }
    if ($cmake -notmatch 'TMNT_VERSION_MAJOR=\$\{PROJECT_VERSION_MAJOR\}') { $problems.Add('the executable does not receive the project version') }
    if ($version -notmatch '^\d+\.\d+\.\d+$') { $problems.Add("version $version is not MAJOR.MINOR.PATCH") }
    if ((Read-Text 'packaging\README.txt') -notmatch '@VERSION@') { $problems.Add('packaging/README.txt does not take the version from @VERSION@') }
    foreach ($script in 'tools\package-release.ps1', 'tools\package-linux.sh', 'tools\stamp-release-hashes.ps1') {
        if ((Read-Text $script) -match "(?m)^[^#]*-v$([regex]::Escape($version))-") { $problems.Add("$script hardcodes $version") }
    }
    $problems
}

Invoke-Check 'updater: asset names, repository and folders match the packaging scripts' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $config = Read-Text 'src\update_config.cpp'
    $windows = [regex]::Match((Read-Text 'tools\package-release.ps1'), '(?m)^\$Name = "(.+)-v\$Version-(.+)"').Groups
    $linux = [regex]::Match((Read-Text 'tools\package-linux.sh'), '(?m)^NAME="(.+)-v\$VERSION-(.+)"').Groups
    foreach ($asset in @(
            @($windows[1].Value, "-$($windows[2].Value).zip"), @($windows[1].Value, "-$($windows[2].Value).sha256"),
            @($linux[1].Value, "-$($linux[2].Value).AppImage"), @($linux[1].Value, "-$($linux[2].Value).sha256"))) {
        $needle = "`"$($asset[0])-v`" + version.CoreString() + `"$($asset[1])`""
        if (-not $config.Contains($needle)) { $problems.Add("update_config.cpp does not look for $($asset[0])-v<version>$($asset[1])") }
    }
    $repository = 'saulob/tmnt-turtles-in-time-reshelled-recompiled'
    foreach ($url in "https://api.github.com/repos/$repository/releases/latest", "https://github.com/$repository/releases/download/") {
        if (-not $config.Contains($url)) { $problems.Add("update_config.cpp does not use $url") }
    }
    if (-not (& git -C $Root remote get-url origin).Contains($repository)) { $problems.Add("origin is not $repository") }
    if ((Read-Text 'src\update_config.cpp') -notmatch 'TMNT_UPDATE_TEST"\);\s*\r?\n\s*return value && \*value == "1"') { $problems.Add('updater test overrides are not behind TMNT_UPDATE_TEST=1') }
    $problems
}

Invoke-Check 'game executables: the Linux packaging pins the same two executables as Windows' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $build = Read-Text 'tools\build.ps1'
    $linux = Read-Text 'tools\package-linux.sh'
    foreach ($pin in @(@('default.xex', 'DEFAULT_XEX_SHA256'), @('TMNT_r.xex', 'TMNT_R_XEX_SHA256'))) {
        $windows = [regex]::Match($build, "'$([regex]::Escape($pin[0]))'\s*=\s*'([0-9A-Fa-f]{64})'").Groups[1].Value
        $pinned = [regex]::Match($linux, "(?m)^$($pin[1])=([0-9a-f]{64})\s*$").Groups[1].Value
        if (-not $windows -or $windows.ToLowerInvariant() -ne $pinned) { $problems.Add("package-linux.sh pins another $($pin[0]) than tools/build.ps1") }
        if ($linux -notmatch "check_xex $([regex]::Escape($pin[0]))") { $problems.Add("package-linux.sh does not check $($pin[0])") }
    }
    if ($linux -notmatch 'game/extracted/TMNT_r\.xex"? "\$WORK/game/extracted/"') { $problems.Add('package-linux.sh does not sync both executables, which the Linux codegen step reads') }
    $problems
}

Invoke-Check 'sources: every CMake source exists and every source file is built or is a test' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $cmake = Read-Text 'CMakeLists.txt'
    $referenced = @([regex]::Matches($cmake, '(?<!\}/)(?:src|tests)/[A-Za-z0-9_./-]+\.(?:cpp|c|h)') | ForEach-Object { $_.Value } | Sort-Object -Unique)
    foreach ($path in $referenced) { if (-not (Test-Path -LiteralPath (Join-Root $path))) { $problems.Add("CMakeLists.txt names $path, which does not exist") } }
    $cmakeAll = $cmake + (Get-Content -Raw -LiteralPath (Join-Root 'generated\default\sources.cmake') -ErrorAction SilentlyContinue)
    foreach ($file in Get-ChildItem -LiteralPath (Join-Root 'src') -Filter '*.cpp' -File) {
        $relative = "src/$($file.Name)"
        if ($relative -notin $referenced -and $cmake -notmatch [regex]::Escape($file.Name)) { $problems.Add("$relative is not in any CMake target") }
    }
    $problems
}

Invoke-Check 'sources: Windows-only APIs stay out of code the Linux build compiles' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $cmake = Read-Text 'CMakeLists.txt'
    $windowsOnly = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($block in [regex]::Matches($cmake, '(?s)if\(WIN32\)(.*?)\nendif\(\)')) {
        foreach ($m in [regex]::Matches($block.Groups[1].Value, 'src/[A-Za-z0-9_./-]+\.cpp')) { [void]$windowsOnly.Add($m.Value) }
    }
    foreach ($file in Get-ChildItem -LiteralPath (Join-Root 'src') -Filter '*.cpp' -File -Recurse) {
        $relative = ($file.FullName.Substring($Root.Length + 1)) -replace '\\', '/'
        if ($windowsOnly.Contains($relative)) { continue }
        $text = [IO.File]::ReadAllText($file.FullName)
        if ($text -match '(?m)^\s*#\s*include\s*<(windows|shellapi|shlobj|d3d12|dxgi\d*)\.h>' -and $text -notmatch '_WIN32|REX_PLATFORM_WIN32|WIN32_LEAN') {
            $problems.Add("$relative includes a Windows header without a platform guard and is not in a Windows-only CMake block")
        }
    }
    $problems
}

Invoke-Check 'settings: every launcher setting key in the source is documented, and the documented ones exist' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $notSettings = '^tmnt_turtles_in_time_reshelled_?$', '^tmnt_launcher$'
    $source = (Get-ChildItem -LiteralPath (Join-Root 'src') -Include '*.cpp', '*.h' -File -Recurse | Get-Content -Raw) -join "`n"
    $docs = (@('README.md', 'docs\launcher.md', 'docs\user-data.md', 'docs\display.md', 'docs\input.md') | ForEach-Object { Read-Text $_ }) -join "`n"
    $keys = @([regex]::Matches($source, '"(tmnt_[a-z0-9_]+)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique |
        Where-Object { $key = $_; -not ($notSettings | Where-Object { $key -match $_ }) })
    foreach ($key in $keys) {
        $documented = $docs.Contains($key) -or ($key -match '^tmnt_player_[1-4]_name$' -and $docs.Contains('tmnt_player_1_name'))
        if (-not $documented) { $problems.Add("the setting $key is not documented") }
    }
    foreach ($m in [regex]::Matches($docs, '`(tmnt_(?:aspect_ratio|graphics_adapter|language|launcher_[a-z]+|msaa_compatibility|show_[a-z_]+|skip_launcher))`')) {
        if (-not $source.Contains('"' + $m.Groups[1].Value + '"')) { $problems.Add("the documented setting $($m.Groups[1].Value) is not in the source") }
    }
    $problems
}

# ---- Windows packaging -----------------------------------------------------------------------------------------------

Invoke-Check 'windows package: release check refuses data, diagnostics and stale runtime files' {
    $script = Read-Text 'tools\package-release.ps1'
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($needed in 'DEBUG_SAULO', 'renderdoc', 'VkLayer', 'vulkan-1', 'user_data', 'logs', 'default.xex', 'TMNT_r.xex', 'LIVE', 'XEX2', 'USERPROFILE', 'SHA256', 'rexruntime-patch', 'Test-PatchedRuntime') {
        if ($script -notmatch [regex]::Escape($needed)) { $problems.Add("package-release.ps1 has no rule for '$needed'") }
    }
    $problems
}

Invoke-Check 'windows package: tools/test-release-check.ps1 (scratch folders only)' {
    $global:LASTEXITCODE = 0
    $output = & $PowerShellHost -NoProfile -File (Join-Path $PSScriptRoot 'test-release-check.ps1') 2>&1
    if ($LASTEXITCODE) { return @($output | Select-Object -Last 6 | ForEach-Object { "$_" }) }
}

Invoke-Check 'runtime: tools/test-rexruntime-source-reset.ps1 (scratch repository only)' {
    $global:LASTEXITCODE = 0
    $output = & $PowerShellHost -NoProfile -File (Join-Path $PSScriptRoot 'test-rexruntime-source-reset.ps1') 2>&1
    if ($LASTEXITCODE) { return @($output | Select-Object -Last 6 | ForEach-Object { "$_" }) }
}

# ---- Linux / AppImage ------------------------------------------------------------------------------------------------

Invoke-Check 'linux: presets, toolchain and RPATH' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $presets = Read-Text 'CMakePresets.json'
    foreach ($needed in 'linux-amd64-release', 'linux-amd64-relwithdebinfo', 'linux-amd64-debug', 'clang-20', 'clang++-20') {
        if ($presets -notmatch [regex]::Escape($needed)) { $problems.Add("CMakePresets.json has no $needed") }
    }
    $cmake = Read-Text 'CMakeLists.txt'
    if ($cmake -notmatch 'RPATH is \$ORIGIN') { $problems.Add('CMakeLists.txt does not document the $ORIGIN RPATH of the Linux program') }
    $linux = (Read-Text 'tools\package-linux.sh') + (Read-Text 'tools\linux-policy.sh')
    if ($linux.IndexOf('RPATH/RUNPATH is', [StringComparison]::Ordinal) -lt 0 -or $linux.IndexOf("'`$ORIGIN'", [StringComparison]::Ordinal) -lt 0) { $problems.Add('package-linux.sh does not require an $ORIGIN RPATH/RUNPATH') }
    $problems
}

Invoke-Check 'linux: AppImage inputs (desktop file, icons, AppRun, licenses)' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $platform = Read-Text 'src\platform.cpp'
    $appId = [regex]::Match($platform, 'constexpr char kApplicationId\[\] = "([^"]+)";').Groups[1].Value
    if ($appId -notmatch '^[A-Za-z0-9_-]+(\.[A-Za-z0-9_-]+)+$') { return 'src/platform.cpp has no dotted kApplicationId' }
    $desktopPath = "packaging\linux\$appId.desktop"
    if (-not (Test-Path -LiteralPath (Join-Root $desktopPath))) { return "$desktopPath is missing" }
    $desktop = Read-Text $desktopPath
    foreach ($needed in '[Desktop Entry]', 'Type=Application', 'Name=', 'Exec=', 'Icon=tmnt_turtles_in_time_reshelled', 'Categories=', "StartupWMClass=$appId") {
        if ($desktop.IndexOf($needed, [StringComparison]::Ordinal) -lt 0) { $problems.Add("$desktopPath has no '$needed'") }
    }
    foreach ($size in 16, 24, 32, 48, 64, 128, 256, 512) {
        $png = Join-Root "packaging\icons\app-icon-$size.png"
        if (-not (Test-Path -LiteralPath $png)) { $problems.Add("app-icon-$size.png is missing"); continue }
        $bytes = [IO.File]::ReadAllBytes($png)
        if ($bytes.Length -lt 24 -or [BitConverter]::ToString($bytes[0..3]) -ne '89-50-4E-47') { $problems.Add("app-icon-$size.png is not a PNG"); continue }
        $width = ([int]$bytes[16] -shl 24) -bor ([int]$bytes[17] -shl 16) -bor ([int]$bytes[18] -shl 8) -bor [int]$bytes[19]
        $height = ([int]$bytes[20] -shl 24) -bor ([int]$bytes[21] -shl 16) -bor ([int]$bytes[22] -shl 8) -bor [int]$bytes[23]
        if ($width -ne $size -or $height -ne $size) { $problems.Add("app-icon-$size.png is ${width}x${height}") }
    }
    foreach ($file in 'packaging\icons\app-icon.svg', 'packaging\icons\app-icon.ico', 'packaging\windows\tmnt_turtles_in_time_reshelled.rc', 'packaging\linux\AppRun') {
        if (-not (Test-Path -LiteralPath (Join-Root $file))) { $problems.Add("$file is missing") }
    }
    $apprun = Read-Text 'packaging\linux\AppRun'
    if ($apprun -notmatch '(?m)^exec "\$HERE/usr/bin/tmnt_turtles_in_time_reshelled" "\$@"') { $problems.Add('AppRun does not exec the program with its arguments') }
    if ($apprun -notmatch 'GLIBCXX_3\.4\.32') { $problems.Add('AppRun does not choose the bundled C++ runtime by GLIBCXX version') }
    foreach ($license in 'packaging\licenses\ReXGlue-LICENSE.txt', 'packaging\licenses\THIRD-PARTY-LICENSES.txt', 'packaging\licenses\THIRD-PARTY-LICENSES-LINUX.txt', 'LICENSE') {
        if (-not (Test-Path -LiteralPath (Join-Root $license)) -or (Get-Item -LiteralPath (Join-Root $license)).Length -lt 200) { $problems.Add("$license is missing or empty") }
    }
    $notices = Read-Text 'packaging\licenses\THIRD-PARTY-LICENSES-LINUX.txt'
    foreach ($component in 'type2-runtime', 'libfuse', 'squashfuse', 'Zstandard', 'zlib', 'mimalloc', 'musl', 'libgcc') {
        if ($notices -notmatch [regex]::Escape("AppImage runtime: $component")) { $problems.Add("THIRD-PARTY-LICENSES-LINUX.txt has no notice for the AppImage runtime component $component") }
    }
    $problems
}

Invoke-Check 'linux: AppImage packaging script rules (layout, forbidden files, glibc, FUSE fallback)' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $script = (Read-Text 'tools\package-linux.sh') + (Read-Text 'tools\linux-policy.sh')
    foreach ($needed in
        'MAX_GLIBC=2.31', 'VERSION_ID="20.04"', 'check-glibc-baseline.sh', 'appimagetool', 'APPIMAGETOOL_SHA256', 'build-appimage-runtime.sh',
        'sha256sum', 'EXPECTED=', 'forbidden name', 'Xbox 360 package', 'local path in file', 'Windows binary', 'Vulkan loader or layer file',
        'GLIBCXX_3.4.32', '.sha256', 'check-glibc-baseline.sh" --max "$MAX_GLIBC" "$OUT"', 'UTF-16 local path in file', 'DEBUG_SAULO in file',
        '--verify-only', 'scan_appdir "$CHECK/squashfs-root"', '--check-image', 'holds_built_libraries', 'objcopy --only-keep-debug', '$NAME-symbols') {
        if ($script.IndexOf($needed, [StringComparison]::Ordinal) -lt 0) { $problems.Add("package-linux.sh has no '$needed'") }
    }
    foreach ($name in 'default.xex', 'TMNT_r.xex', 'user_data', 'logs', 'settings.toml', '*.log', '*.dmp', '*.sav') {
        if ($script.IndexOf($name, [StringComparison]::OrdinalIgnoreCase) -lt 0) { $problems.Add("package-linux.sh does not refuse $name") }
    }
    $problems
}

Invoke-Check 'linux: AppImage runtime pins and the extract-and-run fallback patch' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $env = Read-Text 'patches\appimage-runtime\baseline.env'
    foreach ($key in 'RUNTIME_SHA256', 'RUNTIME_COMMIT') {
        $m = [regex]::Match($env, "(?m)^$key=(\S+)")
        if (-not $m.Success) { $problems.Add("appimage-runtime/baseline.env has no $key") }
        elseif ($key -eq 'RUNTIME_SHA256' -and $m.Groups[1].Value -notmatch '^[0-9a-f]{64}$') { $problems.Add('RUNTIME_SHA256 is not a SHA-256') }
    }
    if ($env -notmatch '(?m)^ALPINE\w*=\S+') { $problems.Add('appimage-runtime/baseline.env pins no Alpine root file system') }
    $patch = Read-Text 'patches\appimage-runtime\0001-extract-and-run-when-fuse-fails.patch'
    foreach ($needed in 'extract', 'fuse') { if ($patch -notmatch "(?i)$needed") { $problems.Add("the runtime patch does not mention $needed") } }
    $builder = Read-Text 'tools\build-appimage-runtime.sh'
    if ($builder -match 'releases/latest|/latest/') { $problems.Add('build-appimage-runtime.sh downloads a floating latest') }
    $platform = Read-Text 'src\platform.cpp'
    if ($platform -notmatch 'getenv\("APPIMAGE"\)') { $problems.Add('the relaunch does not use the APPIMAGE path') }
    $problems
}

Invoke-Check 'linux: tools/check-glibc-baseline.sh version logic (self-test)' {
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if (-not $bash) { return 'NOT RUN: bash is not available' }
    Push-Location $Root
    try { $output = & bash tools/check-glibc-baseline.sh --self-test 2>&1; $code = $LASTEXITCODE } finally { Pop-Location }
    if ($code) { return @($output | ForEach-Object { "$_" }) }
}

Invoke-Check 'linux: tools/linux-policy.sh self-test (release policy on canned readelf output)' {
    if (-not (Get-Command bash -ErrorAction SilentlyContinue)) { return 'NOT RUN: bash is not available' }
    Push-Location $Root
    try { $output = & bash tools/linux-policy.sh --self-test 2>&1; $code = $LASTEXITCODE } finally { Pop-Location }
    if ($code) { return @($output | ForEach-Object { "$_" }) }
}

function Find-LinuxDistro {
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return $null }
    $names = if ($env:TMNT_LINUX_DISTRO) { @($env:TMNT_LINUX_DISTRO) } else {
        @(& wsl.exe --list --quiet 2>$null | ForEach-Object { ("$_" -replace "`0", '').Trim() } | Where-Object { $_ })
    }
    foreach ($name in $names) {
        & wsl.exe -d $name --exec grep -q '20\.04' /etc/os-release 2>$null
        if ($LASTEXITCODE) { continue }
        & wsl.exe -d $name --exec which clang-20 *> $null
        if (-not $LASTEXITCODE) { return $name }
    }
    return $null
}

Invoke-Check 'linux: tools/test-appimage-release.sh on a focal WSL distro (pins, fixture AppDir, refused damage, fixture AppImage with TMNT_APPIMAGE_TOOLS)' {
    $distro = Find-LinuxDistro
    if (-not $distro) { return 'NOT RUN: no Ubuntu 20.04 WSL distribution with clang-20 (tools/setup-linux-build-env.sh), or set TMNT_LINUX_DISTRO' }
    $wslRoot = "$(& wsl.exe -d $distro --exec wslpath -a ($Root -replace '\\', '/'))".Trim()
    $arguments = @('-d', $distro, '--exec', 'env', "TMNT_APPIMAGE_TOOLS=$env:TMNT_APPIMAGE_TOOLS", 'bash', "$wslRoot/tools/test-appimage-release.sh", $wslRoot)
    $output = @(& wsl.exe @arguments 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    Write-Host "   $distro`: $($output | Select-Object -Last 1)"
    foreach ($line in $output) { if ($line -like 'NOT RUN:*') { Write-Host "   $line" -ForegroundColor Yellow } }
    if ($code) { $output | Where-Object { $_ -like 'FAIL:*' -or $_ -like 'ERROR:*' } }
}

Invoke-Check 'syntax: every PowerShell script parses; every shell script passes bash -n' {
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File) {
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$null, [ref]$errors)
        foreach ($e in $errors) { $problems.Add("$($file.Name):$($e.Extent.StartLineNumber): $($e.Message)") }
    }
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if ($bash) {
        Push-Location $Root
        try {
            $scripts = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.sh' -File | ForEach-Object { "tools/$($_.Name)" }) + 'packaging/linux/AppRun'
            foreach ($script in $scripts) {
                $out = & bash -n $script 2>&1
                if ($LASTEXITCODE) { $problems.Add("${script}: $($out -join ' ')") }
            }
        } finally { Pop-Location }
    }
    $problems
}

# ---- Windows and Linux feature parity ----------------------------------------------------------------------------------

# The text of a source file a non-Windows build compiles: every #if defined(_WIN32) branch is taken out.
function Get-NonWindowsSource([string]$Relative) {
    $kept = [System.Collections.Generic.List[string]]::new()
    $stack = [System.Collections.Generic.Stack[object]]::new()
    foreach ($line in (Read-Text $Relative) -split "\r?\n") {
        if ($line -match '^\s*#\s*(if|ifdef|ifndef)\b') {
            $windows = $line -match '^\s*#\s*(if\s+defined\(_WIN32\)(\s*$|\s+&&)|ifdef\s+_WIN32)'
            $notWindows = $line -match '^\s*#\s*(if\s+!defined\(_WIN32\)|ifndef\s+_WIN32)'
            $stack.Push([pscustomobject]@{ Windows = $windows; NotWindows = $notWindows; InElse = $false })
            continue
        }
        if ($line -match '^\s*#\s*(else|elif)\b' -and $stack.Count) { $stack.Peek().InElse = $true; continue }
        if ($line -match '^\s*#\s*endif\b' -and $stack.Count) { [void]$stack.Pop(); continue }
        $inWindows = $false
        foreach ($frame in $stack) {
            if (($frame.Windows -and -not $frame.InElse) -or ($frame.NotWindows -and $frame.InElse)) { $inWindows = $true }
        }
        if (-not $inWindows) { $kept.Add($line) }
    }
    return $kept -join "`n"
}

# Windows-only sources and the file that does the same job elsewhere.
$PlatformSourcePairs = @{
    'src/online/websocket_win.cpp' = 'src/online/websocket_posix.cpp'
    'src/clipboard_win32.cpp'      = 'src/clipboard_sdl.cpp'
}

# Each player-facing feature: what a Linux build must compile. A genuine platform limit belongs in docs/linux.md, not here.
$ParityFeatures = @(
    @{ Name = 'Launcher'; File = 'src/launcher.cpp'; Needs = 'void PageGraphics', 'static void PageGameplay' }
    @{ Name = 'Graphics card'; File = 'src/graphics_adapters.cpp'; Needs = 'vkEnumeratePhysicalDevices', 'ListGraphicsAdapters' }
    @{ Name = 'Graphics card applied'; File = 'src/settings.cpp'; Needs = 'SetFlagByName(kBackendAdapterCvar' }
    @{ Name = 'Render resolution, MSAA, anti-aliasing, texture filtering'; File = 'src/launcher.cpp'; Needs = '"Render resolution"', '"Anti-aliasing"', '"tmnt_msaa_compatibility"', '"Texture filtering"' }
    @{ Name = 'Languages and Portugues Brasil'; File = 'src/settings.cpp'; Needs = 'void ApplyLanguage()', 'localization::SetActiveLanguage' }
    @{ Name = 'Skip intro'; File = 'src/skip_intro_tmnt_r.cpp'; Needs = 'REX_HOOK_RAW' }
    @{ Name = 'F2 FPS and F3 Ping'; File = 'src/tmnt_turtles_in_time_reshelled_app.h'; Needs = 'FrameCounter', 'Ping' }
    @{ Name = 'Achievements'; File = 'src/achievements_overlay.cpp'; Needs = 'namespace tmnt' }
    @{ Name = 'Menu cleanup'; File = 'src/fire_movie_patch.cpp'; Needs = 'namespace tmnt' }
    @{ Name = 'Online: WebSocket'; File = 'src/online/websocket_posix.cpp'; Needs = 'SSL_connect' }
    @{ Name = 'Online: Direct'; File = 'src/netplay/udp_transport.cpp'; Needs = 'sendto', 'recvfrom' }
    @{ Name = 'Online: friends and invites'; File = 'src/online_page.cpp'; Needs = 'namespace tmnt' }
    @{ Name = 'Detailed logging'; File = 'src/log_filter.cpp'; Needs = 'REXCVAR_DEFINE_BOOL(detailed_logging' }
    @{ Name = 'Open logs folder'; File = 'src/launcher.cpp'; Needs = 'Open logs folder' }
    @{ Name = 'Quit from the launcher'; File = 'src/launcher.cpp'; Needs = 'Finish(cb_.quit)' }
    @{ Name = 'Exit Game from the game'; File = 'src/tmnt_turtles_in_time_reshelled_app.h'; Needs = 'tmnt::StopOnline();', 'void OnClosing' }
    @{ Name = 'Portable user_data'; File = 'src/platform.cpp'; Needs = 'getenv("APPIMAGE")', 'PortFolder()' }
    @{ Name = 'Updater'; File = 'src/update_config.cpp'; Needs = 'UpdatePackageKind::kAppImage', 'HasOriginalAppImage' }
    @{ Name = 'Crash reports'; File = 'src/crash_report.cpp'; Needs = 'InstallPosixHandlers', 'Linux fatal signal' }
    @{ Name = 'Crash reports installed'; File = 'src/platform.cpp'; Needs = 'InstallCrashReporting(', 'Linux crash reporting enabled for current log.' }
    @{ Name = 'Vulkan failure window'; File = 'src/platform.cpp'; Needs = 'Vulkan setup required' }
)

Invoke-Check 'parity: every player-facing feature is compiled on Linux, and every Windows-only source has a counterpart' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $cmake = (Read-Text 'CMakeLists.txt') -replace '\r', ''
    foreach ($block in [regex]::Matches($cmake, '(?s)if\(WIN32\)(.*?)(?=\nelse\(\)|\nendif\(\))')) {
        foreach ($list in [regex]::Matches($block.Groups[1].Value, 'list\(APPEND TMNT_\w*SOURCES([^)]*)\)')) {
            foreach ($m in [regex]::Matches($list.Groups[1].Value, 'src/[A-Za-z0-9_./-]+\.cpp')) {
                if ($m.Value -notin $PlatformSourcePairs.Keys) { $problems.Add("$($m.Value) is built on Windows only and has no counterpart in `$PlatformSourcePairs") }
            }
        }
    }
    foreach ($pair in $PlatformSourcePairs.GetEnumerator()) {
        if ($cmake -notmatch [regex]::Escape($pair.Value)) { $problems.Add("$($pair.Value), the counterpart of $($pair.Key), is not in CMakeLists.txt") }
    }
    foreach ($feature in $ParityFeatures) {
        if ($cmake -notmatch [regex]::Escape($feature.File) -and $feature.File -notlike '*.h') { $problems.Add("$($feature.Name): $($feature.File) is not in CMakeLists.txt") }
        $source = Get-NonWindowsSource $feature.File
        foreach ($needed in $feature.Needs) {
            if (-not $source.Contains($needed)) { $problems.Add("$($feature.Name): the Linux build of $($feature.File) has no '$needed'") }
        }
    }
    $settings = Get-NonWindowsSource 'src/settings.cpp'
    foreach ($m in [regex]::Matches((Read-Text 'src/settings.cpp'), '(?m)^REXCVAR_DEFINE_\w+\((tmnt_\w+)')) {
        if (-not $settings.Contains($m.Value)) { $problems.Add("the setting $($m.Groups[1].Value) is defined for Windows only") }
    }
    $problems
}

Invoke-Check 'parity: Windows-only ReXGlue patches that change what a player sees are declared' {
    $declared = '0040', '0055', '0079', '0080'
    $found = @(Read-PatchManifest | Where-Object { $_.Fields.Count -eq 5 -and $_.Fields[4] -eq 'vulkan-candidate' } | ForEach-Object { $_.Fields[0].Substring(0, 4) })
    foreach ($number in $found) { if ($number -notin $declared) { "$number is a new deferred Vulkan candidate: list it in docs/linux.md and here" } }
    $linuxDoc = Read-Text 'docs\linux.md'
    foreach ($number in $declared) { if ($number -in $found -and $linuxDoc -notmatch $number) { "docs/linux.md does not mention deferred patch $number" } }
}

# ---- Repository hygiene ----------------------------------------------------------------------------------------------

Invoke-Check 'hygiene: no game data, logs, saves, player data or build output in the project files' {
    if (-not $GitAvailable) { return 'NOT RUN: git is not available' }
    $problems = [System.Collections.Generic.List[string]]::new()
    $forbidden = '(^|/)(default\.xex|TMNT_r\.xex|TMNT\.bf|build_log\.txt)$', '(^|/)(user_data|user_data\.migrating|logs|game|dist|build|build-release|scratchpad)/', '\.(log|dmp|pdb|sav|xsh|xpso|bak|tmp|exe|dll|so|AppImage)$', '(^|/)[^/]*renderdoc[^/]*\.(dll|so)$', '(^|/)VkLayer[^/]*\.(json|dll|so)$'
    foreach ($file in $ProjectFiles) {
        $normalized = $file -replace '\\', '/'
        if ($normalized -like 'tools/rexglue/*') { continue }
        foreach ($pattern in $forbidden) { if ($normalized -match $pattern) { $problems.Add("$normalized matches $pattern"); break } }
    }
    $problems
}

# Temporary diagnostics of the current development branch, with how many markers each file may hold: remove a row when
# its file is clean. -Release accepts none of them.
$TemporaryMarkers = @{
}

Invoke-Check 'hygiene: player-facing documents make no stale claims and advertise nothing removed' {
    $problems = [System.Collections.Generic.List[string]]::new()
    $stale = 'early development', 'not known to be playable', 'has not been run', 'has not been built', 'online multiplayer is not supported',
        'internal render', 'ultrawide', 'no online multiplayer', 'or online multiplayer'
    foreach ($document in 'README.md', 'packaging\README.txt', 'docs\linux.md', 'docs\user-data.md', 'docs\project-status.md') {
        $text = (Read-Text $document) -replace '\s+', ' '
        foreach ($phrase in $stale) {
            if ($text.IndexOf($phrase, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $problems.Add("$document says '$phrase'") }
        }
    }
    foreach ($needed in 'SmartScreen', 'F2', 'F3', 'user_data', 'Linux', 'Online') {
        if ((Read-Text 'packaging\README.txt') -notmatch $needed) { $problems.Add("packaging/README.txt does not mention $needed") }
        if ((Read-Text 'README.md') -notmatch $needed) { $problems.Add("README.md does not mention $needed") }
    }
    $problems
}

Invoke-Check 'hygiene: no credentials, packers, or antivirus changes in the project files' {
    if (-not $GitAvailable) { return 'NOT RUN: git is not available' }
    $problems = [System.Collections.Generic.List[string]]::new()
    $credential = '(?i)\b(ADMIN_TOKEN|METRICS_SECRET|CLOUDFLARE_API_TOKEN|CF_API_TOKEN|account_id)\b\s*[:=]\s*["'']?[A-Za-z0-9_-]{16,}', 'ghp_[A-Za-z0-9]{20,}', 'github_pat_[A-Za-z0-9_]{20,}', '-----BEGIN [A-Z ]*PRIVATE KEY-----'
    foreach ($file in $ProjectFiles) {
        $normalized = $file -replace '\\', '/'
        if ($normalized -like 'third_party/*' -or $normalized -like 'tools/rexglue/*' -or $normalized -like 'tests/*' -or $normalized -like 'server/tmnt-lobby/test/*' -or $normalized -eq 'tools/test.ps1') { continue }
        if ([IO.Path]::GetExtension($normalized) -notin ($TextExtensions + '.js', '.html')) { continue }
        $path = Join-Root $normalized
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $text = [IO.File]::ReadAllText($path)
        foreach ($pattern in $credential) { if ($text -match $pattern) { $problems.Add("$normalized matches a credential pattern") } }
    }
    foreach ($script in 'tools\package-release.ps1', 'tools\package-linux.sh', 'tools\linux-policy.sh', 'tools\build-updater.ps1') {
        $text = Read-Text $script
        foreach ($word in 'upx', 'virustotal', 'Add-MpPreference', 'Set-MpPreference') {
            if ($text -match "(?i)\b$word\b") { $problems.Add("$script uses $word") }
        }
    }
    $problems
}

Invoke-Check 'hygiene: no temporary markers or local absolute paths in the project files' {
    if (-not $GitAvailable) { return 'NOT RUN: git is not available' }
    $problems = [System.Collections.Generic.List[string]]::new()
    $temporary = [System.Collections.Generic.List[string]]::new()
    $markerAllowed = '^tests/', '^docs/', '^tools/package-release\.ps1$', '^tools/linux-policy\.sh$', '^tools/test.*\.(ps1|sh)$'
    $localPath = ('S:\\+VisualStudio', 'C:\\+Users', '/Users/[A-Za-z]', 'scratchpad', 'AppData[\\/]Local[\\/]Temp') -join '|'
    foreach ($file in $ProjectFiles) {
        $normalized = $file -replace '\\', '/'
        if ($normalized -like 'tools/rexglue/*' -or $normalized -like 'third_party/*' -or $normalized -eq 'tools/test.ps1') { continue }
        if ([IO.Path]::GetExtension($normalized) -notin $TextExtensions) { continue }
        $path = Join-Root $normalized
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $text = [IO.File]::ReadAllText($path)
        if ($text.Contains('DEBUG_SAULO') -and -not ($markerAllowed | Where-Object { $normalized -match $_ })) {
            $markers = [regex]::Matches($text, 'DEBUG_SAULO').Count
            if ($Release -or -not $TemporaryMarkers.ContainsKey($normalized)) { $problems.Add("$normalized has a DEBUG_SAULO marker") }
            elseif ($markers -gt $TemporaryMarkers[$normalized]) { $problems.Add("$normalized has $markers DEBUG_SAULO markers, $($TemporaryMarkers[$normalized]) are listed") }
            else { $temporary.Add($normalized) }
        }
        if ($normalized -notlike 'tests/*' -and $text -match $localPath) { $problems.Add("$normalized names a local path") }
        if ($normalized -notlike 'tests/*' -and $normalized -notlike 'patches/*' -and $text -match '(?m)^\s*(//|#)\s*TODO\b') { $problems.Add("$normalized has a TODO") }
    }
    if (-not $Release) {
        foreach ($listed in $TemporaryMarkers.Keys) { if ($listed -notin $temporary) { $problems.Add("$listed has no DEBUG_SAULO marker left: remove it from `$TemporaryMarkers") } }
        $script:TemporaryMarkerFiles = $temporary.Count
    }
    $problems
}

Invoke-Check 'hygiene: .gitignore keeps game data, player data, logs and build output out' {
    $ignore = Read-Text '.gitignore'
    foreach ($needed in '/game/', 'user_data/', '*.log', '/build/', '/out/', '/dist/', '/tools/rexglue/') {
        if ($ignore.IndexOf($needed, [StringComparison]::Ordinal) -lt 0) { "'.gitignore' does not ignore $needed" }
    }
}

$Node = Get-Command node -ErrorAction SilentlyContinue

Invoke-Check 'online service: Worker tests (node --test, no network)' {
    if (-not $Node) { return 'NOT RUN: node is not installed' }
    Push-Location (Join-Root 'server/tmnt-lobby')
    try {
        & $Node.Source --test 'test/*.test.js' 2>&1 | Select-Object -Last 8 | ForEach-Object { Write-Host "   $_" }
        if ($LASTEXITCODE) { "node --test failed ($LASTEXITCODE)" }
    } finally { Pop-Location }
}

Invoke-Check 'online instances: four isolated local test instances (PrepareOnly, nothing is started)' {
    $script = Join-Root 'tools/test-online-instance.ps1'
    if (-not (Test-Path -LiteralPath $script)) { return 'tools/test-online-instance.ps1 is missing' }
    $output = & $script 2>&1 | Out-String
    if ($LASTEXITCODE) { $output.Trim() -split "`r?`n" | Select-Object -Last 6 }
}

# Real windows and input drivers: Saulo runs these by hand.
$WindowedTests = 'tmnt_shutdown_order_tests'

Invoke-Check 'cpp tests: build tmnt_runtime_tests and run every headless test program (-CppTests <build folder>)' {
    if (-not $CppTests) { return 'NOT RUN: pass -CppTests <a configured build folder> to build and run the C++ tests' }
    $build = if ([IO.Path]::IsPathRooted($CppTests)) { $CppTests } else { Join-Root $CppTests }
    if (-not (Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))) { return "NOT RUN: $build is not a configured CMake build folder" }
    & cmake --build $build --target tmnt_runtime_tests 2>&1 | Select-Object -Last 3 | ForEach-Object { Write-Host "   $_" }
    if ($LASTEXITCODE) { return 'cmake --build --target tmnt_runtime_tests failed' }
    $problems = [System.Collections.Generic.List[string]]::new()
    # The online integration and connection tests run against a local lobby on loopback; without Node they skip.
    $server = $null
    $out = $null
    if ($Node) {
        $port = 18000 + (Get-Random -Maximum 2000)
        $out = Join-Path ([IO.Path]::GetTempPath()) "tmnt-lobby-$port.log"
        $server = Start-Process -FilePath $Node.Source -PassThru -WindowStyle Hidden -RedirectStandardOutput $out -RedirectStandardError "$out.err" `
            -ArgumentList @((Join-Root 'server/tmnt-lobby/local/server.js'), '--host', '127.0.0.1', '--port', "$port", '--db', ':memory:')
        $listening = $false
        for ($i = 0; $i -lt 100 -and -not $listening -and -not $server.HasExited; $i++) {
            try { ([Net.Sockets.TcpClient]::new('127.0.0.1', $port)).Close(); $listening = $true } catch { Start-Sleep -Milliseconds 100 }
        }
        if ($listening) { $env:TMNT_LOBBY_TEST_URL = "ws://127.0.0.1:$port/" } else { $problems.Add('the local lobby did not start') }
    }
    try {
        $ext = if ($IsLinux) { '' } else { '.exe' }
        $programs = @(Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Name -match "^tmnt_.+_tests$([regex]::Escape($ext))$" -and $_.BaseName -notin $WindowedTests })
        foreach ($program in $programs) {
            & $program.FullName *> $null
            # 77: the program had nothing to run against (the integration test without a lobby).
            if ($LASTEXITCODE -and $LASTEXITCODE -ne 77) { $problems.Add("$($program.Name) failed ($LASTEXITCODE)") }
        }
        Write-Host "   $($programs.Count) test programs run$(if ($env:TMNT_LOBBY_TEST_URL) { ' (online tests against a local lobby)' })"
    } finally {
        Remove-Item Env:TMNT_LOBBY_TEST_URL -ErrorAction SilentlyContinue
        if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
        if ($out) { Remove-Item -LiteralPath $out, "$out.err" -ErrorAction SilentlyContinue }
    }
    $problems
}

# ---- Audio client teardown (patch 0086) --------------------------------------------------------------------------------

function Find-CppCompiler {
    foreach ($name in 'clang++-20', 'clang++') {
        $found = Get-Command $name -ErrorAction SilentlyContinue
        if ($found) { return $found.Source }
    }
    if ($IsLinux) { return $null }
    . (Join-Path $PSScriptRoot 'rexruntime-patch.ps1')
    try { $clang = Join-Path (Get-VisualStudioRoot) 'VC\Tools\Llvm\x64\bin\clang++.exe' } catch { return $null }
    if (Test-Path -LiteralPath $clang) { return $clang }
    return $null
}

# Paths and quoted defines go through a response file so every PowerShell version passes them unchanged.
function Invoke-Compiler([string]$Compiler, [string[]]$Arguments, [string]$ResponseFile) {
    $quoted = $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '\\', '/' -replace '"', '\"') + '"' } else { $_ -replace '\\', '/' } }
    [IO.File]::WriteAllLines($ResponseFile, [string[]]$quoted)
    $output = & $Compiler "@$ResponseFile" 2>&1
    return [pscustomobject]@{ Code = $LASTEXITCODE; Output = @($output | ForEach-Object { "$_" }) }
}

$CppCompiler = Find-CppCompiler
$CompilerHelp = 'NOT RUN: no clang++ found (on Windows: Visual Studio with the C++ Clang tools; on Linux: clang++-20)'

Invoke-Check 'Audio teardown regression: tests/audio_client_teardown_tests.cpp, threaded, standalone' {
    if (-not $CppCompiler) { return $CompilerHelp }
    $work = Join-Path ([IO.Path]::GetTempPath()) 'tmnt_audio_teardown_test'
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    New-Item -ItemType Directory $work | Out-Null
    try {
        $program = Join-Path $work $(if ($IsLinux) { 'audio_teardown' } else { 'audio_teardown.exe' })
        $arguments = @('-std=c++20', '-O1', '-pthread', "-DTMNT_PATCH_DIR=`"$($PatchDir -replace '\\', '/')`"",
            (Join-Root 'tests\audio_client_teardown_tests.cpp'), '-o', $program)
        $compiled = Invoke-Compiler $CppCompiler $arguments (Join-Path $work 'build.rsp')
        if ($compiled.Code) { return @('compile failed') + @($compiled.Output | Select-Object -First 8) }
        $log = Join-Path $work 'output.txt'
        $run = Start-Process -FilePath $program -NoNewWindow -PassThru -RedirectStandardOutput $log
        [void]$run.Handle
        if (-not $run.WaitForExit(60000)) {
            Stop-Process -Id $run.Id -Force -ErrorAction SilentlyContinue
            return 'the regression test did not finish within 60 s: a teardown wait is stuck'
        }
        $lines = @(Get-Content -LiteralPath $log)
        if ($run.ExitCode) { return @("the regression test failed ($($run.ExitCode))") + @($lines | Select-Object -Last 8) }
        Write-Host "   $($lines | Select-Object -Last 1)"
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# A test program with no link dependencies: compiled, run with a time limit, thrown away.
function Invoke-FocusedTest([string]$Source, [string[]]$Includes = @(), [string[]]$Extra = @()) {
    if (-not $CppCompiler) { return $CompilerHelp }
    $name = [IO.Path]::GetFileNameWithoutExtension($Source)
    $work = Join-Path ([IO.Path]::GetTempPath()) "tmnt_$name"
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    New-Item -ItemType Directory $work | Out-Null
    try {
        $program = Join-Path $work $(if ($IsLinux) { $name } else { "$name.exe" })
        $arguments = @('-std=c++23', '-O1', '-w', "-DTMNT_ROOT_DIR=`"$($Root -replace '\\', '/')`"", "-I$(Join-Root 'src')") +
            @($Includes | ForEach-Object { "-I$_" }) + $Extra + @((Join-Root $Source), '-o', $program)
        $compiled = Invoke-Compiler $CppCompiler $arguments (Join-Path $work 'build.rsp')
        if ($compiled.Code) { return @('compile failed') + @($compiled.Output | Select-Object -First 8) }
        $log = Join-Path $work 'output.txt'
        $run = Start-Process -FilePath $program -NoNewWindow -PassThru -RedirectStandardOutput $log
        [void]$run.Handle
        if (-not $run.WaitForExit(30000)) {
            Stop-Process -Id $run.Id -Force -ErrorAction SilentlyContinue
            return "$name did not finish within 30 s"
        }
        $lines = @(Get-Content -LiteralPath $log)
        if ($run.ExitCode) { return @("$name failed ($($run.ExitCode))") + @($lines | Where-Object { $_ -match 'FAIL' } | Select-Object -First 8) }
        Write-Host "   $($lines | Select-Object -Last 1)"
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Invoke-Check 'Title music level: no volume change at Press START, tests/title_bgm_level_tests.cpp, standalone' {
    Invoke-FocusedTest 'tests\title_bgm_level_tests.cpp'
}

Invoke-Check 'Skip intro: setting, boot path Off and On, later worlds, game image, tests/skip_intro_tests.cpp, standalone' {
    Invoke-FocusedTest 'tests\skip_intro_tests.cpp'
}

Invoke-Check 'Host keys: F3 Ping, ReXGlue F3/F4 overlays removed, Ping text and layout, tests/host_key_tests.cpp, standalone' {
    $sdkInclude = Join-Root $(if ($IsLinux) { 'tools\rexglue\linux-amd64\include' } else { 'tools\rexglue\win-amd64\include' })
    if (-not (Test-Path -LiteralPath (Join-Path $sdkInclude 'imgui.h'))) { return "NOT RUN: $sdkInclude has no imgui.h: tools\build.ps1 unpacks the pinned SDK" }
    Invoke-FocusedTest 'tests\host_key_tests.cpp' @($sdkInclude, (Join-Root 'third_party\trg-launcher\include'))
}

Invoke-Check 'Aspect ratio: 16:9 only in the launcher, old and invalid values normalized, docs, tests/aspect_ratio_tests.cpp, standalone' {
    Invoke-FocusedTest 'tests\aspect_ratio_tests.cpp' @() @("-DTMNT_SRC_DIR=`"$((Join-Root 'src') -replace '\\', '/')`"")
}

Invoke-Check 'Internal render resolution removed: launcher, setting, source, config, docs, patches 0036-0038, Render resolution 1x-4x, tests/internal_render_removed_tests.cpp, standalone' {
    Invoke-FocusedTest 'tests\internal_render_removed_tests.cpp'
}

Invoke-Check 'Detailed logging: normal log and Detailed-only lines, failures kept, default On, saved choice, pt-BR, tests/log_policy_tests.cpp, standalone' {
    $trg = Join-Root 'third_party\trg-launcher'
    $libraries = if ($IsLinux) { @() } else { @('-luser32', '-lshell32', '-lole32', '-lcomdlg32') }
    Invoke-FocusedTest 'tests\log_policy_tests.cpp' @((Join-Path $trg 'include')) (@(
        (Join-Path $trg 'src\settings.cpp'), (Join-Path $trg 'src\platform.cpp')) + $libraries)
}

Invoke-Check 'Menu cleanup: Leaderboards, Xbox Live Party and Gamer Card removed, rows and footers intact, tests/fire_movie_patch_tests.cpp, standalone' {
    Invoke-FocusedTest 'tests\fire_movie_patch_tests.cpp'
}

Invoke-Check 'Portugues Brasil: registration, live launcher language, tables, font and banks against the game data, tests/localization_tests.cpp, standalone' {
    Invoke-FocusedTest 'tests\localization_tests.cpp'
}

Invoke-Check 'Patched AudioSystem syntax: audio_system.cpp with the whole patch stack, compile only' {
    $tree = $PatchedSdk
    try {
        if (-not $CppCompiler) { return $CompilerHelp }
        if (-not $tree) { return 'NOT RUN: the patch stack did not produce a patched source tree' }
        $thirdparty = Join-Path $SdkRepo 'thirdparty'
        $includes = 'fmt\include', 'spdlog\include', 'sdl3\include', 'simde', 'tomlplusplus\include', 'tracy\public', 'utfcpp\source'
        foreach ($include in $includes) {
            if (-not (Test-Path -LiteralPath (Join-Path $thirdparty $include))) { return "NOT RUN: $(Join-Path $thirdparty $include) is missing. $SdkHelp" }
        }
        $platformDefine = if ($IsLinux) { 'REX_PLATFORM_LINUX=1' } else { 'REX_PLATFORM_WINDOWS=1' }
        $arguments = @('-std=c++23', '-fsyntax-only', '-w', '-march=x86-64-v2', "-D$platformDefine", '-DREXGLUE_BUILD_CONFIG="RelWithDebInfo"',
            '-DREXGLUE_ENABLE_PROFILING', "-I$(Join-Path $tree 'include')", "-I$(Join-Path $tree 'src')")
        if (-not $IsLinux) { $arguments += '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS' }
        $arguments += @($includes | ForEach-Object { "-I$(Join-Path $thirdparty $_)" })
        $source = Join-Path $tree 'src\audio\audio_system.cpp'
        if ([IO.File]::ReadAllText($source) -notmatch 'WaitForCallbackExit') { return 'the patched audio_system.cpp does not hold patch 0086' }
        $checked = Invoke-Compiler $CppCompiler ($arguments + $source) (Join-Path (Split-Path $tree -Parent) 'syntax.rsp')
        if ($checked.Code) { return @($checked.Output | Where-Object { $_ -match 'error' } | Select-Object -First 8) + 'audio_system.cpp does not compile with the patch stack' }
    } finally {
        if ($tree) { Remove-Item -LiteralPath (Split-Path $tree -Parent) -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# ---- Summary ---------------------------------------------------------------------------------------------------------

Write-Host ''
Write-Host '== Summary'
foreach ($result in $Results) {
    $color = switch ($result.Status) { 'PASS' { 'Green' } 'FAIL' { 'Red' } default { 'Yellow' } }
    Write-Host ("{0,-8} {1}{2}" -f $result.Status, $result.Name, $(if ($result.Detail) { " ($($result.Detail))" } else { '' })) -ForegroundColor $color
}
$failed = @($Results | Where-Object Status -eq 'FAIL').Count
$notRun = @($Results | Where-Object Status -eq 'NOT RUN').Count
Write-Host "$($Results.Count) checks: $(@($Results | Where-Object Status -eq 'PASS').Count) passed, $failed failed, $notRun not run"
if ($TemporaryMarkerFiles) { Write-Host "Development gate: $TemporaryMarkerFiles listed files hold temporary DEBUG_SAULO diagnostics; tools\test.ps1 -Release fails until they are removed." -ForegroundColor Yellow }
if ($failed) { exit 1 }
$global:LASTEXITCODE = 0
