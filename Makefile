# Put all the source files here
SRCS = main.c wos.c

# Binary will be generated with this name (.elf, etc)
PROJ_NAME = hello_world

DEBUG = 1

# Normally you shouldn't need to change anything below this line
################################################################

CMSIS = ./CMSIS

CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size
RM = rm

ARCH_OPTS = -mlittle-endian -mthumb -mcpu=cortex-m3

CFLAGS = -Wall -fdata-sections -ffunction-sections

ifdef DEBUG
CFLAGS += -g -Og
else
CFLAGS += -O3
endif

CFLAGS += -I.
CFLAGS += -I$(CMSIS)/CM3/CoreSupport -I$(CMSIS)/CM3/DeviceSupport/ST/STM32F10x
CFLAGS += -DSTM32F10X_MD

STARTUP = $(CMSIS)/CM3/DeviceSupport/ST/STM32F10x/startup/gcc_ride7/startup_stm32f10x_md.s
SRCS += $(CMSIS)/CM3/DeviceSupport/ST/STM32F10x/system_stm32f10x.c $(CMSIS)/CM3/CoreSupport/core_cm3.c
OBJS = $(SRCS:.c=.o)

LINK_SCRIPT = stm32_flash.ld

LDFLAGS = -T$(LINK_SCRIPT) -Wl,--gc-sections
LDFLAGS += --specs=rdimon.specs

.PHONY: all clean

all: $(PROJ_NAME).elf $(PROJ_NAME).bin

$(PROJ_NAME).bin: $(PROJ_NAME).elf
	$(OBJCOPY) -O binary $^ $@

$(PROJ_NAME).elf: $(OBJS) $(STARTUP)
	$(CC) $(ARCH_OPTS) $(LDFLAGS) $^ -o $@
	$(SIZE) $@

$(OBJS): %.o:%.c
	$(CC) -c $(ARCH_OPTS) $(CFLAGS) -c $< -o $@

clean:
	-$(RM) $(PROJ_NAME).bin $(PROJ_NAME).elf $(OBJS)
