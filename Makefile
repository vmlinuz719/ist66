# Vibe coded makefile, sorry

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Werror -Iinclude -O3 -flto

# Source files
SRCS = alu.c fpu.c cpu.c lpt.c pch.c ppt.c tty.c panel.c bishop.c render.c channel.c ch_7310.c

# Object files
OBJS = $(SRCS:.c=.o)

# Output executable
TARGET = acr7000

# Default target
all: $(TARGET)

# Link the object files into the executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lSDL2 -lSDL2_ttf -lSDL2_gfx

# Compile each .c into a .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGET) tests/*.o $(TEST_BINS)

.PHONY: all clean test

