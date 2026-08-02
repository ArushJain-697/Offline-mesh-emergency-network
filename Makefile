CC = gcc

# Detect libsodium location (Homebrew on Mac, system path on Linux/WSL)
SODIUM_PREFIX := $(shell brew --prefix libsodium 2>/dev/null || echo /usr)
CFLAGS  = -Wall -pthread -I$(SODIUM_PREFIX)/include
LDFLAGS = -L$(SODIUM_PREFIX)/lib -lsodium

CHAT_BIN  = mesh_cli
CHAT_SRCS = mesh_cli.c mesh_backend.c mesh_discovery.c mesh_crypto.c mesh_frame.c

.PHONY: all build chat chat_node clean help test

all: build

build: $(CHAT_BIN)

$(CHAT_BIN): $(CHAT_SRCS)
	$(CC) $(CFLAGS) -o $(CHAT_BIN) $(CHAT_SRCS) $(LDFLAGS)

PASSWORD ?= mysecretpassword

chat_node: $(CHAT_BIN)
ifndef PORT
	$(error "Usage: make chat_node PORT=9001 [PASSWORD=...]")
endif
	@./$(CHAT_BIN) $(PORT) $(PASSWORD)

test: test_frame.c mesh_frame.c
	$(CC) $(CFLAGS) -o test_frame test_frame.c mesh_frame.c $(LDFLAGS)
	@./test_frame

clean:
	rm -f $(CHAT_BIN) test_frame *.o chat_history_*.txt nodes.dat identity.dat

help:
	@echo "Makefile targets:"
	@echo "  make build              -> compile mesh_cli"
	@echo "  make chat_node PORT=N [PASSWORD=pw] -> start node on port N (auto-discovers network)"
	@echo "  make clean              -> remove binaries and data files"
