# Builds the release archive: build\release\TasomachiVR-<version>.zip
#
# The archive's whole design is that it unzips ONTO tasomachi\Binaries\Win64 and nothing
# else is required. That works because the loader does at startup what an installer would
# otherwise have to do from outside the game folder: it copies the UEVR profile into
# %APPDATA% and writes the audio fix into the player's Engine.ini. Neither of those is in
# the zip, and neither has to be.
#
# So the layout below is exactly what the game folder must end up containing:
#
#   Binaries\Win64\dsound.dll            the loader, named for the DLL the game already loads
#   Binaries\Win64\Uninstall.bat         removes all of it again, including what is outside
#   Binaries\Win64\TasomachiVR\          the mod
#       TasomachiVR.ini                  every setting, commented
#       UEVRBackend.dll, ...             UEVR itself and the runtimes it needs
#       profile\config.txt               the UEVR profile seeded on first launch
#       profile\scripts\*.lua
#       profile\plugins\TasomachiVR.dll  the plugin
#
# Run .\build.ps1 and .\build_plugin.ps1 first; this script refuses to package a missing
# binary rather than shipping a half archive.

param(
    [string]$Version = (Get-Date -Format "yyyy.MM.dd"),
    [string]$UevrDir = "H:\UEVR"
)

$ErrorActionPreference = "Stop"
$root    = Split-Path -Parent $MyInvocation.MyCommand.Path
$out     = Join-Path $root "build\release"
$name    = "TasomachiVR-$Version"
$stage   = Join-Path $out $name
$payload = Join-Path $stage "TasomachiVR"

function Need([string]$path, [string]$why) {
    if (-not (Test-Path $path)) { throw "$why is missing: $path" }
    return $path
}

Need (Join-Path $root "build\dsound.dll")          "the loader (run .\build.ps1)"          | Out-Null
Need (Join-Path $root "build\TasomachiVR.dll")     "the plugin (run .\build_plugin.ps1)"   | Out-Null

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $payload "profile\plugins") | Out-Null

# --- the loader and the uninstaller, at the top level -----------------------------------
Copy-Item (Join-Path $root "build\dsound.dll")   (Join-Path $stage "dsound.dll")   -Force
Copy-Item (Join-Path $root "payload\Uninstall.bat") (Join-Path $stage "Uninstall.bat") -Force

# --- UEVR's own binaries ----------------------------------------------------------------
foreach ($f in @("UEVRBackend.dll", "UEVRPluginNullifier.dll", "openxr_loader.dll", "openvr_api.dll")) {
    Copy-Item (Need (Join-Path $UevrDir $f) "a UEVR binary") (Join-Path $payload $f) -Force
}

# --- the mod ----------------------------------------------------------------------------
Copy-Item (Join-Path $root "payload\TasomachiVR.ini") (Join-Path $payload "TasomachiVR.ini") -Force
Copy-Item (Join-Path $root "payload\profile\config.txt") (Join-Path $payload "profile\config.txt") -Force
Copy-Item (Join-Path $root "payload\profile\scripts") (Join-Path $payload "profile") -Recurse -Force
Copy-Item (Join-Path $root "build\TasomachiVR.dll") (Join-Path $payload "profile\plugins\TasomachiVR.dll") -Force

Copy-Item (Join-Path $root "INSTALL.txt")      (Join-Path $stage "INSTALL.txt")      -Force
Copy-Item (Join-Path $root "THIRD-PARTY.txt")  (Join-Path $payload "THIRD-PARTY.txt") -Force
Copy-Item (Join-Path $root "LICENSE")          (Join-Path $payload "LICENSE")         -Force

# --- the archive ------------------------------------------------------------------------
$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

Write-Host ""
Write-Host "Packaged $name" -ForegroundColor Green
Get-ChildItem -Recurse -File $stage | ForEach-Object {
    "  {0,10:N0}  {1}" -f $_.Length, $_.FullName.Substring($stage.Length + 1)
}
Write-Host ""
Write-Host "  -> $zip"
Write-Host "  Unzip its contents into tasomachi\Binaries\Win64 and launch from Steam."
