# Packages the Windows64 dedicated-server prototype into two portable folders:
#   dist\MinecraftLCE-Server   (run_server.bat, server.properties)
#   dist\MinecraftLCE-Client   (run_client.bat, server.txt)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\package_pc_build.ps1
#   powershell -ExecutionPolicy Bypass -File .\package_pc_build.ps1 -SkipBuild
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $root "Minecraft.Client\bin\x64\Debug"
$dist = Join-Path $root "dist"

if (-not $SkipBuild) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root "build_pc_x64_debug.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}

if (-not (Test-Path (Join-Path $bin "Minecraft.Client.exe"))) {
    throw "Minecraft.Client.exe not found in $bin"
}

function Copy-GameFiles([string]$target) {
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    New-Item -ItemType Directory -Path $target | Out-Null

    Copy-Item (Join-Path $bin "Minecraft.Client.exe") $target
    foreach ($dll in @("iggy_w64.dll", "mss64.dll")) {
        $p = Join-Path $bin $dll
        if (Test-Path $p) { Copy-Item $p $target }
    }
    foreach ($dir in @("Common", "Windows64", "Windows64Media", "music")) {
        $p = Join-Path $bin $dir
        if (Test-Path $p) { Copy-Item $p $target -Recurse }
    }
}

$serverDir = Join-Path $dist "MinecraftLCE-Server"
$clientDir = Join-Path $dist "MinecraftLCE-Client"

Write-Host "Packaging server -> $serverDir"
Copy-GameFiles $serverDir
# Server package gets its own executable name so it looks/behaves like a
# dedicated server rather than a client.
Copy-Item (Join-Path $serverDir "Minecraft.Client.exe") (Join-Path $serverDir "Minecraft.Server.exe") -Force
Remove-Item (Join-Path $serverDir "Minecraft.Client.exe") -Force
Copy-Item (Join-Path $bin "server.properties") $serverDir
foreach ($bat in @("run_server.bat", "run_server_flat.bat", "run_server_windowed.bat")) {
    $src = Join-Path $bin $bat
    if (Test-Path $src) {
        $content = Get-Content $src -Raw
        $content = $content -replace 'Minecraft\.Client\.exe', 'Minecraft.Server.exe'
        Set-Content (Join-Path $serverDir $bat) $content -Encoding ASCII
    }
}
Copy-Item (Join-Path $root "SERVER_README.md") $serverDir
$ru = Join-Path $dist "README_RU.txt"
if (Test-Path $ru) { Copy-Item $ru $serverDir }

Write-Host "Packaging client -> $clientDir"
Copy-GameFiles $clientDir
Copy-Item (Join-Path $bin "server.txt") $clientDir
Copy-Item (Join-Path $bin "run_client.bat") $clientDir
Copy-Item (Join-Path $root "SERVER_README.md") $clientDir
if (Test-Path $ru) { Copy-Item $ru $clientDir }

Write-Host ""
Write-Host "Done."
Write-Host "Server: $serverDir  (run_server.bat)"
Write-Host "Client: $clientDir  (run_client.bat <ip:port>)"
