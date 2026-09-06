<#
.SYNOPSIS
    Find, install and record the ESP-IDF this repo builds against.

.DESCRIPTION
    The one thing that has to happen before anything else in here works, and the
    step that goes wrong most often, because "no ESP-IDF found" has three quite
    different causes and one message:

      - there is no IDF on the machine at all
      - there is one, but not the version this repo wants
      - there is the right one, in a layout the scripts do not look in

    So this reports before it does anything: every IDF it can find, with its
    version, and which one - if any - would be used. What happens next depends
    on what that search turned up. A checkout of the right version that the
    scripts were simply not finding is written into .env.local and that is the
    end of it. Nothing suitable, and it offers to install one, which is a clone
    of a few hundred MB followed by a toolchain download of some GB - so it
    asks first, unless -Yes.

    The version is not a preference. neos/main/idf_component.yml points the
    espressif/usb dependency at $IDF_PATH/components/usb precisely because the
    BSP's own choice will not compile on 5.4.2, and the P4 support this needs
    does not exist before 5.3 - so an install is pinned to the version the repo
    was built against rather than to whatever is current.

    Installing puts the checkout where scripts/env.ps1 already looks - under
    <IDF_TOOLS_PATH>\..\esp-idf\<version>\esp-idf, the layout IDF's own Windows
    installer uses - so the result is found afterwards even by a shell that
    knows nothing about .env.local.

.PARAMETER Version
    The IDF tag to look for and to install. v5.4.2, which is what the firmware
    is built against; there is rarely a reason to change it.

.PARAMETER IdfPath
    Use, or install into, this directory instead of searching. A checkout
    already there is used as it stands.

.PARAMETER Targets
    Chips to install toolchains for. esp32p4 - the Tab5 - by default. "all" is
    several GB more.

.PARAMETER Check
    Report what is installed and stop. Changes nothing, downloads nothing.

.PARAMETER Yes
    Do not ask before cloning and installing.

.PARAMETER Reinstall
    Run the IDF's install script again over a checkout that is already there.
    The way to repair a half-finished toolchain, or to add a target.

.PARAMETER NoWrite
    Leave .env.local alone.

.EXAMPLE
    .\do.ps1 setup-toolchain -Check
.EXAMPLE
    .\do.ps1 setup-toolchain
.EXAMPLE
    .\do.ps1 setup-toolchain -Yes
.EXAMPLE
    .\do.ps1 setup-toolchain -IdfPath D:\esp\v5.4.2\esp-idf
.EXAMPLE
    .\do.ps1 setup-toolchain -Reinstall -Targets esp32p4,esp32c6
