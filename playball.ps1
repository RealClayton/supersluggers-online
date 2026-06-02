# ============================================================================
#               MARIO SUPER SLUGGERS: CUSTOM NETPLAY LAUNCHER
# ============================================================================
# Launches standalone Dolphin with Real Wiimote support.
# Supports BOTH connection methods automatically:
#   - Mayflash DolphinBar (Mode 4) — just plug in and sync
#   - Standard Bluetooth adapter    — pair Wii Remote via Windows Bluetooth
#
# No proxy needed for local input. Each player uses their own real controller.

$ErrorActionPreference = "Stop"

# 1. Path Resolutions
$BaseDir = $PSScriptRoot
$ROMPath = Join-Path $BaseDir "00_ROM\Mario Super Sluggers (USA) (En,Fr,Es).wbfs"
$DolphinPath = Join-Path $BaseDir "dolphin-src\Binary\x64\Dolphin.exe"
$DolphinDir = Split-Path $DolphinPath

Clear-Host
Write-Host "===================================================================" -ForegroundColor Green
Write-Host "        MARIO SUPER SLUGGERS ONLINE: REAL WIIMOTE EDITION          " -ForegroundColor Green
Write-Host "===================================================================" -ForegroundColor Green
Write-Host ""
Write-Host " Controller Setup:" -ForegroundColor Cyan
Write-Host "   Option A: Mayflash DolphinBar (Mode 4) - plug in, sync remote" -ForegroundColor White
Write-Host "   Option B: Standard Bluetooth - pair Wii Remote via Windows BT" -ForegroundColor White
Write-Host "   Both work automatically. Just sync your Wii Remote and play!" -ForegroundColor Yellow
Write-Host ""
Write-Host "-------------------------------------------------------------------" -ForegroundColor Green
Write-Host " Workspace:  $BaseDir" -ForegroundColor Gray
Write-Host " ROM:        $ROMPath" -ForegroundColor Gray
Write-Host " Dolphin:    $DolphinPath" -ForegroundColor Gray
Write-Host "-------------------------------------------------------------------" -ForegroundColor Green

# 2. Assert Prerequisites
if (-not (Test-Path $ROMPath)) {
    Write-Host "[Error] Game WBFS ROM not found at: $ROMPath" -ForegroundColor Red
    Exit 1
}
if (-not (Test-Path $DolphinPath)) {
    Write-Host "[Error] Dolphin.exe not found. Please compile the custom Dolphin fork first." -ForegroundColor Red
    Exit 1
}

# 3. Configure Portable Isolation Profile
Write-Host "[Launcher] Initializing Portable Isolation Profile..." -ForegroundColor Cyan

# Force Dolphin into portable mode
$PortableFile = Join-Path $DolphinDir "portable.txt"
if (-not (Test-Path $PortableFile)) {
    New-Item -Path $PortableFile -ItemType File -Value "" | Out-Null
}

# Write custom Dolphin.ini config
$ConfigDir = Join-Path $DolphinDir "User\Config"
if (-not (Test-Path $ConfigDir)) {
    New-Item -Path $ConfigDir -ItemType Directory -Force | Out-Null
}

# Ask user for their Role and Remote IP
Write-Host "-------------------------------------------------------------------" -ForegroundColor Green
Write-Host " Please select your role for this online versus session:" -ForegroundColor Cyan
Write-Host "  [1] Host   (Player 1 / Home Team - you control Wiimote 1)" -ForegroundColor White
Write-Host "  [2] Client (Player 2 / Away Team - you control Wiimote 2)" -ForegroundColor White
Write-Host ""
$choice = Read-Host "Select role [1 or 2, default is 1]"
$Role = "host"
if ($choice -eq "2") {
    $Role = "client"
    Write-Host "[Selection] Role set to CLIENT (Player 2)" -ForegroundColor Green
} else {
    Write-Host "[Selection] Role set to HOST (Player 1)" -ForegroundColor Green
}

$LastIPFile = Join-Path $BaseDir "last_ip.txt"
$DefaultIP = "127.0.0.1"
if (Test-Path $LastIPFile) {
    $DefaultIP = (Get-Content -Path $LastIPFile -ErrorAction SilentlyContinue).Trim()
    if ([string]::IsNullOrWhiteSpace($DefaultIP)) {
        $DefaultIP = "127.0.0.1"
    }
}

Write-Host ""
$RemoteIP = Read-Host "Enter your friend's IP address [Default: $DefaultIP] (Press Enter to keep)"
if ([string]::IsNullOrWhiteSpace($RemoteIP)) {
    $RemoteIP = $DefaultIP
} else {
    $RemoteIP = $RemoteIP.Trim()
    # Save the new IP for next time
    Set-Content -Path $LastIPFile -Value $RemoteIP
}
Write-Host "[Selection] Remote IP set to $RemoteIP" -ForegroundColor Green
Write-Host "-------------------------------------------------------------------" -ForegroundColor Green

