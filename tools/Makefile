# Vibe coded makefile, sorry

# Compiler and flags
CC = gcc
CFLAGS = -O3 -Wall -I../include

# Output executable
TARGET = nbt2tap tap2nbt

# Default target
all: $(TARGET)

# Link the object files into the executable
%: %.c
	$(CC) $(CFLAGS) -o $@ $^ ../9ball.c

# Clean up build artifacts
clean:
	rm -f $(TARGET)

.PHONY: all clean

