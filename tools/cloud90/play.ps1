# Cloud90 Game Link (PLAN-GAME-LINK.md): runs Super Mario Galaxy with this
# fork's runtime, built entirely from source (no Nebula-Setup/launcher).
# Same environment as installer/src/Launcher/LaunchOptions.cs.
#   .\tools\cloud90\play.ps1            assemble build-game\app and start
#   .\tools\cloud90\play.ps1 -NoStart   only assemble
param([switch]$NoStart)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$game = Join-Path $repo "build-game"
$app = Join-Path $game "app"
$content = Join-Path $game "content"

$files = @{
    "NebulaRuntime.exe"      = "build-runtime\NebulaRuntime.exe"
    "RMGE01_game.dll"        = "build-game\modules\game\RMGE01_game.dll"
    "RMGE01_home_button.dll" = "build-game\modules\home\RMGE01_home_button.dll"
    "RMGE01_dsp.dll"         = "build-game\modules\dsp\RMGE01_dsp.dll"
    "RMGE01_boot_image.bin"  = "build-game\generated\RMGE01_boot_image.bin"
    "runtime-env.json"       = "installer\runtime-env.json"
}
New-Item -ItemType Directory -Force $app | Out-Null
foreach ($name in $files.Keys) {
    $source = Join-Path $repo $files[$name]
    if (-not (Test-Path -LiteralPath $source)) { throw "Falta $source (compila primero)" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $app $name) -Force
}
if (-not (Test-Path -LiteralPath $content)) { throw "Falta $content (nebula-recomp package-content)" }
"Juego armado en $app"
if ($NoStart) { return }
if (Get-Process -Name "NebulaRuntime" -ErrorAction SilentlyContinue) { throw "El juego ya está abierto." }

$saves = Join-Path $env:LOCALAPPDATA "Nebula\saves"
New-Item -ItemType Directory -Force $saves | Out-Null
foreach ($e in (Get-Content -Raw (Join-Path $app "runtime-env.json") | ConvertFrom-Json)) {
    Set-Item -Path "Env:$($e.name)" -Value $e.value
}
$env:GALAXY_NAND_ROOT = $saves
$env:GALAXY_NATIVE_THP_VIDEO_DECODE = "1"
$env:GALAXY_DUSK_DSP_HLE = "0"
Start-Process -FilePath (Join-Path $app "NebulaRuntime.exe") -WorkingDirectory $app `
    -ArgumentList @("`"$content`"", "`"$(Join-Path $app 'RMGE01_game.dll')`"")
"Juego iniciado. F5 = cambio al juego (curar, golpe, 1-UP, quitar vida, star bits +/-, monedas +/-), F6 = efecto de pantalla, F7 = giro, F8 = siguiente efecto de control, F9 = pánico."
