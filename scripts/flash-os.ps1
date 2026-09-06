<#
.SYNOPSIS
    Install the lot: build everything, flash the firmware, fill the card.

.DESCRIPTION
    A checkout to a working tablet in one command. Three steps, all of which
    exist on their own and are called here rather than reimplemented:

        build-all     firmware and every app, then abi_check
        flash         the firmware over USB
        deploy-card   the app ELFs and autorun.cfg onto the card

    That order is not arbitrary. Flashing reboots the tablet, so it goes before
    anything that would want it running; and the card is written last because it
    is the step most likely to be skipped - the reader is a separate thing to
    remember, and a tablet with new firmware and last week's apps is a coherent
    state to stop in, while the reverse is not.

    The tablet and the card are both looked for at the start, before the long
    part, so a missing one is a warning you get now rather than a failure you
    get in ten minutes. Neither is fatal up front: plugging the reader in while
    the build runs is the normal way to use this.

.PARAMETER Port
    Serial port, e.g. COM16. Auto-detected from the ESP32-P4's USB-Serial-JTAG
    device (VID 303A) when omitted, or taken from NEOS_PORT in .env.local.

.PARAMETER Baud
    Flashing baud rate. idf.py's own default when omitted.

.PARAMETER Drive
    The card's drive letter. Defaults to NEOS_CARD_DRIVE from .env.local, and to
    G: if that is not set either.

.PARAMETER Autorun
    Which app the card starts on boot. "launcher" when omitted; pass an empty
    string to leave the card's existing autorun.cfg alone.

.PARAMETER Apps
    Build only these apps, by directory name under apps/. All of them when
    omitted. It narrows the build and not the card: deploy-card copies every app
    it finds built, which is the right thing when only one of them changed.

.PARAMETER SkipBuild
    Flash and deploy what is already built. For a second tablet, or after a
    build-all you have already watched.

.PARAMETER NoFlash
    Build and fill the card, but leave the tablet alone.

.PARAMETER NoCard
    Build and flash, but do not touch the card.

.PARAMETER Clean
    fullclean every project before building it.

.PARAMETER Quiet
    One line per project from the build instead of all of idf.py's output. What
    fails is still printed in full.

.PARAMETER Monitor
    Stay on the console when everything is done. Ctrl-] to leave.

.EXAMPLE
    .\do.ps1 flash-os
.EXAMPLE
    .\do.ps1 flash-os -Quiet -Drive E:
.EXAMPLE
    .\do.ps1 flash-os -SkipBuild -Autorun clock
.EXAMPLE
    .\do.ps1 flash-os -NoCard -Monitor
#>
[CmdletBinding()]
param(
    [string]   $Port,
    [int]      $Baud,
    [string]   $Drive,
    [string]   $Autorun = 'launcher',
    [string[]] $Apps,
    [switch]   $SkipBuild,
    [switch]   $NoFlash,
    [switch]   $NoCard,
    [switch]   $Clean,
    [switch]   $Quiet,
    [switch]   $Monitor
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

function Say($text) {
    Write-Host ''
    Write-Host "=== $text " -ForegroundColor Cyan -NoNewline
    Write-Host ('=' * [math]::Max(0, 72 - $text.Length)) -ForegroundColor DarkGray
}

if (-not $Drive) { $Drive = Get-NeosSetting 'NEOS_CARD_DRIVE' 'G:' }

# --- look for the hardware first ----------------------------------------------
# Both of these are wanted at the far end of a build that takes minutes. Saying
# so now costs nothing and is the difference between plugging something in and
# starting over.
if (-not $NoFlash -and -not $Port) {
    $Port = Find-NeosPort
    if (-not $Port) {
        Write-Warning ('no Tab5 found on USB yet - plug it in before the build ends, ' +
                       'or pass -Port COM##')
    }
}
if (-not $NoCard -and -not (Test-Path $Drive)) {
    Write-Warning "$Drive is not mounted yet - put the card in the reader before the build ends"
}

$steps = @()

# --- build --------------------------------------------------------------------
if (-not $SkipBuild) {
    Say 'build'
    $argv = @{}
    if ($Apps)  { $argv['Apps'] = $Apps }
    if ($Clean) { $argv['Clean'] = $true }
    if ($Quiet) { $argv['Quiet'] = $true }

    & (Join-Path $PSScriptRoot 'build-all.ps1') @argv
    if ($LASTEXITCODE -ne 0) {
        Write-Host ''
        Write-Host 'build failed - nothing flashed, card untouched' -ForegroundColor Red
        exit 1
    }
    $steps += 'built'
}

# --- flash --------------------------------------------------------------------
# Before the card, because this reboots the tablet.
if (-not $NoFlash) {
    Say 'flash'
    $argv = @{}
    if ($Port) { $argv['Port'] = $Port }
    if ($Baud) { $argv['Baud'] = $Baud }

    & (Join-Path $PSScriptRoot 'flash.ps1') @argv
    if ($LASTEXITCODE -ne 0) {
        Write-Host ''
        Write-Host 'flash failed - card untouched' -ForegroundColor Red
        exit 1
    }
    $steps += 'flashed'
}

# --- the card -----------------------------------------------------------------
# Last, and allowed to be the one that does not happen: new firmware with the
# apps already on the card is a tablet that boots.
$card_ok = $true
if (-not $NoCard) {
    Say 'card'
    if (-not (Test-Path $Drive)) {
        Write-Host ("$Drive is still not mounted - skipping the card. Put it in and " +
                    "run: .\do.ps1 deploy-card") -ForegroundColor Yellow
        $card_ok = $false
    } else {
        $argv = @{ Drive = $Drive; Autorun = $Autorun }
        & (Join-Path $PSScriptRoot 'deploy-card.ps1') @argv
        $steps += 'card written'
    }
}

# --- done ---------------------------------------------------------------------
Write-Host ''
if ($steps) {
    Write-Host ($steps -join ', ') -ForegroundColor Green
} else {
    Write-Host 'nothing to do' -ForegroundColor Yellow
}

if ($Monitor) {
    Say 'monitor'
    $argv = @{ Target = @('monitor') }
    if ($Port) { $argv['Port'] = $Port }
    & (Join-Path $PSScriptRoot 'flash.ps1') @argv
    exit $LASTEXITCODE
}

if (-not $card_ok) { exit 1 }
exit 0
