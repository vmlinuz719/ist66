# Vibe coded makefile, sorry

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Iinclude -O3 -flto # -fprofile-use 

# Source files
SRCS = alu.c fpu.c cpu.c lpt.c pch.c ppt.c tty.c panel.c bishop.c

# Object files
OBJS = $(SRCS:.c=.o)

# Output executable
TARGET = ist66

# Default target
all: $(TARGET)

# Link the object files into the executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lSDL2 -lSDL2_ttf -lSDL2_gfx

# Compile each .c into a .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Test
TEST_BINS = tests/test_fpu tests/test_cpu

test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do echo "=== $$t ==="; ./$$t || fail=1; done; exit $$fail

tests/test_fpu: tests/test_fpu.o fpu.o
	$(CC) $(CFLAGS) -o $@ $^ -lcunit

tests/test_cpu: tests/test_cpu.o
	$(CC) $(CFLAGS) -o $@ $^ -lcunit

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGET) tests/*.o $(TEST_BINS)

.PHONY: all clean test

