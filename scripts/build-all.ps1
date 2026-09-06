<#
.SYNOPSIS
    Build the firmware and every app, then check the apps against the ABI.

.DESCRIPTION
    One command for the whole repo. The firmware and the apps are separate IDF
    projects - eight of them - and building them by hand is eight `idf.py build`
    runs in eight directories, with a different rule for reading the result in
    each case.

    That different rule is the reason this exists rather than a for-loop. An app
    build always ends in `FAILED: <name>.elf` / `ld returned 1`: apps link
    -nostdlib -shared against the syscall table, so the ordinary firmware ELF
    target can never link, and ninja reports the failure it was always going to
    hit. The target that matters, <name>.app.elf, is finished before it. So an
    app is judged by which targets failed - the firmware ELF alone is the
    expected one, anything else is a real error - and never by the exit code,
    which is non-zero for a clean build and a broken one alike.

    Which apps there are is read from the directories rather than listed here,
    so an app added to apps/ is built without editing this file. lab/ counts as
    well when it is there - the private repo of apps that are not published yet.
    (deploy-card does keep a list, because what goes on a card is a choice.)

    Finally abi_check.py over everything that built, because a missing syscall
    is otherwise first heard about from the loader on the device.

.PARAMETER Apps
    Build only these apps, by directory name under apps/ or lab/. All of them
    when omitted.

.PARAMETER NoFirmware
    Skip neos/ and build only the apps.

.PARAMETER NoApps
    Build only the firmware.

.PARAMETER Clean
    idf.py fullclean in each project first. Slow - it discards the whole build
    directory - but it is the way past a stale CMake cache.

.PARAMETER Quiet
    Keep each project's output to itself unless it fails, leaving just the
    one-line-per-project summary. The output of anything that does fail is
    printed in full.

.EXAMPLE
    .\do.ps1 build-all
.EXAMPLE
    .\do.ps1 build-all -Quiet
.EXAMPLE
    .\do.ps1 build-all -Apps clock,nupogodi
.EXAMPLE
    .\do.ps1 build-all -NoFirmware -Clean
#>
[CmdletBinding()]
param(
    [string[]] $Apps,
    [switch]   $NoFirmware,
    [switch]   $NoApps,
    [switch]   $Clean,
    [switch]   $Quiet,
    [switch]   $NoCcache
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

function Die($msg) {
    Write-Host "error: $msg" -ForegroundColor Red
    exit 1
}

$repo = Split-Path -Parent $PSScriptRoot

# --- what to build ------------------------------------------------------------
$projects = @()

if (-not $NoFirmware) {
    $fw = Join-Path $repo 'neos'
    if (-not (Test-Path (Join-Path $fw 'CMakeLists.txt'))) {
        Die "no firmware project at $fw"
    }
    $projects += [pscustomobject]@{ Name = 'neos'; Path = $fw; IsApp = $false; Elf = $null }
}

if (-not $NoApps) {
    # From the directories, not from a list: an app added to apps/ is built
    # without anybody remembering to come back here.
    #
    # And from lab/ as well when there is one. That is the private repo of
    # apps not ready to publish - gitignored here, cloned separately. An app
    # in it is an ordinary project at the same depth as one under apps/, so
    # there is nothing to do differently except look.
    $found = @()
    foreach ($where in @('apps', 'lab')) {
        $root = Join-Path $repo $where
        $found += @(Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName 'CMakeLists.txt') })
    }
    $found = @($found | Sort-Object Name)

    # An app's name is its directory on the card, so the same name in both
    # places has no answer: -Apps could not say which, and deploy-card would
    # put two ELFs in one place. Say so instead of picking one.
    $clash = @($found | Group-Object Name | Where-Object { $_.Count -gt 1 })
    if ($clash) {
        $what = ($clash | ForEach-Object {
            $dirs = ($_.Group | ForEach-Object { Split-Path (Split-Path $_.FullName -Parent) -Leaf })
            '{0} (in {1})' -f $_.Name, ($dirs -join ' and ')
        }) -join '; '
        Die "the same app name in two places: $what. Rename one."
    }

    if ($Apps) {
        $names = $found | ForEach-Object { $_.Name }
        $unknown = @($Apps | Where-Object { $_ -notin $names })
        if ($unknown) {
            Die ("no such app: {0}. There is: {1}" -f ($unknown -join ', '), ($names -join ', '))
        }
        $found = @($found | Where-Object { $_.Name -in $Apps })
    }

    foreach ($d in $found) {
        $projects += [pscustomobject]@{
            Name  = $d.Name
            Path  = $d.FullName
            IsApp = $true
            # project_elf() in the app's CMakeLists names the ELF after the
            # project, which is the directory name.
            Elf   = Join-Path $d.FullName "build\$($d.Name).app.elf"
        }
    }
}

