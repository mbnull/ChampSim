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
 * Usage (bare-metal):
 *   qemu-system-riscv32 -M virt -nographic -kernel program.elf \
 *     -plugin /path/to/libchampsim_tracer.so,arg="output=trace.bin,skip=0,count=1000000"
 *   qemu-system-riscv64 -M virt -nographic -kernel program.elf \
 *     -plugin /path/to/libchampsim_tracer.so,arg="output=trace.bin,skip=0,count=1000000"
 *
 * Debug text dump:
 *   Add arg="debug=trace.txt" to also write a human-readable text file.
 *
 * Design note on memory callbacks:
 *   Each instruction gets a heap-allocated trace_instr_format_t pre-filled at
 *   translation time. All three callbacks (exec, mem, exec_after) receive the
 *   same pointer via userdata, so mem callbacks always write into the correct
 *   struct regardless of which host thread fires them.
 */

#include <glib.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES      4

typedef struct {
    uint64_t ip;
    uint8_t  is_branch;
    uint8_t  branch_taken;
    uint8_t  destination_registers[NUM_INSTR_DESTINATIONS];
    uint8_t  source_registers[NUM_INSTR_SOURCES];
    uint64_t destination_memory[NUM_INSTR_DESTINATIONS];
    uint64_t source_memory[NUM_INSTR_SOURCES];
} trace_instr_format_t;

/*
 * Per-instruction execution context.
 * Allocated once at translation time, shared by all callbacks for that insn.
 * is_conditional is not part of the trace format — it's used only to resolve
 * branch_taken at runtime.
 */
typedef struct {
    trace_instr_format_t instr;
    uint8_t              is_conditional;
    uint8_t              is_load;   /* 1 if this insn is a load  (opcode 0x03) */
    uint8_t              is_store;  /* 1 if this insn is a store (opcode 0x23) */
} insn_ctx_t;

/* Global state */
static FILE    *trace_file = NULL;
static FILE    *debug_file = NULL;
static uint64_t instr_count = 0;
static uint64_t skip_count  = 0;
static uint64_t trace_count = 1000000;
static GMutex   lock;

/*
 * Per-vCPU pending state.
 * pending_ctx points to the heap-allocated ctx of the previous instruction.
 * It is flushed at the start of the NEXT vcpu_insn_exec, by which time all
 * mem callbacks for that instruction have already written their addresses in.
 *
 * NOTE: we register vcpu_insn_exec_cb only ONCE per instruction.
 * QEMU 10 silently drops a second registration on the same insn, so the
 * old "exec + exec_after" pattern no longer works. Instead we flush the
 * previous ctx at the top of the current exec callback.
 */
static __thread insn_ctx_t *pending_ctx     = NULL;
static __thread uint64_t    pending_next_pc = 0;

/* ------------------------------------------------------------------ */

/* ChampSim magic register IDs (from inc/trace_instruction.h) */
#define CHAMPSIM_REG_SP    6   /* REG_STACK_POINTER       */
#define CHAMPSIM_REG_FLAGS 25  /* REG_FLAGS               */
#define CHAMPSIM_REG_IP    26  /* REG_INSTRUCTION_POINTER */

/* Map a RISC-V register number to a ChampSim register ID.
 * x2 (sp) -> CHAMPSIM_REG_SP so ChampSim recognises call/return patterns.
 * All others pass through as-is (1-31, avoiding 0 which means "unused"). */
static inline uint8_t map_riscv_reg(int reg) {
    if (reg == 2) return CHAMPSIM_REG_SP;
    if (reg >= 1 && reg < 32) return (uint8_t)reg;
    return 0;
}

static void add_reg(uint8_t *arr, size_t n, uint8_t reg) {
    if (reg == 0) return;
    for (size_t i = 0; i < n; i++) {
        if (arr[i] == 0) { arr[i] = reg; return; }
        if (arr[i] == reg) return;
    }
}

