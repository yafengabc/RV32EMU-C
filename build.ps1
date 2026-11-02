param(
    [switch]$Native,
    [switch]$Debug
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"
New-Item -ItemType Directory -Force $build | Out-Null

$cc = $null
foreach ($candidate in @("gcc", "clang")) {
    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($cmd) { $cc = $cmd.Source; break }
}
if (-not $cc) { throw "gcc or clang not found in PATH" }

$src = Get-ChildItem (Join-Path $root "src\*.c") | ForEach-Object { $_.FullName }
$out = Join-Path $build "rv32emu-c.exe"
if (-not $IsWindows -and $PSVersionTable.PSEdition -eq "Core") { $out = Join-Path $build "rv32emu-c" }

$cflags = @("-std=c11", "-Wall", "-Wextra")
if ($Debug) {
    $cflags += @("-O0", "-g3")
} else {
    $cflags += @("-O3", "-DNDEBUG", "-flto")
}
if ($Native) { $cflags += "-march=native" }

Write-Host "CC  = $cc"
Write-Host "OUT = $out"
& $cc @cflags -o $out @src -pthread
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "OK"
