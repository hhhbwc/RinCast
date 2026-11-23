<#
  RinCast PC installer: the steps RinCast-<version>-Setup.exe runs, and what a manual install
  from the unpacked release zip or a staged package (dist\RinCast) runs. It asks for
  administrator rights if it needs them.

      powershell -ExecutionPolicy Bypass -File install.ps1 [-InstallDir <path>] [-SkipAudio] [-NoStart] [-AllowPublic]

  What it does:
    1. Copies RinCast Streamer and the SteamVR driver to %ProgramFiles%\RinCast.
    2. Adds Windows Firewall rules so headsets can reach the streamer on Private and
       Domain networks. Public networks stay closed until someone allows them in RinCast
       Streamer (General, Networks), which a reinstall keeps. -AllowPublic allows them now.
    3. Registers the RinCast XR OpenXR runtime, if this is a local build that has it.
    4. Registers the SteamVR driver with vrpathreg (if SteamVR is installed).
    5. Installs the RinCast Speaker / RinCast Microphone audio driver, if this
       package has one. Release packages normally don't.
    6. Creates a Start menu shortcut and an uninstaller entry.

  Only step 1 can stop the install: without its files there is nothing to register. Every step
  after it that the streamer can still do by itself (firewall rules, the two registrations,
  audio, shortcuts) reports what failed and carries on with the rest, so a partial install is
  reported as one instead of as "not installed".
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'RinCast'),
    [switch]$SkipAudio,
    [switch]$NoStart,
    [switch]$AllowPublic,
    [switch]$Elevated  # internal: this is the window that self-elevation opened
)

$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$sentinelName = '.rincast-install'

function Close-Window {
    # The elevated window closes when the script ends; keep the result readable.
    if ($Elevated) { Read-Host 'Press Enter to close this window' | Out-Null }
}

function Step($text) { Write-Host "-> $text" -ForegroundColor Cyan }
# Not Write-Warning: its prefix is translated, and RinCast's setup looks for these.
function Warn($text) { Write-Host "!  $text" -ForegroundColor Yellow }

# Where a failure came from. The setup program quotes only the text after
# "RinCast was not installed: ", so a message that is just a localized Windows string -
# "The system cannot find the file specified" said nothing about which command, on which line,
# with which code, and the firewall step was where it stopped.
function Get-ErrorAt($record) {
    if ($record.InvocationInfo -and $record.InvocationInfo.ScriptName) {
        return "$(Split-Path -Leaf $record.InvocationInfo.ScriptName):$($record.InvocationInfo.ScriptLineNumber)"
    }
    return 'unknown line'
}
function Get-ErrorDetail($e) {
    # A CIM failure carries a reason name on the outer exception and the real Win32 code on the
    # exception inside it, so a number found anywhere in the chain wins over a reason name.
    $reason = ''
    for ($x = $e; $x; $x = $x.InnerException) {
        $code = $x.PSObject.Properties['NativeErrorCode']
        if ($code -and $code.Value) {
            if ($code.Value -is [int] -or $code.Value -is [long] -or $code.Value -is [uint32]) { return "win32=$($code.Value)" }
            if (-not $reason) { $reason = "$($code.Value)" }
        }
    }
    if ($reason) { return "cim=$reason" }
    if ($e -and $e.HResult) { return ('hresult=0x{0:X8}' -f $e.HResult) }
    return 'no error code'
}

$issues = @()
# Runs one step's work. It failing costs that step, nothing else; the streamer's own health
# check reports a closed firewall, and RinCast Streamer re-registers the driver it needs.
function Invoke-Guarded {
    param([string]$what, [scriptblock]$body)
    try {
        & $body | Out-Null
        return $true
    } catch {
        Warn "$what failed [$(Get-ErrorAt $_), $($_.Exception.GetType().Name), $(Get-ErrorDetail $_.Exception)]: $($_.Exception.Message)"
        $script:issues += $what
        return $false
    }
}

trap {
    Write-Host "`nRinCast was not installed: $($_.Exception.Message) [at $(Get-ErrorAt $_), $($_.Exception.GetType().Name), $(Get-ErrorDetail $_.Exception)]" -ForegroundColor Red
    Close-Window
    exit 1
}

