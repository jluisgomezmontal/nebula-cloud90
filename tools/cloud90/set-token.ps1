# Cloud90 Game Link: saves the game's token (from Cloud90 → Juegos) in
# %LOCALAPPDATA%\Nebula\cloud90-link.ini, sealed with Windows DPAPI for
# this user (the runtime opens it; nobody else can). The token can also be
# pasted in the game: F1 > CLOUD90 > PASTE TOKEN FROM CLIPBOARD.
#   .\tools\cloud90\set-token.ps1                                ask for the token
#   .\tools\cloud90\set-token.ps1 -Server http://localhost:3001  local API
#   .\tools\cloud90\set-token.ps1 -Remove                        forget it
param([string]$Server = "", [switch]$Remove)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security
$dir = Join-Path $env:LOCALAPPDATA "Nebula"
$file = Join-Path $dir "cloud90-link.ini"

# Keep the server line already there unless a new one is given.
$oldServer = ""
if (Test-Path -LiteralPath $file) {
    $line = Get-Content -LiteralPath $file | Where-Object { $_ -like "server=*" } | Select-Object -First 1
    if ($line) { $oldServer = $line.Substring(7).Trim() }
}
if (-not $Server) { $Server = $oldServer }

if ($Remove) {
    $lines = @()
    if ($Server) { $lines += "server=$Server" }
    Set-Content -LiteralPath $file -Value $lines -Encoding ascii
    "Token borrado. El juego ya no se conectará a Cloud90."
    return
}
$token = (Read-Host "Pega tu token de Game Link (empieza con c90gl_)").Trim()
if ($token -notmatch '^c90gl_[A-Za-z0-9_-]{14,94}$') { throw "Ese no parece un token de Game Link." }
$sealed = [Security.Cryptography.ProtectedData]::Protect(
    [Text.Encoding]::UTF8.GetBytes($token),
    [Text.Encoding]::UTF8.GetBytes("Cloud90GameLink"),
    [Security.Cryptography.DataProtectionScope]::CurrentUser)
New-Item -ItemType Directory -Force $dir | Out-Null
$lines = @("token_protected=$([Convert]::ToBase64String($sealed))")
if ($Server) { $lines += "server=$($Server.TrimEnd('/'))" }
Set-Content -LiteralPath $file -Value $lines -Encoding ascii
"Token guardado (cifrado) en $file. Abre el juego y se conectará solo."
