// -----------------------------------------------------------
// mesh_gui.c  –  Simple GTK front-end for Open Mesh Chat
//   - Incoming messages: left, grey bubble
//   - Outgoing messages: right, green bubble
//   - Thread-safe appends using gdk_threads_add_idle()
// -----------------------------------------------------------

#include <gtk/gtk.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>
#include "mesh_backend.h"

GtkWidget *chat_box;   // vertical box inside scrolled window where each message widget is added
GtkWidget *entry;
char node_name;

// ---------------------- Thread-safe append ----------------------
typedef struct {
    char *message;
} IdleData;

static gboolean append_chat_idle(gpointer user_data);

// helper - create a bubble widget and add to chat_box (must run on main thread)
static void create_message_widget(const char *msg)
{
    // Determine if outgoing (You -> ...) or incoming
    gboolean outgoing = FALSE;
    if (g_str_has_prefix(msg, "[You")) outgoing = TRUE;

    // container for a single row
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    // inside row put a frame (bubble) containing a label
    GtkWidget *bubble = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(bubble), GTK_SHADOW_NONE);
    gtk_widget_set_name(bubble, outgoing ? "bubble-out" : "bubble-in");

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0); // left align text inside bubble
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_label_set_markup(GTK_LABEL(label), g_markup_escape_text(msg, -1));

    // padding inside the bubble
    GtkWidget *pad = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(pad), label);
    gtk_container_add(GTK_CONTAINER(bubble), pad);

    // alignment: pack_start for incoming, pack_end for outgoing
    if (outgoing) {
        gtk_widget_set_halign(row, GTK_ALIGN_END);
        gtk_box_pack_end(GTK_BOX(row), bubble, FALSE, FALSE, 4);
    } else {
        gtk_widget_set_halign(row, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), bubble, FALSE, FALSE, 4);
    }

    // add the row to chat_box
    gtk_box_pack_start(GTK_BOX(chat_box), row, FALSE, FALSE, 6);
    gtk_widget_show_all(row);

    // scroll to bottom
    GtkWidget *scroller = gtk_widget_get_parent(chat_box);
    // chat_box is inside scrolled window's child (a viewport or direct). Find scrolled window:
    while (scroller && !GTK_IS_SCROLLED_WINDOW(scroller)) {
        scroller = gtk_widget_get_parent(scroller);
    }
    if (GTK_IS_SCROLLED_WINDOW(scroller)) {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
        if (vadj) {
            gtk_adjustment_set_value(vadj, gtk_adjustment_get_upper(vadj));
        }
    }
}

static gboolean append_chat_idle(gpointer user_data) {
    IdleData *data = (IdleData *)user_data;
    create_message_widget(data->message);
    g_free(data->message);
    g_free(data);
    return G_SOURCE_REMOVE;
}

void append_chat(const char *msg) {
    IdleData *data = g_new0(IdleData, 1);
    data->message = g_strdup(msg);
    gdk_threads_add_idle(append_chat_idle, data);
}

// ---------------------- UI helpers (CSS) ----------------------
static void load_css(void)
{
    const char *css =
        "frame#bubble-in {"
        "  background: #f1f0f0;"
        "  border-radius: 10px;"
        "  padding: 8px;"
        "  margin: 2px;"
        "}"
        "frame#bubble-out {"
        "  background: #daf8cb;"
        "  border-radius: 10px;"
        "  padding: 8px;"
        "  margin: 2px;"
        "}"
        "label {"
        "  font-family: 'Sans';"
        "  font-size: 12px;"
        "  color: #111111;"
        "}";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    GdkDisplay *display = gdk_display_get_default();
    GdkScreen *screen = gdk_display_get_default_screen(display);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// ---------------------- on_send() (same logic, kept behavior) ----------------------
void on_send(GtkButton *btn, gpointer data) {
    const char *text_original = gtk_entry_get_text(GTK_ENTRY(entry));
    if (text_original == NULL || strlen(text_original) == 0) return;

    const char *text = text_original;
    while (*text == ' ') text++;

    // BROADCAST starting with '/'
    if (*text == '/') {
        text++;
        while (*text == ' ') text++;
        if ((text[0]=='a' || text[0]=='A') &&
            (text[1]=='l' || text[1]=='L') &&
            (text[2]=='l' || text[2]=='L'))
        {
            const char *msg = text + 3;
            if (*msg == ':' || *msg == ' ') {
                while (*msg == ':' || *msg == ' ') msg++;
                if (strlen(msg) == 0) {
                    append_chat("⚠️ Broadcast message cannot be empty.");
                } else {
                    backend_broadcast(msg);
                    char display[600];
                    sprintf(display, "[You ➜ ALL]: %s", msg);
                    append_chat(display);
                }
                gtk_entry_set_text(GTK_ENTRY(entry), "");
                return;
            }
        }
    }

    // DIRECT message X: msg
    if (strlen(text) >= 2 && text[1] == ':') {
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
        return;
    }

    append_chat("⚠️ Use format 'B: message' or '/all message'");
    gtk_entry_set_text(GTK_ENTRY(entry), "");
}

// ---------------------- receiver_thread ----------------------
void *receiver_thread(void *arg) {
    backend_receiver_thread((void*)append_chat);
    return NULL;
}

// ---------------------- main ----------------------
int main(int argc, char *argv[]) {
    GtkWidget *window, *vbox, *hbox, *scrolled, *send_btn;
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
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 500);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    load_css();

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Scrolled area with a vertical box as child
    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    // chat_box will hold one 'row' per message
    chat_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(scrolled), chat_box);

    // Entry + send button
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);

    send_btn = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(hbox), send_btn, FALSE, FALSE, 0);

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
}
