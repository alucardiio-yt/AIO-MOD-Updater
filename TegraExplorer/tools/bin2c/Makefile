NATIVE_CC ?= gcc

ifeq (, $(shell which $(NATIVE_CC) 2>/dev/null))
$(error "Native GCC is missing. Please install it first. If its path is custom, set it with export NATIVE_CC=<path to native gcc toolchain>")
endif

ifeq ($(OS),Windows_NT)
TARGET := bin2c.exe
else
TARGET := bin2c
endif

.PHONY: all clean

all: $(TARGET)
	@echo > /dev/null

clean:
	@rm -f bin2c bin2c.exe

$(TARGET): bin2c.c
	@$(NATIVE_CC) -o $@ bin2c.c
