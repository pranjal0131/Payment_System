# run-tests.ps1 — compile and run the zero-dependency test suite.
# Usage:  powershell -ExecutionPolicy Bypass -File .\run-tests.ps1
#
# Mirrors build.ps1 but compiles tests/tests.cpp (which has its own main()).
# Exits non-zero if any test fails, so it can gate a commit / CI step.

$ErrorActionPreference = "Stop"
$mingwBin = "C:\msys64\mingw64\bin"
$gpp = Join-Path $mingwBin "g++.exe"
if (-not (Test-Path $gpp)) { throw "g++ not found at $gpp" }
# g++ needs its own runtime DLLs on PATH to even run.
$env:Path = "$mingwBin;$env:Path"

$root  = $PSScriptRoot
$build = Join-Path $root "build"
if (-not (Test-Path $build)) { New-Item -ItemType Directory -Path $build | Out-Null }

$out = Join-Path $build "tests.exe"
Write-Host "Compiling test suite with g++ 14 (C++20)..." -ForegroundColor Cyan
$args = @(
    "-std=c++20", "-Wall", "-Wextra", "-O2", "-pthread",
    "-I", (Join-Path $root "include"),
    (Join-Path $root "tests\tests.cpp"),
    "-o", $out
)
& $gpp @args
if ($LASTEXITCODE -ne 0) { throw "Test build failed." }

Write-Host "Running tests..." -ForegroundColor Cyan
& $out
$code = $LASTEXITCODE
if ($code -eq 0) { Write-Host "Tests passed." -ForegroundColor Green }
else             { Write-Host "Tests FAILED (exit $code)." -ForegroundColor Red }
exit $code
