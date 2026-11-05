# Patches the prebuilt 4J libraries so they can be linked with modern MSVC.
#
# The libs were built with VS2012 (_MSC_VER=1700). Modern MSVC objects (and the
# static STL, libcpmtd.lib) carry the ABI tag _MSC_VER=1900 (VS2015..VS2022 family),
# so the linker rejects the mix with LNK2038.
#
# The tag lives in the .drectve section of every COFF object inside the .lib as
# the plain ASCII string "_MSC_VER=1700". "1700" and "1900" have the same length,
# so a byte-for-byte replacement does not move anything in the archive.
#
# Usage: powershell -ExecutionPolicy Bypass -File patch_4jlibs.ps1

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$srcDir   = Join-Path $repoRoot 'Minecraft.Client\Windows64\4JLibs\libs'
$dstDir   = $PSScriptRoot

$libs = @(
    '4J_Input_d.lib',
    '4J_Render_PC_d.lib',
    '4J_Storage_d.lib',
    '4J_Profile_d.lib',
    '4J_Input.lib',
    '4J_Render_PC.lib',
    '4J_Storage.lib',
    '4J_Profile.lib'
)

$old = [System.Text.Encoding]::ASCII.GetBytes('_MSC_VER=1700')
$new = [System.Text.Encoding]::ASCII.GetBytes('_MSC_VER=1900')

function Replace-Bytes([byte[]]$data, [byte[]]$from, [byte[]]$to) {
    $count = 0
    for ($i = 0; $i -le $data.Length - $from.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $from.Length; $j++) {
            if ($data[$i + $j] -ne $from[$j]) { $match = $false; break }
        }
        if ($match) {
            [Array]::Copy($to, 0, $data, $i, $to.Length)
            $count++
            $i += $from.Length - 1
        }
    }
    return $count
}

foreach ($name in $libs) {
    $src = Join-Path $srcDir $name
    if (-not (Test-Path $src)) {
        Write-Host "skip (missing): $name"
        continue
    }

    $bytes = [System.IO.File]::ReadAllBytes($src)
    $n = Replace-Bytes $bytes $old $new

    $dst = Join-Path $dstDir $name
    [System.IO.File]::WriteAllBytes($dst, $bytes)
    Write-Host "$name : $n tag(s) patched"
}
