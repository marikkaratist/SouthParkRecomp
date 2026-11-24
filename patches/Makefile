TARGET = patches.elf

CC      ?= clang
LD      ?= ld.lld

# There is no decompilation of South Park to take headers from. Game types
# that patches need are declared in this directory as they come up.
DECOMP_INCLUDE_DIRS :=

CFLAGS   := -target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls -mno-odd-spreg \
			-fomit-frame-pointer -ffreestanding -Wall -Wextra
CPPFLAGS := -DMIPS $(DECOMP_INCLUDE_DIRS)
LDFLAGS  := -nostdlib -T patches.ld -Map patches.map --unresolved-symbols=ignore-all --emit-relocs

C_SRCS := $(wildcard *.c)
C_OBJS := $(C_SRCS:.c=.o)
C_DEPS := $(C_SRCS:.c=.d)

$(TARGET): $(C_OBJS) patches.ld
	$(LD) $(C_OBJS) $(LDFLAGS) -o $@

$(C_OBJS): %.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -MMD -MF $(@:.o=.d) -c -o $@

ifeq ($(OS),Windows_NT)
clean:
	powershell -NoProfile -Command "foreach ($$path in @('$(C_OBJS)','$(TARGET)','$(C_DEPS)','patches.map')) { if (Test-Path -LiteralPath $$path) { Remove-Item -LiteralPath $$path -Force } }"
else
clean:
	rm -f $(C_OBJS) $(TARGET) $(C_DEPS) patches.map
endif

-include $(C_DEPS)

.PHONY: clean
