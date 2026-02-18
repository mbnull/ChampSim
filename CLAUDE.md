# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ChampSim is a trace-based simulator for microarchitecture study. It simulates an out-of-order CPU with configurable cache hierarchies, branch predictors, prefetchers, and replacement policies.

## Build System

### Dependencies
ChampSim uses vcpkg as a submodule for dependency management. Initialize dependencies:
```bash
git submodule update --init
vcpkg/bootstrap-vcpkg.sh
vcpkg/vcpkg install
```

### Configuration and Build
ChampSim uses a two-step build process with JSON configuration:

1. Configure: `./config.sh <configuration_file.json>`
2. Build: `make`

The configuration script is a Python script (`config.sh`) that generates:
- `_configuration.mk` - Makefile fragment with build targets
- `.csconfig/` - Generated configuration files and object files
- Module-specific files in module directories

The binary is output to `bin/<executable_name>` (default: `bin/champsim`).

### Running Simulations
```bash
bin/champsim --warmup-instructions 200000000 --simulation-instructions 500000000 path/to/trace.champsimtrace.xz
```

Traces are compressed with xz and typically have `.champsimtrace.xz` extension.

## Testing

### C++ Unit Tests
Built with Catch2. Run all tests:
```bash
make test
```

Run specific test by number (e.g., test #043):
```bash
make test TEST_NUM=043
```

Test executable: `test/bin/000-test-main`
Test sources: `test/cpp/src/`

### Python Tests
```bash
make pytest
```

Python tests are in `test/python/`.

### Clean Targets
- `make clean` - Remove object files and binaries
- `make configclean` - Remove configuration files and clean
- `make compile_commands_clean` - Remove compile_commands.json files

## Architecture

### Core Components

**Out-of-Order CPU (`O3_CPU`)**: Simulates a superscalar out-of-order processor with:
- Instruction fetch, decode, dispatch, execute, retire pipeline stages
- ROB (Reorder Buffer), LSQ (Load/Store Queue)
- Register renaming and scheduling
- Source: `src/ooo_cpu.cc`, Header: `inc/ooo_cpu.h`

**Cache Hierarchy**: Configurable multi-level cache system:
- L1I (Instruction), L1D (Data), L2C, LLC (Last Level Cache)
- TLBs: ITLB, DTLB, STLB
- PTW (Page Table Walker)
- DIB (Decoded Instruction Buffer)
- Source: `src/cache.cc`, Header: `inc/cache.h`

**DRAM Controller**: Memory controller simulation
- Source: `src/dram_controller.cc`, Header: `inc/dram_controller.h`

**Channel System**: Communication between components
- Source: `src/channel.cc`, Header: `inc/channel.h`

### Module System

ChampSim uses a plugin-based module system for extending functionality. Modules are organized by type:

**Branch Predictors** (`branch/`):
- `bimodal/` - Simple 2-bit counter predictor
- `gshare/` - Global history predictor
- `hashed_perceptron/` - Perceptron-based predictor
- `perceptron/` - Basic perceptron predictor

**BTB (Branch Target Buffer)** (`btb/`):
- `basic_btb/` - Basic branch target buffer

**Prefetchers** (`prefetcher/`):
- `next_line/` - Sequential prefetcher
- `ip_stride/` - Instruction pointer stride prefetcher
- `no/` - No prefetching
- `spp_dev/` - Signature Path Prefetcher
- `va_ampm_lite/` - Virtual address AMPM lite

**Replacement Policies** (`replacement/`):
- `lru/` - Least Recently Used
- `drrip/` - Dynamic Re-Reference Interval Prediction
- `srrip/` - Static Re-Reference Interval Prediction
- `ship/` - SHiP replacement policy
- `random/` - Random replacement

### Adding Custom Modules

To add a new module (e.g., a prefetcher):

1. Create module directory: `mkdir prefetcher/mypref`
2. Copy template: `cp prefetcher/no/no.cc prefetcher/mypref/mypref.cc`
3. Implement your algorithm in the `.cc` file
4. Add to configuration JSON:
```json
{
    "L2C": {
        "prefetcher": "mypref"
    }
}
```
5. Configure and build: `./config.sh config.json && make`

Module interface functions vary by type:
- Branch predictors: `predict_branch()`, `last_branch_result()`
- Prefetchers: `prefetcher_cache_operate()`, `prefetcher_cache_fill()`
- Replacement policies: `find_victim()`, `update_replacement_state()`

### Configuration System

Configuration is JSON-based. See `champsim_config.json` for a fully-specified example. Key sections:

- `executable_name` - Output binary name
- `block_size`, `page_size` - Memory granularity
- `num_cores` - Number of CPU cores
- `ooo_cpu[]` - CPU configuration (ROB size, queue sizes, widths, branch predictor, BTB)
- `L1I`, `L1D`, `L2C`, `LLC` - Cache configurations (sets, ways, latencies, prefetchers, replacement)
- `ITLB`, `DTLB`, `STLB` - TLB configurations
- `physical_memory` - DRAM configuration
- `virtual_memory` - Virtual memory system configuration

All configuration options are optional and will use defaults if not specified.

### Key Headers

- `inc/champsim.h` - Core types and constants
- `inc/address.h` - Address manipulation and slicing
- `inc/cache.h` - Cache interface
- `inc/ooo_cpu.h` - CPU interface
- `inc/instruction.h` - Instruction representation
- `inc/modules.h` - Module interface definitions
- `inc/defaults.hpp` - Default configuration values

### Legacy Module Support

The build system supports legacy modules through a compatibility layer:
- `config/legacy.py` - Generates legacy bridge code
- Legacy modules are detected by `__legacy__` marker files
- Bridge files (`legacy_bridge.h`, `legacy_bridge.cc`) are auto-generated

## Development Workflow

1. Modify configuration JSON or add/modify modules
2. Run `./config.sh <config.json>` to regenerate build files
3. Run `make` to build
4. Run tests with `make test` or `make pytest`
5. Run simulations with `bin/champsim`

When modifying core simulator code (in `src/` or `inc/`), just run `make` after changes.

## Code Organization

- `src/` - Core simulator implementation
- `inc/` - Public headers
- `branch/`, `btb/`, `prefetcher/`, `replacement/` - Module implementations
- `config/` - Python configuration scripts
- `test/` - Test suite (C++ and Python)
- `tracer/` - Trace generation utilities
- `docs/` - Documentation
- `.csconfig/` - Generated configuration (gitignored)
- `bin/` - Output binaries (gitignored)
