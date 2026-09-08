<#
.SYNOPSIS
    Copy built apps and autorun.cfg onto a NeOS card.

.DESCRIPTION
    NeOS reads autorun.cfg from the root of the card to decide what to launch,
    and loads apps from /apps/<dir>/. Each app directory needs the ELF named by
    its manifest plus the manifest itself.

    Nothing on the card is deleted: apps not listed here are left alone, so a
    card can carry apps that are not in this repo.

    What goes on it is the list below, plus everything in lab/ - the private
    repo of apps that are not published yet - when this checkout has one.

.PARAMETER Drive
    The card's drive letter. Defaults to NEOS_CARD_DRIVE from .env.local, and
    to G: if that is not set either - see .env.local.example.

.PARAMETER Autorun
    Which app directory NeOS should launch on boot. Defaults to "launcher".
    Pass an empty string to leave an existing autorun.cfg untouched.

.EXAMPLE
    .\scripts\deploy-card.ps1
    .\scripts\deploy-card.ps1 -Drive E: -Autorun hello
#>
[CmdletBinding()]
param(
    [string] $Drive,
    [string] $Autorun = 'launcher'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot 'env.ps1')

# Which letter a card reader takes is a property of the machine, not of this
# repo, so the default comes from .env.local rather than from a value in here.
if (-not $Drive) { $Drive = Get-NeosSetting 'NEOS_CARD_DRIVE' 'G:' }

# What goes on a card, by app directory name. A list and not a scan of apps/,
# because which of the published apps a card carries is a choice.
$names = @('launcher', 'hello', 'matrix', 'system', 'mandel', 'clock', 'nupogodi',
           'lanscan')

# lab/ is different, and is taken whole. It is the private repo of apps still
# being worked on - gitignored here, and absent from most checkouts. An app is
# in it precisely because it is the one being tried on the tablet, so there is
# no choice left to make. Listing them above is not possible in any case: this
# file is public and they are not.
$names += @(Get-ChildItem (Join-Path $repo 'lab') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'manifest.json') } |
    ForEach-Object { $_.Name } | Sort-Object)

# name -> the project directory that builds it, apps\ first and then lab\.
$apps = @{}
foreach ($name in $names) {
    foreach ($where in @('apps', 'lab')) {
        $proj = Join-Path $repo (Join-Path $where $name)
        if (Test-Path (Join-Path $proj 'CMakeLists.txt')) { $apps[$name] = $proj; break }
    }
    if (-not $apps.ContainsKey($name)) {
        Write-Warning "$name : no project under apps\ or lab\ - skipping"
    }
}

if (-not (Test-Path $Drive)) {
    throw ("$Drive is not mounted - is the card in the reader? Pass -Drive, or " +
           "set NEOS_CARD_DRIVE in .env.local if the reader is not on $Drive.")
}

foreach ($name in ($apps.Keys | Sort-Object)) {
    $src = Join-Path $apps[$name] "build\$name.app.elf"
    if (-not (Test-Path $src)) {
        $rel = $src.Substring($repo.Length).TrimStart('\')
        Write-Warning "$name : not built yet ($rel) - skipping"
        continue
    }

    $dest = Join-Path $Drive "apps\$name"
    New-Item -ItemType Directory -Path $dest -Force | Out-Null

    # The manifest names the entry file; the card's convention is app.elf.
    $manifestSrc = Join-Path $apps[$name] 'manifest.json'
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

    # Data the app reads off the card rather than carries in its image.
    #
    # An app with a `card\` directory gets its contents copied alongside the
    # ELF. So far that is lanscan's oui.bin - a megabyte of IEEE registry that
    # would be absurd compiled into an app on every card, and which the app
    # binary-searches in place. It is generated rather than tracked, so it is
    # normally simply absent, and the app says so instead of failing.
    $data = Join-Path $apps[$name] 'card'
    if (Test-Path $data) {
        foreach ($file in Get-ChildItem $data -File) {
            Copy-Item $file.FullName (Join-Path $dest $file.Name) -Force
            $fkb = [math]::Round($file.Length / 1KB, 1)
            Write-Host ("{0,-10}    {1}  ({2} KB)" -f '', $file.Name, $fkb)
        }
    }
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
