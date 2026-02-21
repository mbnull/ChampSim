# Makefile for ChampSim QEMU Tracer Plugin

# QEMU plugin directory - adjust this to your QEMU installation
QEMU_CFLAGS := $(shell pkg-config --cflags glib-2.0)
QEMU_LIBS := $(shell pkg-config --libs glib-2.0)

# Compiler and flags
CC := gcc
CFLAGS := -O3 -Wall -fPIC $(QEMU_CFLAGS) -I../../inc
LDFLAGS := -shared $(QEMU_LIBS)

# Target
TARGET := libchampsim_tracer.so

# Source files
SRCS := champsim_tracer.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET) trace_dump librv32m_count.so

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

trace_dump: trace_dump.c
	$(CC) -O2 -o $@ $<

librv32m_count.so: rv32m_count.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) trace_dump librv32m_count.so

install: $(TARGET)
	@echo "Plugin built successfully: $(TARGET)"
	@echo "Use with bare-metal: qemu-system-riscv32 -M virt -nographic -kernel program.elf -plugin ./$(TARGET),arg=\"output=trace.bin\""
	@echo "Use with Linux binary: qemu-riscv32 -plugin ./$(TARGET),arg=\"output=trace.bin\" program.elf"
