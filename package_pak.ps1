# Cooks the mod project and builds the patch pak that carries our post-process AnimBP.
#
# Only Content\TasomachiVR\ goes into the pak. The skeleton the AnimBP was compiled
# against is deliberately left out: at runtime its path resolves to the game's own asset,
# which is what makes the Blueprint compatible with the game's mesh without redistributing
# any of the game's content. Anything else the cooker dragged in is left out for the same
# reason.
#
# See docs\ANIMBP.md. The project MUST be named "tasomachi" - /Game/ maps to
# <project>\Content\, and the pak's internal paths have to match what the game resolves.
param(
    [Parameter(Mandatory = $true)][string]$UnrealDir,
    [Parameter(Mandatory = $true)][string]$ProjectDir,
    [string]$GameDir = "H:\Steam\steamapps\common\TASOMACHI",
    [string]$PakName = "TasomachiVR_P.pak",
    [switch]$SkipCook
)

$ErrorActionPreference = "Stop"

$editor = Join-Path $UnrealDir "Engine\Binaries\Win64\UE4Editor-Cmd.exe"
$unrealPak = Join-Path $UnrealDir "Engine\Binaries\Win64\UnrealPak.exe"
foreach ($tool in @($editor, $unrealPak)) {
    if (-not (Test-Path $tool)) { throw "Not found: $tool  (is this a 4.25 install?)" }
}

$uproject = Get-ChildItem -Path $ProjectDir -Filter *.uproject | Select-Object -First 1
if (-not $uproject) { throw "No .uproject in $ProjectDir" }

$projectName = $uproject.BaseName
if ($projectName -ne "tasomachi") {
    throw ("The project is named '$projectName'. It has to be 'tasomachi': the pak's " +
           "internal paths must match what the game resolves, or the pak mounts and " +
           "nothing inside it is ever found. See docs\ANIMBP.md.")
}

# --- cook ----------------------------------------------------------------------------
# Unversioned, matching how the game was cooked - its packages carry zeroed version
# fields, which is what tools\uasset_names.py had to work around.
if (-not $SkipCook) {
    Write-Host "Cooking..." -ForegroundColor Cyan
    & $editor $uproject.FullName -run=Cook -targetplatform=WindowsNoEditor -unversioned -compressed -stdout
    if ($LASTEXITCODE -ne 0) { throw "Cook failed (exit $LASTEXITCODE)" }
}

$cooked = Join-Path $ProjectDir "Saved\Cooked\WindowsNoEditor\$projectName"
$content = Join-Path $cooked "Content\TasomachiVR"
if (-not (Test-Path $content)) {
    throw ("Nothing cooked into $content. The AnimBP has to live in /Game/TasomachiVR/, " +
           "and the cooker only takes what is reachable - check that it is in a cooked " +
           "directory or referenced by one.")
}

# --- response file --------------------------------------------------------------------
# Each line pairs the file on disk with the path it takes inside the pak. The mount path
# uses the GAME's layout, which is the same as ours only because the project shares its
# name.
$files = Get-ChildItem -Path $content -Recurse -File
if ($files.Count -eq 0) { throw "No cooked files under $content" }

$response = Join-Path $ProjectDir "Saved\TasomachiVR_pak.txt"
$lines = foreach ($file in $files) {
    $relative = $file.FullName.Substring($cooked.Length).TrimStart('\')
    '"{0}" "../../../{1}/{2}"' -f $file.FullName, $projectName, ($relative -replace '\\', '/')
}
Set-Content -Path $response -Value $lines -Encoding ascii

Write-Host "Paking $($files.Count) file(s):" -ForegroundColor Cyan
$lines | ForEach-Object { "   $_" }

# --- pak ------------------------------------------------------------------------------
$outDir = Join-Path $ProjectDir "Saved\Paks"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$pak = Join-Path $outDir $PakName

& $unrealPak $pak "-create=$response" -compress
if ($LASTEXITCODE -ne 0) { throw "UnrealPak failed (exit $LASTEXITCODE)" }

Write-Host "`nBuilt $pak" -ForegroundColor Green

# --- install --------------------------------------------------------------------------
# The _P suffix is what makes the engine mount it with priority over the game's own pak.
$paks = Join-Path $GameDir "tasomachi\Content\Paks"
if (Test-Path $paks) {
    Copy-Item $pak (Join-Path $paks $PakName) -Force
    Write-Host "Installed into $paks" -ForegroundColor Green
    Write-Host "Set Arms=1 and ArmsDebugTilt=30 in TasomachiVR.ini for the first launch."
} else {
    Write-Warning "Game paks folder not found at $paks - copy $PakName there by hand."
}
