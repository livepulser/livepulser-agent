#Requires -RunAsAdministrator
<#
.SYNOPSIS
Installs livepulser-agent as a Windows service running under its own virtual account.

.EXAMPLE
.\install.ps1 -MonitorId 00000000-0000-4000-8000-000000000000

Running it again upgrades the agent. Press Enter at the key prompt to keep the current key.
#>
param(
    [Parameter(Mandatory = $true)][string]$MonitorId,
    [string]$Endpoint = 'https://rs.livepulser.com',
    [string]$Binary = (Join-Path $PSScriptRoot 'livepulser-agent.exe')
)

$ErrorActionPreference = 'Stop'
$Service = 'LivePulserAgent'
$Account = "NT SERVICE\$Service"
$InstallDir = Join-Path $env:ProgramFiles 'LivePulser Agent'
$DataDir = Join-Path $env:ProgramData 'LivePulser'
$StateDir = Join-Path $DataDir 'state'
$Config = Join-Path $DataDir 'agent.conf'
$KeyFile = Join-Path $DataDir 'api-key'
$Exe = Join-Path $InstallDir 'livepulser-agent.exe'
$Utf8 = New-Object System.Text.UTF8Encoding $false

if ($MonitorId -notmatch '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$') {
    throw 'MonitorId must be the monitor UUID shown in the dashboard.'
}
if (-not (Test-Path $Binary)) { throw "Binary not found: $Binary" }

# The current key is only worth keeping for the same monitor.
$installedId = $null
if (Test-Path $Config) {
    $m = Select-String -Path $Config -Pattern '^\s*monitor_id\s*=\s*(\S+)' | Select-Object -Last 1
    if ($m) { $installedId = $m.Matches[0].Groups[1].Value }
}
$keepKey = (Test-Path $KeyFile) -and ((Get-Item $KeyFile).Length -gt 0) -and ($installedId -eq $MonitorId)

$prompt = if ($keepKey) { 'Resource monitor API key (press Enter to keep the current key)' } else { 'Resource monitor API key' }
$secure = Read-Host $prompt -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try { $key = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr).Trim() }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
if ($key -eq '' -and $keepKey) {
    $key = $null
    Write-Host 'Keeping the current API key.'
} elseif ($key -notmatch '^[A-Za-z0-9_-]{43}$') {
    throw 'That does not look like a resource monitor API key.'
}

if (Get-Service $Service -ErrorAction SilentlyContinue) {
    Stop-Service $Service -ErrorAction SilentlyContinue
} else {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $bin = "`"$Exe`" --service --config `"$Config`""
    & sc.exe create $Service binPath= $bin start= delayed-auto obj= $Account DisplayName= 'LivePulser Agent' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe create failed ($LASTEXITCODE)" }
    & sc.exe description $Service 'Reports server resource usage to LivePulser.' | Out-Null
    & sc.exe sidtype $Service unrestricted | Out-Null
    & sc.exe failure $Service reset= 86400 actions= restart/30000/restart/60000/restart/300000 | Out-Null
}

New-Item -ItemType Directory -Force -Path $InstallDir, $DataDir, $StateDir | Out-Null
Copy-Item -Force $Binary $Exe

& icacls.exe $DataDir /inheritance:r /grant:r 'SYSTEM:(OI)(CI)F' 'Administrators:(OI)(CI)F' "${Account}:(RX)" | Out-Null
if (Test-Path $Config) {
    # Keep everything else in the file; point it at this monitor and endpoint.
    $lines = [IO.File]::ReadAllLines($Config) | Where-Object { $_ -notmatch '^\s*(monitor_id|endpoint)\s*=' }
    $lines = @("endpoint=$Endpoint", "monitor_id=$MonitorId") + $lines
    [IO.File]::WriteAllText($Config, (($lines -join "`r`n") + "`r`n"), $Utf8)
} else {
    [IO.File]::WriteAllText($Config, "endpoint=$Endpoint`r`nmonitor_id=$MonitorId`r`napi_key_file=$KeyFile`r`nstate_dir=$StateDir`r`n", $Utf8)
}
if ($key) { [IO.File]::WriteAllText($KeyFile, "$key`r`n", $Utf8) }
$key = $null
& icacls.exe $Config /grant "${Account}:(R)" | Out-Null
& icacls.exe $KeyFile /grant "${Account}:(R)" | Out-Null
& icacls.exe $StateDir /grant "${Account}:(OI)(CI)M" | Out-Null

& $Exe --config $Config --check
if ($LASTEXITCODE -ne 0) { throw "Configuration check failed (exit code $LASTEXITCODE); the service was not started." }

Start-Service $Service
Write-Host "LivePulser Agent is running. Logs: $(Join-Path $StateDir 'agent.log')"
