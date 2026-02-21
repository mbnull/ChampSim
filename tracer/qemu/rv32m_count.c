/*
 * QEMU plugin: count RV32M (multiply/divide) instructions executed.
 *
 * Usage:
 *   qemu-riscv32 -plugin ./librv32m_count.so program
 *   qemu-system-riscv32 -M virt -nographic -kernel prog.elf -plugin ./librv32m_count.so
 */

#include <glib.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t total_insns;
static uint64_t m_count[8]; /* MUL MULH MULHSU MULHU DIV DIVU REM REMU */

static const char *m_names[8] = {
    "MUL", "MULH", "MULHSU", "MULHU", "DIV", "DIVU", "REM", "REMU"
};

static void vcpu_insn_exec(unsigned int vcpu_index, void *userdata) {
    uint64_t idx = (uint64_t)userdata;
    __atomic_fetch_add(&m_count[idx], 1, __ATOMIC_RELAXED);
}

static void vcpu_insn_exec_total(unsigned int vcpu_index, void *userdata) {
    __atomic_fetch_add(&total_insns, 1, __ATOMIC_RELAXED);
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec_total,
                                               QEMU_PLUGIN_CB_NO_REGS, NULL);

        if (qemu_plugin_insn_size(insn) != 4) continue;

        uint8_t buf[4];
        qemu_plugin_insn_data(insn, buf, 4);
        uint32_t word = *(uint32_t *)buf;

        uint32_t opcode = word & 0x7F;
        uint32_t funct7 = word >> 25;
        uint32_t funct3 = (word >> 12) & 0x7;

        /* RV32M: opcode=0x33 (OP), funct7=0x01 */
        if (opcode == 0x33 && funct7 == 0x01) {
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_insn_exec, QEMU_PLUGIN_CB_NO_REGS,
                (void *)(uint64_t)funct3);
        }
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p) {
    uint64_t m_total = 0;
    fprintf(stderr, "\n=== RV32M Instruction Count ===\n");
    for (int i = 0; i < 8; i++) {
        fprintf(stderr, "  %-8s: %" PRIu64 "\n", m_names[i], m_count[i]);
        m_total += m_count[i];
    }
    fprintf(stderr, "  %-8s: %" PRIu64 "\n", "TOTAL M", m_total);
    fprintf(stderr, "  %-8s: %" PRIu64 "\n", "ALL INSN", total_insns);
    if (total_insns)
        fprintf(stderr, "  M ratio : %.4f%%\n", 100.0 * m_total / total_insns);
    fprintf(stderr, "===============================\n");
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv) {
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
