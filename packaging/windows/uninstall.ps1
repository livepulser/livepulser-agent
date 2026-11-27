#Requires -RunAsAdministrator
<#
.SYNOPSIS
Removes the livepulser-agent service. -Purge also deletes configuration, the API key and state.
#>
param([switch]$Purge)

$ErrorActionPreference = 'Stop'
$Service = 'LivePulserAgent'

if (Get-Service $Service -ErrorAction SilentlyContinue) {
    Stop-Service $Service -ErrorAction SilentlyContinue
    & sc.exe delete $Service | Out-Null
}
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $env:ProgramFiles 'LivePulser Agent')
if ($Purge) {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $env:ProgramData 'LivePulser')
}
Write-Host 'LivePulser Agent removed.'
