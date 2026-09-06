<#
.SYNOPSIS
    Build and flash the NeOS firmware over USB.

.DESCRIPTION
    idf.py with the environment already set up, which is the only hard part on
    a machine where IDF's own export.ps1 does not run. It finds the IDF, finds
    the virtualenv python, exports the toolchain onto PATH, finds the tablet by
    its USB VID, and then gets out of the way - everything after that is an
    ordinary `idf.py flash`, and its output is idf.py's.

    Firmware only. Apps do not live in flash: they are ELFs on the card, and
    they get there with `upload --app <name>` or `deploy-card`. A change that
    touches both - anything under neos/ and an app at once - wants this first,
    because flashing reboots the tablet.

    The partition table is custom, so a device coming from an older build needs
    this and not just an app image.

.PARAMETER Port
    Serial port, e.g. COM16. Auto-detected from the ESP32-P4's USB-Serial-JTAG
    device (VID 303A) when omitted, or taken from NEOS_PORT in .env.local.

.PARAMETER Baud
    Flashing baud rate. idf.py's own default when omitted.

.PARAMETER Monitor
    Stay attached to the console afterwards. Ctrl-] to leave.

.PARAMETER Target
    What to ask idf.py for instead of "flash" - "build" to compile without
    touching the tablet, "monitor" to just watch, "fullclean" and so on.

.EXAMPLE
    .\do.ps1 flash
.EXAMPLE
    .\do.ps1 flash -Monitor
.EXAMPLE
    .\do.ps1 flash -Port COM16 -Baud 921600
.EXAMPLE
    .\do.ps1 flash -Target build          # no tablet needed
#>
[CmdletBinding()]
param(
    [string]   $Port,
    [int]      $Baud,
    [switch]   $Monitor,
    [string[]] $Target = @('flash')
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

function Die($msg) {
    Write-Host "error: $msg" -ForegroundColor Red
    exit 1
}

$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repo 'neos'
if (-not (Test-Path (Join-Path $project 'CMakeLists.txt'))) {
    Die "no firmware project at $project"
}

# --- the IDF ------------------------------------------------------------------
$idf = Enter-NeosIdfEnv
if (-not $idf) {
    Die ('no ESP-IDF found. Run: .\do.ps1 setup-toolchain -Check - which says ' +
         'whether there is one to point at or one to install. Or set IDF_PATH ' +
         '(and IDF_TOOLS_PATH if the tools are somewhere unusual) in .env.local ' +
         'by hand - see .env.local.example')
}
$py = Find-NeosIdfTool 'python'
if (-not $py) { Die "no IDF python under $(Get-NeosToolsPath)" }

# --- the tablet ---------------------------------------------------------------
# Only when the tablet is actually wanted: -Target build has no port and should
# not fail for the want of one.
$needs_port = @($Target | Where-Object { $_ -match 'flash|monitor' }).Count -gt 0
if ($needs_port -or $Monitor) {
    if (-not $Port) {
        $Port = Find-NeosPort
        if (-not $Port) {
            Die ('could not find the Tab5. Plug it in (USB-C, data cable), pass ' +
                 '-Port COM##, or set NEOS_PORT in .env.local')
        }
    }
    # "16" for COM16 is the obvious thing to type and esptool will not take it.
    if ($Port -match '^\d+$') { $Port = "COM$Port" }
}

# --- run it -------------------------------------------------------------------
$argv = @('-C', $project)
if ($Port) { $argv += @('-p', $Port) }
if ($Baud) { $argv += @('-b', "$Baud") }
$argv += $Target
if ($Monitor -and $Target -notcontains 'monitor') { $argv += 'monitor' }

Write-Host "idf.py $($argv -join ' ')" -ForegroundColor DarkGray
& $py (Join-Path $idf 'tools\idf.py') @argv
exit $LASTEXITCODE
