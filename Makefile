# Vibe coded makefile, sorry

# Compiler and flags
CC = gcc
CFLAGS = -O3 -Wall -Iinclude

# Source files
SRCS = alu.c fpu_helpers.c cpu.c lpt.c pch.c ppt.c iocpu.c tty.c panel.c

# Object files
OBJS = $(SRCS:.c=.o)

# Output executable
TARGET = ist66

# Default target
all: $(TARGET)

# Link the object files into the executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -flto -o $@ $^ softfloat.a -lSDL2

# Compile each .c into a .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

