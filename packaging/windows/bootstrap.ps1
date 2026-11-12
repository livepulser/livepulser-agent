#Requires -RunAsAdministrator
<#
.SYNOPSIS
Downloads livepulser-agent @VERSION@, checks it against SHA256SUMS and runs its install.ps1.

.DESCRIPTION
Published with each release as install.ps1. From an elevated PowerShell prompt:

  & ([scriptblock]::Create((irm https://github.com/livepulser/livepulser-agent/releases/latest/download/install.ps1))) -MonitorId <monitor-id>

The bundled installer asks for the API key, so the key never appears in the command
line or the PowerShell history. Running it again upgrades the agent.
#>
param(
    [Parameter(Mandatory = $true)][string]$MonitorId,
    [string]$Endpoint = 'https://rs.livepulser.com'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Version = '@VERSION@'
if ($Version.StartsWith('@')) { throw 'This is the unreleased template; download install.ps1 from a release instead.' }

[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$name = "livepulser-agent-$Version-windows-amd64"
$base = "https://github.com/livepulser/livepulser-agent/releases/download/v$Version"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("livepulser-agent-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    Write-Host "==> Downloading livepulser-agent $Version"
    $zip = Join-Path $tmp "$name.zip"
    $sums = Join-Path $tmp 'SHA256SUMS'
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$name.zip" -OutFile $zip
    Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums

    $line = Get-Content $sums | Where-Object { ($_ -split '\s+')[1] -in @("$name.zip", "*$name.zip") } | Select-Object -First 1
    if (-not $line) { throw "$name.zip is not listed in SHA256SUMS" }
    $expected = ($line -split '\s+')[0].ToLowerInvariant()
    $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
    if ($actual -ne $expected) { throw "Checksum mismatch for $name.zip; nothing was installed." }

    Expand-Archive -Path $zip -DestinationPath $tmp -Force
    $installer = Join-Path (Join-Path $tmp $name) 'install.ps1'
    if (-not (Test-Path $installer)) { throw 'The archive has no install.ps1' }
    & $installer -MonitorId $MonitorId -Endpoint $Endpoint
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