if (-not $projects) { Die 'nothing to build - -NoFirmware and -NoApps together' }

# --- the IDF ------------------------------------------------------------------
# Same as flash.ps1: IDF's own export script picks its virtualenv by the name of
# whatever python is on PATH, and is wrong whenever that is not the version the
# IDF installed. Enter-NeosIdfEnv goes one layer down and does not care.
$idf = Enter-NeosIdfEnv
if (-not $idf) {
    Die ('no ESP-IDF found. Run: .\do.ps1 setup-toolchain -Check - which says ' +
         'whether there is one to point at or one to install. Or set IDF_PATH ' +
         '(and IDF_TOOLS_PATH if the tools are somewhere unusual) in .env.local ' +
         'by hand - see .env.local.example')
}
$py = Find-NeosIdfTool 'python'
if (-not $py) { Die "no IDF python under $(Get-NeosToolsPath)" }
$idfpy = Join-Path $idf 'tools\idf.py'

# --- ccache -------------------------------------------------------------------
<#
    The reason a run of this does not take an hour.

    Every app is its own IDF project, so each one compiles the whole IDF - about
    a thousand objects and 186 MB of build tree - to link an ELF of a couple of
    kilobytes against libmain.a and nothing else. Seven apps, seven times the
    same work, because the seven sdkconfigs are byte-identical: it is not seven
    different builds, it is one build done seven times.

    ccache collapses that. Compiling the same IDF source for two apps differs in
    exactly two tokens - the -I of the generated config directory, and the
    -fmacro-prefix-map naming the app - and both are absolute paths inside the
    repo, so CCACHE_BASEDIR rewrites them relative to each build directory and
    they come out identical. The first app pays for the IDF; the rest hit cache.

    Two details that are not optional. IDF ships its own ccache and this uses
    that one by path, because a ccache.exe on PATH may be some other install's
    broken shim - and the build stops on it with a message about a missing mingw
    file, three steps from anything to do with this repo. And the launcher is
    baked into CMakeCache at configure time, so a build directory configured
    before ccache was switched on will not use it until it is reconfigured;
    that is checked per project below rather than assumed.
#>
$tools = Get-NeosToolsPath
$ccache = $null
if (-not $NoCcache) {
    $found = @(Resolve-Path (Join-Path $tools 'tools\ccache\*\*\ccache.exe') -ErrorAction SilentlyContinue)
    if ($found) {
        # Newest last, by version and not by name: the directory is the version,
        # and as text "4.10.2" sorts before "4.8".
        $ccache = ($found | Sort-Object {
            $v = Split-Path (Split-Path (Split-Path $_.Path -Parent) -Parent) -Leaf
            try { [version]$v } catch { [version]'0.0' }
        })[-1].Path
        $env:PATH = (Split-Path -Parent $ccache) + ';' + $env:PATH
        $env:IDF_CCACHE_ENABLE = '1'
        $env:CCACHE_BASEDIR = $repo
        # Without this the cache is worth nothing here, which took a measurement
        # to believe: the build carries -ggdb, and with debug info ccache hashes
        # the working directory so that debugger paths stay honest. The working
        # directory is each app's own build tree, so every app misses on every
        # object - basedir rewrites the paths in the arguments and cannot touch
        # this. Turning it off is safe for what these builds produce: an app is
        # linked -Wl,--strip-all -Wl,--strip-debug, so the debug info that would
        # name the wrong directory is not in the ELF that ships.
        $env:CCACHE_NOHASHDIR = '1'
        Write-Host "ccache : $ccache" -ForegroundColor DarkGray
    } else {
        Write-Host "ccache : none under $tools - every app will compile the IDF again" -ForegroundColor Yellow
    }
}

