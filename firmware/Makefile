# Command-line build, same sources and flags as the CubeIDE project.
#   make            -> build/pid_testbench.elf / .bin / .hex
#   make flash      -> program over ST-LINK (needs STM32_Programmer_CLI)
#   make clean

TARGET  = pid_testbench
BUILD   = build
PREFIX ?= arm-none-eabi-

CC      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE    = $(PREFIX)size

C_SOURCES  = $(wildcard Core/Src/*.c) $(wildcard Drivers/STM32G0xx_HAL_Driver/Src/*.c)
AS_SOURCES = Core/Startup/startup_stm32g070rbtx.s

DEFS = -DUSE_HAL_DRIVER -DSTM32G070xx
INCS = -ICore/Inc \
       -IDrivers/STM32G0xx_HAL_Driver/Inc \
       -IDrivers/STM32G0xx_HAL_Driver/Inc/Legacy \
       -IDrivers/CMSIS/Device/ST/STM32G0xx/Include \
       -IDrivers/CMSIS/Include

MCU     = -mcpu=cortex-m0plus -mthumb
CFLAGS  = $(MCU) $(DEFS) $(INCS) -std=gnu11 -O2 -g3 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -MMD -MP
LDFLAGS = $(MCU) -TSTM32G070RBTX_FLASH.ld --specs=nano.specs --specs=nosys.specs \
          -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -Wl,--print-memory-usage -lm

OBJECTS = $(addprefix $(BUILD)/,$(C_SOURCES:.c=.o)) $(addprefix $(BUILD)/,$(AS_SOURCES:.s=.o))

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -c $(MCU) -x assembler-with-cpp $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS) STM32G070RBTX_FLASH.ld
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex $< $@

flash: $(BUILD)/$(TARGET).elf
	STM32_Programmer_CLI -c port=SWD -w $< -v -rst

clean:
	rm -rf $(BUILD)

-include $(OBJECTS:.o=.d)

.PHONY: all flash clean
