@echo off
title Mario Super Sluggers Netplay Release Packager
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package_release.ps1"
pause
