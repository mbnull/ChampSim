// Minimal probe: does vcpu_mem_cb userdata actually arrive?
#include <glib.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct { uint64_t ip; uint64_t src[4]; uint64_t dst[2]; } ctx_t;

static void on_exec(unsigned int cpu, void *ud) {
    ctx_t *c = ud;
    memset(c->src, 0, sizeof(c->src));
    memset(c->dst, 0, sizeof(c->dst));
}
static void on_mem_r(unsigned int cpu, qemu_plugin_meminfo_t i, uint64_t va, void *ud) {
    ctx_t *c = ud;
    fprintf(stderr, "MEM_R ctx=%p ip=0x%lx va=0x%lx\n", (void*)c, c->ip, va);
    for (int j=0;j<4;j++) if (!c->src[j]) { c->src[j]=va; break; }
}
static void on_mem_w(unsigned int cpu, qemu_plugin_meminfo_t i, uint64_t va, void *ud) {
    ctx_t *c = ud;
    fprintf(stderr, "MEM_W ctx=%p ip=0x%lx va=0x%lx\n", (void*)c, c->ip, va);
    for (int j=0;j<2;j++) if (!c->dst[j]) { c->dst[j]=va; break; }
}
static void on_after(unsigned int cpu, void *ud) {
    ctx_t *c = ud;
    if (c->src[0] || c->dst[0])
        fprintf(stderr, "AFTER ctx=%p ip=0x%lx src0=0x%lx dst0=0x%lx\n",
                (void*)c, c->ip, c->src[0], c->dst[0]);
}
static void on_tb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    for (size_t i=0; i<qemu_plugin_tb_n_insns(tb); i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        ctx_t *c = g_new0(ctx_t, 1);
        c->ip = qemu_plugin_insn_vaddr(insn);
        qemu_plugin_register_vcpu_insn_exec_cb(insn, on_exec, QEMU_PLUGIN_CB_NO_REGS, c);
        qemu_plugin_register_vcpu_mem_cb(insn, on_mem_r, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_R, c);
        qemu_plugin_register_vcpu_mem_cb(insn, on_mem_w, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_W, c);
        qemu_plugin_register_vcpu_insn_exec_cb(insn, on_after, QEMU_PLUGIN_CB_NO_REGS, c);
    }
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb);
    return 0;
}