<# ccache's own counters, as a hashtable. Empty if it will not talk. #>
function Get-CcacheStats {
    if (-not $ccache) { return @{} }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $lines = & $ccache --print-stats 2>$null } finally { $ErrorActionPreference = $prev }
    $stats = @{}
    foreach ($line in @($lines)) {
        if ("$line" -match '^(\w+)\s+(\d+)$') { $stats[$Matches[1]] = [int64]$Matches[2] }
    }
    return $stats
}

<#
    idf.py in one project, with its output both kept and - unless -Quiet - put
    on the screen as it arrives.

    Kept because an app's result is read out of which targets ninja reported as
    failed. stderr is merged in because that is where a good deal of the build
    goes, the FAILED lines with it.

    That merge is the reason for the rest of the shape of this. PowerShell 5.1
    wraps every stderr line from a native command in an ErrorRecord: printing
    those directly gives each one the full five-line error decoration, a
    NativeCommandError over a compiler warning, so each is flattened to its text
    and written out as text. And an ErrorRecord arriving under
    $ErrorActionPreference = 'Stop' is terminating whatever the command
    eventually exits with, so the preference is stood down for the call.

    Returns the lines, with the exit status left on the caller's $LASTEXITCODE.
#>
function Invoke-Idf {
    param([string]$Project, [string[]]$Target)

    $argv = @('-C', $Project) + $Target
    Write-Host "idf.py $($argv -join ' ')" -ForegroundColor DarkGray

    $lines = New-Object System.Collections.Generic.List[string]
    $show = -not $Quiet

    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $py $idfpy @argv 2>&1 | ForEach-Object {
            $text = $_.ToString()
            $lines.Add($text)
            if ($show) { Write-Host $text }
        }
    } finally {
        $ErrorActionPreference = $prev
    }
    return $lines.ToArray()
}

<# The targets ninja said it could not build. #>
function Get-FailedTargets {
    param([string[]]$Lines)

    $failed = @()
    foreach ($line in $Lines) {
        # "FAILED: hello.elf " - one or more targets, sometimes with paths.
        if ($line -match '^FAILED:\s*(.+?)\s*$') {
            $failed += ($Matches[1] -split '\s+')
        }
    }
    return $failed
}


# --- build --------------------------------------------------------------------
$results = @()
$started = Get-Date
$stats_before = Get-CcacheStats

