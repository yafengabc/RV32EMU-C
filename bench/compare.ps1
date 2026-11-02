param(
    [int]$Reps = 7,
    [uint64]$Max = 20000000,
    [switch]$IncludeTinyOS,
    [switch]$IncludeUnicorn,
    [string]$TinyOSElf = "C:\Users\SONG\Documents\Projects\TinyOS\build\kernel-esp32c3.elf",
    [string]$TinyOSRamdisk = "C:\Users\SONG\Documents\Projects\TinyOS\build\ramdisk-esp32c3.bin",
    [string]$GoRepo = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
$root = Split-Path -Parent $here
$repoParent = Split-Path -Parent $root
$build = Join-Path $root "build"
$data = Join-Path $build "benchdata"
$overlay = Join-Path $here "benchdata"
New-Item -ItemType Directory -Force $data | Out-Null

$cExe = Join-Path $build "rv32emu-c.exe"
$goBench = Join-Path $build "benchrun-go.exe"

if (-not $SkipBuild) {
    & (Join-Path $root "build.ps1")
}

# The Go reference emulator lives in its own repository. Look for it in this
# order: -GoRepo, bench/go-repo.txt, $env:RV32EMU_GO_REPO, then two common
# layouts (C port cloned inside RV32EMU/, or next to RV32EMU/).
function Resolve-GoRepo([string]$raw) {
    if (-not $raw) { return $null }
    $raw = $raw.Trim().Trim('"')
    $candidate = if ([System.IO.Path]::IsPathRooted($raw)) { $raw } else { Join-Path $here $raw }
    foreach ($c in @($candidate, (Join-Path $repoParent $raw))) {
        if (Test-Path (Join-Path $c "go.mod")) { return (Resolve-Path $c).Path }
    }
    return $null
}

if ($GoRepo) { $GoRepo = Resolve-GoRepo $GoRepo }
if (-not $GoRepo) {
    $pointer = Join-Path $here "go-repo.txt"
    if (Test-Path $pointer) {
        $line = Get-Content $pointer | Where-Object { $_ -and -not $_.StartsWith("#") } | Select-Object -First 1
        $GoRepo = Resolve-GoRepo $line
    }
}
if (-not $GoRepo -and $env:RV32EMU_GO_REPO) { $GoRepo = Resolve-GoRepo $env:RV32EMU_GO_REPO }
if (-not $GoRepo) { $GoRepo = Resolve-GoRepo "../RV32EMU" }
if (-not $GoRepo) { $GoRepo = Resolve-GoRepo "../" }
if (-not $GoRepo) {
    Write-Error "Go checkout not found. Pass -GoRepo <path>, set RV32EMU_GO_REPO or edit bench/go-repo.txt."
    exit 1
}
Write-Host "Go repo: $GoRepo"

# benchrun imports rv32emu/internal/..., which Go only permits from inside the
# rv32emu module. A scratch module named "rv32emu" is assembled from an exact
# copy of the checkout's internal/{cpu,elf,mem,perip,rom} packages plus the
# harness at tools/benchrun, and `go build` happens there - the Go checkout is
# never modified and nothing has to be added to the Go repository.
#
# The harness is copied verbatim (same import paths, same loop shape) because
# that is what the Go repository compiles; restructuring it changes codegen and
# shifts the numbers by a wide margin.
$genbench = Join-Path $build "genbench.exe"
$genfib = Join-Path $build "genfib.exe"
$goBuild = 0
Push-Location $overlay
try {
    go build -buildvcs=false -o $genbench ./genbench
    go build -buildvcs=false -o $genfib ./genfib
    if ($LASTEXITCODE -ne 0) { $goBuild = $LASTEXITCODE }
}
finally {
    Pop-Location
}
if ($goBuild -eq 0) {
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ("rv32emu-bench-" + [guid]::NewGuid().ToString("N"))
    $toolsDir = Join-Path $stage "tools\benchrun"
    New-Item -ItemType Directory -Force $toolsDir | Out-Null
    try {
        $goDirective = "go 1.22"
        $repoMod = Join-Path $GoRepo "go.mod"
        if (Test-Path $repoMod) {
            $m = Select-String -Path $repoMod -Pattern '^go\s+(.+)$' | Select-Object -First 1
            if ($m) { $goDirective = "go " + $m.Matches[0].Groups[1].Value.Trim() }
        }
        $mod = "module rv32emu`r`n`r`n$goDirective`r`n"
        [System.IO.File]::WriteAllText((Join-Path $stage "go.mod"), $mod)
        foreach ($pkg in @("cpu", "elf", "mem", "perip", "rom")) {
            $from = Join-Path (Join-Path $GoRepo "internal") $pkg
            if (-not (Test-Path $from)) {
                throw "missing emulator package: $from (is -GoRepo a checkout of RV32EMU?)"
            }
            Copy-Item $from (Join-Path $stage "internal\$pkg") -Recurse -Force
        }
        Copy-Item (Join-Path $overlay "benchrun\main.go") (Join-Path $toolsDir "main.go") -Force
        Push-Location $stage
        try {
            # the scratch module is outside any VCS, so stamping has to be off
            go build -buildvcs=false -o $goBench ./tools/benchrun
            $goBuild = $LASTEXITCODE
        }
        finally {
            Pop-Location
        }
    }
    finally {
        Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
if ($goBuild -ne 0) { Write-Error "go build failed"; exit 1 }

Push-Location $data
try {
    & (Join-Path $build "genbench.exe")
    & (Join-Path $build "genfib.exe")
}
finally {
    Pop-Location
}

$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    & $python.Source (Join-Path $here "gen_workloads.py") $data
} else {
    Write-Warning "python not found: skipping alu_loop/mem_loop/pi_big workloads"
}

$workloads = @(
    [pscustomobject]@{ Name = "fib(28)"; File = (Join-Path $data "fib.elf"); Max = 0; Ramdisk = "" },
    [pscustomobject]@{ Name = "pi (Leibniz)"; File = (Join-Path $data "pi_big.elf"); Max = $Max; Ramdisk = "" },
    [pscustomobject]@{ Name = "ALU loop"; File = (Join-Path $data "alu_loop.elf"); Max = $Max; Ramdisk = "" },
    [pscustomobject]@{ Name = "memory loop"; File = (Join-Path $data "mem_loop.elf"); Max = $Max; Ramdisk = "" },
    [pscustomobject]@{ Name = "bubble sort"; File = (Join-Path $data "bench_sort.elf"); Max = $Max; Ramdisk = "" },
    [pscustomobject]@{ Name = "sieve"; File = (Join-Path $data "bench_sieve.elf"); Max = $Max; Ramdisk = "" }
)

if ($IncludeTinyOS -and (Test-Path $TinyOSElf) -and (Test-Path $TinyOSRamdisk)) {
    $workloads += [pscustomobject]@{
        Name = "TinyOS boot"
        File = $TinyOSElf
        Max = [uint64]5000000
        Ramdisk = $TinyOSRamdisk
    }
}

$workloads = $workloads | Where-Object {
    if (Test-Path $_.File) { $true }
    else { Write-Warning "missing workload: $($_.File)"; $false }
}

# Tight loops are the noisiest "budget exhausted" workloads, so they always run
# inside a 2M-instruction window and the best of three medians is kept. fib,
# sort, sieve and the TinyOS boot are self-terminating and use -Max / 5M with
# the plain median.
$Best = @("ALU loop", "memory loop", "pi")

function Get-Bench {
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [string]$Tag,
        [switch]$TakeBest
    )
    if (-not $TakeBest) {
        $out = & $Exe @Arguments 2>$null
        $line = ($out | Select-String '^\[bench\] median') | Select-Object -Last 1
        if (-not $line) { throw "$Tag produced no median line" }
        $text = $line.Line
        $mips = [double]([regex]::Match($text, 'mips=([0-9.]+)').Groups[1].Value)
        $inst = [uint64]([regex]::Match($text, 'inst=([0-9]+)').Groups[1].Value)
        return [pscustomobject]@{ Mips = $mips; Inst = $inst }
    }
    $best = $null
    for ($i = 0; $i -lt 3; $i++) {
        $out = & $Exe @Arguments 2>$null
        $line = ($out | Select-String '^\[bench\] (rep|median)') | Select-Object -Last 1
        if (-not $line) { throw "$Tag produced no result line" }
        $text = $line.Line
        $mips = [double]([regex]::Match($text, 'mips=([0-9.]+)').Groups[1].Value)
        $inst = [uint64]([regex]::Match($text, 'inst=([0-9]+)').Groups[1].Value)
        if ($null -eq $best -or $mips -gt $best.Mips) {
            $best = [pscustomobject]@{ Mips = $mips; Inst = $inst }
        }
    }
    return $best
}

$rows = @()
foreach ($w in $workloads) {
    Write-Host ("bench {0} ..." -f $w.Name)
    $takeBest = $false
    foreach ($key in $Best) { if ($w.Name -like "*$key*") { $takeBest = $true } }
    $workMax = $w.Max
    if ($takeBest -and ($workMax -eq 0 -or $workMax -gt 2000000)) { $workMax = 2000000 }
    $goArgs = @("-elf", $w.File, "-max", [string]$workMax, "-reps", [string]$Reps)
    $cArgs  = @("-elf", $w.File, "-bench", [string]$workMax, "-reps", [string]$Reps, "-quiet")
    if ($w.Ramdisk) {
        $goArgs += @("-ramdisk", $w.Ramdisk)
        $cArgs  += @("-ramdisk", $w.Ramdisk)
    }
    $go = Get-Bench -Exe $goBench -Arguments $goArgs -Tag "Go/$($w.Name)" -TakeBest:$takeBest
    $cc = Get-Bench -Exe $cExe   -Arguments $cArgs  -Tag "C/$($w.Name)"  -TakeBest:$takeBest
    $name = if ($takeBest) { "$($w.Name) ($([math]::Round($workMax/1000000,1))M inst)" } else { $w.Name }
    $rows += [pscustomobject]@{
        Workload = $name
        Inst = $cc.Inst
        GoMips = $go.Mips
        CMips = $cc.Mips
        Ratio = $cc.Mips / $go.Mips
        Note = ""
    }
}

if ($IncludeUnicorn) {
    $ucC = Get-Command gcc -ErrorAction SilentlyContinue
    if ($ucC) {
        Push-Location $repoParent
        try {
            & gcc -O2 -o (Join-Path $build "bench_unicorn.exe") `
                (Join-Path $here "bench_unicorn.c") `
                -I C:/msys64/ucrt64/include -L C:/msys64/ucrt64/lib -lunicorn 2>$null
        }
        finally {
            Pop-Location
        }
    }
    $ucExe = Join-Path $build "bench_unicorn.exe"
    if (Test-Path $ucExe) {
        foreach ($w in $workloads) {
            if ($w.Ramdisk) { continue }
            $instReq = if ($w.Max -eq 0) { [uint64]2000000 } else { $w.Max }
            if ($instReq -gt 2000000) { $instReq = 2000000 }
            $out = & $ucExe -elf $w.File -reps $Reps -max $instReq 2>$null
            $line = ($out | Select-String '^\[bench\] unicorn median') | Select-Object -Last 1
            $done = ($out | Select-String 'counted=([0-9]+)') | Select-Object -Last 1
            $note = "did not run (Unicorn 2.1.4 branch bug)"
            if ($line -and $done) {
                $counted = [uint64]([regex]::Match($done.Line, 'counted=([0-9]+)').Groups[1].Value)
                if ($counted -ge $instReq) {
                    $note = ([regex]::Match($line.Line, 'mips=([0-9.]+)').Groups[1].Value) + " MIPS"
                }
            }
            $rows += [pscustomobject]@{
                Workload = $w.Name
                Inst = $instReq
                GoMips = $null
                CMips = $null
                Ratio = $null
                Note = $note
            }
        }
    } else {
        Write-Warning "bench_unicorn.exe not available (needs unicorn dev files)"
    }
}

$lines = @()
$lines += "# Go vs C emulator benchmark"
$lines += ""
$cpuName = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
if ($cpuName) { $cpuName = $cpuName.Trim() } else { $cpuName = "unknown CPU" }
$lines += ("Host: {0}, {1} logical CPUs" -f $cpuName, [Environment]::ProcessorCount)
$lines += ("Reps: {0} (median), pure in-process RunHot/1000 batches" -f $Reps)
$lines += ""
$lines += "| workload | instructions | Go MIPS | C MIPS | C/Go | Unicorn |"
$lines += "|---|---:|---:|---:|---:|---|"
foreach ($r in $rows) {
    $go = if ($null -eq $r.GoMips) { "-" } else { "{0:F2}" -f $r.GoMips }
    $cc = if ($null -eq $r.CMips) { "-" } else { "{0:F2}" -f $r.CMips }
    $ratio = if ($null -eq $r.Ratio) { "-" } else { "{0:F2}x" -f $r.Ratio }
    $note = if ($r.Note) { $r.Note } else { "" }
    $lines += ("| {0} | {1} | {2} | {3} | {4} | {5} |" -f $r.Workload, $r.Inst, $go, $cc, $ratio, $note)
}
$lines += ""
$lines += "Ratio > 1 means the C port is faster. Tight loops run in a 2M-instruction"
$lines += "window (best of three runs) because Go occasionally slows down when run"
$lines += "right up to a 20M cap; self-terminating programs use their natural length."
$lines += "Unicorn 2.1.4 cannot run these images (branch handling inside long TBs);"
$lines += "see bench/README.md for the evidence and for the straight-line probe."

$report = Join-Path $here "results.md"
$lines | Set-Content -Encoding utf8 $report
$lines | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "saved: $report"
