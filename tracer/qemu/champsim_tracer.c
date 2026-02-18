/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * ChampSim Tracer QEMU Plugin for RISC-V
 *
 * This plugin generates ChampSim-compatible traces for RV32I/RV64I programs.
 *
 * Usage (bare-metal):
 *   qemu-system-riscv32 -M virt -nographic -kernel program.elf -plugin /path/to/libchampsim_tracer.so,arg="output=trace.bin,skip=0,count=1000000"
 *   qemu-system-riscv64 -M virt -nographic -kernel program.elf -plugin /path/to/libchampsim_tracer.so,arg="output=trace.bin,skip=0,count=1000000"
 *
 * Usage (Linux binaries):
 *   qemu-riscv32 -plugin /path/to/libchampsim_tracer.so,arg="output=trace.bin,skip=0,count=1000000" program.elf
 *   qemu-riscv64 -plugin /path/to/libchampsim_tracer.so,arg="output=trace.bin,skip=0,count=1000000" program.elf
 */

#include <assert.h>
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* Trace instruction format matching ChampSim's input_instr */
#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES 4

typedef struct {
    uint64_t ip;                                        // instruction pointer
    uint8_t is_branch;                                  // is this a branch?
    uint8_t branch_taken;                               // was the branch taken?
    uint8_t destination_registers[NUM_INSTR_DESTINATIONS]; // output registers
    uint8_t source_registers[NUM_INSTR_SOURCES];        // input registers
    uint64_t destination_memory[NUM_INSTR_DESTINATIONS]; // output memory addresses
    uint64_t source_memory[NUM_INSTR_SOURCES];          // input memory addresses
} trace_instr_format_t;

/* Global state */
static FILE *trace_file = NULL;
static uint64_t instr_count = 0;
static uint64_t skip_count = 0;
static uint64_t trace_count = 1000000;
static char *output_filename = NULL;
static GMutex lock;

/* Current instruction being traced */
static __thread trace_instr_format_t curr_instr;

/* RISC-V register mapping to ChampSim format */
static inline uint8_t map_riscv_reg(int reg) {
    /* RISC-V has 32 integer registers (x0-x31)
     * We map them directly to 0-31 for ChampSim */
    if (reg >= 0 && reg < 32) {
        return (uint8_t)reg;
    }
    return 0; // x0 (zero register) for invalid
}

/* Add register to the set if not already present */
static void add_reg_to_set(uint8_t *reg_array, size_t array_size, uint8_t reg) {
    if (reg == 0) return; // Skip zero register

    for (size_t i = 0; i < array_size; i++) {
        if (reg_array[i] == 0) {
            reg_array[i] = reg;
            return;
        }
        if (reg_array[i] == reg) {
            return; // Already in set
        }
    }
}

/* Add memory address to the set if not already present */
static void add_mem_to_set(uint64_t *mem_array, size_t array_size, uint64_t addr) {
    if (addr == 0) return;

    for (size_t i = 0; i < array_size; i++) {
        if (mem_array[i] == 0) {
            mem_array[i] = addr;
            return;
        }
        if (mem_array[i] == addr) {
            return; // Already in set
        }
    }
}

/* Reset current instruction */
static void reset_curr_instr(uint64_t pc) {
    memset(&curr_instr, 0, sizeof(curr_instr));
    curr_instr.ip = pc;
}

/* Write current instruction to trace file */
static void write_curr_instr(void) {
    g_mutex_lock(&lock);

    instr_count++;

    if (instr_count > skip_count &&
        instr_count <= (skip_count + trace_count)) {

        if (trace_file) {
            fwrite(&curr_instr, sizeof(trace_instr_format_t), 1, trace_file);
        }

        /* Print progress every 100k instructions */
        if ((instr_count - skip_count) % 100000 == 0) {
            qemu_plugin_outs("Traced ");
            g_autofree gchar *out = g_strdup_printf("%" PRIu64 " instructions\n",
                                                     instr_count - skip_count);
            qemu_plugin_outs(out);
        }
    }

    /* Stop tracing if we've reached the limit */
    if (instr_count >= (skip_count + trace_count)) {
        if (trace_file) {
            fclose(trace_file);
            trace_file = NULL;
        }
        qemu_plugin_outs("Tracing complete!\n");
    }

    g_mutex_unlock(&lock);
}

/* Callback for instruction execution */
static void vcpu_insn_exec(unsigned int vcpu_index, void *userdata) {
    uint64_t pc = (uint64_t)userdata;
    reset_curr_instr(pc);
}

/* Callback for memory read */
static void vcpu_mem_read(unsigned int vcpu_index, qemu_plugin_meminfo_t info,
                          uint64_t vaddr, void *userdata) {
    add_mem_to_set(curr_instr.source_memory, NUM_INSTR_SOURCES, vaddr);
}

