<#
.SYNOPSIS
    Machine-local settings for the scripts in this directory.

.DESCRIPTION
    The PowerShell half of scripts/_env.py. Dot-source it and call Get-NeosSetting:

        . (Join-Path $PSScriptRoot 'env.ps1')
        $drive = Get-NeosSetting 'NEOS_CARD_DRIVE' 'G:'

    Paths and device names differ per machine - where the IDF is checked out,
    which COM port the tablet enumerated as, which letter the card reader took.
    None of that belongs in the repo, so it is read from .env.local at the repo
    root (see .env.local.example) and, failing that, worked out at runtime.

    Precedence, highest first: the parameter, the process environment, then
    .env.local, then the fallback. The environment wins over the file so that a
    value exported by idf.py's export script is never silently overridden.
#>

$script:NeosRepo = Split-Path -Parent $PSScriptRoot
$script:NeosEnv = $null

function Read-NeosEnvFile {
    if ($null -ne $script:NeosEnv) { return $script:NeosEnv }

    $script:NeosEnv = @{}
    $path = Join-Path $script:NeosRepo '.env.local'
    if (-not (Test-Path $path)) { return $script:NeosEnv }

    foreach ($line in Get-Content $path) {
        $line = $line.Trim()
        if (-not $line -or $line.StartsWith('#')) { continue }
        # "export KEY=value" too, so the same file can be sourced by a shell.
        if ($line.StartsWith('export ')) { $line = $line.Substring(7).TrimStart() }
        $i = $line.IndexOf('=')
        if ($i -lt 1) { continue }
        $key = $line.Substring(0, $i).Trim()
        $value = $line.Substring($i + 1).Trim()
        if ($value.Length -ge 2 -and $value[0] -eq $value[-1] -and ($value[0] -eq '"' -or $value[0] -eq "'")) {
            $value = $value.Substring(1, $value.Length - 2)
        }
        $script:NeosEnv[$key] = $value
    }
    return $script:NeosEnv
}

<# A setting from the environment, then .env.local, then the default. #>
function Get-NeosSetting {
    param([string]$Name, [string]$Default = $null)

    $fromEnv = [Environment]::GetEnvironmentVariable($Name)
    if ($fromEnv) { return $fromEnv }

    $file = Read-NeosEnvFile
    if ($file.ContainsKey($Name) -and $file[$Name]) { return $file[$Name] }

    return $Default
}

<#
    The Tab5's serial port, by USB VID. The ESP32-P4 exposes a native
    USB-Serial-JTAG device rather than going through a bridge chip, so the port
    is Espressif's own VID (303A) and needs no board database. $null if the
    tablet is not plugged in.
#>
function Find-NeosPort {
    $configured = Get-NeosSetting 'NEOS_PORT'
    if ($configured) { return $configured }

    $dev = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.DeviceID -match 'VID_303A' -and $_.Name -match 'COM\d+' } |
        Select-Object -First 1
    if ($dev -and $dev.Name -match '(COM\d+)') { return $matches[1] }
    return $null
}

<#
    Where the IDF's tools and virtualenvs live. IDF_TOOLS_PATH when the shell
    has been through the export script, else the location the IDF installer
    uses by default, which is per-user and so differs by machine and platform.
#>
function Get-NeosToolsPath {
    $configured = Get-NeosSetting 'IDF_TOOLS_PATH'
    if ($configured) { return $configured }
    return (Join-Path $env:USERPROFILE '.espressif')
}

<#
    An executable that ships in the IDF python environment - esptool, say.
    Searched next to the interpreter idf.py exports, then through every venv
    under the tools path, newest first, then PATH. $null if there is none.
#>
function Find-NeosIdfTool {
    param([string]$Name)

    $candidates = @()
    if ($env:IDF_PYTHON_ENV_PATH) {
        $candidates += (Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\$Name.exe")
    }

    $envs = Join-Path (Get-NeosToolsPath) 'python_env'
    if (Test-Path $envs) {
        # Newest first: idf5.4_py3.11_env sorts after idf5.2_py3.9_env, and a
        # machine with both wants the one the current IDF installed.
        $candidates += Get-ChildItem $envs -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "Scripts\$Name.exe" }
    }

    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

<#
    Where the IDF itself is checked out - the directory with tools/idf.py in
    it. IDF_PATH when the shell has been through the export script, else worked
    out from the tools path: the Windows installer puts .espressif and the
    versioned checkout side by side, so one is found from the other without
    knowing which drive somebody chose. $null if there is no IDF to be found.
#>
function Get-NeosIdfPath {
    $candidates = @()

    $configured = Get-NeosSetting 'IDF_PATH'
    if ($configured) { $candidates += $configured }

    $base = Split-Path -Parent (Get-NeosToolsPath)
    if ($base) {
        # <base>\esp-idf\v5.4.2\esp-idf - the installer's layout, newest first,
        # because a machine that has kept two versions wants the later one.
        $versions = Join-Path $base 'esp-idf'
        if (Test-Path $versions) {
            $candidates += Get-ChildItem $versions -Directory |
                Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName 'esp-idf' }
        }
        # And the two shapes a hand-cloned checkout takes.
        $candidates += $versions
        $candidates += (Join-Path $base 'esp\esp-idf')
    }

    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c 'tools\idf.py'))) { return $c }
    }
    return $null
}

<#
    Put this shell in the state idf.py expects: IDF_PATH, IDF_TOOLS_PATH, and
    the toolchain on PATH.

    IDF ships export.ps1 to do exactly this and it is the thing to use when it
    works - but it picks its virtualenv by the name of whatever `python` is on
    PATH, so a machine whose PATH python is 3.12 and whose IDF installed a 3.11
    env is told the environment is missing. idf_tools.py is the same data one
    layer down and does not care.

    Returns the IDF path, or $null if there is no IDF and the caller should say
    so in its own words. A shell that already has idf.py is left alone.
#>
function Enter-NeosIdfEnv {
    if ($env:IDF_PATH -and (Get-Command 'idf.py' -ErrorAction Ignore)) {
        return $env:IDF_PATH
    }

    $idf = Get-NeosIdfPath
    if (-not $idf) { return $null }

    $py = Find-NeosIdfTool 'python'
    if (-not $py) { return $null }

    $env:IDF_PATH = $idf
    $env:IDF_TOOLS_PATH = Get-NeosToolsPath

    # idf_tools.py chats to stderr about tools it found on PATH and will not be
    # using. That is information, not failure - but a caller with
    # $ErrorActionPreference = 'Stop' turns the first such line into a
    # terminating NativeCommandError as soon as the output is captured, so the
    # preference is stood down for exactly this call and stderr dropped.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $exported = & $py (Join-Path $idf 'tools\idf_tools.py') export --format key-value 2>$null
    } finally {
        $ErrorActionPreference = $prev
    }

    foreach ($line in $exported) {
        if ($line -notmatch '^([A-Za-z_0-9]+)=(.*)$') { continue }
        $name = $Matches[1]
        # The export writes PATH with the old one spliced back in by name.
        $value = $Matches[2] -replace '%PATH%', $env:PATH
        if ($value) { Set-Item -Path "env:$name" -Value $value }
    }
    return $idf
}
