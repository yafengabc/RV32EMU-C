// genwork generates the -max parity images, the ecall-terminated run images
// and the plain loop images used by `bench compare` (Go port of the old
// bench/gen_workloads.py).
//
//	go run ./genwork -out DIR [-n N]
package main

import (
	"encoding/binary"
	"flag"
	"os"
	"path/filepath"
)

const base = 0x40000000

// ---- RV32IM encoders (identical to bench/gen_workloads.py) ----
func i(rd, rs1, imm, f3, op int) uint32 {
	return uint32(((imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op)
}
func r(rd, rs1, rs2, f3, f7 int) uint32 {
	return uint32((f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x33)
}
func b(rs1, rs2, imm, f3 int) uint32 {
	u := imm & 0x1FFF
	return uint32(((u>>12)&1)<<31 | ((u>>5)&0x3F)<<25 | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
		((u>>1)&0xF)<<8 | ((u>>11)&1)<<7 | 0x63)
}
func lui(rd, imm20 int) uint32 {
	return uint32((imm20 << 12) | (rd << 7) | 0x37)
}
func j(rd, imm int) uint32 {
	x := imm & 0x1FFFFF
	return uint32(((x>>20)&1)<<31 | ((x>>1)&0x3FF)<<21 | ((x>>11)&1)<<20 | ((x>>12)&0xFF)<<12 | (rd << 7) | 0x6F)
}

var filler = i(5, 5, 1, 0, 0x13) // addi x5, x5, 1

// write one PT_LOAD ELF32 at base, exactly like the Python version.
func writeELF(out, name string, words []uint32) error {
	text := make([]byte, 4*len(words))
	for i, w := range words {
		binary.LittleEndian.PutUint32(text[4*i:], w)
	}
	ehdr := make([]byte, 52)
	copy(ehdr[0:], []byte{0x7f, 'E', 'L', 'F', 1, 1, 1, 0})
	binary.LittleEndian.PutUint16(ehdr[16:], 2)    // e_type
	binary.LittleEndian.PutUint16(ehdr[18:], 243)  // e_machine
	binary.LittleEndian.PutUint32(ehdr[20:], 1)    // e_version
	binary.LittleEndian.PutUint32(ehdr[24:], base) // e_entry
	binary.LittleEndian.PutUint32(ehdr[28:], 52)   // e_phoff
	binary.LittleEndian.PutUint32(ehdr[36:], 0)    // e_flags
	binary.LittleEndian.PutUint16(ehdr[40:], 52)   // e_ehsize
	binary.LittleEndian.PutUint16(ehdr[42:], 32)   // e_phentsize
	binary.LittleEndian.PutUint16(ehdr[44:], 1)    // e_phnum
	ph := make([]byte, 32)
	binary.LittleEndian.PutUint32(ph[0:], 1)     // PT_LOAD
	binary.LittleEndian.PutUint32(ph[4:], 84)    // offset
	binary.LittleEndian.PutUint32(ph[8:], base)  // vaddr
	binary.LittleEndian.PutUint32(ph[12:], base) // paddr
	binary.LittleEndian.PutUint32(ph[16:], uint32(len(text)))
	binary.LittleEndian.PutUint32(ph[20:], uint32(len(text)))
	binary.LittleEndian.PutUint32(ph[24:], 7) // flags RWX
	binary.LittleEndian.PutUint32(ph[28:], 4) // align
	return os.WriteFile(filepath.Join(out, name), append(append(ehdr, ph...), text...), 0o644)
}

func main() {
	out := flag.String("out", ".", "output directory")
	n := flag.Int("n", 2_000_000, "parity filler length")
	flag.Parse()
	if err := os.MkdirAll(*out, 0o755); err != nil {
		panic(err)
	}
	ecall := i(0, 0, 0, 0, 0x73)

	// 4-instruction ALU loop: the branch jumps back to word 0.
	alu := []uint32{i(5, 5, 1, 0, 0x13), i(6, 0, 1, 0, 0x13), b(5, 6, -8, 4), 0x00000013}

	// 3-instruction memory loop over one page, unsigned +.
	mem := []uint32{
		lui(8, 0x3FC80),      // x8 = 0x3FC80000 (SRAM scratch, NOT the code page)
		i(9, 0, 1, 0, 0x13),  // step = 1
		i(10, 0, 0, 0, 0x13), // sum = 0
		i(14, 8, 0, 2, 0x03), // loop: lw x14, 0(x8)
		r(10, 10, 14, 0, 0),  // sum += x14
		i(0, 8, 0, 2, 0x23),  // sw x0, 0(x8)
		i(8, 8, 4, 0, 0x13),  // ptr += 4
		i(9, 9, 1, 0, 0x13),  // n++
		b(9, 0, -20, 1),      // bne n, x0 -> loop
	}

	// M-extension loop: mul, divu and a branch.
	pi := []uint32{
		lui(5, 0x20000),      // x5 = 0x20000000
		i(6, 0, 1, 0, 0x13),  // x6 = 1
		i(7, 0, 0, 0, 0x13),  // x7 = 0
		r(9, 8, 6, 0, 0x01),  // loop: mul x9, x8, x6
		r(10, 9, 6, 5, 0x01), // divu x10, x9, x6
		r(7, 7, 10, 0, 0),    // add x7, x7, x10
		i(6, 6, 1, 0, 0x13),  // x6++
		b(6, 0, -16, 1),      // bne x6, x0 -> loop
	}

	// parity images: body + filler to exactly N instructions + ecall
	writeFiles := func(name string, body []uint32, n int) {
		words := append(append([]uint32{}, body...), make([]uint32, n-len(body))...)
		for i := len(body); i < n; i++ {
			words[i] = filler
		}
		words = append(words, ecall)
		if err := writeELF(*out, name, words); err != nil {
			panic(err)
		}
	}
	writeFiles("alu_n.elf", alu, *n)
	writeFiles("mem_n.elf", mem, *n)
	writeFiles("pi_n.elf", pi, *n)

	// real programs (ecall-terminated)
	writeRun := func(name string, body []uint32) {
		if err := writeELF(*out, name, append(append([]uint32{}, body...), ecall)); err != nil {
			panic(err)
		}
	}
	writeRun("alu_run.elf", alu)
	writeRun("mem_run.elf", mem)
	writeRun("pi_run.elf", pi)

	// plain endless loops: bench compare runs these with -max 2M
	if err := writeELF(*out, "alu_loop.elf", append(append([]uint32{}, alu[:3]...), j(0, -12))); err != nil {
		panic(err)
	}
	if err := writeELF(*out, "mem_loop.elf", mem); err != nil {
		panic(err)
	}
	if err := writeELF(*out, "pi_big.elf", pi); err != nil {
		panic(err)
	}
	println("generated -max parity images, run images and loop images in", *out)
}
