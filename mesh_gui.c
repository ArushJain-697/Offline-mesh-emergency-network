// -----------------------------------------------------------
// mesh_gui.c  –  Simple GTK front-end for Open Mesh Chat
// -----------------------------------------------------------
#include <gtk/gtk.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>
#include "mesh_backend.h"


GtkWidget *text_view;
GtkWidget *entry;
char node_name;

// -----------------------------------------------------------
// append_chat() (Thread-safe version)
// -----------------------------------------------------------
typedef struct {
    char *message;
} IdleData;

static gboolean append_chat_idle(gpointer user_data) {
    IdleData *data = (IdleData *)user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, data->message, -1);
    gtk_text_buffer_insert(buffer, &end, "\n", -1);
    g_free(data->message);
    g_free(data);
    return G_SOURCE_REMOVE;
}

void append_chat(const char *msg) {
    IdleData *data = g_new(IdleData, 1);
    data->message = g_strdup(msg);
    gdk_threads_add_idle(append_chat_idle, data);
}


// -----------------------------------------------------------
// on_send()
// ✅ --- FINAL LOGIC FIX --- ✅
// -----------------------------------------------------------
void on_send(GtkButton *btn, gpointer data) {
    const char *text_original = gtk_entry_get_text(GTK_ENTRY(entry));
    
    // Use fprintf(stderr, ...) to print debug info to the terminal
    fprintf(stderr, "\n--- DEBUG (v5) --- \n");
    fprintf(stderr, "1. on_send triggered. Input: \"%s\"\n", text_original);
    fflush(stderr);

    if (text_original == NULL || strlen(text_original) == 0) return;

    const char *text = text_original;

    while (*text == ' ') text++;
    fprintf(stderr, "2. After trimming leading spaces: \"%s\"\n", text);
    fflush(stderr);

    // --- BROADCAST CHECK ---
    if (*text == '/') {
        fprintf(stderr, "3. Found '/' at the start.\n");
        fflush(stderr);
        text++; // Move past the '/'
        
        while (*text == ' ') text++; // Skip spaces
        fprintf(stderr, "4. After trimming spaces post-slash: \"%s\"\n", text);
        fflush(stderr);

        // --- THIS IS THE CASE-INSENSITIVE CHECK ---
        // This logic checks for 'a' OR 'A', 'l' OR 'L'.
        // This is why /ALL, /all, and /All will all work.
        if ((text[0] == 'a' || text[0] == 'A') &&
            (text[1] == 'l' || text[1] == 'L') &&
            (text[2] == 'l' || text[2] == 'L'))
        {
            // It's "/all"
            fprintf(stderr, "5. Matched 'all'. This is a broadcast.\n");
            fflush(stderr);
            const char *msg = text + 3; // Move past "all"

            // Now, the next char MUST be a separator (space, colon, or end)
            if (*msg == ':' || *msg == ' ' || *msg == '\0') {
                fprintf(stderr, "6. Found valid separator.\n");
                fflush(stderr);
                while (*msg == ':' || *msg == ' ') msg++; // Skip all separators
                fprintf(stderr, "7. Final message payload: \"%s\"\n", msg);
                fflush(stderr);

                if (strlen(msg) == 0) {
                    append_chat("⚠️ Broadcast message cannot be empty.");
                } else {
                    backend_broadcast(msg);
                    char display[600];
                    sprintf(display, "[You ➜ ALL]: %s", msg);
                    append_chat(display);
                }
                gtk_entry_set_text(GTK_ENTRY(entry), "");
                fprintf(stderr, "8. Broadcast logic complete.\n\n");
                fflush(stderr);
                return;
            } else {
                // It was something like "/allhello"
                fprintf(stderr, "6. FAILED MATCH. 'all' not followed by separator.\n\n");
                fflush(stderr);
            }
        } else {
            // It was something like "/foo"
            fprintf(stderr, "5. FAILED MATCH. Did not match 'all'.\n\n");
            fflush(stderr);
        }
    } else {
        // Did not start with '/'
        fprintf(stderr, "3. Did NOT find '/' at the start.\n");
        fflush(stderr);
    }

    // --- DIRECT MESSAGE CHECK ---
    if (strlen(text) >= 2 && text[1] == ':') {
        fprintf(stderr, "3b. Checking for Direct Message. Found ':'.\n");
        fflush(stderr);
        
        // This 'toupper' also makes the destination case-insensitive
        // so 'a: hello' and 'A: hello' both work.
        char to = toupper(text[0]);
        if (to < 'A' || to > 'E') {
            append_chat("⚠️ Invalid node. Use A, B, C, D, or E.");
            gtk_entry_set_text(GTK_ENTRY(entry), "");
            return;
        }
        const char *msg = text + 2;
        while (*msg == ' ') msg++;
        if (strlen(msg) == 0) {
            append_chat("⚠️ Message cannot be empty.");
        } else {
            backend_send_message(to, msg);
            char display[600];
            sprintf(display, "[You ➜ %c]: %s", to, msg);
            append_chat(display);
        }
        gtk_entry_set_text(GTK_ENTRY(entry), "");
        fprintf(stderr, "8. DM logic complete.\n\n");
        fflush(stderr);
        return;
    }

    // --- INVALID FORMAT ---
    fprintf(stderr, "X. No match. Falling through to 'Invalid format' error.\n\n");
    fflush(stderr);
    append_chat("⚠️ Use format 'B: message' or '/all message'");
    gtk_entry_set_text(GTK_ENTRY(entry), "");
}

// -----------------------------------------------------------
// receiver_thread()
// -----------------------------------------------------------
void *receiver_thread(void *arg) {
    backend_receiver_thread((void*)append_chat);
    return NULL;
}

// -----------------------------------------------------------
// main()
// -----------------------------------------------------------
int main(int argc, char *argv[]) {
    GtkWidget *window, *vbox, *hbox, *scroll, *send_btn;
    pthread_t tid;

    gtk_init(&argc, &argv);

    printf("Enter node name (A/B/C/D/E): ");
    if (scanf(" %c", &node_name) != 1) return 1;
    node_name = toupper(node_name);

    backend_init(node_name);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[50];
    sprintf(title, "Open Mesh Chat - Node %c", node_name);
    gtk_window_set_title(GTK_WINDOW(window), title);
    // This is the line that was incomplete in your file:
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 400);

    // --- ALL THIS CODE WAS MISSING ---
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), text_view);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);

    send_btn = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(hbox), send_btn, FALSE, FALSE, 0);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(send_btn, "clicked", G_CALLBACK(on_send), NULL); 
    g_signal_connect(entry, "activate", G_CALLBACK(on_send), NULL); 
    
    gtk_widget_show_all(window);

    if (pthread_create(&tid, NULL, receiver_thread, NULL) != 0) {
        perror("pthread_create failed");
        return 1;
    }
    
    gtk_main();

    backend_close();
    return 0;
    // --- THIS IS THE MISSING '}' ---
}