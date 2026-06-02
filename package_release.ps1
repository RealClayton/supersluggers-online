# ============================================================================
#         MARIO SUPER SLUGGERS NETPLAY: AUTOMATIC RELEASE PACKAGER
# ============================================================================
# This script bundles the compiled isolated Dolphin emulator, launch scripts,
# and setup guidelines into a single portable zip file for your friend.
#
# It automatically excludes source files and unneeded binaries to keep the
# file size small and easy to send.

$ErrorActionPreference = "Stop"

$BaseDir = $PSScriptRoot
$StagingName = "supersluggers-netplay"
$StagingDir = Join-Path $BaseDir $StagingName
$ZipFile = Join-Path $BaseDir "supersluggers-netplay-v4.zip"

Clear-Host
Write-Host "===================================================================" -ForegroundColor Green
Write-Host "         MARIO SUPER SLUGGERS NETPLAY: PACKAGING UTILITY           " -ForegroundColor Green
Write-Host "===================================================================" -ForegroundColor Green
Write-Host ""

# 1. Verify Dolphin compiled binaries exist
$DolphinSourceBin = Join-Path $BaseDir "dolphin-src\Binary\x64"
if (-not (Test-Path (Join-Path $DolphinSourceBin "Dolphin.exe"))) {
    Write-Host "[Error] Dolphin.exe not found at $DolphinSourceBin." -ForegroundColor Red
    Write-Host "Please build the Custom Dolphin Fork before packaging." -ForegroundColor Yellow
    Exit 1
}

# 2. Clean previous staging or zip if exists
if (Test-Path $StagingDir) {
    Write-Host "[Cleanup] Removing existing staging directory..." -ForegroundColor Gray
    Remove-Item -Path $StagingDir -Recurse -Force | Out-Null
}
if (Test-Path $ZipFile) {
    Write-Host "[Cleanup] Removing existing release ZIP file..." -ForegroundColor Gray
    Remove-Item -Path $ZipFile -Force | Out-Null
}

Write-Host "[Packager] Initializing packaging structure..." -ForegroundColor Cyan

# 3. Create release directories
$RomStaging = Join-Path $StagingDir "00_ROM"
$DolphinStaging = Join-Path $StagingDir "dolphin-src\Binary\x64"

New-Item -ItemType Directory -Path $RomStaging -Force | Out-Null
New-Item -ItemType Directory -Path $DolphinStaging -Force | Out-Null

# 4. Copy Dolphin binaries and essential runtime files
Write-Host "[Packager] Copying custom Dolphin binaries (this may take a moment)..." -ForegroundColor Cyan
Copy-Item -Path "$DolphinSourceBin\*" -Destination $DolphinStaging -Recurse -Force | Out-Null

# Clean unneeded debugging/compiled temporary files from Dolphin staging to minimize zip size
Get-ChildItem -Path $DolphinStaging -Include "*.pdb", "*.ilk", "*.exp", "*.lib" -Recurse | Remove-Item -Force

# 5. Copy launch scripts
Write-Host "[Packager] Injecting P2P launch scripts..." -ForegroundColor Cyan
Copy-Item -Path (Join-Path $BaseDir "playball.bat") -Destination $StagingDir -Force | Out-Null
Copy-Item -Path (Join-Path $BaseDir "playball.ps1") -Destination $StagingDir -Force | Out-Null

# 6. Add ROM Placement instructions
$RomInstructionFile = Join-Path $RomStaging "PLACE_MARIO_SUPER_SLUGGERS_ROM_HERE.txt"
$RomInstructions = @"
===========================================================================
                     MARIO SUPER SLUGGERS ROM PLACEMENT
===========================================================================
Please place your NTSC-USA (RMBE01) Mario Super Sluggers Game ROM file here.
The game file can be in .wbfs or .iso format.

Ensure it is named exactly or has the extension of:
"Mario Super Sluggers (USA) (En,Fr,Es).wbfs" (or .iso)

Once placed, run "playball.bat" in the root directory to launch the game!
===========================================================================
"@
Set-Content -Path $RomInstructionFile -Value $RomInstructions

# 7. Add Setup Guide
$ReadmeFile = Join-Path $StagingDir "README_SETUP_GUIDE.txt"
$ReadmeContent = @"
===========================================================================
             MARIO SUPER SLUGGERS NETPLAY: SETUP GUIDE
===========================================================================

Welcome to Mario Super Sluggers Netplay! This package contains a fully isolated,
pre-configured, custom Dolphin emulator set up for frame-delay-free online play.

---------------------------------------------------------------------------
1. QUICK START SETUP
---------------------------------------------------------------------------
Step 1: Obtain a NTSC-USA copy of the Mario Super Sluggers Game ROM.
Step 2: Place the ROM file inside the "00_ROM" folder in this directory.
Step 3: Double click "playball.bat" to start the launch wizard.

---------------------------------------------------------------------------
2. CONTROLLER SETUP
---------------------------------------------------------------------------
You can use a real Wii Remote via two methods:
A. MAYFLASH DOLPHINBAR (RECOMMENDED):
   - Set the DolphinBar to MODE 4.
   - Press the SYNC button on both the bar and your Wii Remote to pair.
   
B. STANDARD BLUETOOTH:
   - Pair your Wii Remote through Windows Bluetooth settings first.
   
Both methods are auto-scanned by the emulator once playball.bat starts the game.

---------------------------------------------------------------------------
3. STARTING A NETPLAY MATCH
---------------------------------------------------------------------------
- One player acts as HOST (Home Team) and the other acts as CLIENT (Away Team).
- Host starts "playball.bat" first, chooses "Host" [1], and enters Client's IP.
- Client starts "playball.bat", chooses "Client" [2], and enters Host's IP.
- The client launcher will block and automatically wait for the host.
- When the Host boots, the Client's game will sync-launch automatically!

Have fun playing ball!
===========================================================================
"@
Set-Content -Path $ReadmeFile -Value $ReadmeContent

# 8. Create ZIP Archive
Write-Host "[Packager] Compressing package into supersluggers-netplay-release.zip..." -ForegroundColor Yellow
Compress-Archive -Path $StagingDir -DestinationPath $ZipFile -Force | Out-Null

# 9. Cleanup Staging folder
Write-Host "[Packager] Cleaning up temporary files..." -ForegroundColor Gray
Remove-Item -Path $StagingDir -Recurse -Force | Out-Null

Write-Host ""
Write-Host "===================================================================" -ForegroundColor Green
Write-Host "       RELEASE PACKAGE GENERATED SUCCESSFULLY!                     " -ForegroundColor Green
Write-Host "===================================================================" -ForegroundColor Green
Write-Host " File location: $ZipFile" -ForegroundColor White
Write-Host " You can now send this ZIP file directly to your friend!" -ForegroundColor Yellow
Write-Host "===================================================================" -ForegroundColor Green
Write-Host ""
