# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ChampSim is a trace-based microarchitecture simulator. It models a CPU pipeline with configurable cache hierarchies, branch predictors, prefetchers, and replacement policies. Traces (`.champsimtrace.xz`) drive the simulation.

## Build Commands

```bash
# First-time setup
git submodule update --init && vcpkg/bootstrap-vcpkg.sh && vcpkg/vcpkg install

# Configure + build (always reconfigure after changing JSON or adding modules)
./config.sh <config.json> && make

# Run simulation
bin/champsim --warmup-instructions 200000000 --simulation-instructions 500000000 path/to/trace.champsimtrace.xz

# Tests
make test                  # All C++ tests (Catch2)
make test TEST_NUM=043     # Single test by number
make pytest                # Python tests

# Clean
make clean                 # Object files and binaries
make configclean           # Also removes generated config
```

## Architecture

### Simulation Loop (`src/champsim.cc`)

`do_phase()` runs warmup or ROI phases. Each cycle:
1. Sorts all operables (CPUs, caches, DRAM) by `current_time` (supports multi-frequency)
2. Calls `operate()` on each component
3. Reads trace packets into CPU input queues
4. Checks phase completion (retired instruction count or trace EOF)

Livelock detection monitors IPC every 10M cycles.

### CPU Models

**O3_CPU** (`src/ooo_cpu.cc`, `inc/ooo_cpu.h`): Out-of-order superscalar with ROB, LSQ, register renaming, and full pipeline (fetch → decode → dispatch → schedule → execute → retire).

**InOrderCPU** (`src/inorder_cpu.cc`, `inc/inorder_cpu.h`): Inherits from O3_CPU, overrides `operate()` with a strict 5-stage pipeline (fetch → decode → execute → memory → writeback). Stages execute in reverse order per cycle. Skips DIB lookup. Select via `"cpu_model": "inorder"` in config JSON (see `inorder_config.json` for example).

### Component Communication

Components communicate through **Channels** (`inc/channel.h`):
- **RQ** (Read Queue): loads/prefetches
- **WQ** (Write Queue): stores
- **PQ** (Prefetch Queue): prefetch-only
- **returned**: response queue back to requestor

Each request carries address, instruction ID, and dependency list (`instr_depend_on_me`).

### Address Type System (`inc/address.h`)

Strong types prevent accidental address misuse:
- `champsim::address` — full 64-bit address
- `champsim::block_number` / `block_offset` — cache block granularity
- `champsim::page_number` / `page_offset` — page granularity

Use `.slice<upper, lower>()`, `.slice_upper<N>()`, `.slice_lower<N>()` to extract bits. Use `champsim::splice()` to combine slices.

### Module System

Pluggable modules in `branch/`, `btb/`, `prefetcher/`, `replacement/`. Each directory contains one module per subdirectory.

**Module interfaces** (detected via SFINAE in `inc/modules.h`):
- Branch predictors: `predict_branch()`, `last_branch_result()`
- BTB: `btb_prediction()`, `update_btb()`
- Prefetchers: `prefetcher_cache_operate()`, `prefetcher_cache_fill()`, `prefetcher_cycle_operate()`
- Replacement: `find_victim()`, `update_replacement_state()`

Modules are bound to their parent (`O3_CPU` for branch/BTB, `CACHE` for prefetcher/replacement) via type-erased pimpls.

**Adding a module:**
1. `mkdir prefetcher/mypref && cp prefetcher/no/no.cc prefetcher/mypref/mypref.cc`
2. Implement the interface functions
3. Reference in config JSON: `"L2C": { "prefetcher": "mypref" }`
4. `./config.sh config.json && make`

### Configuration System

JSON-based config → Python scripts (`config/`) → generated build files:
- `config/parse.py` — validates JSON, merges defaults
- `config/modules.py` — discovers module paths
- `config/makefile.py` — generates `_configuration.mk`
- `config/cxx.py` + `config/instantiation_file.py` — generates C++ module instantiation code

Output: `_configuration.mk`, `.csconfig/` directory. See `champsim_config.json` for all options.

### Key Headers

- `inc/champsim.h` — core types and constants
- `inc/instruction.h` — `ooo_model_instr` with pipeline flags (`dib_checked`, `fetch_issued`, `decoded`, `scheduled`, `executed`, `completed`)
- `inc/modules.h` — module interface definitions and SFINAE detection
- `inc/defaults.hpp` — default configuration values
- `inc/cache.h` — cache interface
- `inc/ooo_cpu.h` — CPU interface (also base for InOrderCPU)

### Legacy Modules

Detected by `__legacy__` marker files. Bridge code auto-generated via `config/legacy.py`.
