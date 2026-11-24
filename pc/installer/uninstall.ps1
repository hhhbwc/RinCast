<#
  Removes the per-user RinCast install: streamer, SteamVR driver registration, shortcuts,
  uninstaller entry and settings. System-wide firewall, OpenXR and audio-driver cleanup is
  only attempted when this script is explicitly run as administrator.

      powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-KeepSettings]
#>
[CmdletBinding()]
param([switch]$KeepSettings)

$ErrorActionPreference = 'Continue'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$userUn = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\RinCast'
$machineUn = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\RinCast'

$installDir = $PSScriptRoot.TrimEnd('\')
$loc = (Get-ItemProperty $userUn -ErrorAction SilentlyContinue).InstallLocation
if (-not $loc -and $isAdmin) { $loc = (Get-ItemProperty $machineUn -ErrorAction SilentlyContinue).InstallLocation }
if ($loc) { $installDir = $loc.TrimEnd('\') }

$running = Get-Process 'RinCast Streamer' -ErrorAction SilentlyContinue
if ($running) {
    $running | Stop-Process -Force
    $running | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}

$streamer = Join-Path $installDir 'RinCast Streamer.exe'
if (Test-Path $streamer) {
    Start-Process $streamer -ArgumentList '--restore-audio-defaults' -Wait -ErrorAction SilentlyContinue
}

if ($isAdmin) {
    $setup = Join-Path $installDir 'rincast-audio-setup.exe'
    if (Test-Path $setup) { & $setup remove | Out-Null }

    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Subject -eq 'CN=RinCast Test Signing' | Remove-Item -ErrorAction SilentlyContinue
    }

    $manifest = Join-Path $installDir 'openxr\rincast-openxr.json'
    $xr = 'HKLM:\SOFTWARE\Khronos\OpenXR\1'
    Remove-ItemProperty -Path "$xr\AvailableRuntimes" -Name $manifest -ErrorAction SilentlyContinue
    $active = (Get-ItemProperty $xr -ErrorAction SilentlyContinue).ActiveRuntime
    if ($active -and ($active -ieq $manifest)) {
        $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
        $steamXr = if ($steam) { Join-Path $steam 'steamapps\common\SteamVR\steamxr_win64.json' }
        if ($steamXr -and (Test-Path $steamXr)) { Set-ItemProperty $xr -Name ActiveRuntime -Value $steamXr }
        else { Remove-ItemProperty $xr -Name ActiveRuntime -ErrorAction SilentlyContinue }
    }

    $names = @(Get-NetFirewallRule -Group 'RinCast' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
    foreach ($display in 'RinCast Streamer', 'RinCast Streamer (USB cable)', 'rincast streamer.exe') {
        $names += @(Get-NetFirewallRule -DisplayName $display -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
    }
    foreach ($name in ($names | Where-Object { $_ } | Select-Object -Unique)) {
        Remove-NetFirewallRule -Name $name -ErrorAction SilentlyContinue
    }
}

$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
$vrpathreg = if ($steam) { Join-Path $steam 'steamapps\common\SteamVR\bin\win64\vrpathreg.exe' }
if ($vrpathreg -and (Test-Path $vrpathreg)) {
    & $vrpathreg removedriver (Join-Path $installDir 'driver\rincast') | Out-Null
}

Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'RinCast Streamer' -ErrorAction SilentlyContinue
$programs = if ($isAdmin) { [Environment]::GetFolderPath('CommonPrograms') } else { [Environment]::GetFolderPath('Programs') }
Remove-Item (Join-Path $programs 'RinCast Streamer.lnk') -ErrorAction SilentlyContinue
$userInstall = (Get-ItemProperty $userUn -ErrorAction SilentlyContinue).InstallLocation
if (-not $userInstall -or $userInstall.TrimEnd('\') -ieq $installDir) { Remove-Item $userUn -Recurse -ErrorAction SilentlyContinue }
if ($isAdmin) {
    $machineInstall = (Get-ItemProperty $machineUn -ErrorAction SilentlyContinue).InstallLocation
    if (-not $machineInstall -or $machineInstall.TrimEnd('\') -ieq $installDir) { Remove-Item $machineUn -Recurse -ErrorAction SilentlyContinue }
}

if (-not $KeepSettings) {
    Remove-Item (Join-Path $env:LOCALAPPDATA 'RinCast') -Recurse -Force -ErrorAction SilentlyContinue
}

$isDriveRoot = $installDir -eq [IO.Path]::GetPathRoot($installDir).TrimEnd('\')
$isDefault = $installDir -ieq (Join-Path $env:LOCALAPPDATA 'Programs\RinCast') -or
    ($isAdmin -and $installDir -ieq (Join-Path $env:ProgramFiles 'RinCast'))
if (-not $isDriveRoot -and ($isDefault -or (Test-Path (Join-Path $installDir '.rincast-install')))) {
    Start-Process cmd.exe -WindowStyle Hidden -ArgumentList "/c timeout /t 2 >nul & rmdir /s /q `"$installDir`""
    Write-Host 'RinCast was removed.' -ForegroundColor Green
} else {
    Write-Host "RinCast was removed, but $installDir was kept because it has no installer marker. Delete it yourself if you don't need it." -ForegroundColor Yellow
}