#>
[CmdletBinding()]
param(
    [string]   $Version = 'v5.4.2',
    [string]   $IdfPath,
    [string[]] $Targets = @('esp32p4'),
    [switch]   $Check,
    [switch]   $Yes,
    [switch]   $Reinstall,
    [switch]   $NoWrite
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$repo = Split-Path -Parent $PSScriptRoot
$IDF_REPO = 'https://github.com/espressif/esp-idf.git'

function Die($msg) {
    Write-Host "error: $msg" -ForegroundColor Red
    exit 1
}

function Say($text) {
    Write-Host ''
    Write-Host "=== $text " -ForegroundColor Cyan -NoNewline
    Write-Host ('=' * [math]::Max(0, 72 - $text.Length)) -ForegroundColor DarkGray
}

<#
    The version of a checkout, as a tag name.

    version.txt is there in a release tarball and absent from a git clone, which
    is what everybody actually has, so the tag it is checked out at is the
    answer most of the time. A shallow clone still carries the tag it was made
    with. "(unknown)" rather than an empty string, because this ends up in a
    table somebody reads.
#>
function Get-IdfVersion($dir) {
    $vt = Join-Path $dir 'version.txt'
    if (Test-Path $vt) {
        $line = (Get-Content $vt -TotalCount 1).Trim()
        if ($line) { return $line }
    }
    if (Get-Command git -ErrorAction Ignore) {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { $d = & git -C $dir describe --tags 2>$null } finally { $ErrorActionPreference = $prev }
        if ($LASTEXITCODE -eq 0 -and $d) { return $d.Trim() }
    }
    return '(unknown)'
}

<#
    Every IDF checkout this machine appears to have.

    A fixed list of shapes rather than a search of the disk: the installer's own
    layout, the two shapes a hand clone takes, and the places the documentation
    tells people to put one. A checkout somewhere else entirely is what -IdfPath
    is for - finding it would mean walking whole drives, which is not worth the
    minutes it costs.
#>
function Find-IdfCheckouts {
    $bases = @(
        $env:USERPROFILE
        (Split-Path -Parent (Get-NeosToolsPath))
        'C:\'
        'D:\'
        'C:\Espressif'
        (Join-Path $env:USERPROFILE 'Espressif')
    ) | Where-Object { $_ } | Select-Object -Unique

    $patterns = @(
        'esp-idf'            # <base>\esp-idf
        'esp-idf\*\esp-idf'  # <base>\esp-idf\v5.4.2\esp-idf - the installer's
        'esp\esp-idf'        # <base>\esp\esp-idf - the docs' own instructions
        'esp\*\esp-idf'      # <base>\esp\v5.2.1\esp-idf
        'frameworks\*'       # C:\Espressif\frameworks\esp-idf-v5.4.2
    )

    $found = @()
    foreach ($base in $bases) {
        if (-not (Test-Path $base)) { continue }
        foreach ($pat in $patterns) {
            $hits = Resolve-Path (Join-Path $base $pat) -ErrorAction SilentlyContinue
            foreach ($h in $hits) {
                $p = $h.Path
                if (Test-Path (Join-Path $p 'tools\idf.py')) { $found += $p }
            }
        }
    }

    # Whatever the shell or .env.local already names, which may be none of the
    # above and is the one that would actually be used.
    foreach ($named in @($env:IDF_PATH, (Get-NeosSetting 'IDF_PATH'))) {
        if ($named -and (Test-Path (Join-Path $named 'tools\idf.py'))) { $found += $named }
    }

    # By what they point at, not by how they are spelled: the same checkout
    # arrives here as a glob result and again as whatever .env.local calls it,
    # and those two spellings differ in separator often enough to list one
    # directory twice.
    $seen = @{}
    $unique = foreach ($f in $found) {
        $full = (Get-Item -LiteralPath $f).FullName
        if (-not $seen.ContainsKey($full)) { $seen[$full] = $true; $full }
    }
    return @($unique)
}

<#
    Set a key in .env.local, keeping everything else in the file.

    The file is hand-written and full of comments explaining what each key is
    for, so it is edited a line at a time rather than regenerated: an active
    line for the key is replaced in place, and a key that is only there as a
    commented-out example is appended below rather than uncommented, so the
    example stays legible.
#>
function Set-EnvLocalKey {
    param([string]$Path, [string]$Name, [string]$Value)

    # Written exactly as given, native separators and all.
    #
    # The example file uses forward slashes and every script in here takes them,
    # so converting looks tidier and is wrong: IDF_PATH does not stay inside
    # this repo. idf_tools.py matches it as a *string* against the paths in
    # $IDF_TOOLS_PATH\idf-env.json, which are native; a forward-slash IDF_PATH
    # matches nothing, so the install is treated as one with no recorded targets
    # and every toolchain is required. The export then fails on the toolchains
    # for chips that were never installed, exports nothing at all, and the build
    # that follows says it cannot find a compiler - three steps from the cause.
    $Value = $Value -replace '/', '\'

    $lines = @()
    if (Test-Path $Path) { $lines = @(Get-Content $Path) }

    $done = $false
    $out = foreach ($line in $lines) {
        if (-not $done -and $line -match "^\s*$([regex]::Escape($Name))\s*=") {
            $done = $true
            "$Name=$Value"
        } else {
            $line
        }
    }
    $out = @($out)

    if (-not $done) {
        # One banner for the block, not one per key: the second key of a run
        # lands directly under the first rather than starting a section of its
        # own.
        $banner = '# Written by scripts/setup-toolchain.ps1'
        if ($out -notcontains $banner) {
            if ($out.Count -and $out[-1].Trim()) { $out += '' }
            $out += $banner
        }
        $out += "$Name=$Value"
    }

    Set-Content -Path $Path -Value $out -Encoding ascii
}

# --- what is here -------------------------------------------------------------
Say 'looking'

if (-not (Get-Command git -ErrorAction Ignore)) {
    Die 'git is not on PATH, and both finding and installing an IDF need it. https://git-scm.com/download/win'
}

$tools = Get-NeosToolsPath
Write-Host "tools path : $tools" -ForegroundColor DarkGray
Write-Host "wanted     : ESP-IDF $Version, targets $($Targets -join ', ')" -ForegroundColor DarkGray

$checkouts = @()
if ($IdfPath) {
    if (Test-Path (Join-Path $IdfPath 'tools\idf.py')) {
        $checkouts = @((Resolve-Path $IdfPath).Path)
    } else {
        Write-Host "nothing at $IdfPath yet" -ForegroundColor DarkGray
    }
} else {
    $checkouts = Find-IdfCheckouts
}

$seen = @()
foreach ($c in $checkouts) {
    $v = Get-IdfVersion $c
    $match = ($v -eq $Version)
    $seen += [pscustomobject]@{ Path = $c; Version = $v; Match = $match }
}

Write-Host ''
if ($seen) {
    Write-Host 'ESP-IDF checkouts found:'
    foreach ($s in $seen) {
        $mark = if ($s.Match) { '->' } else { '  ' }
        $color = if ($s.Match) { 'Green' } else { 'DarkGray' }
        Write-Host ('  {0} {1,-12} {2}' -f $mark, $s.Version, $s.Path) -ForegroundColor $color
    }
} else {
    Write-Host 'ESP-IDF checkouts found: none' -ForegroundColor Yellow
}

# What the scripts would pick right now, which is the question that matters and
# is not the same question as whether an IDF exists.
$current = Get-NeosIdfPath
Write-Host ''
if ($current) {
    Write-Host "the scripts currently use: $current ($(Get-IdfVersion $current))"
} else {
    Write-Host 'the scripts currently find: nothing' -ForegroundColor Yellow
}

$usable = @($seen | Where-Object { $_.Match } | Select-Object -First 1)
$idf = if ($usable) { $usable[0].Path } else { $null }

if ($Check) {
    Write-Host ''
    if ($idf) {
        Write-Host "$Version is present at $idf" -ForegroundColor Green
        if ($current -ne $idf) {
            Write-Host 'but it is not what the scripts pick - run without -Check to record it' -ForegroundColor Yellow
        }
    } else {
        Write-Host "$Version is not installed - run without -Check to install it" -ForegroundColor Yellow
    }
    exit 0
}

# --- install ------------------------------------------------------------------
$installed_now = $false

if (-not $idf) {
    # Where IDF's own Windows installer puts it, which is also where env.ps1
    # looks unprompted - so this ends up found with or without .env.local.
    $target = if ($IdfPath) { $IdfPath } else {
        Join-Path (Split-Path -Parent $tools) "esp-idf\$Version\esp-idf"
    }

    Say 'install'
    Write-Host "ESP-IDF $Version is not on this machine."
    Write-Host "  clone into : $target"
    Write-Host "  toolchains : $tools"
    Write-Host "  targets    : $($Targets -join ', ')"
    Write-Host '  this downloads several GB and takes a while.' -ForegroundColor Yellow

    $drive = Split-Path -Qualifier $target
    if ($drive) {
        $free = (Get-PSDrive -Name $drive.TrimEnd(':') -ErrorAction SilentlyContinue).Free
        if ($free) {
            $gb = [math]::Round($free / 1GB, 1)
            $color = if ($gb -lt 10) { 'Yellow' } else { 'DarkGray' }
            Write-Host "  free on $drive  : $gb GB (10 GB is a comfortable margin)" -ForegroundColor $color
        }
    }

    if (-not $Yes) {
        Write-Host ''
        $answer = Read-Host 'go ahead? [y/N]'
        if ($answer -notmatch '^(y|yes)$') {
            Write-Host 'nothing done' -ForegroundColor Yellow
            exit 1
        }
    }

    if (Test-Path $target) {
        if (@(Get-ChildItem $target -Force -ErrorAction SilentlyContinue).Count) {
            Die "$target exists and is not empty - move it aside, or pass -IdfPath somewhere else"
        }
    } else {
        New-Item -ItemType Directory -Path $target -Force | Out-Null
    }

    # Long paths, because the IDF's own directory names plus a deep build tree
    # go past 260 characters and the failure that follows names a file rather
    # than the limit that stopped it.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $longpaths = & git config --get core.longpaths 2>$null
    $ErrorActionPreference = $prev
    if ($longpaths -ne 'true') {
        Write-Host 'git core.longpaths is not set - setting it for this user' -ForegroundColor DarkGray
        & git config --global core.longpaths true
    }

    Say "clone $Version"
    # Shallow, and shallow submodules: the full history is some GB of other
    # people's past and nothing here reads it.
    & git clone --branch $Version --depth 1 --recursive --shallow-submodules $IDF_REPO $target
    if ($LASTEXITCODE -ne 0) { Die "git clone failed ($LASTEXITCODE)" }

    Say 'toolchains'
    # install.ps1 reads IDF_TOOLS_PATH from the environment, and this is the one
    # place its value is decided - so set it rather than letting the installer
    # default somewhere the scripts will not look.
    $env:IDF_TOOLS_PATH = $tools
    & (Join-Path $target 'install.ps1') ($Targets -join ',')
    if ($LASTEXITCODE -ne 0) { Die "install.ps1 failed ($LASTEXITCODE)" }

    $idf = $target
    $installed_now = $true

} elseif ($Reinstall) {
    Say 'toolchains'
    $env:IDF_TOOLS_PATH = $tools
    & (Join-Path $idf 'install.ps1') ($Targets -join ',')
    if ($LASTEXITCODE -ne 0) { Die "install.ps1 failed ($LASTEXITCODE)" }
    $installed_now = $true

} else {
    Say 'already installed'
    Write-Host "$Version is at $idf"
    Write-Host 'nothing to download. -Reinstall runs the IDF installer over it again.' -ForegroundColor DarkGray
}

# --- record it ----------------------------------------------------------------
# Even when the checkout sits where env.ps1 would have found it anyway: this
# machine may well have more than one IDF, and naming the one to use is the
# difference between a build and a puzzle.
if (-not $NoWrite) {
    Say '.env.local'
    $envfile = Join-Path $repo '.env.local'
    if (-not (Test-Path $envfile)) {
        $example = Join-Path $repo '.env.local.example'
        if (Test-Path $example) {
            Copy-Item $example $envfile
            Write-Host 'created from .env.local.example' -ForegroundColor DarkGray
        }
    }
    Set-EnvLocalKey $envfile 'IDF_PATH' $idf
    Set-EnvLocalKey $envfile 'IDF_TOOLS_PATH' $tools
    # Echoed as they are written, separators and all: this is the line somebody
    # compares against the file when the build cannot find a compiler.
    Write-Host "IDF_PATH=$($idf -replace '/', '\')"
    Write-Host "IDF_TOOLS_PATH=$($tools -replace '/', '\')"
}

# --- check it works -----------------------------------------------------------
Say 'verify'
$env:IDF_PATH = $idf
$env:IDF_TOOLS_PATH = $tools

$py = Find-NeosIdfTool 'python'
if (-not $py) {
    Die ("no python environment under $tools - the toolchain install did not finish. " +
         'Run this again with -Reinstall.')
}
Write-Host "python : $py" -ForegroundColor DarkGray

# idf.py warns on every run when this is unset, and the warning is the first
# line of its own output. It is the venv root - two levels up from the
# interpreter, which lives in Scripts\ inside it.
$env:IDF_PYTHON_ENV_PATH = Split-Path -Parent (Split-Path -Parent $py)

$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$out = & $py (Join-Path $idf 'tools\idf.py') --version 2>&1 | ForEach-Object { $_.ToString() }
$code = $LASTEXITCODE
$ErrorActionPreference = $prev

if ($code -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    Die "idf.py did not run ($code). Try -Reinstall."
}
# The version line, not whatever idf.py warned about on the way to it.
$ver = @($out | Where-Object { $_ -match 'ESP-IDF\s+v' } | Select-Object -Last 1)
if (-not $ver) { $ver = @($out | Select-Object -Last 1) }
Write-Host "idf.py : $($ver[0].Trim())" -ForegroundColor DarkGray

# The chip's compiler specifically. idf.py runs perfectly well without it, so
# a target that was never installed is otherwise first noticed by a build.
#
# Not Find-NeosIdfTool: that searches the python venv, where esptool and the
# rest live. A cross-compiler is not in there - it is unpacked under the tools
# path, one directory per version.
$gccs = @(Resolve-Path (Join-Path $tools 'tools\riscv32-esp-elf\*\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe') -ErrorAction SilentlyContinue)
if ($gccs) {
    Write-Host "riscv gcc : $($gccs[-1].Path)" -ForegroundColor DarkGray
} else {
    Write-Host ('no riscv32-esp-elf-gcc under ' + (Join-Path $tools 'tools') +
                ' - esp32p4 is not among the installed targets. Run again with ' +
                '-Reinstall.') -ForegroundColor Yellow
}

Write-Host ''
if ($installed_now) {
    Write-Host "ESP-IDF $Version installed at $idf" -ForegroundColor Green
} else {
    Write-Host "ESP-IDF $Version ready at $idf" -ForegroundColor Green
}
Write-Host ''
Write-Host 'Next: .\do.ps1 build-all       (firmware and every app)' -ForegroundColor DarkGray
Write-Host '      .\do.ps1 flash-os       (... then the tablet and the card)' -ForegroundColor DarkGray
exit 0