/* Callback for memory write */
static void vcpu_mem_write(unsigned int vcpu_index, qemu_plugin_meminfo_t info,
                           uint64_t vaddr, void *userdata) {
    add_mem_to_set(curr_instr.destination_memory, NUM_INSTR_DESTINATIONS, vaddr);
}

/* Callback after instruction execution */
static void vcpu_insn_exec_after(unsigned int vcpu_index, void *userdata) {
    write_curr_instr();
}

/* Parse RISC-V instruction to extract register operands */
static void parse_riscv_insn(uint32_t insn, uint64_t pc) {
    uint32_t opcode = insn & 0x7F;
    uint32_t rd = (insn >> 7) & 0x1F;
    uint32_t rs1 = (insn >> 15) & 0x1F;
    uint32_t rs2 = (insn >> 20) & 0x1F;
    uint32_t funct3 = (insn >> 12) & 0x7;

    /* Detect branches */
    if (opcode == 0x63) { // Branch instructions
        curr_instr.is_branch = 1;
        add_reg_to_set(curr_instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        add_reg_to_set(curr_instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs2));
    }
    /* JAL */
    else if (opcode == 0x6F) {
        curr_instr.is_branch = 1;
        curr_instr.branch_taken = 1;
        if (rd != 0) {
            add_reg_to_set(curr_instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
        }
    }
    /* JALR */
    else if (opcode == 0x67) {
        curr_instr.is_branch = 1;
        curr_instr.branch_taken = 1;
        add_reg_to_set(curr_instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        if (rd != 0) {
            add_reg_to_set(curr_instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
        }
    }
    /* R-type instructions */
    else if (opcode == 0x33 || opcode == 0x3B) {
        add_reg_to_set(curr_instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        add_reg_to_set(curr_instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs2));
        if (rd != 0) {
            add_reg_to_set(curr_instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
        }
    }
    /* I-type instructions (including loads) */
    else if (opcode == 0x13 || opcode == 0x1B || opcode == 0x03 || opcode == 0x73) {
        add_reg_to_set(curr_instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        if (rd != 0) {
            add_reg_to_set(curr_instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
        }
    }
    /* S-type instructions (stores) */
    else if (opcode == 0x23) {
        add_reg_to_set(curr_instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        add_reg_to_set(curr_instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs2));
    }
    /* U-type instructions (LUI, AUIPC) */
    else if (opcode == 0x37 || opcode == 0x17) {
        if (rd != 0) {
            add_reg_to_set(curr_instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
        }
    }
}

/* Callback for instruction translation */
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n_insns = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n_insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);

        /* Register callback before instruction execution */
        qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                               QEMU_PLUGIN_CB_NO_REGS,
                                               (void *)pc);

        /* Parse instruction for register operands */
        size_t insn_size = qemu_plugin_insn_size(insn);
        const uint8_t *insn_data = (const uint8_t *)qemu_plugin_insn_data(insn);

        if (insn_size == 4) {
            uint32_t insn_word = *(uint32_t *)insn_data;
            parse_riscv_insn(insn_word, pc);
        }

        /* Register memory callbacks */
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem_read,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_R, NULL);
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem_write,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_W, NULL);

        /* Register callback after instruction execution */
        qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec_after,
                                               QEMU_PLUGIN_CB_NO_REGS, NULL);
    }
}

/* Plugin initialization */
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv) {
    /* Parse arguments */
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);

        if (g_strcmp0(tokens[0], "output") == 0) {
            output_filename = g_strdup(tokens[1]);
        } else if (g_strcmp0(tokens[0], "skip") == 0) {
            skip_count = g_ascii_strtoull(tokens[1], NULL, 10);
        } else if (g_strcmp0(tokens[0], "count") == 0) {
            trace_count = g_ascii_strtoull(tokens[1], NULL, 10);
        } else {
            fprintf(stderr, "Unknown option: %s\n", tokens[0]);
            return -1;
        }
    }

    /* Set default output filename if not specified */
    if (!output_filename) {
        output_filename = g_strdup("champsim.trace");
    }

    /* Open trace file */
    trace_file = fopen(output_filename, "wb");
    if (!trace_file) {
        fprintf(stderr, "Failed to open trace file: %s\n", output_filename);
        return -1;
    }

    g_mutex_init(&lock);

    /* Print configuration */
    g_autofree gchar *config = g_strdup_printf(
        "ChampSim Tracer Configuration:\n"
        "  Output file: %s\n"
        "  Skip instructions: %" PRIu64 "\n"
        "  Trace instructions: %" PRIu64 "\n",
        output_filename, skip_count, trace_count);
    qemu_plugin_outs(config);

    /* Register translation block callback */
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);

    return 0;
}
