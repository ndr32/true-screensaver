# Builds bin\TrueScreensaver.exe from src\truescreensaver.c with Zig's C compiler.
# Install Zig once with:  winget install zig.zig
$ErrorActionPreference = 'Stop'

$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) {
    $zig = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\zig.zig*\*\zig.exe" -ErrorAction SilentlyContinue |
           Select-Object -First 1 -ExpandProperty FullName
}
if (-not $zig) { throw "Zig not found. Install it with: winget install zig.zig" }

$out = Join-Path $PSScriptRoot 'bin'
New-Item -ItemType Directory -Force $out | Out-Null

# -Os: optimize for size, -s: strip symbols, subsystem windows: no console window.
& $zig cc -target x86_64-windows-gnu -Os -s -Wall -Wextra `
    '-Wl,--subsystem,windows' `
    (Join-Path $PSScriptRoot 'src\truescreensaver.c') `
    -o (Join-Path $out 'TrueScreensaver.exe') `
    -luser32 -lshell32 -lgdi32 -ladvapi32 -ldwmapi

if ($LASTEXITCODE -ne 0) { throw "Build failed" }
Remove-Item (Join-Path $out 'TrueScreensaver.pdb') -ErrorAction SilentlyContinue
Write-Host "Built $out\TrueScreensaver.exe"
