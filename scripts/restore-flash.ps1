<#
.SYNOPSIS
    Restore the M5Stack Tab5 (ESP32-P4) stock flash image captured in original_flash/.

.DESCRIPTION
    Extracts the flash image from original_flash/tab5-stock-backup.tar.gz, verifies
    it against the SHA-256 recorded inside the archive, then writes it back to the
    device with esptool.

.PARAMETER Port
    Serial port, e.g. COM16. Auto-detected from the ESP32-P4's USB-Serial-JTAG
    device (VID 303A) when omitted, or taken from NEOS_PORT in .env.local.

.PARAMETER Baud
    Upload baud rate. Defaults to 921600.

.PARAMETER Force
    Skip the confirmation prompt.

.PARAMETER Esptool
    Path to esptool. Falls back to ESPTOOL from .env.local, then to the copy in
    the ESP-IDF virtualenv, then to PATH.

.EXAMPLE
    .\restore-flash.ps1
.EXAMPLE
    .\restore-flash.ps1 -Port COM16 -Force
#>
[CmdletBinding()]
param(
    [string]$Port,
    [int]$Baud = 921600,
    [switch]$Force,
    [string]$Esptool
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'env.ps1')

$Chip      = 'esp32p4'
$Root      = Split-Path -Parent $PSScriptRoot
$Archive   = Join-Path $Root 'original_flash\tab5-stock-backup.tar.gz'
$ImageName = 'tab5-backup-full.bin'
$ShaName   = 'tab5-backup-full.bin.sha256'

function Die($msg) {
    Write-Host "error: $msg" -ForegroundColor Red
    exit 1
}

# --- locate esptool -----------------------------------------------------------
function Find-Esptool {
    if ($Esptool) {
        if (Test-Path $Esptool) { return $Esptool }
        Die "esptool not found at: $Esptool"
    }
    # Where the IDF put it, which is a different path on every machine: the
    # search walks the exported virtualenv and the venvs under the tools path
    # before falling back to PATH. See scripts/env.ps1.
    $found = Find-NeosIdfTool 'esptool'
    if ($found) { return $found }
    Die ('esptool not found. Run this from an IDF-exported shell, set ESPTOOL ' +
         'in .env.local, install it (pip install esptool), or pass -Esptool <path>')
}
$EsptoolBin = Find-Esptool

# --- locate port --------------------------------------------------------------
# The Tab5's ESP32-P4 exposes a native USB-Serial-JTAG device: VID 303A, PID 1001.
if (-not $Port) {
    # NEOS_PORT if the machine has one set, else the board on the bus.
    $Port = Find-NeosPort
    if (-not $Port) {
        Die ('Could not find the Tab5. Plug it in (USB-C, data cable), pass ' +
             '-Port COM##, or set NEOS_PORT in .env.local')
    }
    Write-Host "port: $Port"
}

# --- extract and verify -------------------------------------------------------
if (-not (Test-Path $Archive)) { Die "backup archive not found: $Archive" }

$TempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("tab5-restore-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $TempDir | Out-Null
$Image   = Join-Path $TempDir $ImageName
$ShaFile = Join-Path $TempDir $ShaName

try {
    Write-Host "extracting $(Split-Path -Leaf $Archive) ..."
    # tar.exe ships with Windows 10 1803+ and handles .tar.gz natively.
    & tar.exe -xzf $Archive -C $TempDir $ImageName $ShaName
    if ($LASTEXITCODE -ne 0) { Die "failed to extract $ImageName / $ShaName from $Archive" }
    if (-not (Test-Path $Image)) { Die "$ImageName missing from $Archive" }

    if (Test-Path $ShaFile) {
        $expected = ((Get-Content $ShaFile -Raw).Trim() -split '\s+')[0]
        $actual   = (Get-FileHash $Image -Algorithm SHA256).Hash.ToLower()
        if ($expected.ToLower() -ne $actual) {
            Die "checksum mismatch!`n  expected: $expected`n  actual:   $actual`nThe backup is corrupt - refusing to flash it."
        }
        Write-Host "sha256 verified: $actual"
    } else {
        Write-Warning "$ShaName missing from archive, skipping checksum verification"
    }

    $size = (Get-Item $Image).Length
    Write-Host "image size: $size bytes"

    # --- confirm --------------------------------------------------------------
    if (-not $Force) {
        Write-Host ""
        Write-Host "This will OVERWRITE the entire 16MB flash on the device at $Port." -ForegroundColor Yellow
        Write-Host "Everything currently on it - app, NVS, SPIFFS data - will be lost." -ForegroundColor Yellow
        Write-Host ""
        Write-Host "Note: this image is a raw dump of one specific unit, so its NVS carries that"
        Write-Host "unit's calibration and any Wi-Fi credentials stored at capture time."
        Write-Host ""
        try {
            $reply = Read-Host "Type 'yes' to continue"
        } catch {
            Die 'Cannot prompt for confirmation in a non-interactive session. Re-run with -Force to skip the prompt.'
        }
        if ($reply -ne 'yes') { Write-Host 'aborted.'; exit 1 }
    }

    # --- flash ----------------------------------------------------------------
    Write-Host 'writing flash ...'
    & $EsptoolBin --chip $Chip -p $Port -b $Baud write_flash 0x0 $Image
    if ($LASTEXITCODE -ne 0) { Die "esptool failed with exit code $LASTEXITCODE" }

    Write-Host ''
    Write-Host 'restore complete.' -ForegroundColor Green
}
finally {
    Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
}
