# rv32emu-c — RV32IMC / ESP32-C3 模拟器的 C11 移植

这是 [RV32EMU](https://github.com/yafengabc/RV32EMU)（Go 版 `rv32emu`）的独立 C 项目。目标不是重新设计，而是**逐层对齐原实现的行为**，再用同一批固件和同一套计时方法比较两者的速度。

本仓库只包含 C 移植；Go 版在单独的仓库里，对比脚本会在本机找到它的 checkout 后一起编译、计时。

## 目录

```
rv32emu-c/
├─ build.ps1                  Windows 一键编译
├─ Makefile                   make 环境
├─ src/
│  ├─ rv32.h                  公共类型、地址映射、接口
│  ├─ cpu.c                   Step、RV32I/M、C 扩展、CSR、trap、PMP
│  ├─ block.c                 基本块预解码缓存 + RunHot + 块链
│  ├─ mem.c                   32 MiB 平铺总线、RAM/ROM、代码页版本
│  ├─ perip.c                 UART/GPIO/USJ/RTC/TIMG0/INTMTX/SPI1
│  ├─ rom.c                   esp_rom_spiflash_read/write/config_param 桩
│  ├─ elf.c                   ELF32 / raw bin 加载
│  ├─ disasm.c                -trace 用的反汇编
│  └─ main.c                  CLI、machine、软件复位、bench 模式
└─ bench/
   ├─ compare.ps1             Go vs C 自动对比（-IncludeTinyOS / -IncludeUnicorn）
   ├─ gen_workloads.py        pi / ALU / memory 负载
   ├─ bench_unicorn.c          Unicorn 2.1.4 的 C 侧对照与直线代码测速
   ├─ bench_unicorn.py         Unicorn 探针（Python 绑定）
   ├─ README.md               Unicorn 对比的复现方法与结论
   └─ results.md              最近一次结果
```

两个前提：

1. **Go 版 checkout**。`bench/compare.ps1` 需要一个 RV32EMU checkout 来编译 Go 侧计时器。
   它会依次尝试 `-GoRepo <路径>`、`bench/go-repo.txt`、环境变量 `RV32EMU_GO_REPO`，
   最后猜测两种常见布局（C 仓库放在 `RV32EMU/rv32emu-c`，或与 `RV32EMU/` 并列）。
   默认的 `bench/go-repo.txt` 写的是 `../RV32EMU`，按自己的目录结构改一行即可。
   计时器源码（`bench/benchdata/benchrun/main.go`，以及 `genbench`/`genfib`）随本仓库提供，
   构建时把它和 Go 仓库里的 `internal/` 包复制到一个临时 Go module 里编译，**不会修改 Go 仓库、也不需要给它加文件**；
   所以 Go 侧必须与本 C 版对应同一批提交，计时才有意义。
2. `bench_unicorn.c` 需要 MSYS2 的 `mingw-w64-ucrt-x86_64-unicorn`（头文件 + `libunicorn`），没有时可以整段跳过。

## 构建

Windows（MSYS2 UCRT64 / MinGW gcc）：

```powershell
cd rv32emu-c
.\build.ps1
# 可选：.\build.ps1 -Native   # 加 -march=native
```

如果系统禁止运行本地脚本，用：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

有 make 的环境：

```powershell
make
```

手工命令：

```powershell
gcc -std=c11 -O3 -DNDEBUG -flto -o build/rv32emu-c.exe src/*.c -pthread
```

## 运行

```powershell
# 裸机 ELF
.\build\rv32emu-c.exe -elf fib.elf

# TinyOS（ELF + littlefs 镜像）
.\build\rv32emu-c.exe -elf ..\TinyOS\build\kernel-esp32c3.elf `
    -ramdisk ..\TinyOS\build\ramdisk-esp32c3.bin

# 只跑 500 万条并看寄存器
.\build\rv32emu-c.exe -elf fib.elf -max 5000000 -regs

# 纯模拟计时（不包含进程启动、ELF 加载和终端输出）
.\build\rv32emu-c.exe -elf fib.elf -bench 0 -reps 7 -quiet
```

已支持的参数：`-elf -bin -entry -ramdisk -max -step -tickdiv -nopmp -traplog -trace -breakpc -dumpram -regs -rxrate -rxperiod -rxdelay -bench -reps -quiet`。

与原 Go CLI 的差异只在调试便利功能：没有 `-watch`、`-fault`、`-traceirq`、`-tracefrom/-traceto`。核心执行、异常、PMP、外设和软件复位路径都是对齐的。

## 移植时保留的关键设计

- **平铺地址空间**：`0x3F000000..0x40FFFFFF` 一块 32 MiB 连续内存，取指和 RAM 访存都只是一次减法 + 边界检查。
- **基本块缓存**：4 KiB 页版本号失效；块不跨页；块内最后一条一定是控制转移。
- **块链**：`run_block()` 跑完一个块后直接取后继块继续，避免短块频繁函数调用。
- **精确异常语义**：`mepc` 指向故障指令、对齐检查、U-mode PMP、vectored mtvec、`mstatus.MPIE/MIE`、`mret`。
- **外设按整字访问**：TIMG0/INTMTX/SPI1 擦除/ROM flash 桩/UART FIFO 都保持原语义。
- **软件复位**：`RTC_CNTL_OPTIONS0.SW_SYS_RST` 只立旗，由 `main` 重装一台 machine，flash 内容保留。

## 正确性验证

在移植过程中做了三层对照：

1. `fib(28)` 的最终寄存器、PC、指令数（15,426,855）和 timg0 fires 与 Go 版逐字一致（`a0=0x4D973=317811`）。
2. TinyOS `kernel-esp32c3.elf + ramdisk` 跑到 `tos>`，5,000,000 条指令后的 UART 文本、`timg0 fires=498`、`interrupts=296`、`exceptions=19` 与 Go 版一致。
3. 同一份 C 程序分别用 `-max`（RunHot）和 `-step`（逐条 Step）在 1M 指令内对比 PC/寄存器；早期版本正是靠这个抓到 `C.JR` 解码漏了 `term` 标志的 bug。

## 速度对比

计时口径：**只计 `RunHot(1000)` 批次 + TIMG0 tick**，不含进程启动、ELF 加载、终端输出；每个负载跑 7 次取中位数。Go 侧用 `bench/benchdata/benchrun`（与 Go 仓库 `tools/benchrun` 同一份源码），C 侧用 `-bench`，两边是同一套批次循环。

主机：AMD Ryzen 7 255（8C/16T），Windows，Go 1.27.3，gcc 16.2.0，C 编译选项 `-O3 -DNDEBUG -flto`。

| 负载 | 执行指令数 | Go MIPS | C MIPS | C/Go |
|---|---:|---:|---:|---:|
| fib(28) 递归 | 15,426,855 | 388.35 | 488.04 | 1.26x |
| pi（Leibniz，含 M 扩展） | 2,000,000 | 543.64 | 572.54 | 1.05x |
| ALU 紧循环 | 2,000,000 | 528.25 | 607.68 | 1.15x |
| 内存读写循环 | 2,000,000 | 133.62 | 174.82 | 1.31x |
| 冒泡排序循环 | 20,000,000 | 106.60 | 139.18 | 1.31x |
| 素数筛循环 | 20,000,000 | 173.71 | 246.89 | 1.42x |
| TinyOS 启动到 shell | 5,000,000 | 342.93 | 440.40 | 1.28x |

结论：

- C 版在全部负载上都快 **1.05x ~ 1.42x**，没有出现某一类负载反而变慢的情况。
- **纯 ALU/分支**（ALU loop、fib）C 快约 25~30%：Go 的块解释器已经编译得很好，C 的优势主要来自更直接的结构体数组访问和更少的边界/接口开销。
- **访存 + 分支**（内存循环、冒泡、筛法）C 快约 31~42%：每次 load/store 都要走 `priv/PMP/平铺区` 判断，C 生成的代码更紧凑。
- **真实系统负载**（TinyOS 启动，含 U-mode、PMP、timer 中断、UART）C 快约 28%，说明外围和异常路径也没有把优势吃掉。
- 紧循环用 2M 指令预算计量：Go/C 都需要 `-max` 才能离开死循环。这类短负载抖动最大——同一二进制连跑 5 次，Go 在 499~555 MIPS（ALU）/ 499~544 MIPS（pi）、C 在 578~641 / 570~586 之间波动，所以表里 pi 的 1.05x 不代表稳定差距，但 **C 在每一次运行里都更快**；20M 的长循环最稳定（ALU：C 62.3 vs Go 46.6 MIPS，1.34x）。fib、排序、筛法、TinyOS 是自终止程序：200 次重复、2M/20M 两种预算下比值都稳定在 ±0.05。

### 与 Unicorn 2.1.4 的对比

尝试过：MSYS2 的 `mingw-w64-ucrt-x86_64-unicorn`（QEMU TCG 2.1.4）C 库 + 自带
`bench/bench_unicorn.c`，以及 Python 绑定。结论是这一版 Unicorn 的 RISC-V 前端**跑不了**
这些镜像：

- 长 TB 里被跳转的分支会把**跳转目标**当成分支执行，然后在 TB 末尾抛 `UC_ERR_EXCEPTION`
  （`j +12`、`beq +8`、每个循环的第一条分支都能复现）——所以 4 条指令的循环只跑一圈，
  `fib` 勉强跑到 200 万条（90 MIPS），其它镜像只跑几条就停；
- `EBREAK` 不被解码（`UC_ERR_INSN_INVALID`），`ECALL` 能进 `UC_HOOK_INTR`；
- 越界 load/store 会悄悄落到相邻 4 KiB 页，`0x3FC80000` 的数据区读到的其实是 12 KiB
  之后的内容；
- `uc_emu_start(..., count=N)` 只在 TB 边界检查计数，最后一段 TB 的尾部分支被裁掉，
  循环型镜像的「计 N 条」直接跳出循环；`count=0` 一直跑到映射末尾后继续取下一张
  图的零页，永不返回。

能正常跑的只有**单页内的直线代码**：`bench/bench_unicorn.py` 计时 16000 条 `nop`，
无 hook 约 0.018 s、带 `UC_HOOK_CODE` 约 0.021 s（这 16k 条里 Python 的 `emu_start`
调用开销占了大头，但已经说明在同样的「每条指令都要记一次数」口径下，JIT 并不占便宜；
C 侧同样 16000 条约 0.000028 s）。Go 版 README 里 Unicorn 在真实算法上只快 2 倍，
也是同一个原因——JIT 的优势被访存和分支稀释。详细复现方法见 `bench/README.md`。

复现：

```powershell
cd rv32emu-c
.\bench\compare.ps1 -Reps 7 -IncludeTinyOS
```

脚本会编译两边、生成 `fib/pi/alu/mem/sort/sieve` ELF，输出表格并写入 `bench/results.md`。

## 内存管理 / 泄漏检查

Go 版靠 GC 兜底，C 版没有；这个项目里所有所有权都是“谁开谁关”，并且可以
在一个进程里反复开会话（GUI 会这么做），所以专门做了以下检查：

```powershell
cd rv32emu-c
.\build_asan.ps1                 # ASan 版本（clang -fsanitize=address + lld）
python bench\memcheck.py         # 一键：ASan + 会话泄漏 + 核心单测
```

覆盖范围：

- **ASan**（`bench/asan_test.py`）：TinyOS 启动、脚本化控制台（cat/ls/reboot）、
  缺失/损坏/截断 ELF、无参数、6 个基准镜像各 200 万条 —— 抓越界、UAF、double free。
- **会话泄漏**（`tools/leakrun.c` + `build/rv32emu-c.dll`）：同一进程里把
  TinyOS 启动/退出重复 6 次，`-join` 让 `_shutdown` 前经过一次
  `EnterCriticalSection`（Windows heap 的 FindCollision 钩子会抓 double free）。
  结果：工作时间集、私有内存、句柄数在第一次之后完全不变，`_shutdown` 后回落。
- **核心单测**（`tools/leakunit.c`）：新建/释放 `bus_t + cpu_t` 12 次，私有内存
  始终回落到基线（~780 KiB，含 7.5 MiB 总线窗口的提交峰值）；在**同一个 bus**
  上重复加载 ELF 也只增长一次（总线原地复用已挂载的 RAM 区），释放后同样回落。

分配约定：`bus_new()` 拥有 `flat/code_page/code_gen`，`cpu_new()` 不拥有总线；
ELF 加载器为未映射段 `AddRAM` 的区由总线释放；`console_start()/console_stop()`
成对管理控制台模式和缓冲区；`uart_set_tx_hook()` 之后 TX 不再落盘缓存。
Windows 上没有 LeakSanitizer，未释放的内存用泄漏驱动里的数字判断。

## 备注

- UART 接收用后台线程 + 环形缓冲模拟 Go 版的 goroutine；复位只清芯片侧 RX FIFO，主机侧预输入保留。
- 编译需要 pthread。MSYS2 UCRT64 的 `gcc -pthread` 可直接使用；Linux 同样。
- 生成物（`build/`、`*.elf`）已在 `.gitignore` 中排除。
