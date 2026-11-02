# Build the AddressSanitizer flavour of rv32emu-c (Windows, MSYS2 UCRT64).
#
#   .\build_asan.ps1
#
# Uses clang (ucrt64) with -fsanitize=address and lld. On Windows the dynamic
# ASan runtime is required, so the runtime DLL must be next to the exe or on
# PATH. LeakSanitizer does not exist on Windows; use tools/leakrun.c for the
# leak side of the check and bench/asan_test.py for memory-safety bugs.
param(
    [string]$Clang = "clang"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out = Join-Path $root "build\asan"
New-Item -ItemType Directory -Force $out | Out-Null
$src = Get-ChildItem (Join-Path $root "src\*.c") | ForEach-Object { $_.FullName }
$exe = Join-Path $out "rv32emu-c-asan.exe"

& $Clang -fsanitize=address -fuse-ld=lld -g -O1 -o $exe @src
if ($LASTEXITCODE -ne 0) { throw "clang build failed" }

# make sure the ASan runtime can be found next to the exe
$dll = "C:\msys64\ucrt64\bin\libclang_rt.asan_dynamic-x86_64.dll"
if (Test-Path $dll) { Copy-Item $dll $out -Force }
Write-Host "built $exe"
Write-Host "run: python bench\asan_test.py"
