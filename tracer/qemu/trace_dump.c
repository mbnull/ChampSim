/*
 * trace_dump.c - Dump a ChampSim binary trace file as human-readable text
 *
 * Produces the same format as the debug=trace.txt output from the tracer.
 *
 * Build:
 *   gcc -O2 -o trace_dump trace_dump.c
 *
 * Usage:
 *   ./trace_dump trace.bin              # dump all
 *   ./trace_dump trace.bin 100          # dump first 100 instructions
 *   ./trace_dump trace.bin 0 500        # skip 0, dump 500
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <trace.bin> [count] [skip]\n", argv[0]);
        return 1;
    }

    const char *path  = argv[1];
    uint64_t    count = (argc >= 3) ? strtoull(argv[2], NULL, 10) : UINT64_MAX;
    uint64_t    skip  = (argc >= 4) ? strtoull(argv[3], NULL, 10) : 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }

    printf("# ip branch taken dst_regs[0,1] src_regs[0,1,2,3] dst_mem[0,1] src_mem[0,1,2,3]\n");

    trace_instr_format_t t;
    uint64_t n = 0;   /* instructions read */
    uint64_t w = 0;   /* instructions written */

    while (fread(&t, sizeof(t), 1, f) == 1) {
        n++;
        if (n <= skip)
            continue;
        if (w >= count)
            break;
        w++;

        printf("ip=0x%016" PRIx64
               " branch=%d taken=%d"
               " dst_regs=[%02d,%02d]"
               " src_regs=[%02d,%02d,%02d,%02d]"
               " dst_mem=[0x%08" PRIx64 ",0x%08" PRIx64 "]"
               " src_mem=[0x%08" PRIx64 ",0x%08" PRIx64 ",0x%08" PRIx64 ",0x%08" PRIx64 "]\n",
               t.ip, t.is_branch, t.branch_taken,
               t.destination_registers[0], t.destination_registers[1],
               t.source_registers[0], t.source_registers[1],
               t.source_registers[2], t.source_registers[3],
               t.destination_memory[0], t.destination_memory[1],
               t.source_memory[0], t.source_memory[1],
               t.source_memory[2], t.source_memory[3]);
    }

    fprintf(stderr, "total in file: %" PRIu64 "  skipped: %" PRIu64 "  printed: %" PRIu64 "\n",
            n, skip, w);

    fclose(f);
    return 0;
}