$ROMDir = Join-Path $BaseDir "00_ROM"
$IniFile = Join-Path $ConfigDir "Dolphin.ini"
$IniContent = @"
[Interface]
ConfirmStop = False
UsePanicHandlers = False
AllowOnlyOneInstance = False
[Core]
GFXBackend = D3D11
EnableCheats = True
WiimoteContinuousScanning = True
CPUThread = False
[Display]
AspectRatio = 1
[Wii]
Widescreen = True
SensorBarPosition = 1
[General]
ISOPaths = 1
ISOPath0 = $ROMDir
[GameStateSync]
Enabled = True
Role = $Role
RemoteIP = $RemoteIP
Port = 5556
"@

Set-Content -Path $IniFile -Value $IniContent

# Set controller sources based on Role:
# - Host: Wiimote 1 = Real Wiimote (Source 2), Wiimote 2 = Emulated Wiimote (Source 1)
# - Client: Wiimote 1 = Emulated Wiimote (Source 1), Wiimote 2 = Real Wiimote (Source 2)
if ($Role -eq "host") {
    $Wiimote1Source = 2
    $Wiimote2Source = 1
} else {
    $Wiimote1Source = 1
    $Wiimote2Source = 2
}

$WiimoteIniFile = Join-Path $ConfigDir "WiimoteNew.ini"
$WiimoteIniContent = @"
[Wiimote1]
Source = $Wiimote1Source
[Wiimote2]
Source = $Wiimote2Source
[Wiimote3]
Source = 0
[Wiimote4]
Source = 0
"@

Set-Content -Path $WiimoteIniFile -Value $WiimoteIniContent

Write-Host "[Launcher OK] Configured symmetric inputs for $Role role. Real Wiimote + Network Sync." -ForegroundColor Green

# 3.5 P2P Sync-Launch Handshake (Replicates Dolphin Netplay Launch)
Write-Host ""
if ($Role -eq "client") {
    Write-Host "[Sync] Waiting for Host to boot the match..." -ForegroundColor Yellow
    Write-Host "[Sync] (Make sure your friend runs playball.bat and chooses Host)" -ForegroundColor Gray
    
    $UdpListener = New-Object System.Net.Sockets.UdpClient(5558)
    # Ignore SIO_UDP_CONNRESET (ICMP Port Unreachable) errors so it doesn't crash if the Host is not listening yet
    $IOCONTROL_SIO_UDP_CONNRESET = -1744830452
    $UdpListener.Client.IOControl($IOCONTROL_SIO_UDP_CONNRESET, [byte[]]@(0,0,0,0), $null)
    
    # Send a tiny PING to Remote IP on port 5558 to register our address on a UDP relay server (if present)
    $PingBytes = [System.Text.Encoding]::ASCII.GetBytes("PING")
    $UdpListener.Send($PingBytes, $PingBytes.Length, $RemoteIP, 5558) | Out-Null
    
    $RemoteEndpoint = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
    
    try {
        # Blocks and loops until Host sends the BOOT_GAME signal (ignoring self-sent PINGs)
        while ($true) {
            $BytesReceived = $UdpListener.Receive([ref]$RemoteEndpoint)
            $Message = [System.Text.Encoding]::ASCII.GetString($BytesReceived).Trim()
            if ($Message -eq "BOOT_GAME") {
                Write-Host "[Sync OK] Host signal received! Sync-launching game..." -ForegroundColor Green
                break
            }
        }
    } catch {
        Write-Host "[Warning] Sync signal failed. Launching standalone..." -ForegroundColor Yellow
    } finally {
        $UdpListener.Close()
    }
} else {
    # Host sends the sync signal to Client
    Write-Host "[Sync] Sending boot signal to Client at $RemoteIP..." -ForegroundColor Yellow
    try {
        $UdpSender = New-Object System.Net.Sockets.UdpClient
        $Bytes = [System.Text.Encoding]::ASCII.GetBytes("BOOT_GAME")
        $UdpSender.Send($Bytes, $Bytes.Length, $RemoteIP, 5558) | Out-Null
        $UdpSender.Close()
        Write-Host "[Sync OK] Client signaled successfully! Sync-launching game..." -ForegroundColor Green
    } catch {
        Write-Host "[Warning] Could not signal Client. Make sure they have playball.bat open." -ForegroundColor Yellow
    }
    # Slight sleep to let the client receive and process
    Start-Sleep -Milliseconds 150
}

# 4. Launch Dolphin (Standalone — no netplay mode, no proxy needed)
Write-Host ""
Write-Host "[Launcher] Booting Dolphin with Real Wiimote support..." -ForegroundColor Cyan
Write-Host "[Launcher] Sync your Wii Remote now (press 1+2 or sync button)." -ForegroundColor Yellow
Write-Host "===================================================================" -ForegroundColor Green

$DolphinProcess = Start-Process -FilePath $DolphinPath -ArgumentList "-e `"$ROMPath`"" -PassThru

# 5. Lifecycle Monitoring
try {
    while (-not $DolphinProcess.HasExited) {
        Start-Sleep -Milliseconds 500
    }
}
finally {
    Write-Host ""
    Write-Host "===================================================================" -ForegroundColor Yellow
    Write-Host "[Teardown] Dolphin exited. Session complete." -ForegroundColor Yellow
    Write-Host "===================================================================" -ForegroundColor Yellow
}
