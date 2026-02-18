# ChampSim QEMU Tracer for RISC-V

This QEMU plugin generates ChampSim-compatible traces for RISC-V (RV32I/RV64I) baremetal programs.

## Features

- Supports RV32I and RV64I instruction sets
- Captures instruction pointer, branch behavior, register dependencies, and memory accesses
- Compatible with ChampSim's `input_instr` trace format
- Configurable instruction skip and trace count
- Thread-safe for multi-core tracing

## Prerequisites

- QEMU with plugin support (QEMU 5.0+)
- GLib development libraries
- GCC or compatible C compiler

### Installing QEMU with Plugin Support

Most modern distributions include QEMU with plugin support:

```bash
# Ubuntu/Debian
sudo apt-get install qemu-user qemu-system-misc libglib2.0-dev pkg-config

# Fedora/RHEL
sudo dnf install qemu-user qemu-system-riscv libglib2-devel pkgconfig

# Arch Linux
sudo pacman -S qemu glib2 pkgconf
```

To verify plugin support:
```bash
qemu-riscv32 --help | grep plugin
```

## Building

```bash
cd tracer/qemu
make
```

This will produce `libchampsim_tracer.so`.

## Usage

### Basic Usage

```bash
qemu-riscv32 -plugin ./libchampsim_tracer.so program.elf
```

This generates `champsim.trace` in the current directory.

### With Options

```bash
qemu-riscv32 -plugin ./libchampsim_tracer.so,arg="output=mytrace.bin,skip=1000000,count=10000000" program.elf
```

**Plugin Arguments:**
- `output=<filename>` - Output trace file (default: `champsim.trace`)
- `skip=<count>` - Number of instructions to skip before tracing (default: 0)
- `count=<count>` - Number of instructions to trace (default: 1000000)

### For RV64I Programs

```bash
qemu-riscv64 -plugin ./libchampsim_tracer.so,arg="output=trace64.bin" program64.elf
```

### Compressing Traces

ChampSim typically uses xz-compressed traces:

```bash
xz -9 mytrace.bin
# Produces mytrace.bin.xz
```

## Example Workflow

1. **Compile your RISC-V baremetal program:**
```bash
riscv32-unknown-elf-gcc -march=rv32i -mabi=ilp32 -nostdlib -o program.elf program.c
```

2. **Generate trace:**
```bash
qemu-riscv32 -plugin ./libchampsim_tracer.so,arg="output=program.trace,skip=0,count=5000000" program.elf
```

3. **Compress trace:**
```bash
xz -9 program.trace
```

4. **Run ChampSim simulation:**
```bash
cd ../..
./config.sh champsim_config.json
make
bin/champsim --warmup-instructions 1000000 --simulation-instructions 4000000 tracer/qemu/program.trace.xz
```

## Trace Format

The plugin generates binary traces with the following structure per instruction:

```c
struct trace_instr {
    uint64_t ip;                          // Instruction pointer (PC)
    uint8_t is_branch;                    // 1 if branch instruction
    uint8_t branch_taken;                 // 1 if branch was taken
    uint8_t destination_registers[2];     // Destination register IDs
    uint8_t source_registers[4];          // Source register IDs
    uint64_t destination_memory[2];       // Memory write addresses
    uint64_t source_memory[4];            // Memory read addresses
};
```

### RISC-V Register Mapping

RISC-V registers (x0-x31) are mapped directly to register IDs 0-31:
- x0 (zero) = 0
- x1 (ra) = 1
- x2 (sp) = 2
- ...
- x31 = 31

## Supported Instructions

The tracer handles all RV32I base instructions:
- **R-type**: ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND
- **I-type**: ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI, LB, LH, LW, LBU, LHU
- **S-type**: SB, SH, SW
- **B-type**: BEQ, BNE, BLT, BGE, BLTU, BGEU
- **U-type**: LUI, AUIPC
- **J-type**: JAL, JALR

For RV64I, it also handles 64-bit variants (e.g., ADDIW, LWU, LD, SD).

## Limitations

- Compressed instructions (RVC) are expanded by QEMU before reaching the plugin
- Branch taken information for conditional branches is captured after execution
- Floating-point and vector registers are not currently tracked
- System instructions (CSR operations) are tracked but may not be fully detailed

## Troubleshooting

**Plugin fails to load:**
- Ensure QEMU was built with plugin support
- Check that glib2 is properly installed
- Verify the plugin path is correct

**No trace file generated:**
- Check file permissions in the output directory
- Ensure the program executes instructions (not stuck in infinite loop before skip count)

**Trace file is empty:**
- Verify your program executes enough instructions
- Check that `skip` count is not too high
- Ensure the program doesn't exit before tracing begins

**Compilation errors:**
- Install glib2 development headers: `sudo apt-get install libglib2.0-dev`
- Install pkg-config: `sudo apt-get install pkg-config`

## Performance Notes

- Tracing adds significant overhead (10-100x slowdown)
- Use `skip` parameter to avoid tracing initialization code
- Limit `count` to reasonable values (1-10 million instructions)
- Consider tracing specific regions of interest rather than entire programs

## Differences from PIN Tracer

- QEMU plugin API is simpler than PIN but less detailed
- Branch taken information may be less precise for some edge cases
- No support for x86-specific features (obviously)
- Better suited for embedded/baremetal programs
- Cross-architecture tracing without native hardware

## Contributing

To extend this tracer:
- Add support for RV32M/A/F/D extensions in `parse_riscv_insn()`
- Implement compressed instruction (RVC) handling
- Add support for vector extensions (RVV)
- Improve branch prediction accuracy tracking

## License

Licensed under Apache License 2.0, same as ChampSim.