static void add_mem(uint64_t *arr, size_t n, uint64_t addr) {
    if (addr == 0) return;
    for (size_t i = 0; i < n; i++) {
        if (arr[i] == 0) { arr[i] = addr; return; }
        if (arr[i] == addr) return;
    }
}

/* ------------------------------------------------------------------ */

static void write_debug_line(const trace_instr_format_t *t) {
    fprintf(debug_file,
            "ip=0x%016" PRIx64
            " branch=%d taken=%d"
            " dst_regs=[%02d,%02d]"
            " src_regs=[%02d,%02d,%02d,%02d]"
            " dst_mem=[0x%08" PRIx64 ",0x%08" PRIx64 "]"
            " src_mem=[0x%08" PRIx64 ",0x%08" PRIx64 ",0x%08" PRIx64 ",0x%08" PRIx64 "]\n",
            t->ip, t->is_branch, t->branch_taken,
            t->destination_registers[0], t->destination_registers[1],
            t->source_registers[0], t->source_registers[1],
            t->source_registers[2], t->source_registers[3],
            t->destination_memory[0], t->destination_memory[1],
            t->source_memory[0], t->source_memory[1],
            t->source_memory[2], t->source_memory[3]);
}

static void flush_instr(const trace_instr_format_t *t) {
    g_mutex_lock(&lock);

    instr_count++;

    if (instr_count > skip_count &&
        instr_count <= (skip_count + trace_count)) {

        if (trace_file)
            fwrite(t, sizeof(trace_instr_format_t), 1, trace_file);
        if (debug_file)
            write_debug_line(t);

        if ((instr_count - skip_count) % 100000 == 0) {
            g_autofree gchar *msg = g_strdup_printf(
                "Traced %" PRIu64 " instructions\n", instr_count - skip_count);
            qemu_plugin_outs(msg);
        }
    }

    if (instr_count >= (skip_count + trace_count)) {
        if (trace_file) { fclose(trace_file); trace_file = NULL; }
        if (debug_file) { fclose(debug_file); debug_file = NULL; }
        qemu_plugin_outs("Tracing complete!\n");
    }

    g_mutex_unlock(&lock);
}

/* ------------------------------------------------------------------ */

/*
 * Single exec callback per instruction.
 * Flushes the PREVIOUS instruction (mem callbacks have already run for it),
 * then resets the current ctx ready for this instruction's mem callbacks.
 */
static void vcpu_insn_exec(unsigned int vcpu_index, void *userdata) {
    insn_ctx_t *ctx = (insn_ctx_t *)userdata;

    if (pending_ctx) {
        if (pending_ctx->is_conditional)
            pending_ctx->instr.branch_taken =
                (ctx->instr.ip != pending_next_pc) ? 1 : 0;
        flush_instr(&pending_ctx->instr);
    }

    /* Reset only the runtime fields; registers were pre-filled at translation */
    memset(ctx->instr.destination_memory, 0, sizeof(ctx->instr.destination_memory));
    memset(ctx->instr.source_memory,      0, sizeof(ctx->instr.source_memory));
    ctx->instr.branch_taken = ctx->instr.is_branch && !pending_ctx ? 0 : ctx->instr.branch_taken;
    /* For unconditional branches branch_taken was set at translation time;
     * for conditional branches it will be resolved when the next insn runs. */
    if (!ctx->is_conditional && ctx->instr.is_branch)
        ctx->instr.branch_taken = 1;
    else if (ctx->is_conditional)
        ctx->instr.branch_taken = 0;

    pending_ctx     = ctx;
    pending_next_pc = ctx->instr.ip + 4;
}


/* Use QEMU's meminfo to distinguish real loads/stores from spurious callbacks.
 * qemu_plugin_mem_is_store() returns true for writes, false for reads.
 * We ignore the ctx->is_load/is_store flags since QEMU fires mem callbacks
 * one instruction late in system mode — the callback for insn N fires during
 * insn N+1's exec window, so the ctx opcode check is unreliable. */
