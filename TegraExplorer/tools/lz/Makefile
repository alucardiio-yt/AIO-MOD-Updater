NATIVE_CC ?= gcc

ifeq (, $(shell which $(NATIVE_CC) 2>/dev/null))
$(error "Native GCC is missing. Please install it first. If its path is custom, set it with export NATIVE_CC=<path to native gcc toolchain>")
endif

ifeq ($(OS),Windows_NT)
TARGET := lz77.exe
else
TARGET := lz77
endif

.PHONY: all clean

all: $(TARGET)
	@echo > /dev/null

clean:
	@rm -f lz77 lz77.exe

$(TARGET): lz.c lz77.c
	@$(NATIVE_CC) -o $@ lz.c lz77.c
