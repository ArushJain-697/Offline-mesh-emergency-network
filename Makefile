# Makefile for Mesh Chat

mesh_chat: mesh_cli.c mesh_backend.c mesh_backend.h
	gcc mesh_cli.c mesh_backend.c -o mesh_chat -lpthread

clean:
	rm -f mesh_chat nodes.dat
