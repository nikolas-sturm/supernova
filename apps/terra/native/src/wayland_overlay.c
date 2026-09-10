#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gdk/gdkwayland.h>
#include <gtk/gtk.h>
#include <wayland-client.h>
#include <webkit2/webkit2.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

typedef struct {
    GtkWidget* window;
    WebKitWebView* web_view;
    struct wl_compositor* compositor;
    struct zwlr_layer_shell_v1* shell;
    struct zwlr_layer_surface_v1* surface;
    struct wl_surface* wayland_surface;
    gboolean prewarm;
    gboolean prewarm_reported;
    gboolean show_requested;
    gboolean layer_configured;
    gboolean page_loaded;
    gboolean ready_reported;
    gchar* revision;
} OverlayState;

static void maybe_report_ready(OverlayState* state) {
    if (state->prewarm && state->page_loaded && !state->prewarm_reported) {
        state->prewarm_reported = TRUE;
        puts("TERRA_OVERLAY_PREWARMED");
        fflush(stdout);
    }
    if (state->show_requested && state->layer_configured && state->page_loaded &&
        !state->ready_reported) {
        state->ready_reported = TRUE;
        puts("TERRA_OVERLAY_READY");
        fflush(stdout);
    }
}

static void layer_configure(void* data, struct zwlr_layer_surface_v1* surface, uint32_t serial,
                            uint32_t width, uint32_t height) {
    OverlayState* state = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    if (width > 0 && height > 0) {
        gtk_window_resize(GTK_WINDOW(state->window), (gint)width, (gint)height);
    }
    state->layer_configured = TRUE;
    maybe_report_ready(state);
}

