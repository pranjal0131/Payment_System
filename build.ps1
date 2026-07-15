# build.ps1 — quick build using the MSYS2 g++ already on this machine.
# Usage:  powershell -ExecutionPolicy Bypass -File .\build.ps1
#         then:  .\build\payment.exe
#
# This is the "no extra tools needed" path. For the production-standard build,
# install cmake (pacman -S mingw-w64-x86_64-cmake) and use CMakeLists.txt.

$ErrorActionPreference = "Stop"
$mingwBin = "C:\msys64\mingw64\bin"
$gpp = Join-Path $mingwBin "g++.exe"
if (-not (Test-Path $gpp)) { throw "g++ not found at $gpp" }
# g++ needs its own runtime DLLs (libstdc++, libwinpthread) on PATH to even run.
$env:Path = "$mingwBin;$env:Path"

$root  = $PSScriptRoot
$build = Join-Path $root "build"
if (-not (Test-Path $build)) { New-Item -ItemType Directory -Path $build | Out-Null }

# Collect every .cpp under src/.
$sources = Get-ChildItem -Path (Join-Path $root "src") -Recurse -Filter *.cpp | ForEach-Object { $_.FullName }
Write-Host "Compiling $($sources.Count) source file(s) with g++ 14 (C++20)..." -ForegroundColor Cyan

$out = Join-Path $build "payment.exe"
$args = @(
    "-std=c++20", "-Wall", "-Wextra", "-O2", "-pthread",
    "-I", (Join-Path $root "include")
) + $sources + @("-o", $out)

& $gpp @args
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
Write-Host "Build OK -> $out" -ForegroundColor Green
