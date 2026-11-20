# Build firmware for PicoRV32 (RV32IM).
#   GNU toolchain (default):   make            (needs riscv64-unknown-elf-gcc or riscv32-...)
#   Zig as a cross compiler:   make ZIG=1      (pip install ziglang)
CROSS   ?= riscv64-unknown-elf-
CFLAGS  = -O2 -ffreestanding -fno-builtin -nostdlib -Wall
ifeq ($(ZIG),1)
CC      = python3 -m ziglang cc -target riscv32-freestanding-none -mcpu=generic_rv32+m+zicsr -mno-relax
OBJCOPY = python3 -m ziglang objcopy
else
CC      = $(CROSS)gcc -march=rv32im_zicsr -mabi=ilp32
OBJCOPY = $(CROSS)objcopy
endif

all: firmware_b0.hex

firmware.elf: start.S main.c beatcnn_ref.c beatcnn_params.h test_vectors.h link.ld
	$(CC) $(CFLAGS) -T link.ld -o $@ start.S main.c beatcnn_ref.c -lgcc

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@

firmware_b0.hex: firmware.bin makehex.py
	python makehex.py $< firmware

clean:
	rm -f firmware.elf firmware.bin firmware_b*.hex
