// join_cli.c
// This program simply initializes the mesh backend,
// auto-detects local IP, updates & syncs nodes.dat,
// then exits immediately.
//
// Used for "make join"

#include <stdio.h>
#include "mesh_backend.h"

int main(void) {

    printf("\n");
    printf("+==========================================+\n");
    printf("|      MESH NETWORK JOIN INITIALIZER        |\n");
    printf("|   Auto-IP + Distributed nodes.dat Sync    |\n");
    printf("+==========================================+\n\n");

    if (backend_init_auto() != 0) {
        printf("Error: Could not initialize mesh backend.\n");
        return 1;
    }

    printf("Successfully joined mesh.\n");
    printf("nodes.dat updated + broadcasted to all systems.\n");
    printf("Now run:  make chat\n\n");

    // Close the backend cleanly and reset slot on exit
    // BUT since this is a join tool, we DO NOT reset our slot.
    // So instead of backend_close() we directly exit without clearing slot.
    // backend_close() resets slot to 0.0.0.0 which we DON'T want here.

    return 0;
}
