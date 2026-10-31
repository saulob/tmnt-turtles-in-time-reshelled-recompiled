<#
.SYNOPSIS
  The headless parity gate for Windows and Linux: every check that needs neither the game, the launcher, a window nor a
  build. Prints one PASS, FAIL or NOT RUN line per check and a summary; exits 1 when a check fails.

.DESCRIPTION
  Checks the ReXGlue patch manifest against the patch files, the names and the documentation, and that both platform
  series apply to the pinned SDK source; the Windows and Linux packaging scripts, the AppImage inputs (desktop file,
  icons, licenses, FUSE-fallback runtime pins), the glibc baseline logic, the release check, and the repository for
  temporary markers, local paths, game data and player data. Nothing is built, launched or published.

  -SdkRepo (or REXGLUE_SDK_REPO) is a clone of the official ReXGlue repository; without it the patch apply check is
  NOT RUN. -CppTests <build folder> additionally builds the tmnt_runtime_tests target there (a configured build) and
  runs every test program; without it that check is NOT RUN. Everything else is static.

.EXAMPLE
  .\tools\test.ps1
  .\tools\test.ps1 -SdkRepo S:\path\to\rexglue-sdk
#>
[CmdletBinding()]
param(
    [string]$SdkRepo = $env:REXGLUE_SDK_REPO,
    [string]$CppTests
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
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

Invoke-Check 'patch apply: the Windows and Linux series apply to the pinned SDK source in order' {
    $baseline = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $PatchDir 'baseline.env')) { if ($line -match '^([A-Z0-9_]+)=(.*)$') { $baseline[$Matches[1]] = $Matches[2] } }
    if ($baseline['SDK_COMMIT'] -notmatch '^[0-9a-f]{40}$') { return 'baseline.env has no SDK_COMMIT' }
    if (-not $GitAvailable) { return 'NOT RUN: git is not available' }
    if (-not $SdkRepo -or -not (Test-Path -LiteralPath $SdkRepo)) { return 'NOT RUN: pass -SdkRepo (a clone of the official ReXGlue repository) or set REXGLUE_SDK_REPO' }
    & git -C $SdkRepo cat-file -e "$($baseline['SDK_COMMIT'])^{commit}" 2>$null
    if ($LASTEXITCODE) { return "NOT RUN: $SdkRepo does not hold commit $($baseline['SDK_COMMIT'])" }
    if (-not (Get-Command tar -ErrorAction SilentlyContinue)) { return 'NOT RUN: tar is not available' }
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($platform in 'windows', 'linux') {
        $work = Join-Path ([IO.Path]::GetTempPath()) "tmnt_patch_apply_$platform"
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
        New-Item -ItemType Directory $work | Out-Null
        try {
            $archive = Join-Path $work 'sdk.tar'
            & git -C $SdkRepo archive --format=tar -o $archive $baseline['SDK_COMMIT']
            $tree = Join-Path $work 'tree'
            New-Item -ItemType Directory $tree | Out-Null
            & tar -xf $archive -C $tree
            & git -C $tree init -q
            $other = if ($platform -eq 'windows') { 'linux' } else { 'windows' }
            $names = [string[]]@(Get-ChildItem -LiteralPath $PatchDir -Filter '*.patch' -File | Where-Object { $_.Name -notlike "*.$other.patch" } | ForEach-Object Name)
            [Array]::Sort($names, [StringComparer]::Ordinal)
            foreach ($name in $names) {
                & git -C $tree apply --whitespace=nowarn (Join-Path $PatchDir $name) 2>&1 | Out-Null
                if ($LASTEXITCODE) { $problems.Add("$name does not apply on the $platform series"); break }
            }
            Write-Host "   $platform series: $($names.Count) patches"
        } finally {
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
        }
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
    foreach ($m in [regex]::Matches($docs, '`(tmnt_(?:aspect_ratio|graphics_adapter|internal_render_scale|language|launcher_[a-z]+|msaa_compatibility|show_[a-z_]+|skip_launcher))`')) {
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
    $linux = Read-Text 'tools\package-linux.sh'
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
    $script = Read-Text 'tools\package-linux.sh'
    foreach ($needed in
        'MAX_GLIBC=2.31', 'VERSION_ID="20.04"', 'check-glibc-baseline.sh', 'appimagetool', 'APPIMAGETOOL_SHA256', 'build-appimage-runtime.sh',
        'sha256sum', 'EXPECTED=', 'forbidden name', 'Xbox 360 package', 'local path in file', 'Windows binary', 'RenderDoc or Vulkan layer file',
        'GLIBCXX_3.4.32', '.sha256', 'check-glibc-baseline.sh" --max "$MAX_GLIBC" "$OUT"', 'UTF-16 local path in file', 'DEBUG_SAULO in file',
        '--verify-only', 'scan_appdir "$CHECK/squashfs-root"') {
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

Invoke-Check 'hygiene: no temporary markers or local absolute paths in the project files' {
    if (-not $GitAvailable) { return 'NOT RUN: git is not available' }
    $problems = [System.Collections.Generic.List[string]]::new()
    $markerAllowed = '^tests/', '^docs/', '^tools/package-release\.ps1$', '^tools/package-linux\.sh$', '^tools/test.*\.ps1$'
    $localPath = ('S:\\+VisualStudio', 'C:\\+Users', '/Users/[A-Za-z]', 'scratchpad', 'AppData[\\/]Local[\\/]Temp') -join '|'
    foreach ($file in $ProjectFiles) {
        $normalized = $file -replace '\\', '/'
        if ($normalized -like 'tools/rexglue/*' -or $normalized -like 'third_party/*' -or $normalized -eq 'tools/test.ps1') { continue }
        if ([IO.Path]::GetExtension($normalized) -notin $TextExtensions) { continue }
        $path = Join-Root $normalized
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $text = [IO.File]::ReadAllText($path)
        if ($text.Contains('DEBUG_SAULO') -and -not ($markerAllowed | Where-Object { $normalized -match $_ })) { $problems.Add("$normalized has a DEBUG_SAULO marker") }
        if ($normalized -notlike 'tests/*' -and $text -match $localPath) { $problems.Add("$normalized names a local path") }
        if ($normalized -notlike 'tests/*' -and $normalized -notlike 'patches/*' -and $text -match '(?m)^\s*(//|#)\s*TODO\b') { $problems.Add("$normalized has a TODO") }
    }
    $problems
}

Invoke-Check 'hygiene: .gitignore keeps game data, player data, logs and build output out' {
    $ignore = Read-Text '.gitignore'
    foreach ($needed in '/game/', 'user_data/', '*.log', '/build/', '/out/', '/dist/', '/tools/rexglue/') {
        if ($ignore.IndexOf($needed, [StringComparison]::Ordinal) -lt 0) { "'.gitignore' does not ignore $needed" }
    }
}

Invoke-Check 'cpp tests: build tmnt_runtime_tests and run every test program (-CppTests <build folder>)' {
    if (-not $CppTests) { return 'NOT RUN: pass -CppTests <a configured build folder> to build and run the C++ tests' }
    $build = if ([IO.Path]::IsPathRooted($CppTests)) { $CppTests } else { Join-Root $CppTests }
    if (-not (Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))) { return "NOT RUN: $build is not a configured CMake build folder" }
    & cmake --build $build --target tmnt_runtime_tests 2>&1 | Select-Object -Last 3 | ForEach-Object { Write-Host "   $_" }
    if ($LASTEXITCODE) { return 'cmake --build --target tmnt_runtime_tests failed' }
    $problems = [System.Collections.Generic.List[string]]::new()
    $ext = if ($IsLinux) { '' } else { '.exe' }
    $programs = @(Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Name -match "^tmnt_.+_tests$([regex]::Escape($ext))$" })
    foreach ($program in $programs) {
        & $program.FullName *> $null
        if ($LASTEXITCODE) { $problems.Add("$($program.Name) failed ($LASTEXITCODE)") }
    }
    Write-Host "   $($programs.Count) test programs run"
    $problems
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
if ($failed) { exit 1 }
$global:LASTEXITCODE = 0
