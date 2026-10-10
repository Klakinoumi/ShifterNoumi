# GX100 USB bootloader (see src/main.c and Firmware/App/Inc/gx100_boot.h).
#
#   make          -> build/gx100-bootloader.{elf,bin,hex}   (must fit in 4 KB)
#   make test     -> host tests of the protocol (gcc)
#
# Same toolchain requirements as Firmware/Makefile.

PREFIX  ?= arm-none-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size
HOSTCC  ?= gcc

BUILD  := build
TARGET := $(BUILD)/gx100-bootloader

SRCS := src/startup.c src/main.c src/usb.c src/boot_proto.c src/flash_stm32.c

INCLUDES := \
	-Isrc \
	-I../Firmware/App/Inc \
	-I../Firmware/Drivers/CMSIS/Device/ST/STM32F1xx/Include \
	-I../Firmware/Drivers/CMSIS/Include

CFLAGS := -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -std=gnu11 -Os -g3 \
          -DSTM32F103xB $(INCLUDES) -ffreestanding -ffunction-sections \
          -fdata-sections -Wall -Wextra -MMD -MP
LDFLAGS := -mcpu=cortex-m3 -mthumb -nostartfiles -nostdlib -Tbootloader.ld \
           -Wl,--gc-sections -Wl,-Map=$(TARGET).map -Wl,--print-memory-usage -lgcc

OBJS := $(addprefix $(BUILD)/obj/,$(SRCS:.c=.o))

.PHONY: all clean test
all: $(TARGET).bin $(TARGET).hex
	@$(SIZE) $(TARGET).elf

$(TARGET).elf: $(OBJS) bootloader.ld
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Host build of the protocol against a RAM-simulated flash. The same binary
# serves the Python wizard's integration test (Wizard/tests).
$(BUILD)/host/boot_proto_host: src/boot_proto.c test/boot_hw_sim.c test/boot_proto_host.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c99 -Wall -Wextra -Isrc -I../Firmware/App/Inc $^ -o $@

$(BUILD)/host/test_boot_proto: src/boot_proto.c test/boot_hw_sim.c test/test_boot_proto.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c99 -Wall -Wextra -Isrc -I../Firmware/App/Inc $^ -o $@

test: $(BUILD)/host/test_boot_proto $(BUILD)/host/boot_proto_host
	$(BUILD)/host/test_boot_proto

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
