/*
 * RV32I test program for ChampSim trace generation
 * Exercises loads, stores, branches, and function calls so that
 * src_mem / dst_mem fields appear in the debug trace.
 *
 * Compile with:
 *   riscv32-unknown-elf-gcc -march=rv32i -mabi=ilp32 -O0 -nostdlib -T link.ld -o test.elf test.c
 */

#include <stdint.h>

#define UART0 0x10000000UL
#define UART_THR 0x00
#define UART_LSR 0x05
#define UART_LSR_EMPTY_MASK 0x40
#define VIRT_TEST 0x100000UL

/* ------------------------------------------------------------------ */
/* UART / exit helpers                                                  */
/* ------------------------------------------------------------------ */

uint32_t LARGE_ARRAY[1000] = {0};
uint32_t LARGE_ARRAY_DST[1000] = {0};
static inline void uart_putc(char c)
{
  volatile unsigned char* uart = (unsigned char*)UART0;
  while ((uart[UART_LSR] & UART_LSR_EMPTY_MASK) == 0)
    ;
  uart[UART_THR] = c;
}

static inline void uart_puts(const char* s)
{
  while (*s)
    uart_putc(*s++);
}

static inline void qemu_exit(void)
{
  volatile uint32_t* test = (uint32_t*)VIRT_TEST;
  *test = 0x5555;
}

/* ------------------------------------------------------------------ */
/* Memory workload                                                      */
/* ------------------------------------------------------------------ */

#define N 16

/* Placed in .bss so they live in RAM and generate real load/store addresses */
static uint32_t src[N];
static uint32_t dst[N];

/* Simple array copy: generates N loads (src_mem) + N stores (dst_mem) */
static void array_copy(uint32_t* out, const uint32_t* in, int n)
{
  for (int i = 0; i < n; i++)
    out[i] = in[i];
}

/* Dot product: generates 2*N loads, conditional branches, accumulation */
static uint32_t dot_product(const uint32_t* a, const uint32_t* b, int n)
{
  uint32_t sum = 0;
  for (int i = 0; i < n; i++)
    sum += a[i] * b[i];
  return sum;
}

/* Bubble sort: lots of conditional branches + loads + stores */
static void bubble_sort(uint32_t* arr, int n)
{
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - 1 - i; j++) {
      if (arr[j] > arr[j + 1]) {
        uint32_t tmp = arr[j];
        arr[j] = arr[j + 1];
        arr[j + 1] = tmp;
      }
    }
  }
}

/* ------------------------------------------------------------------ */

int main(void)
{
  /* Initialise src with a simple pattern (stores -> dst_mem) */
  for (int i = 0; i < N; i++)
    src[i] = (uint32_t)(N - i); /* descending so sort has work to do */

  array_copy(dst, src, N);
  array_copy(LARGE_ARRAY_DST, LARGE_ARRAY, 1000);
  bubble_sort(dst, N);

  uint32_t result = dot_product(src, dst, N);

  /* Print result digit by digit over UART so the run doesn't get optimised away */
  uart_puts("result=");
  /* print up to 10 decimal digits */
  char buf[11];
  int pos = 10;
  buf[pos] = '\0';
  if (result == 0) {
    buf[--pos] = '0';
  } else {
    uint32_t v = result;
    while (v && pos > 0) {
      buf[--pos] = '0' + (v % 10);
      v /= 10;
    }
  }
  uart_puts(buf + pos);
  uart_putc('\n');

  qemu_exit();
  return 0;
}
