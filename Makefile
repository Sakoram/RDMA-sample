# Compiler
CC = gcc

# Compiler flags    -D_GNU_SOURCE is for sched.h
CFLAGS = -Wall -Werror -g -Iinclude -D_GNU_SOURCE

# Linker flags
LDFLAGS = -lfabric -lSDL2

# Automatically find all .c files in src directory and its subdirectories
SRC = $(shell find src -name '*.c')

# Automatically find all .h files in include directory and its subdirectories
HEADERS = $(shell find include -name '*.h')

# Object files (convert source file paths to object file paths)
OBJ = $(SRC:src/%.c=build/%.o)

# Executable name
EXEC = my_msg

# Default target
all: $(EXEC)

# Rule to link object files to create executable
$(EXEC): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Rule to compile source files into object files, creating directories as needed
build/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target to remove object files and executable
clean:
	rm -rf build $(EXEC)

.PHONY: all clean