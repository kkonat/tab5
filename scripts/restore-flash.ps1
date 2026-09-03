<#
.SYNOPSIS
    Restore the M5Stack Tab5 (ESP32-P4) stock flash image captured in original_flash/.

.DESCRIPTION
    Extracts the flash image from original_flash/tab5-stock-backup.tar.gz, verifies
    it against the SHA-256 recorded inside the archive, then writes it back to the
    device with esptool.

.PARAMETER Port
    Serial port, e.g. COM16. Auto-detected from the ESP32-P4's USB-Serial-JTAG
    device (VID 303A) when omitted.

.PARAMETER Baud
    Upload baud rate. Defaults to 921600.

.PARAMETER Force
    Skip the confirmation prompt.

.PARAMETER Esptool
    Path to esptool. Falls back to the ESP-IDF 5.4.2 copy, then to PATH.

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
    $idf = 'C:\ESP-IDF\.espressif\python_env\idf5.4_py3.11_env\Scripts\esptool.exe'
    if (Test-Path $idf) { return $idf }
    foreach ($c in @('esptool.exe', 'esptool', 'esptool.py')) {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    Die 'esptool not found. Install it (pip install esptool) or pass -Esptool <path>'
}
$EsptoolBin = Find-Esptool

# --- locate port --------------------------------------------------------------
# The Tab5's ESP32-P4 exposes a native USB-Serial-JTAG device: VID 303A, PID 1001.
function Find-Port {
    $dev = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.DeviceID -match 'VID_303A' -and $_.Name -match 'COM\d+' } |
        Select-Object -First 1
    if ($dev -and $dev.Name -match '(COM\d+)') { return $matches[1] }
    return $null
}

if (-not $Port) {
    $Port = Find-Port
    if (-not $Port) {
        Die 'Could not auto-detect the Tab5. Plug it in (USB-C, data cable) and/or pass -Port COM##'
    }
    Write-Host "auto-detected port: $Port"
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
