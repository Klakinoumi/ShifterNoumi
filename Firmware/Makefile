# Command-line build of the GX100 firmware, independent of STM32CubeIDE.
# Same compiler, flags and sources as the CubeIDE project (Debug/ is CubeIDE's,
# this Makefile only writes to build/).
#
#   make                     Release (-Os), linked behind the USB bootloader
#                            -> build/release-bootloader/gx100-app.{elf,bin,hex}
#   make LAYOUT=standalone   Linked at 0x08000000 like the CubeIDE project, for
#                            flashing with an ST-Link and no bootloader
#   make CONFIG=debug        Same flags as CubeIDE's Debug configuration (-O0)
#   make clean
#
# The flash map is described in App/Inc/gx100_boot.h. The linker script is
# STM32F103C8TX_FLASH.ld (CubeIDE's) with only the FLASH region changed, so
# that neither layout can spill into the settings and calibration pages
# (0x0800F800-0x0800FFFF).
#
# Needs arm-none-eabi-gcc on PATH (Arm GNU Toolchain, or the one bundled with
# STM32CubeIDE under plugins/*gnu-tools-for-stm32*/tools/bin), or pass
# PREFIX=/path/to/bin/arm-none-eabi-

CONFIG ?= release
LAYOUT ?= bootloader
PREFIX ?= arm-none-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

BUILD := build/$(CONFIG)-$(LAYOUT)
TARGET := $(BUILD)/gx100-app

# Link order follows CubeIDE's objects.list (it decides the flash layout).
SRCS := \
	$(sort $(wildcard App/Src/*.c)) \
	Core/Src/adc.c \
	Core/Src/dma.c \
	Core/Src/gpio.c \
	Core/Src/main.c \
	Core/Src/stm32f1xx_hal_msp.c \
	Core/Src/stm32f1xx_it.c \
	Core/Src/syscalls.c \
	Core/Src/sysmem.c \
	Core/Src/system_stm32f1xx.c \
	Core/Startup/startup_stm32f103c8tx.s \
	$(addprefix Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_, \
		hal.c hal_adc.c hal_adc_ex.c hal_cortex.c hal_dma.c hal_exti.c \
		hal_flash.c hal_flash_ex.c hal_gpio.c hal_gpio_ex.c hal_pcd.c \
		hal_pcd_ex.c hal_pwr.c hal_rcc.c hal_rcc_ex.c ll_usb.c) \
	Middlewares/ST/STM32_USB_Device_Library/Class/HID/Src/usbd_hid.c \
	Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c \
	Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c \
	Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c \
	USB_DEVICE/App/usb_device.c \
	USB_DEVICE/App/usbd_cdc_if.c \
	USB_DEVICE/App/usbd_composite.c \
	USB_DEVICE/App/usbd_desc.c \
	USB_DEVICE/Target/usbd_conf.c

INCLUDES := \
	-ICore/Inc \
	-IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
	-IUSB_DEVICE/App \
	-IUSB_DEVICE/Target \
	-IDrivers/STM32F1xx_HAL_Driver/Inc \
	-IDrivers/STM32F1xx_HAL_Driver/Inc/Legacy \
	-IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc \
	-IMiddlewares/ST/STM32_USB_Device_Library/Class/HID/Inc \
	-IDrivers/CMSIS/Device/ST/STM32F1xx/Include \
	-IDrivers/CMSIS/Include \
	-IApp/Inc

ARCH := -mcpu=cortex-m3 -mthumb -mfloat-abi=soft --specs=nano.specs

ifeq ($(CONFIG),debug)
OPT := -O0 -g3 -DDEBUG
else ifeq ($(CONFIG),release)
OPT := -Os -g3
else
$(error CONFIG must be release or debug)
endif

# FLASH region per layout: origin, length (see gx100_boot.h).
ifeq ($(LAYOUT),bootloader)
FLASH_REGION := ORIGIN = 0x08001000, LENGTH = 0xE7F8
else ifeq ($(LAYOUT),standalone)
FLASH_REGION := ORIGIN = 0x08000000, LENGTH = 0xF800
else
$(error LAYOUT must be bootloader or standalone)
endif
LDSCRIPT := $(BUILD)/gx100_app.ld

CFLAGS  := $(ARCH) -std=gnu11 $(OPT) -DUSE_HAL_DRIVER -DSTM32F103xB $(INCLUDES) \
           -ffunction-sections -fdata-sections -Wall -MMD -MP
ASFLAGS := $(ARCH) $(OPT) -x assembler-with-cpp -MMD -MP
LDFLAGS := $(ARCH) -T$(LDSCRIPT) --specs=nosys.specs -static \
           -Wl,-Map=$(TARGET).map -Wl,--gc-sections -Wl,--print-memory-usage \
           -Wl,--start-group -lc -lm -Wl,--end-group

OBJS := $(addprefix $(BUILD)/obj/,$(patsubst %.s,%.o,$(SRCS:.c=.o)))

.PHONY: all clean
all: $(TARGET).bin $(TARGET).hex
	@$(SIZE) $(TARGET).elf

$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

$(LDSCRIPT): STM32F103C8TX_FLASH.ld Makefile
	@mkdir -p $(dir $@)
	sed -E 's/^[[:space:]]*FLASH[[:space:]]*\(rx\)[[:space:]]*:.*/  FLASH (rx) : $(FLASH_REGION)/' $< > $@
	@grep -q '$(FLASH_REGION)' $@ || (echo "FLASH region not found in $<" && rm -f $@ && false)

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

clean:
	rm -rf build

-include $(OBJS:.o=.d)
