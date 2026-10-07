param([switch]$Test)
$ErrorActionPreference = 'Stop'
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
$cmakeDir = 'F:\VS\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    & "$cmakeDir\cmake.exe" --preset vs-x64
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & "$cmakeDir\cmake.exe" --build --preset release
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    if ($Test) {
        & "$cmakeDir\ctest.exe" --preset release
        exit $LASTEXITCODE
    }
} finally { Pop-Location }
