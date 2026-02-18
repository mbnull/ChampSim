/*
 * Simple RV32I test program for ChampSim trace generation
 *
 * Compile with:
 *   riscv32-unknown-elf-gcc -march=rv32i -mabi=ilp32 -nostdlib -T link.ld -o test.elf test.c
 */

#define UART0 0x10000000L
#define UART_THR 0x00
#define UART_LSR 0x05
#define UART_LSR_EMPTY_MASK 0x40

static inline void uart_putc(char c)
{
  volatile unsigned char* uart = (unsigned char*)UART0;

  // 等待发送缓冲区空
  while ((uart[UART_LSR] & UART_LSR_EMPTY_MASK) == 0)
    ;

  uart[UART_THR] = c;
}

int main()
{
  char* s = "Hello QEMU Bare Metal!\n";
  while (*s) {
    uart_putc(*s++);
  }
  return 1;
}