foreach ($p in $projects) {
    Write-Host ''
    Write-Host "--- $($p.Name) " -ForegroundColor Cyan -NoNewline
    Write-Host ('-' * [math]::Max(0, 72 - $p.Name.Length)) -ForegroundColor DarkGray

    if ($Clean) {
        Invoke-Idf $p.Path @('fullclean') | Out-Null
    }

    # The compiler launcher is written into CMakeCache when the project is
    # configured, so a tree configured before ccache was switched on goes on
    # compiling without it however the environment is set now. Reconfiguring is
    # cheap - seconds - and doing it only when the cache actually lacks the
    # launcher keeps it off the common path.
    if ($ccache -and -not $Clean) {
        $cache = Join-Path $p.Path 'build\CMakeCache.txt'
        if ((Test-Path $cache) -and
            -not (Select-String -Path $cache -Pattern 'CMAKE_C_COMPILER_LAUNCHER' -Quiet)) {
            Write-Host 'configured without ccache - reconfiguring' -ForegroundColor DarkGray
            Invoke-Idf $p.Path @('reconfigure') | Out-Null
        }
    }

    $out = Invoke-Idf $p.Path @('build')
    $code = $LASTEXITCODE

    if (-not $p.IsApp) {
        # The firmware links like anything else, so its exit code means what it
        # says.
        $ok = ($code -eq 0)
        $note = if ($ok) { 'built' } else { "idf.py exited $code" }
    } else {
        # The expected casualty, and only it: ninja gets as far as the firmware
        # ELF target - which an app has no way to link - after <name>.app.elf is
        # already written. Any other failed target is a build that broke.
        $expected = "$($p.Name).elf"
        $unexpected = @(Get-FailedTargets $out |
            Where-Object { (Split-Path $_ -Leaf) -ne $expected })

        $ok = ($unexpected.Count -eq 0) -and (Test-Path $p.Elf)
        if ($unexpected.Count -gt 0) {
            $note = "failed: $($unexpected -join ', ')"
        } elseif (-not (Test-Path $p.Elf)) {
            $note = "no $(Split-Path $p.Elf -Leaf)"
        } else {
            $kb = [math]::Round((Get-Item $p.Elf).Length / 1KB, 1)
            $note = "$kb KB"
        }
    }

    if (-not $ok -and $Quiet) {
        # Held back above; a failure is the one time somebody wants it.
        $out | ForEach-Object { Write-Host $_ }
    }

    $color = if ($ok) { 'Green' } else { 'Red' }
    Write-Host "$($p.Name): $note" -ForegroundColor $color

    $results += [pscustomobject]@{
        Name = $p.Name; IsApp = $p.IsApp; Ok = $ok; Note = $note; Elf = $p.Elf
    }
}

# --- the ABI check ------------------------------------------------------------
# Everything that produced an ELF in one run, because abi_check prints the
# firmware's ABI and symbol count once for the whole set.
$elfs = @($results | Where-Object { $_.IsApp -and $_.Ok } | ForEach-Object { $_.Elf })
$abi_ok = $true

if ($elfs) {
    Write-Host ''
    Write-Host '--- abi_check ' -ForegroundColor Cyan -NoNewline
    Write-Host ('-' * 66) -ForegroundColor DarkGray
    & $py (Join-Path $PSScriptRoot 'abi_check.py') @elfs
    $abi_ok = ($LASTEXITCODE -eq 0)
}

# --- summary ------------------------------------------------------------------
$elapsed = (Get-Date) - $started
Write-Host ''
Write-Host ('{0} project(s) in {1:mm\:ss}' -f $results.Count, $elapsed)

# What the cache saved, which is the difference between this run and seven
# separate ones. Printed from the counters' movement rather than their totals,
# so it describes this run and not the machine's history.
$stats_after = Get-CcacheStats
if ($stats_after.Count) {
    $delta = {
        param($name)
        [int64]$stats_after[$name] - [int64]$stats_before[$name]
    }
    $hit = (& $delta 'direct_cache_hit') + (& $delta 'preprocessed_cache_hit')
    $miss = & $delta 'cache_miss'
    if (($hit + $miss) -gt 0) {
        $pct = [math]::Round(100 * $hit / ($hit + $miss))
        Write-Host ("  ccache {0} hit, {1} compiled ({2}% cached)" -f $hit, $miss, $pct) -ForegroundColor DarkGray
    }
}
foreach ($r in $results) {
    $mark = if ($r.Ok) { 'ok  ' } else { 'FAIL' }
    $color = if ($r.Ok) { 'Green' } else { 'Red' }
    Write-Host ('  {0} {1,-12} {2}' -f $mark, $r.Name, $r.Note) -ForegroundColor $color
}

$failed = @($results | Where-Object { -not $_.Ok })
if ($failed -or -not $abi_ok) {
    if (-not $failed) { Write-Host '  abi_check rejected a build' -ForegroundColor Red }
    exit 1
}

Write-Host ''
Write-Host 'Next: .\do.ps1 flash          (firmware to the tablet)' -ForegroundColor DarkGray
Write-Host '      .\do.ps1 deploy-card    (apps to the card)' -ForegroundColor DarkGray
exit 0