static void vcpu_mem_access(unsigned int vcpu_index, qemu_plugin_meminfo_t info,
                            uint64_t vaddr, void *userdata) {
    /* Filter out spurious callbacks with clearly invalid addresses.
     * Real memory accesses are always above the first page (0x1000).
     * QEMU occasionally fires mem callbacks on branch/ALU instructions
     * in system mode with tiny vaddr values (register numbers, offsets). */
    if (vaddr < 0x1000)
        return;
    insn_ctx_t *ctx = (insn_ctx_t *)userdata;
    if (qemu_plugin_mem_is_store(info))
        add_mem(ctx->instr.destination_memory, NUM_INSTR_DESTINATIONS, vaddr);
    else
        add_mem(ctx->instr.source_memory, NUM_INSTR_SOURCES, vaddr);
}

/* ------------------------------------------------------------------ */

static insn_ctx_t *build_insn_ctx(uint64_t pc, uint32_t insn_word) {
    insn_ctx_t *ctx = g_new0(insn_ctx_t, 1);
    ctx->instr.ip   = pc;

    uint32_t opcode = insn_word & 0x7F;
    uint32_t rd     = (insn_word >> 7)  & 0x1F;
    uint32_t rs1    = (insn_word >> 15) & 0x1F;
    uint32_t rs2    = (insn_word >> 20) & 0x1F;

    if (opcode == 0x63) { /* B-type: conditional branch
                             reads_ip + reads_flags + writes_ip -> BRANCH_CONDITIONAL */
        ctx->instr.is_branch  = 1;
        ctx->is_conditional   = 1;
        add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, CHAMPSIM_REG_IP);
        add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, CHAMPSIM_REG_FLAGS);
        add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, CHAMPSIM_REG_IP);
        /* also record the actual RISC-V source regs for completeness */
        add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs2));
    } else if (opcode == 0x6F) { /* JAL
                                    writes_ip, no reads_sp/flags -> BRANCH_DIRECT_JUMP
                                    if rd==x1/x5 (link reg) treat as BRANCH_DIRECT_CALL */
        ctx->instr.is_branch   = 1;
        ctx->instr.branch_taken = 1;
        add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, CHAMPSIM_REG_IP);
        if (rd == 1 || rd == 5) { /* ra or t0 = link register -> call */
            add_reg(ctx->instr.source_registers,      NUM_INSTR_SOURCES,      CHAMPSIM_REG_IP);
            add_reg(ctx->instr.source_registers,      NUM_INSTR_SOURCES,      CHAMPSIM_REG_SP);
            add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, CHAMPSIM_REG_SP);
        }
        if (rd) add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
    } else if (opcode == 0x67) { /* JALR
                                    rs1==x1/x5 && rd==x0 -> BRANCH_RETURN
                                    rd==x1/x5             -> BRANCH_INDIRECT_CALL
                                    otherwise             -> BRANCH_INDIRECT */
        ctx->instr.is_branch   = 1;
        ctx->instr.branch_taken = 1;
        add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, CHAMPSIM_REG_IP);
        if ((rs1 == 1 || rs1 == 5) && rd == 0) { /* ret */
            add_reg(ctx->instr.source_registers,      NUM_INSTR_SOURCES,      CHAMPSIM_REG_SP);
            add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, CHAMPSIM_REG_SP);
        } else if (rd == 1 || rd == 5) { /* indirect call */
            add_reg(ctx->instr.source_registers,      NUM_INSTR_SOURCES,      CHAMPSIM_REG_IP);
            add_reg(ctx->instr.source_registers,      NUM_INSTR_SOURCES,      CHAMPSIM_REG_SP);
            add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, CHAMPSIM_REG_SP);
            add_reg(ctx->instr.source_registers,      NUM_INSTR_SOURCES,      map_riscv_reg(rs1));
        } else { /* indirect jump */
            add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        }
        if (rd) add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
    } else if (opcode == 0x33 || opcode == 0x3B) { /* R-type */
        add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs2));
        if (rd) add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
    } else if (opcode == 0x13 || opcode == 0x1B || opcode == 0x03 || opcode == 0x73) { /* I-type */
        add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        if (rd) add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
        if (opcode == 0x03) ctx->is_load = 1;  /* load */
    } else if (opcode == 0x23) { /* S-type */
        add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs1));
        add_reg(ctx->instr.source_registers, NUM_INSTR_SOURCES, map_riscv_reg(rs2));
        ctx->is_store = 1;
    } else if (opcode == 0x37 || opcode == 0x17) { /* U-type */
        if (rd) add_reg(ctx->instr.destination_registers, NUM_INSTR_DESTINATIONS, map_riscv_reg(rd));
    }

    return ctx;
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);

        insn_ctx_t *ctx = NULL;
        size_t insn_size = qemu_plugin_insn_size(insn);
        if (insn_size == 4) {
            uint8_t buf[4];
            qemu_plugin_insn_data(insn, buf, 4);
            uint32_t word = *(uint32_t *)buf;
            ctx = build_insn_ctx(pc, word);
        } else {
            /* Compressed (2-byte) or unknown — minimal ctx */
            ctx = g_new0(insn_ctx_t, 1);
            ctx->instr.ip = pc;
        }

        /* All callbacks share the same ctx pointer.
         * Only ONE vcpu_insn_exec_cb is registered — QEMU 10 drops duplicates. */
        qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                               QEMU_PLUGIN_CB_NO_REGS, ctx);
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem_access,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW, ctx);
    }
}

