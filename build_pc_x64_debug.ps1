$ErrorActionPreference = 'Stop'

$copyRoot = $PSScriptRoot
$toolBin = 'H:\Tools\VS2022\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64'
$msbuild = 'H:\Tools\VS2022\MSBuild\Current\Bin\MSBuild.exe'
$solution = Join-Path $copyRoot 'MinecraftPC.sln'
$serialTargets = Join-Path $copyRoot 'm2_serial_compile.targets'
$outputDir = Join-Path $copyRoot 'Minecraft.Client\bin\x64\Debug'
$runtimeDeps = Join-Path $copyRoot 'Minecraft.Client\Windows64\Runtime'
$log = Join-Path $copyRoot 'build_pc_x64_debug.log'

foreach ($path in @($toolBin, $msbuild, $solution, $serialTargets, $runtimeDeps)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required build path is missing: $path" }
}

$env:Path = "$toolBin;$env:Path"
& $msbuild $solution /t:Minecraft_Client `
    /p:Configuration=Debug `
    /p:Platform=x64 `
    /p:PlatformToolset=v143 `
    /p:WindowsTargetPlatformVersion=10.0.26100.0 `
    "/p:CLToolPath=$toolBin" /p:CLToolExe=cl.exe `
    "/p:LinkToolPath=$toolBin" /p:LinkToolExe=link.exe `
    "/p:LibToolPath=$toolBin" /p:LibToolExe=lib.exe `
    "/p:ForceImportAfterCppTargets=$serialTargets" `
    /m:1 /nologo /v:m *> $log

$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) {
    Get-Content -LiteralPath $log -Tail 80
    exit $buildExitCode
}

foreach ($name in @('mss64.dll', 'iggy_w64.dll')) {
    $source = Join-Path $runtimeDeps $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Required runtime DLL is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $outputDir $name) -Force
}

Write-Output "Build succeeded: $(Join-Path $outputDir 'Minecraft.Client.exe')"
