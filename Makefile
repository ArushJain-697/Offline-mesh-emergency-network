# Simple Makefile for Mesh Network Project

# Build join program
join: join_cli.c mesh_backend.c mesh_backend.h
	gcc join_cli.c mesh_backend.c -o join

# Build chat program
chat: mesh_cli.c mesh_backend.c mesh_backend.h
	gcc mesh_cli.c mesh_backend.c -o mesh_chat -lpthread

# Remove executables
clean:
	rm -f join mesh_chat
