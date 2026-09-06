<#
.SYNOPSIS
    Run one of the scripts in scripts/ with the interpreter it needs.

.DESCRIPTION
    Everything under scripts/ needs the ESP-IDF virtualenv's python rather than
    whatever "python" happens to be on PATH - the system one has no pyserial,
    and the failure ("pyserial is missing") arrives one step after the mistake,
    which is the wrong end to debug it from. This finds the right interpreter
    and gets out of the way:

        .\do.ps1 screencap              # the port is auto-detected
        .\do.ps1 upload --app hello
        .\do.ps1                        # what there is to run

    Anything after the script name is passed through untouched, so the scripts'
    own --help still works: .\do.ps1 screencap --help

    Which python, in order: $env:NEOS_PYTHON, then the environment idf.py
    exports, then the newest venv under $env:IDF_TOOLS_PATH, then a python on
    PATH that can import serial. Any of those can be set in .env.local, which
    is read first - see .env.local.example and the README's "Setting local
    paths".

    It is called do.ps1 and not do because "do" is a PowerShell keyword: a bare
    "do screencap" is a parse error before anything gets to look for a command
    by that name. ".\do.ps1 screencap" is fine, and so is "do.ps1 screencap"
    with the repo root on PATH - the keyword only matches the whole token.

.EXAMPLE
    .\do.ps1 screencap --name mandel
    .\do.ps1 deploy-card -Drive E:
#>

$ErrorActionPreference = 'Stop'

$repo = $PSScriptRoot
$dir = Join-Path $repo 'scripts'

# Machine-local settings: where the IDF lives, which port the tablet is on,
# which letter the card reader took. Gitignored; .env.local.example documents
# the keys. Get-NeosSetting prefers the environment over the file, so a shell
# that has been through IDF's export script keeps the values it put there.
. (Join-Path $dir 'env.ps1')

function Find-Python {
    if ($env:NEOS_PYTHON -and (Test-Path $env:NEOS_PYTHON)) {
        return $env:NEOS_PYTHON
    }
    # Set by the IDF export, so a shell that has already been through that
    # keeps using the same interpreter the build does.
    if ($env:IDF_PYTHON_ENV_PATH) {
        $p = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
        if (Test-Path $p) { return $p }
    }

    $configured = Get-NeosSetting 'NEOS_PYTHON'
    if ($configured -and (Test-Path $configured)) { return $configured }

    # Where the IDF installer puts its toolchains and venvs. Per-user by
    # default, so the fallback is that default rather than any one install;
    # IDF_TOOLS_PATH in the environment or .env.local overrides it.
    $envs = Join-Path (Get-NeosToolsPath) 'python_env'
    if (Test-Path $envs) {
        # Newest first: idf5.4_py3.11_env sorts after idf5.2_py3.9_env, and a
        # machine that has both wants the one the current IDF installed.
        $found = Get-ChildItem $envs -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1
        if ($found) { return $found }
    }

    # A plain python will do if somebody has installed pyserial into it.
    foreach ($name in 'python', 'python3') {
        $cmd = Get-Command $name -ErrorAction Ignore
        if ($cmd) {
            & $cmd.Source -c 'import serial' 2>$null
            if ($LASTEXITCODE -eq 0) { return $cmd.Source }
        }
    }

    throw ("no ESP-IDF python found - looked under $envs. Set IDF_TOOLS_PATH " +
           "(if the IDF is installed elsewhere) or NEOS_PYTHON (the interpreter " +
           "to use) in .env.local, or install pyserial into the python on PATH. " +
           "See .env.local.example.")
}

<# The first line of a script's docstring or its .SYNOPSIS, for the listing. #>
function Get-Summary($path) {
    $lines = Get-Content $path -TotalCount 40
    $marker = if ($path -like '*.py') { '^\s*"""' } else { '\.SYNOPSIS' }
    $seen = $false
    foreach ($line in $lines) {
        if (-not $seen) {
            if ($line -match $marker) { $seen = $true }
            continue
        }
        if ($line.Trim()) { return $line.Trim() }
    }
    return ''
}

function Show-Usage {
    Write-Host 'usage: .\do.ps1 <script> [arguments]'
    Write-Host ''
    Get-ChildItem $dir -File |
        # _env.py and env.ps1 are imported by the others, not run.
        Where-Object { $_.Extension -in '.py', '.ps1' -and
                       -not $_.BaseName.StartsWith('_') -and $_.BaseName -ne 'env' } |
        Sort-Object BaseName |
        ForEach-Object {
            $summary = Get-Summary $_.FullName
            if ($summary.Length -gt 57) { $summary = $summary.Substring(0, 54) + '...' }
            Write-Host ('  {0,-15} {1}' -f $_.BaseName, $summary)
        }
    Write-Host ''
    Write-Host '  Arguments after the name go to the script: .\do.ps1 screencap --help'
}

if ($args.Count -eq 0) {
    Show-Usage
    exit 0
}

$name = $args[0]
# Assigned directly and not out of an if, because a statement block enumerates
# what it emits: a one-element array comes back out of one as a bare string,
# and splatting a string spreads it one character per argument.
$rest = @()
if ($args.Count -gt 1) {
    $rest = @($args[1..($args.Count - 1)])
}

# .py first, then the one PowerShell can run itself. A name given with its
# extension is taken as typed, so `do.ps1 restore-flash.sh` still reaches the
# file it names.
$script = $null
foreach ($candidate in "$name.py", "$name.ps1", $name) {
    $path = Join-Path $dir $candidate
    if (Test-Path $path -PathType Leaf) { $script = $path; break }
}

if (-not $script) {
    Write-Host "no scripts\$name.py or scripts\$name.ps1" -ForegroundColor Red
    Write-Host ''
    Show-Usage
    exit 1
}

switch ([System.IO.Path]::GetExtension($script)) {
    '.py' {
        & (Find-Python) $script @rest
        exit $LASTEXITCODE
    }
    '.ps1' {
        & $script @rest
        exit $LASTEXITCODE
    }
    default {
        Write-Host "do not know how to run $script" -ForegroundColor Red
        exit 1
    }
}
