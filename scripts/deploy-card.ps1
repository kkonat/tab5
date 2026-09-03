<#
.SYNOPSIS
    Copy built apps and autorun.cfg onto a NeOS card.

.DESCRIPTION
    NeOS reads autorun.cfg from the root of the card to decide what to launch,
    and loads apps from /apps/<dir>/. Each app directory needs the ELF named by
    its manifest plus the manifest itself.

    Nothing on the card is deleted: apps not listed here are left alone, so a
    card can carry apps that are not in this repo.

.PARAMETER Drive
    The card's drive letter. Defaults to G:.

.PARAMETER Autorun
    Which app directory NeOS should launch on boot. Defaults to "launcher".
    Pass an empty string to leave an existing autorun.cfg untouched.

.EXAMPLE
    .\scripts\deploy-card.ps1
    .\scripts\deploy-card.ps1 -Drive H: -Autorun hello
#>
[CmdletBinding()]
param(
    [string] $Drive   = 'G:',
    [string] $Autorun = 'launcher'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# app directory on the card  ->  the .app.elf the project builds
$apps = @{
    'launcher' = 'apps\launcher\build\launcher.app.elf'
    'hello'    = 'apps\hello\build\hello.app.elf'
    'matrix'   = 'apps\matrix\build\matrix.app.elf'
}

if (-not (Test-Path $Drive)) {
    throw "$Drive is not mounted - is the card in the reader?"
}

foreach ($name in $apps.Keys) {
    $src = Join-Path $repo $apps[$name]
    if (-not (Test-Path $src)) {
        Write-Warning "$name : not built yet ($($apps[$name])) - skipping"
        continue
    }

    $dest = Join-Path $Drive "apps\$name"
    New-Item -ItemType Directory -Path $dest -Force | Out-Null

    # The manifest names the entry file; the card's convention is app.elf.
    $manifestSrc = Join-Path $repo "apps\$name\manifest.json"
    if (Test-Path $manifestSrc) {
        Copy-Item $manifestSrc (Join-Path $dest 'manifest.json') -Force
        $entry = (Get-Content $manifestSrc -Raw | ConvertFrom-Json).entry
    } else {
        Write-Warning "$name : no manifest.json in the repo, leaving the card's"
        $entry = 'app.elf'
    }
    if (-not $entry) { $entry = 'app.elf' }

    Copy-Item $src (Join-Path $dest $entry) -Force
    $kb = [math]::Round((Get-Item $src).Length / 1KB, 1)
    Write-Host ("{0,-10} -> {1}\{2}  ({3} KB)" -f $name, $dest, $entry, $kb)
}

if ($Autorun) {
    $cfg = @"
# NeOS autorun - what this card starts on boot.
# The value is a directory under /apps.
app = $Autorun
"@
    Set-Content -Path (Join-Path $Drive 'autorun.cfg') -Value $cfg -Encoding ascii
    Write-Host "autorun.cfg -> app = $Autorun"
}

Write-Host ''
Write-Host 'Card contents:'
Get-ChildItem $Drive -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch 'System Volume Information' } |
    ForEach-Object { '  {0,-40} {1,8}' -f $_.FullName, $_.Length }
