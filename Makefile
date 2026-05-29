CC = gcc
CFLAGS = -Wall -pthread

CHAT_BIN  = mesh_cli
CHAT_SRCS = mesh_cli.c mesh_backend.c mesh_discovery.c

.PHONY: all build chat chat_node clean help

all: build

build: $(CHAT_BIN)

$(CHAT_BIN): $(CHAT_SRCS)
	$(CC) $(CFLAGS) -o $(CHAT_BIN) $(CHAT_SRCS)

chat_node: $(CHAT_BIN)
ifndef PORT
	$(error "Usage: make chat_node PORT=9001")
endif
	@./$(CHAT_BIN) $(PORT)

clean:
	rm -f $(CHAT_BIN) *.o chat_history_*.txt nodes.dat

help:
	@echo "Makefile targets:"
	@echo "  make build              -> compile mesh_cli"
	@echo "  make chat_node PORT=N   -> start node on port N (auto-discovers network)"
	@echo "  make clean              -> remove binaries and data files"
