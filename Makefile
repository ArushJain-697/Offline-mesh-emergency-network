CC = gcc
CFLAGS = -Wall -pthread

CHAT_BIN = mesh_cli
NEW_BIN  = add_node

CHAT_SRCS = mesh_cli.c mesh_backend.c
NEW_SRCS  = add_node.c

.PHONY: all build chat add nodes_init clean help

all: build

build: $(CHAT_BIN) $(NEW_BIN)

$(CHAT_BIN): $(CHAT_SRCS)
	$(CC) $(CFLAGS) -o $(CHAT_BIN) $(CHAT_SRCS)

$(NEW_BIN): $(NEW_SRCS)
	$(CC) $(CFLAGS) -o $(NEW_BIN) $(NEW_SRCS)

chat: $(CHAT_BIN)
	@echo "Running chat program: ./$(CHAT_BIN)"
	@./$(CHAT_BIN)

chat_node: $(CHAT_BIN)
ifndef NODE
	$(error "Usage: make chat_node NODE=A")
endif
	@printf "%s\n" "$(NODE)" | ./$(CHAT_BIN)

add: $(NEW_BIN)
	@echo "--- Add New Node Helper ---"
	@printf "Enter YOUR Node Letter (Helper): "; read helper; \
	printf "Enter NEW Node Letter: "; read new_node; \
	printf "Enter NEW Node IP: "; read new_ip; \
	printf "Enter NEW Node Port: "; read new_port; \
	echo "---------------------------------------"; \
	echo "Broadcasting new node $$new_node ($$new_ip:$$new_port) via $$helper..."; \
	./$(NEW_BIN) $$helper $$new_node $$new_ip $$new_port

nodes_init:
ifndef IP_A
	$(error "Usage: make nodes_init IP_A=<ip> IP_B=<ip> [IP_C=<ip>]")
endif
ifndef IP_B
	$(error "Usage: make nodes_init IP_A=<ip> IP_B=<ip> [IP_C=<ip>]")
endif
	@echo "Creating nodes.dat with A=$(IP_A):9001 and B=$(IP_B):9002"
	@printf "A %s 9001\nB %s 9002\n" "$(IP_A)" "$(IP_B)" > nodes.dat
ifdef IP_C
	@printf "C %s 9003\n" "$(IP_C)" >> nodes.dat
	@echo "Also added C=$(IP_C):9003"
endif
	@echo "nodes.dat created:"
	@cat nodes.dat

clean:
	rm -f $(CHAT_BIN) $(NEW_BIN) .o chat_log_.txt nodes.dat

help:
	@echo "Makefile targets:"
	@echo "  make chat      -> run chat program"
	@echo "  make add       -> Interactive wizard to add a new node (EASY MODE)"
	@echo "  make clean     -> remove binaries and nodes.dat"