/* ------------------------------------------------------------------ */

static void plugin_exit(qemu_plugin_id_t id, void *p) {
    if (pending_ctx) {
        flush_instr(&pending_ctx->instr);
        pending_ctx = NULL;
    }
    if (trace_file) { fclose(trace_file); trace_file = NULL; }
    if (debug_file) { fclose(debug_file); debug_file = NULL; }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv) {
    char *output_filename = NULL;
    char *debug_filename  = NULL;

    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) tok = g_strsplit(argv[i], "=", 2);
        if      (g_strcmp0(tok[0], "output") == 0) output_filename = g_strdup(tok[1]);
        else if (g_strcmp0(tok[0], "skip")   == 0) skip_count  = g_ascii_strtoull(tok[1], NULL, 10);
        else if (g_strcmp0(tok[0], "count")  == 0) trace_count = g_ascii_strtoull(tok[1], NULL, 10);
        else if (g_strcmp0(tok[0], "debug")  == 0) debug_filename = g_strdup(tok[1]);
        else {
            fprintf(stderr, "Unknown option: %s\n", tok[0]);
            return -1;
        }
    }

    if (!output_filename)
        output_filename = g_strdup("champsim.trace");

    trace_file = fopen(output_filename, "wb");
    if (!trace_file) {
        fprintf(stderr, "Failed to open trace file: %s\n", output_filename);
        return -1;
    }

    if (debug_filename) {
        debug_file = fopen(debug_filename, "w");
        if (!debug_file) {
            fprintf(stderr, "Failed to open debug file: %s\n", debug_filename);
            return -1;
        }
        fprintf(debug_file,
                "# ip branch taken dst_regs[0,1] src_regs[0,1,2,3] "
                "dst_mem[0,1] src_mem[0,1,2,3]\n");
    }

    g_mutex_init(&lock);

    g_autofree gchar *cfg = g_strdup_printf(
        "ChampSim Tracer:\n"
        "  output: %s\n"
        "  debug:  %s\n"
        "  skip:   %" PRIu64 "\n"
        "  count:  %" PRIu64 "\n",
        output_filename,
        debug_filename ? debug_filename : "(disabled)",
        skip_count, trace_count);
    qemu_plugin_outs(cfg);

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}
