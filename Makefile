# Vibe coded makefile, sorry

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Werror -Iinclude -O3 -flto -fomit-frame-pointer $(PGO)

# Profile-guided optimization is opt-in. -fprofile-use is fatal under -Werror
# when the .gcda files are missing (a clean tree) or stale (any edit that
# changes a function's control flow), so it is not in the default flags:
#   make clean && make PGO=-fprofile-generate
#   ./acr7000                 ...run a representative workload, then exit...
#   make clean && make pgo
PGO =

# Source files
SRCS = alu.c fpu.c cpu.c lpt.c pch.c ppt.c tty.c panel.c bishop.c render.c channel.c ch_7310.c

# Object files
OBJS = $(SRCS:.c=.o)

# Output executable
TARGET = acr7000

# Default target
all: $(TARGET)

# Rebuild using the profile left behind by a -fprofile-generate run
pgo: PGO = -fprofile-use -Wno-error=missing-profile -Wno-error=coverage-mismatch
pgo: $(TARGET)

# Link the object files into the executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lSDL2 -lSDL2_ttf -lSDL2_gfx

# Compile each .c into a .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build artifacts (keeps *.gcda so "make clean && make pgo" works)
clean:
	rm -f $(OBJS) $(TARGET) tests/*.o $(TEST_BINS)

# Also discard collected profiles
distclean: clean
	rm -f *.gcda

.PHONY: all clean distclean test pgo