static void layer_closed(void* data, struct zwlr_layer_surface_v1* surface) {
    (void)data;
    (void)surface;
    gtk_main_quit();
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void registry_global(void* data, struct wl_registry* registry, uint32_t name,
                            const char* interface, uint32_t version) {
    OverlayState* state = data;
    if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
                                        version < 4 ? version : 4);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                             version < 4 ? version : 4);
    }
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void apply_layer_shell(GtkWidget* widget, gpointer user_data) {
    OverlayState* state = user_data;
    GdkWindow* window = gtk_widget_get_window(widget);
    if (!window || !GDK_IS_WAYLAND_WINDOW(window)) exit(EXIT_FAILURE);

    struct wl_display* display =
        gdk_wayland_display_get_wl_display(gdk_window_get_display(window));
    struct wl_registry* registry = wl_display_get_registry(display);
    if (!registry) exit(EXIT_FAILURE);
    wl_registry_add_listener(registry, &registry_listener, state);
    if (wl_display_roundtrip(display) < 0 || !state->shell || !state->compositor) {
        exit(EXIT_FAILURE);
    }
    wl_registry_destroy(registry);

    gdk_wayland_window_set_use_custom_surface(window);
    struct wl_surface* wayland_surface = gdk_wayland_window_get_wl_surface(window);
    state->wayland_surface = wayland_surface;
    state->surface = zwlr_layer_shell_v1_get_layer_surface(
        state->shell, wayland_surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "terra-stream-overlay");
    if (!state->surface) exit(EXIT_FAILURE);

    zwlr_layer_surface_v1_add_listener(state->surface, &layer_listener, state);
    zwlr_layer_surface_v1_set_size(state->surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(
        state->surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(state->surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        state->surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(wayland_surface);
    if (wl_display_roundtrip(display) < 0 || !state->layer_configured) exit(EXIT_FAILURE);
}

static void hide_overlay(OverlayState* state) {
    gtk_widget_set_opacity(state->window, 0.0);
    if (state->surface && state->wayland_surface && state->compositor) {
        struct wl_region* empty_region = wl_compositor_create_region(state->compositor);
        wl_surface_set_input_region(state->wayland_surface, empty_region);
        wl_region_destroy(empty_region);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            state->surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(state->wayland_surface);
        struct wl_display* display =
            gdk_wayland_display_get_wl_display(gtk_widget_get_display(state->window));
        if (wl_display_roundtrip(display) < 0) gtk_main_quit();
    }
}

static void report_hidden(const OverlayState* state) {
    printf("TERRA_OVERLAY_HIDDEN %s\n", state->revision ? state->revision : "0");
    fflush(stdout);
}

static void show_overlay(OverlayState* state) {
    gtk_widget_show_all(state->window);
    gtk_widget_set_opacity(state->window, 1.0);
    if (state->surface && state->wayland_surface) {
        wl_surface_set_input_region(state->wayland_surface, NULL);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            state->surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(state->wayland_surface);
    }
    gdk_display_flush(gtk_widget_get_display(state->window));
}

static void close_overlay(WebKitUserContentManager* manager, WebKitJavascriptResult* result,
                           gpointer user_data) {
    (void)manager;
    OverlayState* state = user_data;
    JSCValue* value = webkit_javascript_result_get_js_value(result);
    gchar* requested_action = jsc_value_to_string(value);
    const char* action = strcmp(requested_action, "disconnect") == 0
                             ? "disconnect"
                             : strcmp(requested_action, "quit") == 0 ? "quit" : "resume";
    g_free(requested_action);
    state->show_requested = FALSE;
    state->ready_reported = FALSE;
    // Remove overlay input before the parent restores stream capture.
    hide_overlay(state);
    report_hidden(state);
    printf("TERRA_OVERLAY_CLOSED %s\n", action);
    fflush(stdout);
    if (!state->prewarm) gtk_widget_destroy(state->window);
}

static void load_changed(WebKitWebView* web_view, WebKitLoadEvent event, gpointer user_data) {
    (void)web_view;
    if (event != WEBKIT_LOAD_FINISHED) return;
    OverlayState* state = user_data;
    state->page_loaded = TRUE;
    maybe_report_ready(state);
}

static gboolean read_command(GIOChannel* channel, GIOCondition condition, gpointer user_data) {
    OverlayState* state = user_data;
    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    gchar* line = NULL;
    gsize length = 0;
    const GIOStatus status = g_io_channel_read_line(channel, &line, &length, NULL, NULL);
    if (status == G_IO_STATUS_EOF) {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }
    if (status != G_IO_STATUS_NORMAL || !line) {
        g_free(line);
        return G_SOURCE_CONTINUE;
    }

    g_strchomp(line);
    if (g_str_has_prefix(line, "SHOW ") && line[5] != '\0') {
        gchar* request = strchr(line + 5, ' ');
        if (!request || request[1] == '\0') {
            g_free(line);
            return G_SOURCE_CONTINUE;
        }
        *request = '\0';
        g_free(state->revision);
        state->revision = g_strdup(line + 5);
        ++request;
        gchar* script = g_strdup_printf("window.TERRA_OVERLAY_SET_REQUEST('%s')", request);
        webkit_web_view_evaluate_javascript(state->web_view, script, -1, NULL, NULL, NULL, NULL,
                                            NULL);
        g_free(script);
        state->show_requested = TRUE;
        state->ready_reported = FALSE;
        show_overlay(state);
        maybe_report_ready(state);
    } else if (g_str_has_prefix(line, "STATS ") && line[6] != '\0') {
        gchar* script =
            g_strdup_printf("window.TERRA_OVERLAY_SET_STATISTICS('%s')", line + 6);
        webkit_web_view_evaluate_javascript(state->web_view, script, -1, NULL, NULL, NULL, NULL,
                                            NULL);
        g_free(script);
    } else if (g_str_has_prefix(line, "HIDE ") && line[5] != '\0') {
        g_free(state->revision);
        state->revision = g_strdup(line + 5);
        state->show_requested = FALSE;
        state->ready_reported = FALSE;
        hide_overlay(state);
        report_hidden(state);
        if (!state->prewarm) gtk_widget_destroy(state->window);
    }
    g_free(line);
    return G_SOURCE_CONTINUE;
}

static gboolean load_failed(WebKitWebView* web_view, WebKitLoadEvent event, const gchar* uri,
                            GError* error, gpointer user_data) {
    (void)web_view;
    (void)event;
    (void)uri;
    (void)error;
    (void)user_data;
    gtk_main_quit();
    return TRUE;
}

static void web_process_terminated(WebKitWebView* web_view, WebKitWebProcessTerminationReason reason,
                                   gpointer user_data) {
    (void)web_view;
    (void)reason;
    (void)user_data;
    gtk_main_quit();
}

int main(int argc, char** argv) {
    const gboolean prewarm = argc == 3 && strcmp(argv[1], "--prewarm") == 0;
    if (argc != 3) return EXIT_FAILURE;
    const char* url = argv[2];
    setenv("GDK_BACKEND", "wayland", 1);
    if (!gtk_init_check(&argc, &argv)) return EXIT_FAILURE;

    OverlayState state = {0};
    state.prewarm = prewarm;
    state.revision = prewarm ? NULL : g_strdup(argv[1]);
    state.show_requested = !prewarm;
    state.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state.window), "Terra Stream Overlay");
    gtk_window_set_decorated(GTK_WINDOW(state.window), FALSE);
    g_signal_connect(state.window, "realize", G_CALLBACK(apply_layer_shell), &state);
    g_signal_connect(state.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GdkScreen* screen = gtk_widget_get_screen(state.window);
    GdkVisual* visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(state.window, visual);
    gtk_widget_set_app_paintable(state.window, TRUE);

    WebKitUserContentManager* manager = webkit_user_content_manager_new();
    g_signal_connect(manager, "script-message-received::terraOverlayClose",
                     G_CALLBACK(close_overlay), &state);
    if (!webkit_user_content_manager_register_script_message_handler(manager,
                                                                      "terraOverlayClose")) {
        return EXIT_FAILURE;
    }
    const char* bridge =
        "window.TERRA_OVERLAY_NATIVE={close:function(action){window.webkit.messageHandlers."
        "terraOverlayClose.postMessage(action||'resume')}};"
        "window.TERRA_OVERLAY_SET_REQUEST=function(value){window.dispatchEvent(new CustomEvent("
        "'terra-overlay-request',{detail:JSON.parse(decodeURIComponent(value))}))};"
        "window.TERRA_OVERLAY_SET_STATISTICS=function(value){window.dispatchEvent(new CustomEvent("
        "'terra-overlay-statistics',{detail:JSON.parse(decodeURIComponent(value))}))};";
    WebKitUserScript* script = webkit_user_script_new(
        bridge, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);

    state.web_view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(manager));
    g_object_unref(manager);
    const GdkRGBA transparent = {0.0, 0.0, 0.0, 0.0};
    webkit_web_view_set_background_color(state.web_view, &transparent);
    g_signal_connect(state.web_view, "load-changed", G_CALLBACK(load_changed), &state);
    g_signal_connect(state.web_view, "load-failed", G_CALLBACK(load_failed), &state);
    g_signal_connect(state.web_view, "web-process-terminated", G_CALLBACK(web_process_terminated),
                     &state);
    gtk_container_add(GTK_CONTAINER(state.window), GTK_WIDGET(state.web_view));
    webkit_web_view_load_uri(state.web_view, url);
    GIOChannel* input = g_io_channel_unix_new(STDIN_FILENO);
    g_io_channel_set_encoding(input, NULL, NULL);
    g_io_add_watch(input, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, read_command, &state);
    g_io_channel_unref(input);
    if (!prewarm) show_overlay(&state);
    gtk_main();
    g_free(state.revision);
    return EXIT_SUCCESS;
}
