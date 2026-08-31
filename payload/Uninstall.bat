@echo off
REM ===========================================================================
REM  TasomachiVR - uninstall
REM
REM  Double-click this file. It sits next to dsound.dll, in
REM  tasomachi\Binaries\Win64, and removes everything the mod put on this
REM  machine. The game is left exactly as it was.
REM
REM  Four things are removed, and the last two are the ones a plain file delete
REM  would miss:
REM    dsound.dll                 the loader
REM    TasomachiVR\               the mod, its settings and its log
REM    the UEVR profile           in %%APPDATA%%\UnrealVRMod
REM    one line in Engine.ini     the audio fix, so the game goes back to
REM                               muting itself when it loses focus
REM ===========================================================================
setlocal
cd /d "%~dp0"

echo.
echo  Uninstalling TasomachiVR from:
echo    %~dp0
echo.

if exist "dsound.dll" (
    del /q "dsound.dll" && echo   removed  dsound.dll
) else (
    echo   absent   dsound.dll
)

if exist "TasomachiVR\" (
    rmdir /s /q "TasomachiVR" && echo   removed  TasomachiVR\
) else (
    echo   absent   TasomachiVR\
)

REM Unzipped alongside the loader, so it is ours to take away as well.
if exist "INSTALL.txt" (
    del /q "INSTALL.txt" && echo   removed  INSTALL.txt
)

set "PROFILE=%APPDATA%\UnrealVRMod\tasomachi-Win64-Shipping"
if exist "%PROFILE%" (
    rmdir /s /q "%PROFILE%" && echo   removed  UEVR profile
) else (
    echo   absent   UEVR profile
)

REM The audio fix lives in the player's own Engine.ini, outside the game folder,
REM so it has to be taken out by name rather than by deleting a file.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$p = Join-Path $env:LOCALAPPDATA 'tasomachi\Saved\Config\WindowsNoEditor\Engine.ini';" ^
  "if (Test-Path $p) {" ^
  "  $keep = @(); $inAudio = $false; $dropped = $false;" ^
  "  foreach ($l in (Get-Content $p)) {" ^
  "    if ($l -match '^\s*\[(.+)\]\s*$') { $inAudio = ($matches[1] -eq 'Audio') }" ^
  "    elseif ($inAudio -and $l -match '^\s*UnfocusedVolumeMultiplier\s*=') { $dropped = $true; continue }" ^
  "    $keep += $l }" ^
  "  Set-Content -Path $p -Value $keep -Encoding utf8;" ^
  "  if ($dropped) { Write-Host '  removed  Engine.ini audio line' } else { Write-Host '  absent   Engine.ini audio line' }" ^
  "} else { Write-Host '  absent   Engine.ini' }"

echo.
echo  Done. Tasomachi is back to its original state.
echo  This file is the only thing left; it deletes itself now.
echo.
pause

REM Deletes itself last, which a running batch file is allowed to do this way.
(goto) 2>nul & del "%~f0"
