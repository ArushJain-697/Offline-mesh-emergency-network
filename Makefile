# Compiler
CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra -pthread

# Source files
SRCS = mesh_backend.c mesh_gui.c

# Object files
OBJS = $(SRCS:.c=.o)

# Output executable name
TARGET = mesh_gui

# Default rule
all: $(TARGET)

# Link object files into final executable
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(CFLAGS)

# Rule to compile each .c into .o
%.o: %.c
	$(CC) -c $< -o $@ $(CFLAGS)

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