# An elevated PowerShell can run as a different account than the person at the
# desktop, and then HKCU and %LOCALAPPDATA% belong to that admin account.
function Get-DesktopUserSid {
    $session = (Get-Process -Id $PID).SessionId
    $shell = Get-CimInstance Win32_Process -Filter "Name='explorer.exe' AND SessionId=$session" | Select-Object -First 1
    if ($shell) {
        $sid = (Invoke-CimMethod -InputObject $shell -MethodName GetOwnerSid).Sid
        if ($sid -and (Test-Path "Registry::HKEY_USERS\$sid")) { return $sid }
    }
    return [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
}

$InstallDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($InstallDir).TrimEnd('\')

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-InstallDir', "`"$InstallDir`"", '-Elevated')
    foreach ($flag in 'SkipAudio', 'NoStart', 'AllowPublic') {
        if ((Get-Variable $flag -ValueOnly).IsPresent) { $argv += "-$flag" }
    }
    $child = Start-Process powershell.exe -Verb RunAs -ArgumentList $argv -Wait -PassThru
    exit $child.ExitCode
}

if (-not (Test-Path (Join-Path $source 'RinCast Streamer.exe'))) {
    throw "RinCast Streamer.exe isn't next to install.ps1. Unpack the whole release zip first."
}

# uninstall.ps1 removes the whole install folder, so never adopt one that holds other files.
$sentinel = Join-Path $InstallDir $sentinelName
if ($InstallDir -eq [IO.Path]::GetPathRoot($InstallDir).TrimEnd('\')) {
    throw "Choose a folder for RinCast, not the root of a drive ($InstallDir)."
}
if ((Test-Path $InstallDir) -and -not (Test-Path $sentinel) -and $InstallDir -ine (Join-Path $env:ProgramFiles 'RinCast') -and
    (Get-ChildItem $InstallDir -Force | Select-Object -First 1)) {
    throw "$InstallDir already contains other files. Choose an empty folder."
}

$versionFile = Join-Path $source 'VERSION'
$version = if (Test-Path $versionFile) { (Get-Content $versionFile -TotalCount 1).Trim() } else { '0.0.0' }

# 1. Files -------------------------------------------------------------------
Step "Copying files to $InstallDir"
# SteamVR keeps RinCast's driver loaded, so it has to close too, or copying the driver fails
# halfway through and leaves old and new files mixed.
$running = @(Get-Process 'RinCast Streamer', 'vrserver', 'vrmonitor', 'vrcompositor', 'vrdashboard', 'vrwebhelper', 'vrstartup' -ErrorAction SilentlyContinue)
if ($running | Where-Object ProcessName -like 'vr*') { Write-Host '   SteamVR is closing to update its RinCast driver.' }
if ($running) {
    $running | Stop-Process -Force
    # Windows keeps the exe locked until the process has really gone.
    $running | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 500
New-Item -ItemType Directory -Force $InstallDir | Out-Null
if ($source.TrimEnd('\') -ine $InstallDir) {
    foreach ($item in 'RinCast Streamer.exe', 'Uninstall RinCast.exe', 'rincast-audio-setup.exe', 'ui', 'driver', 'openxr', 'audio',
        'install.ps1', 'uninstall.ps1', 'LICENSES', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'VERSION') {
        $from = Join-Path $source $item
        if (Test-Path $from) { Copy-Item $from $InstallDir -Recurse -Force }
    }
}
Set-Content -Path $sentinel -Value 'Created by the RinCast installer. uninstall.ps1 removes this folder only if this file exists.' -Encoding Ascii
# A downloaded zip marks every file as coming from the internet.
Get-ChildItem $InstallDir -Recurse -File | Unblock-File
$exe = Join-Path $InstallDir 'RinCast Streamer.exe'

# 2. Firewall ----------------------------------------------------------------
Step 'Adding firewall rules'
# Someone who allowed Public networks in RinCast Streamer keeps that choice.
$hadPublic = $false
$null = Invoke-Guarded 'Reading the Public-network choice' {
    $script:hadPublic = @(Get-NetFirewallRule -DisplayName 'RinCast Streamer' -ErrorAction SilentlyContinue |
        Where-Object { "$($_.Profile)" -match 'Public|Any' }).Count -gt 0
}
$programProfiles = if ($AllowPublic -or $hadPublic) { 'Private', 'Domain', 'Public' } else { 'Private', 'Domain' }
$profiles = 'Private', 'Domain'
$null = Invoke-Guarded 'Replacing the old firewall rules' {
    # A95: read the names first, then delete one at a time. Get-NetFirewallRule piped into
    # Remove-NetFirewallRule deletes out of the enumerator it is still walking, and once the
    # store shifts under it the next MoveNext is the "file not specified" failure. Two identical
    # 'RinCast Streamer' rules were left behind that way, so this also deduplicates on the way in.
    # The rule RinCast Streamer writes itself has the same name, outside the group, and dismissing
    # Windows' firewall prompt leaves "Block" rules for the exe, which win over Allow rules.
    $names = @(Get-NetFirewallRule -Group 'RinCast' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
    $names += @(Get-NetFirewallRule -DisplayName 'RinCast Streamer' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
    $names += @(Get-NetFirewallRule -DisplayName 'rincast streamer.exe' -ErrorAction SilentlyContinue |
        Where-Object Action -eq 'Block' | Select-Object -ExpandProperty Name)
    $left = @()
    foreach ($name in ($names | Where-Object { $_ } | Select-Object -Unique)) {
        try { Remove-NetFirewallRule -Name $name -ErrorAction Stop } catch { $left += "$name ($($_.Exception.Message))" }
    }
    if ($left) { Warn "Old firewall rules could not be removed and stay in place: $($left -join '; ')" }
    New-NetFirewallRule -DisplayName 'RinCast Streamer' -Group 'RinCast' -Direction Inbound -Action Allow `
        -Program $exe -Protocol UDP -Profile $programProfiles -ErrorAction Stop | Out-Null
    New-NetFirewallRule -DisplayName 'RinCast discovery (UDP 38710)' -Group 'RinCast' -Direction Inbound -Action Allow `
        -Protocol UDP -LocalPort 38710 -Profile $profiles -ErrorAction Stop | Out-Null
    New-NetFirewallRule -DisplayName 'RinCast streaming (UDP 38711)' -Group 'RinCast' -Direction Inbound -Action Allow `
        -Protocol UDP -LocalPort 38711 -Profile $profiles -ErrorAction Stop | Out-Null
}
# A95: say it out loud rather than leave a hole. A rule that reported success but never landed,
# or one an old Block rule still covers, looks exactly like a working install from here.
$null = Invoke-Guarded 'Checking the firewall rules took effect' {
    $absent = @('RinCast Streamer', 'RinCast discovery (UDP 38710)', 'RinCast streaming (UDP 38711)') |
        Where-Object { -not @(Get-NetFirewallRule -Group 'RinCast' -DisplayName $_ -ErrorAction SilentlyContinue) }
    if ($absent) { throw "still missing: $($absent -join ', ')" }
    $blocked = @(Get-NetFirewallApplicationFilter -Program $exe -ErrorAction SilentlyContinue |
        Get-NetFirewallRule -ErrorAction SilentlyContinue | Where-Object { $_.Direction -eq 'Inbound' -and $_.Action -eq 'Block' })
    if ($blocked) { throw "$($blocked.Count) inbound Block rule(s) still cover $exe, and those win over the Allow rules" }
}

# 3. OpenXR runtime (local builds only) ---------------------------------------
if (Test-Path (Join-Path $source 'openxr\rincast-openxr.json')) {
    Step 'Registering the RinCast XR OpenXR runtime'
    $null = Invoke-Guarded 'Registering the RinCast XR OpenXR runtime' {
        $manifest = Join-Path $InstallDir 'openxr\rincast-openxr.json'
        $xrKey = 'HKLM:\SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes'
        New-Item -Force $xrKey -ErrorAction Stop | Out-Null
        New-ItemProperty -Path $xrKey -Name $manifest -Value 0 -PropertyType DWord -Force -ErrorAction Stop | Out-Null
    }
}

# 4. SteamVR driver -----------------------------------------------------------
$driverDir = Join-Path $InstallDir 'driver\rincast'
$null = Invoke-Guarded 'Registering the SteamVR driver' {
    $userSid = Get-DesktopUserSid
    $steam = (Get-ItemProperty "Registry::HKEY_USERS\$userSid\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    $vrpathreg = if ($steam) { Join-Path $steam 'steamapps\common\SteamVR\bin\win64\vrpathreg.exe' }
    if ($userSid -ne $id.User.Value) {
        # vrpathreg writes to the profile of the account it runs as, which is the wrong one here.
        Write-Host '   RinCast Streamer registers the SteamVR driver the first time it starts.' -ForegroundColor DarkGray
    } elseif ($vrpathreg -and (Test-Path $vrpathreg)) {
        Step 'Registering the SteamVR driver'
        & $vrpathreg adddriver $driverDir | Out-Null
    } else {
        Write-Host '   SteamVR not found; RinCast Streamer registers the driver once SteamVR is installed.' -ForegroundColor DarkGray
    }
}

# 5. Audio driver -------------------------------------------------------------
if (-not $SkipAudio) {
    if (Test-Path (Join-Path $source 'audio\RinCastAudio.inf')) {
        $inf = Join-Path $InstallDir 'audio\RinCastAudio.inf'
        Step 'Installing RinCast Speaker and RinCast Microphone'
        $null = Invoke-Guarded 'Installing RinCast Speaker and RinCast Microphone' {
            $cert = Join-Path $InstallDir 'audio\RinCastTest.cer'
            if (Test-Path $cert) {
                # Test-signed development build: trust the test certificate on this machine.
                Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\Root -ErrorAction Stop | Out-Null
                Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\TrustedPublisher -ErrorAction Stop | Out-Null
                $bcd = (bcdedit /enum '{current}') -join "`n"
                if ($bcd -notmatch 'testsigning\s+Yes') {
                    Warn ('The audio driver is test-signed. Enable test signing with "bcdedit /set testsigning on", ' +
                        'reboot, and run "rincast-audio-setup.exe install audio\RinCastAudio.inf" again.')
                }
            }
            & (Join-Path $InstallDir 'rincast-audio-setup.exe') install $inf
            switch ($LASTEXITCODE) {
                0 { }
                3010 { Warn 'Restart Windows to finish installing the audio driver.' }
                default { Warn "Audio driver setup failed (exit code $LASTEXITCODE). Audio uses Steam's streaming devices instead, if Steam has installed them." }
            }
        }
    } else {
        Write-Host "   This package has no RinCast audio driver. Audio uses Steam's streaming devices (Steam Streaming Speakers and Steam Streaming Microphone)." -ForegroundColor DarkGray
    }
}

# 6. Shortcuts and uninstall entry --------------------------------------------
Step 'Creating shortcuts'
$null = Invoke-Guarded 'Creating the Start menu shortcut' {
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'RinCast Streamer.lnk'))
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = $InstallDir
    $lnk.Save()
}

$null = Invoke-Guarded 'Registering the uninstall entry' {
    $un = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\RinCast'
    New-Item -Force $un -ErrorAction Stop | Out-Null
    # Apps > Installed apps runs the uninstaller program where there is one (setup packages).
    $uninstaller = Join-Path $InstallDir 'Uninstall RinCast.exe'
    $uninstall = if (Test-Path $uninstaller) { "`"$uninstaller`"" } else { "powershell.exe -ExecutionPolicy Bypass -File `"$InstallDir\uninstall.ps1`"" }
    $sizeKb = [int]((Get-ChildItem $InstallDir -Recurse -File | Measure-Object Length -Sum).Sum / 1KB)
    $props = @{
        DisplayName     = 'RinCast'
        DisplayVersion  = $version
        Publisher       = 'RinCast contributors'
        DisplayIcon     = $exe
        InstallLocation = $InstallDir
        UninstallString = $uninstall
        URLInfoAbout    = 'https://rincast.com'
        EstimatedSize   = $sizeKb
        NoModify        = 1
        NoRepair        = 1
    }
    if (Test-Path $uninstaller) { $props.QuietUninstallString = "`"$uninstaller`" --quiet" }
    foreach ($k in $props.Keys) {
        $type = if ($props[$k] -is [int]) { 'DWord' } else { 'String' }
        New-ItemProperty -Path $un -Name $k -Value $props[$k] -PropertyType $type -Force -ErrorAction Stop | Out-Null
    }
}

if ($issues) {
    Write-Host "`nRinCast $version is installed, with $($issues.Count) step(s) that need a look: $($issues -join '; ')" -ForegroundColor Yellow
} else {
    Write-Host "`nRinCast $version is installed." -ForegroundColor Green
}
# Through Explorer, so the streamer runs as the desktop user and not elevated.
if (-not $NoStart) { Start-Process explorer.exe -ArgumentList "`"$exe`"" }
Close-Window
