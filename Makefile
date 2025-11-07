CC = gcc
CFLAGS = -Wall -pthread

udp_node: src/udp_node.c
	$(CC) $(CFLAGS) src/udp_node.c -o udp_node

clean:
	rm -f udp_node
