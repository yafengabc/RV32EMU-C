# rv32emu-c — RV32IMC / ESP32-C3 模拟器的 C11 移植

这是 [RV32EMU](https://github.com/yafengabc/RV32EMU)（Go 版 `rv32emu`）的独立 C 项目。目标不是重新设计，而是**逐层对齐原实现的行为**，再用同一批固件和同一套计时方法比较两者的速度。

本仓库只包含 C 移植；Go 版在单独的仓库里，对比脚本会在本机找到它的 checkout 后一起编译、计时。

## 目录

```
rv32emu-c/
├─ build.ps1                  Windows 一键编译
├─ build_asan.ps1             ASan 版本（clang -fsanitize=address + lld）
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
├─ tools/                     泄漏 / 会话测试驱动（Windows）
└─ bench/
   ├─ compare.ps1             Go vs C 自动对比（-IncludeTinyOS / -IncludeUnicorn）
   ├─ go-repo.txt             Go 仓库路径（默认 ../RV32EMU）
   ├─ benchdata/              Go 侧计时器 + fib/pi/sort/sieve ELF 生成器
   ├─ gen_workloads.py        pi / ALU / memory 负载
   ├─ bench_unicorn.c/.py     Unicorn 2.1.4 对照与直线代码测速
   ├─ qemu_check.py           QEMU 结果校验（乐鑫 esp32c3 / 上游 virt）
   ├─ asan_test.py / memcheck.py / leak_test.py
   ├─ README.md               计时口径与 Unicorn 复现结论
   └─ results.md              compare.ps1 生成的最近一次结果（已在 .gitignore）
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
gcc -std=c11 -O3 -DNDEBUG -flto -o build/rv32emu-c.exe src/*.c
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

## 在 QEMU 上验证

`bench/qemu_check.py` 把同样四个程序跑在 QEMU 上，并用独立的 Python 模型
（fib 递推、Leibniz 级数、冒泡、筛法）核对 a0。两种机器都支持：

| 机器 | QEMU | 内存映射 | 收尾方式 |
|---|---|---|---|
| `esp32c3` | 乐鑫维护的 qemu-system-riscv32（`github.com/espressif/qemu`） | 与真机一致：0x40000000 取指、DRAM `0x3FC80000~0x3FCDFFFF`、UART0 `0x60000000` | 打印 a0 后自旋，脚本读到结果再结束进程 |
| `virt` | 上游 qemu-system-riscv32 | DRAM `0x80000000`，程序需要重链接 | 16550 打印 + SiFive test(`0x100000`) 关机 |

乐鑫 QEMU 的 Windows 构建是 `espressif/qemu` release 里的
`qemu-riscv32-softmmu-* -x86_64-w64-mingw32.tar.xz`，解压后 `qemu/bin/qemu-system-riscv32.exe`
即为所需（本机放在 `C:\Users\SONG\Tools\esp-qemu`，也可用 `ESP_QEMU` 指定）。`-M help`
里出现 `esp32c3` 就会自动选中它：

```powershell
python bench\qemu_check.py                  # 自动优先 esp32c3
python bench\qemu_check.py -machine virt -qemu C:\msys64\ucrt64\bin\qemu-system-riscv32.exe
```

最近一次结果（乐鑫 QEMU）：

```
machine: esp32c3   qemu: C:\Users\SONG\Tools\esp-qemu\qemu\bin\qemu-system-riscv32.exe
image base=0x40000000 stack=0x3FCDF000 uart=0x00060000 exit=spin

workload               expected     qemu              sec  result
fib(28)                0x0004D973   0x0004D973      0.039  PASS
pi (Q16.16, N=10000)   0x00032420   0x00032420      0.028  PASS
bubble sort x19000     0x00000000   0x00000000      0.033  PASS
sieve(1001) x1340      0x000000A8   0x000000A8      0.031  PASS
```

注意：

- 上游 `virt` 的 DRAM 在 `0x80000000`，必须重链接；脚本用 `-base/-stack/-data` 重新生成，
  计算部分与 rv32emu 的镜像逐条相同，只换出口。
- `esp32c3` 机器只实现 384 KiB DRAM（到 `0x3FCDFFFF`），所以栈放在 `0x3FCDF000`。
  rv32emu 按 512 KiB 窗口建模，`0x3FCFF000` 也能用——真机/QEMU 不行。
- TinyOS 依赖 C3 的 flash/ROM 布局和 littlefs 分区，要跑得先按 esp-idf 的方式生成 flash 镜像，
  不在本仓库的校验范围内。

## 速度对比

计时口径：**只计 `RunHot(1000)` 批次 + TIMG0 tick**，不含进程启动、ELF 加载、终端输出；每个负载跑 7 次取中位数。Go 侧用 `bench/benchdata/benchrun`（与 Go 仓库 `tools/benchrun` 同一份源码），C 侧用 `-bench`，两边是同一套批次循环。

主机：AMD Ryzen 7 255（8C/16T），Windows，Go 1.27.3，gcc 16.2.0，C 编译选项 `-O3 -DNDEBUG -flto`。

| 负载 | 执行指令数 | Go MIPS | C MIPS | C/Go |
|---|---:|---:|---:|---:|
| fib(28) 递归 | 15,426,855 | ~381 | ~441 | 1.16x |
| pi（Leibniz，含 M 扩展除法） | 2,000,000 | ~379 | ~432 | 1.14x |
| ALU 紧循环 | 2,000,000 | ~429 | ~441 | 1.03x |
| 内存读写循环 | 2,000,000 | ~441 | ~529 | 1.20x |
| 冒泡排序 | 19,950,006 | ~412 | ~465 | 1.13x |
| 素数筛 | 19,931,166 | ~465 | ~503 | 1.08x |
| TinyOS 启动到 shell | 5,000,000 | ~345 | ~406 | 1.18x |

结论：

- 总体 **1.02x ~ 1.22x**，没有哪类负载出现 C 反而更慢；纯 ALU 循环基本追平。
- **访存 + 分支**（内存循环、排序、筛法）差距最大，约 8%~20%：每次 load/store 都要过
  `priv`/PMP/平铺区判断，C 生成的代码更紧凑。
- **纯 ALU / 除法**（pi、ALU loop）约 3%~14%：Go 的块解释器把位域解析摊掉之后，一个块里
  几乎没有额外开销。fib 的递归压栈/出栈让 C 领先约 16%。
- **真实系统负载**（TinyOS 启动，含 U-mode、PMP、timer 中断、UART）约 16%~18%。
- 抖动不小：同一个二进制连跑 5 次，Go 在 pi 上是 368~430 MIPS、TinyOS 是 285~357；
  C 在 pi 上是 425~440。表里是三次完整对比的中位数，比值按区间看。排序/筛法是自然结束的
  ~20M 条程序，相对稳定，但也受主机温度/后台负载影响。
- **计时器必须与 Go 仓库 `tools/benchrun/main.go` 逐字节一致**（`bench/benchdata/benchrun`
  就是这份拷贝）。曾经把热循环包成 `measure()` → `measureBody()` 两层函数再编译，Go 侧
  直接慢 **1.7 倍**，算出来的倍数全是假的——循环的结构会改变 Go 的内联/逃逸分析结果。
- 这些数字会随主机状态漂移；跨机器只比倍数，不比绝对值。

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
C 侧同样 16000 条约 0.000028 s）。早期几个 README 里「Unicorn 快 13.8x / 2.0x」的数字来自另一台机器、另一版 Unicorn，
本仓库复现不出来，已从 Go 版 README 撤下。详细复现方法见 `bench/README.md`。

复现：

```powershell
cd rv32emu-c
.\bench\compare.ps1 -Reps 7 -IncludeTinyOS
```

脚本会编译两边、生成 `fib/pi/alu/mem/sort/sieve` ELF，输出表格并写入 `bench/results.md`。

## 边界

与 Go 版对齐的语义边界（完整列表见 Go 仓库 README 的「当前边界」），以及 C 版特有的部分：

- **指令集**：RV32IMC（I/M/C）。没有 F/D，没有 A —— 原子指令按非法指令 trap。
- **固件布局**：只支持扁平链接、从 `0x40000000` 执行的镜像。ESP-IDF 默认的 `0x42000000`
  指令别名和 `0x3C000000` 数据别名没有建模，这类镜像要先重链接（TinyOS 是扁平链接的）。
- **外设**：只覆盖 TinyOS 用到的 UART0 / GPIO 影子 / TIMG0 / INTMTX（被动影子）/ SPI1 扇区
  擦除 / USJ（黑洞）/ RTC_CNTL（被动）。**UART 接收中断没有建模**，输入走内核的 tick 轮询
  兜底；没有看门狗、deep sleep、USB-Serial-JTAG 协议。未建模地址读 0、写丢弃并按地址
  去重告警。
- **时序**：tick 是「每 N 条指令」而不是墙钟量，单条指令耗时均匀；没有总线竞争、cache、
  flash 等待周期。`-tickdiv` 用来做时序敏感性实验——TinyOS 的 `sched_switch` 竞态就是
  这样复现的。
- **没有 GDB stub**：调试靠 `-traplog` / `-breakpc` / `-dumpram` / `-step` 这套快照工具。
  比 Go 版少的调试开关见上面「已支持的参数」下的说明。
- **控制台 / 平台**：控制台有 Win32 与 POSIX 两条实现，模拟器本体在 Linux 也能编译；
  `tools/leakrun.c`、`tools/leakunit.c` 是 Windows 专用（用 `GetProcessMemoryInfo` 读工作集
  和私有内存），非 Windows 环境跳过。
- **内存所有权**：没有 GC。`bus_new`/`cpu_new`/各设备各自持有资源，`_run` 结束或
  `_shutdown` 时必须释放；对应的泄漏检查在下一节。

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

- UART 接收是**轮询**，不是后台线程：`console_input()` 每个批次从控制台/管道/标准输入读一次，
  再按水位压进 128 字节 FIFO。复位只清芯片侧 RX FIFO，主机侧预输入保留（与真机一致）。
- 不需要 pthread：UART 接收改成轮询之后，代码里没有任何线程 API。
- 生成物（`build/`、`*.elf`）已在 `.gitignore` 中排除。
