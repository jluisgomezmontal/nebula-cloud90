# Cloud90 Game Link (PLAN-GAME-LINK.md): puts this fork's NebulaRuntime.exe
# into the installed Nebula, keeping the original as NebulaRuntime.original.exe.
#   .\tools\cloud90\swap-runtime.ps1            install the fork's runtime
#   .\tools\cloud90\swap-runtime.ps1 -Restore   put the original back
# The game modules (RMGE01_*.dll) built by Nebula-Setup stay as they are.
param(
    [switch]$Restore,
    [string]$Runtime = (Join-Path $PSScriptRoot "..\..\build-runtime\NebulaRuntime.exe"),
    [string]$InstallRoot
)
$ErrorActionPreference = "Stop"

if (-not $InstallRoot) {
    $key = Get-ItemProperty "HKCU:\Software\Nebula" -ErrorAction SilentlyContinue
    $InstallRoot = if ($key -and $key.InstallRoot) { $key.InstallRoot } else { Join-Path $env:LOCALAPPDATA "Programs\Nebula" }
}
if (-not (Test-Path -LiteralPath $InstallRoot)) { throw "Nebula no está instalado en $InstallRoot" }

# The installed version's folder: the newest NebulaRuntime.exe that sits next
# to the compiled game module.
$app = Get-ChildItem -LiteralPath $InstallRoot -Recurse -Filter "RMGE01_game.dll" -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.DirectoryName "NebulaRuntime.exe") } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $app) { throw "No encontré RMGE01_game.dll junto a NebulaRuntime.exe en $InstallRoot (¿terminó Setup?)" }
$dir = $app.DirectoryName
$target = Join-Path $dir "NebulaRuntime.exe"
$backup = Join-Path $dir "NebulaRuntime.original.exe"

if (Get-Process -Name "NebulaRuntime" -ErrorAction SilentlyContinue) { throw "Cierra el juego primero." }

if ($Restore) {
    if (-not (Test-Path -LiteralPath $backup)) { throw "No hay respaldo en $backup" }
    Copy-Item -LiteralPath $backup -Destination $target -Force
    Remove-Item -LiteralPath $backup
    "Runtime original restaurado en $dir"
    return
}

$Runtime = (Resolve-Path -LiteralPath $Runtime).Path
if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $target -Destination $backup }
Copy-Item -LiteralPath $Runtime -Destination $target -Force
"Runtime de Cloud90 instalado en $dir (original: NebulaRuntime.original.exe)"
