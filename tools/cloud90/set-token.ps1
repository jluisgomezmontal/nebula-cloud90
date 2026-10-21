# Cloud90 Game Link: saves the game's token (from Cloud90 → Game Link) in
# %LOCALAPPDATA%\Nebula\cloud90-link.ini, read by the runtime at start.
#   .\tools\cloud90\set-token.ps1                       ask for the token
#   .\tools\cloud90\set-token.ps1 -Server http://localhost:3001   local API
#   .\tools\cloud90\set-token.ps1 -Remove               forget it
param([string]$Server = "", [switch]$Remove)
$ErrorActionPreference = "Stop"
$dir = Join-Path $env:LOCALAPPDATA "Nebula"
$file = Join-Path $dir "cloud90-link.ini"
if ($Remove) {
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
    "Token borrado. El juego ya no se conectará a Cloud90."
    return
}
$token = (Read-Host "Pega tu token de Game Link (empieza con c90gl_)").Trim()
if (-not $token.StartsWith("c90gl_")) { throw "Ese no parece un token de Game Link." }
New-Item -ItemType Directory -Force $dir | Out-Null
$lines = @("token=$token")
if ($Server) { $lines += "server=$($Server.TrimEnd('/'))" }
Set-Content -LiteralPath $file -Value $lines -Encoding ascii
"Token guardado en $file. Abre el juego y se conectará solo."
