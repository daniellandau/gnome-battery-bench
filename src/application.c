/* -*- mode: C; c-file-style: "stroustrup"; indent-tabs-mode: nil; -*- */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <gtk/gtk.h>

#include <gdk/gdkx.h>
#include <X11/keysymdef.h>

#include "application.h"
#include "battery-test.h"
#include "remote-player.h"
#include "power-graphs.h"
#include "power-monitor.h"
#include "system-state.h"
#include "test-run.h"
#include "util.h"

typedef enum {
    DURATION_TIME,
    DURATION_PERCENT
} DurationType;

typedef enum {
    STATE_STOPPED,
    STATE_PROLOGUE,
    STATE_WAITING,
    STATE_RUNNING,
    STATE_STOPPING,
    STATE_EPILOGUE
} State;

struct _GbbApplication {
    GtkApplication parent;
    GbbPowerMonitor *monitor;
    GbbEventPlayer *player;
    GbbSystemState *system_state;

    GFile *log_folder;

    GbbPowerState *start_state;
    GbbPowerStatistics *statistics;

    GtkBuilder *builder;
    GtkWidget *window;

    GtkWidget *headerbar;
    GtkWidget *start_button;
    GtkWidget *delete_button;

    GtkWidget *test_combo;
    GtkWidget *duration_combo;
    GtkWidget *backlight_combo;

    GtkWidget *log_view;
    GtkListStore *log_model;

    GtkWidget *test_graphs;
    GtkWidget *log_graphs;

    State state;
    gboolean stop_requested;
    gboolean exit_requested;

    GbbBatteryTest *test;
    GbbTestRun *run;
};

struct _GbbApplicationClass {
    GtkApplicationClass parent_class;
};

static void application_stop(GbbApplication *application);

G_DEFINE_TYPE(GbbApplication, gbb_application, GTK_TYPE_APPLICATION)

static void
gbb_application_finalize(GObject *object)
{
}

static void
set_label(GbbApplication *application,
          const char     *label_name,
          const char     *format,
          ...) G_GNUC_PRINTF(3, 4);

static void
set_label(GbbApplication *application,
          const char     *label_name,
          const char     *format,
          ...)
{
    va_list args;

    va_start(args, format);
    char *text = g_strdup_vprintf(format, args);
    va_end(args);

    GtkLabel *label = GTK_LABEL(gtk_builder_get_object(application->builder, label_name));
    gtk_label_set_text(label, text);
    g_free(text);
}

static void
clear_label(GbbApplication *application,
            const char     *label_name)
{
    GtkLabel *label = GTK_LABEL(gtk_builder_get_object(application->builder, label_name));
    gtk_label_set_text(label, "");
}

static void
update_labels(GbbApplication *application)
{
    GbbPowerState *state = gbb_power_monitor_get_state(application->monitor);
    set_label(application, "ac",
              "%s", state->online ? "online" : "offline");

    char *title = NULL;
    switch (application->state) {
    case STATE_STOPPED:
        title = g_strdup("GNOME Battery Bench");
        break;
    case STATE_PROLOGUE:
        title = g_strdup("GNOME Battery Bench - setting up");
        break;
    case STATE_WAITING:
        if (state->online)
            title = g_strdup("GNOME Battery Bench - disconnect from AC to start");
        else
            title = g_strdup("GNOME Battery Bench - waiting for data");
        break;
    case STATE_RUNNING:
    {
        int h, m, s;
        const GbbPowerState *start_state = gbb_test_run_get_start_state(application->run);
        break_time((state->time_us - start_state->time_us) / 1000000, &h, &m, &s);
        title = g_strdup_printf("GNOME Battery Bench - running (%d:%02d:%02d)", h, m, s);
        break;
    }
    case STATE_STOPPING:
        title = g_strdup("GNOME Battery Bench - stopping");
        break;
    case STATE_EPILOGUE:
        title = g_strdup("GNOME Battery Bench - cleaning up");
        break;
    }
    gtk_header_bar_set_title(GTK_HEADER_BAR(application->headerbar), title);
    g_free(title);

    if (state->energy_now >= 0)
        set_label(application, "energy-now", "%.1fWH", state->energy_now);
    else
        clear_label(application, "energy-now");

    if (state->energy_full >= 0)
        set_label(application, "energy-full", "%.1fWH", state->energy_full);
    else
        clear_label(application, "energy-full");

    if (state->energy_now >= 0 && state->energy_full >= 0)
        set_label(application, "percentage", "%.1f%%", 100. * state->energy_now / state->energy_full);
    else
        clear_label(application, "percentage");

    if (state->energy_full_design >= 0)
        set_label(application, "energy-full-design", "%.1fWH", state->energy_full_design);
    else
        clear_label(application, "energy-full-design");

    if (state->energy_now >= 0 && state->energy_full_design >= 0)
        set_label(application, "percentage-design", "%.1f%%", 100. * state->energy_now / state->energy_full_design);
    else
        clear_label(application, "percentage-design");

    GbbPowerStatistics *statistics = application->statistics;
    if (statistics && statistics->power >= 0)
        set_label(application, "power-average", "%.1fW", statistics->power);
    else
        clear_label(application, "power-average");

    const GbbPowerState *last_state = NULL;
    if (application->run)
        last_state = gbb_test_run_get_start_state(application->run);

    if (last_state) {
        GbbPowerStatistics *interval_stats = gbb_power_statistics_compute(last_state, state);
        set_label(application, "power-instant", "%.1fW", interval_stats->power);
        gbb_power_statistics_free(interval_stats);
    } else {
        clear_label(application, "power-instant");
    }

    if (statistics && statistics->battery_life >= 0) {
        int h, m, s;
        break_time(statistics->battery_life, &h, &m, &s);
        set_label(application, "estimated-life", "%d:%02d:%02d", h, m, s);
    } else {
        clear_label(application, "estimated-life");
    }
    if (statistics && statistics->battery_life_design >= 0) {
        int h, m, s;
        break_time(statistics->battery_life_design, &h, &m, &s);
        set_label(application, "estimated-life-design", "%d:%02d:%02d", h, m, s);
    } else {
        clear_label(application, "estimated-life-design");
    }

    gbb_power_state_free(state);
}

static void
update_sensitive(GbbApplication *application)
{
    gboolean start_sensitive = FALSE;
    gboolean controls_sensitive = FALSE;

    switch (application->state) {
    case STATE_STOPPED:
        start_sensitive = gbb_event_player_is_ready(application->player);
        controls_sensitive = TRUE;
        break;
    case STATE_PROLOGUE:
    case STATE_WAITING:
    case STATE_RUNNING:
        start_sensitive = !application->stop_requested;
        controls_sensitive = FALSE;
        break;
    case STATE_STOPPING:
    case STATE_EPILOGUE:
        start_sensitive = FALSE;
        controls_sensitive = FALSE;
        break;
    }

    gtk_widget_set_sensitive(application->start_button, start_sensitive);
    gtk_widget_set_sensitive(application->test_combo, controls_sensitive);
    gtk_widget_set_sensitive(application->duration_combo, controls_sensitive);
    gtk_widget_set_sensitive(application->backlight_combo, controls_sensitive);
}

static void
application_set_state(GbbApplication *application,
                      State           state)
{
    application->state = state;
    update_sensitive(application);
    update_labels(application);
}

static void
on_power_monitor_changed(GbbPowerMonitor *monitor,
                         GbbApplication  *application)
{
    if (application->state == STATE_WAITING) {
        GbbPowerState *state = gbb_power_monitor_get_state(monitor);
        if (!state->online) {
            gbb_test_run_set_start_time(application->run, time(NULL));
            gbb_test_run_add(application->run, state);
            application_set_state(application, STATE_RUNNING);
            gbb_event_player_play_file(application->player, application->test->loop_file);
        } else {
            gbb_power_state_free(state);
        }
    } else if (application->state == STATE_RUNNING) {
        if (application->statistics)
            gbb_power_statistics_free(application->statistics);

        GbbPowerState *state = gbb_power_monitor_get_state(monitor);
        const GbbPowerState *start_state = gbb_test_run_get_start_state(application->run);
        application->statistics = gbb_power_statistics_compute(start_state, state);
        gbb_test_run_add(application->run, state);
        update_labels(application);
    }
}

static void
on_player_ready(GbbEventPlayer *player,
                GbbApplication *application)
{
    update_sensitive(application);
    update_labels(application);
}

static GdkFilterReturn
on_root_event (GdkXEvent *xevent,
               GdkEvent  *event,
               gpointer   data)
{
    GbbApplication *application = data;
    XEvent *xev = (XEvent *)xevent;
    if (xev->xany.type == KeyPress) {
        application_stop(application);
        return GDK_FILTER_REMOVE;
    } else {
        return GDK_FILTER_CONTINUE;
    }
}

/* As always, when we XGrabKey, we need to grab with different combinations
 * of ignored modifiers like CapsLock, NumLock; this function figures that
 * out.
 */
static GList *
get_grab_modifiers(GbbApplication *application)
{
    GdkScreen *screen = gtk_widget_get_screen(application->window);
    Display *xdisplay = gdk_x11_display_get_xdisplay(gdk_screen_get_display(screen));
    gboolean used[8] = { FALSE };
    gint super = -1;

    /* Figure out what modifiers are used, and what modifier is Super */
    XModifierKeymap *modmap =  XGetModifierMapping(xdisplay);
    int i, j;
    for (i = 0; i < 8; i++) {
        for (j = 0; j < modmap->max_keypermod; j++) {
            if (modmap->modifiermap[i * modmap->max_keypermod + j]) {
                used[i] = TRUE;
            }
            if (modmap->modifiermap[i * modmap->max_keypermod + j] == XKeysymToKeycode(xdisplay, XK_Super_L))
                super = i;
        }
    }
    XFree(modmap);

    /* We want to effectively grab only if Shift/Control/Mod1/Super
     * are in the same state we expect.
     */
    guint32 to_ignore = ShiftMask | ControlMask | Mod1Mask;
    if (super >= 0)
        to_ignore |= (1 << super);

    for (i = 0; i < 8; i++) {
        if (!used[i])
            to_ignore |= 1 << i;
    }

    /* quick-and-dirty way to find out all the combinations of other
     * modifiers; since the total number of modifier combinations is
     * small, works fine */
    GList *result = NULL;
    guint32 mask = 0;
    for (mask = 0; mask < 255; mask++) {
        if ((mask & to_ignore) == 0)
            result = g_list_prepend(result, GUINT_TO_POINTER(mask));
    }

    return result;
}

static void
setup_stop_shortcut(GbbApplication *application)
{
    GdkScreen *screen = gtk_widget_get_screen(application->window);
    GdkWindow *root = gdk_screen_get_root_window(screen);
    Window xroot = gdk_x11_window_get_xid(root);
    Display *xdisplay = gdk_x11_display_get_xdisplay(gdk_screen_get_display(screen));

    GList *modifiers = get_grab_modifiers(application);
    GList *l;
    for (l = modifiers; l; l = l->next)
        XGrabKey(xdisplay,
                 XKeysymToKeycode(xdisplay, 'q'),
                 ControlMask | Mod1Mask | GPOINTER_TO_UINT(l->data),
                 xroot, False /* owner_events */,
                 GrabModeAsync /* pointer_mode */, GrabModeAsync /* keyboard_mode */);
    g_list_free(modifiers);

    gdk_window_add_filter(root, on_root_event, application);
}

static void
remove_stop_shortcut(GbbApplication *application)
{
    GdkScreen *screen = gtk_widget_get_screen(application->window);
    GdkWindow *root = gdk_screen_get_root_window(screen);
    Window xroot = gdk_x11_window_get_xid(root);
    Display *xdisplay = gdk_x11_display_get_xdisplay(gdk_screen_get_display(screen));

    GList *modifiers = get_grab_modifiers(application);
    GList *l;
    for (l = modifiers; l; l = l->next)
        XUngrabKey(xdisplay,
                   XKeysymToKeycode(xdisplay, 'q'),
                   ControlMask | Mod1Mask | GPOINTER_TO_UINT(l->data),
                   xroot);
    g_list_free(modifiers);

    gdk_window_remove_filter(root, on_root_event, application);
}

enum {
    COLUMN_RUN,
    COLUMN_NAME,
    COLUMN_DURATION,
    COLUMN_DATE
};

static char *
make_duration_string(GbbTestRun *run)
{
    switch (gbb_test_run_get_duration_type(run)) {
    case GBB_DURATION_TIME:
        return g_strdup_printf("%.0f Minutes", gbb_test_run_get_duration_time(run) / 60);
    case GBB_DURATION_PERCENT:
        return g_strdup_printf("Until %.0f%% battery", gbb_test_run_get_duration_percent(run));
    default:
        g_assert_not_reached();
    }
}

static char *
make_date_string(GbbTestRun *run)
{
    GDateTime *start = g_date_time_new_from_unix_local(gbb_test_run_get_start_time(run));
    GDateTime *now = g_date_time_new_now_local();

    char *result;

    gint64 difference = g_date_time_difference(now, start);
    if (difference < G_TIME_SPAN_DAY)
        result = g_date_time_format(start, "%H:%M");
    else if (difference < 7 * G_TIME_SPAN_DAY &&
             g_date_time_get_day_of_week(now) != g_date_time_get_day_of_week(start))
        result = g_date_time_format(start, "%a %H:%M");
    else if (g_date_time_get_year(now) == g_date_time_get_year(start))
        result = g_date_time_format(start, "%m-%d %H:%M");
    else
        result = g_date_time_format(start, "%Y-%m-%d %H:%M");

    g_date_time_unref(start);
    g_date_time_unref(now);

    return result;
}

static void
add_run_to_logs(GbbApplication *application,
                GbbTestRun     *run)
{
    GtkTreeIter iter;

    char *duration = make_duration_string(run);
    char *date = make_date_string(run);

    gtk_list_store_append(application->log_model, &iter);
    gtk_list_store_set(application->log_model, &iter,
                       COLUMN_RUN, run,
                       COLUMN_DURATION, duration,
                       COLUMN_DATE, date,
                       COLUMN_NAME, gbb_test_run_get_name(run),
                       -1);
    g_free(duration);
    g_free(date);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(application->log_view));
    if (!gtk_tree_selection_get_selected(selection, NULL, NULL))
        gtk_tree_selection_select_iter(selection, &iter);
}

static void
write_run_to_disk(GbbApplication *application,
                  GbbTestRun     *run)
{
    GError *error = NULL;

    gint64 start_time = gbb_test_run_get_start_time(application->run);

    if (!g_file_query_exists(application->log_folder, NULL)) {
        if (!g_file_make_directory_with_parents(application->log_folder, NULL, &error)) {
            g_warning("Cannot create log directory: %s\n", error->message);
            g_clear_error(&error);
            return;
        }
    }

    GDateTime *start_datetime = g_date_time_new_from_unix_utc(start_time);
    char *start_string = g_date_time_format(start_datetime, "%F-%T");
    char *file_name = g_strdup_printf("%s-%s.json", start_string, application->test->id);
    GFile *file = g_file_get_child(application->log_folder, file_name);
    g_free(file_name);
    g_free(start_string);
    g_date_time_unref(start_datetime);

    char *file_path = g_file_get_path(file);
    if (!gbb_test_run_write_to_file(application->run, file_path, &error)) {
        g_warning("Can't write test run to disk: %s\n", error->message);
        g_clear_error(&error);
    }
    g_free(file_path);
    g_object_unref(file);
}

static void
application_set_stopped(GbbApplication *application)
{
    application_set_state(application, STATE_STOPPED);

    const GbbPowerState *start_state = gbb_test_run_get_start_state(application->run);
    const GbbPowerState *last_state = gbb_test_run_get_last_state(application->run);
    if (last_state != start_state) {
        write_run_to_disk(application, application->run);
        add_run_to_logs(application, application->run);
    }

    application->test = NULL;

    remove_stop_shortcut(application);

    gbb_system_state_restore(application->system_state);

    g_object_set(G_OBJECT(application->start_button), "label", "Start", NULL);

    if (application->exit_requested)
        gtk_widget_destroy(application->window);
}

static void
application_set_epilogue(GbbApplication *application)
{
    if (application->test->epilogue_file) {
        gbb_event_player_play_file(application->player, application->test->epilogue_file);
        application_set_state(application, STATE_EPILOGUE);
    } else {
        application_set_stopped(application);
    }
}

static void
on_player_finished(GbbEventPlayer *player,
                   GbbApplication *application)
{
    if (application->state == STATE_PROLOGUE) {
        application_set_state(application, STATE_WAITING);

        if (application->stop_requested) {
            application->stop_requested = FALSE;
            application_stop(application);
        }
    } else if (application->state == STATE_RUNNING) {
        if (gbb_test_run_is_done(application->run))
            application_set_epilogue(application);
        else
            gbb_event_player_play_file(player, application->test->loop_file);
    } else if (application->state == STATE_STOPPING) {
        application_set_epilogue(application);
    } else if (application->state == STATE_EPILOGUE) {
        application_set_stopped(application);
    }
}

static void
application_start(GbbApplication *application)
{
    if (application->state != STATE_STOPPED)
        return;

    if (application->run) {
        gbb_power_graphs_set_test_run(GBB_POWER_GRAPHS(application->test_graphs), NULL);
        g_clear_object(&application->run);
    }

    g_clear_pointer(&application->statistics, (GFreeFunc)gbb_power_statistics_free);

    g_object_set(G_OBJECT(application->start_button), "label", "Stop", NULL);

    const char *test_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(application->test_combo));
    application->test = gbb_battery_test_get_for_id(test_id);

    application->run = gbb_test_run_new(application->test);

    const char *duration_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(application->duration_combo));
    if (strcmp(duration_id, "minutes-5") == 0) {
        gbb_test_run_set_duration_time(application->run, 5 * 60);
    } else if (strcmp(duration_id, "minutes-10") == 0) {
        gbb_test_run_set_duration_time(application->run, 10 * 60);
    } else if (strcmp(duration_id, "minutes-30") == 0) {
        gbb_test_run_set_duration_time(application->run, 30 * 60);
    } else if (strcmp(duration_id, "until-percent-5") == 0) {
        gbb_test_run_set_duration_percent(application->run, 5);
    }

    const char *backlight_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(application->backlight_combo));
    int screen_brightness = atoi(backlight_id);

    gbb_test_run_set_screen_brightness(application->run, screen_brightness);

    gbb_system_state_save(application->system_state);
    gbb_system_state_set_brightnesses(application->system_state,
                                      screen_brightness,
                                      0);

    gbb_power_graphs_set_test_run(GBB_POWER_GRAPHS(application->test_graphs), application->run);

    setup_stop_shortcut(application);

    update_labels(application);

    if (application->test->prologue_file) {
        gbb_event_player_play_file(application->player, application->test->prologue_file);
        application_set_state(application, STATE_PROLOGUE);
    } else {
        application_set_state(application, STATE_WAITING);
    }
}

static void
application_stop(GbbApplication *application)
{
    if ((application->state == STATE_WAITING || application->state == STATE_RUNNING)) {
        if (application->state == STATE_RUNNING) {
            gbb_event_player_stop(application->player);
            application_set_state(application, STATE_STOPPING);
        } else {
            application_set_epilogue(application);
        }
    } else if (application->state == STATE_PROLOGUE) {
        application->stop_requested = TRUE;
        update_sensitive(application);
    }
}

static void
on_main_stack_notify_visible_child(GtkStack       *stack,
                                   GParamSpec     *pspec,
                                   GbbApplication *application)
{
    const gchar *visible = gtk_stack_get_visible_child_name(stack);
    gtk_widget_set_visible(application->start_button, g_strcmp0(visible, "test") == 0);
    gtk_widget_set_visible(application->delete_button, g_strcmp0(visible, "logs") == 0);
}

static void
on_start_button_clicked(GtkWidget      *button,
                        GbbApplication *application)
{
    if (application->state == STATE_STOPPED) {
        application_start(application);
    } else if (application->state == STATE_PROLOGUE ||
               application->state == STATE_WAITING ||
               application->state == STATE_RUNNING) {
        application_stop(application);
    }
}

static void
on_delete_button_clicked(GtkWidget      *button,
                        GbbApplication *application)
{
    GtkTreeModel *model;
    GtkTreeIter iter;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(application->log_view));
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        GbbTestRun *run;
        const char *name;
        const char *date;
        gtk_tree_model_get(model, &iter,
                           COLUMN_RUN, &run,
                           COLUMN_NAME, &name,
                           COLUMN_DATE, &date,
                           -1);

        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(application->window),
                                                   GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_QUESTION,
                                                   GTK_BUTTONS_NONE,
                                                   "Delete log?");

        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                                 "Permanently delete log of test '%s' from %s?",
                                                 name, date);

        gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                               "Cancel", GTK_RESPONSE_CANCEL,
                               "Delete", GTK_RESPONSE_OK,
                               NULL);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

        int response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response == GTK_RESPONSE_OK) {
            const char *filename = gbb_test_run_get_filename(run);
            GFile *file = g_file_new_for_path(filename);
            GError *error = NULL;

            if (g_file_delete(file, NULL, &error)) {
                if (gtk_list_store_remove(application->log_model, &iter))
                    gtk_tree_selection_select_iter(selection, &iter);
            } else {
                g_warning("Failed to delete log: %s\n", error->message);
                g_clear_error(&error);
            }
        }

        gtk_widget_destroy(dialog);
    }
}

static gboolean
on_delete_event(GtkWidget      *window,
                GdkEventAny    *event,
                GbbApplication *application)
{
    if (application->state == STATE_STOPPED) {
        return FALSE;
    } else {
        application->exit_requested = TRUE;
        application_stop(application);
        return TRUE;
    }
}

static void
fill_log_from_run(GbbApplication *application,
                  GbbTestRun     *run)
{
    set_label(application, "test-log", "%s", gbb_test_run_get_name(run));
    char *duration = make_duration_string(run);
    set_label(application, "duration-log", "%s", duration);
    g_free(duration);
    set_label(application, "backlight-log", "%d%%",
              gbb_test_run_get_screen_brightness(run));
    const GbbPowerState *start_state = gbb_test_run_get_start_state(run);
    const GbbPowerState *last_state = gbb_test_run_get_last_state(run);
    if (last_state != start_state) {
        GbbPowerStatistics *statistics = gbb_power_statistics_compute(start_state, last_state);
        if (statistics->power >= 0)
            set_label(application, "power-average-log", "%.1fW", statistics->power);
        else
            clear_label(application, "power-average-log");

        if (last_state->energy_full >= 0)
            set_label(application, "energy-full-log", "%.1fWH", last_state->energy_full);
        else
            clear_label(application, "energy-full-log");

        if (last_state->energy_full_design >= 0)
            set_label(application, "energy-full-design-log", "%.1fWH", last_state->energy_full);
        else
            clear_label(application, "energy-full-design-log");

        if (statistics->battery_life >= 0) {
            int h, m, s;
            break_time(statistics->battery_life, &h, &m, &s);
            set_label(application, "estimated-life-log", "%d:%02d:%02d", h, m, s);
        } else {
            clear_label(application, "estimated-life-log");
        }

        if (statistics->battery_life_design >= 0) {
            int h, m, s;
            break_time(statistics->battery_life_design, &h, &m, &s);
            set_label(application, "estimated-life-design-log", "%d:%02d:%02d", h, m, s);
        } else {
            clear_label(application, "estimated-life-design-log");
        }
    }

    gbb_power_graphs_set_test_run(GBB_POWER_GRAPHS(application->log_graphs), run);
}

static int
compare_runs(gconstpointer a,
             gconstpointer b)
{
    gint64 time_a = gbb_test_run_get_start_time((GbbTestRun *)a);
    gint64 time_b = gbb_test_run_get_start_time((GbbTestRun *)b);

    return time_a < time_b ? -1 : (time_a == time_b ? 0 : 1);
}

static void
read_logs(GbbApplication *application)
{
    GError *error = NULL;
    GFileEnumerator *enumerator;
    GList *runs = NULL;

    enumerator = g_file_enumerate_children (application->log_folder,
                                            "standard::name",
                                            G_FILE_QUERY_INFO_NONE,
                                            NULL, &error);
    if (!enumerator)
        goto out;

    while (error == NULL) {
        GFileInfo *info = g_file_enumerator_next_file (enumerator, NULL, &error);
        GFile *child = NULL;
        if (error != NULL)
            goto out;
        else if (!info)
            break;

        const char *name = g_file_info_get_name (info);
        if (!g_str_has_suffix(name, ".json"))
            goto next;

        child = g_file_enumerator_get_child (enumerator, info);
        char *child_path = g_file_get_path(child);
        GbbTestRun *run = gbb_test_run_new_from_file(child_path, &error);
        if (run) {
            runs = g_list_prepend(runs, run);
        } else {
            g_warning("Can't read test log '%s': %s", child_path, error->message);
            g_clear_error(&error);
        }

    next:
        g_clear_object (&child);
        g_clear_object (&info);
    }

out:
    if (error != NULL) {
        g_warning("Error reading logs: %s", error->message);
        g_clear_error(&error);
    }

    g_clear_object (&enumerator);

    runs = g_list_sort(runs, compare_runs);

    GList *l;
    for (l = runs; l; l = l->next) {
        add_run_to_logs(application, l->data);
        g_object_unref(l->data);
    }

    g_list_free(runs);
}

static void
on_log_selection_changed (GtkTreeSelection *selection,
                          GbbApplication   *application)
{
    GtkTreeModel *model;
    GtkTreeIter iter;

    gboolean have_selected = gtk_tree_selection_get_selected(selection, &model, &iter);

    if (have_selected) {
        GbbTestRun *run;
        gtk_tree_model_get(model, &iter, 0, &run, -1);
        fill_log_from_run(application, run);
    }

    gtk_widget_set_sensitive(application->delete_button, have_selected);
}

static void
gbb_application_activate (GApplication *app)
{
    GbbApplication *application = GBB_APPLICATION (app);

    if (application->window) {
        gtk_window_present (GTK_WINDOW(application->window));
        return;
    }

    application->builder = gtk_builder_new();
    GError *error = NULL;
    gtk_builder_add_from_resource(application->builder,
                                  "/org/gnome/BatteryBench/gnome-battery-bench.xml",
                                  &error);
    if (error)
        die("Cannot load user interface: %s\n", error->message);

    application->window = GTK_WIDGET(gtk_builder_get_object(application->builder, "window"));
    g_signal_connect(application->window, "delete-event",
                     G_CALLBACK(on_delete_event), application);

    gtk_application_add_window(GTK_APPLICATION(app), GTK_WINDOW(application->window));

    application->headerbar = GTK_WIDGET(gtk_builder_get_object(application->builder, "headerbar"));

    application->test_combo = GTK_WIDGET(gtk_builder_get_object(application->builder, "test-combo"));
    GList *tests = gbb_battery_test_list_all();
    GList *l;
    for (l = tests; l; l = l->next) {
        GbbBatteryTest *test = l->data;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(application->test_combo),
                                  test->id, test->name);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(application->test_combo), "idle");

    application->duration_combo = GTK_WIDGET(gtk_builder_get_object(application->builder, "duration-combo"));
    application->backlight_combo = GTK_WIDGET(gtk_builder_get_object(application->builder, "backlight-combo"));

    application->start_button = GTK_WIDGET(gtk_builder_get_object(application->builder, "start-button"));
    application->delete_button = GTK_WIDGET(gtk_builder_get_object(application->builder, "delete-button"));

    GtkWidget *test_graphs_parent = GTK_WIDGET(gtk_builder_get_object(application->builder, "test-graphs-parent"));
    application->test_graphs = gbb_power_graphs_new();
    gtk_box_pack_start(GTK_BOX(test_graphs_parent), application->test_graphs, TRUE, TRUE, 0);

    GtkWidget *log_graphs_parent = GTK_WIDGET(gtk_builder_get_object(application->builder, "log-graphs-parent"));
    application->log_graphs = gbb_power_graphs_new();
    gtk_box_pack_start(GTK_BOX(log_graphs_parent), application->log_graphs, TRUE, TRUE, 0);

    gtk_widget_set_sensitive(application->start_button,
                             gbb_event_player_is_ready(application->player));

    g_signal_connect(application->start_button, "clicked",
                     G_CALLBACK(on_start_button_clicked), application);
    g_signal_connect(application->delete_button, "clicked",
                     G_CALLBACK(on_delete_button_clicked), application);

    /****************************************/

    application->log_view = GTK_WIDGET(gtk_builder_get_object(application->builder, "log-view"));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(application->log_view));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_BROWSE);

    g_signal_connect(selection, "changed",
                     G_CALLBACK(on_log_selection_changed), application);
    on_log_selection_changed(selection, application);

    GtkTreeViewColumn *column;
    GtkCellRenderer *renderer;

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Date", renderer, "text", COLUMN_DATE, NULL);
    gtk_tree_view_column_set_expand(column, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(application->log_view), column);

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Name", renderer, "text", COLUMN_NAME, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(application->log_view), column);

    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes("Duration", renderer, "text", COLUMN_DURATION, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(application->log_view), column);

    application->log_model = GTK_LIST_STORE(gtk_builder_get_object(application->builder, "log-model"));

    read_logs(application);

    /****************************************/

    GtkWidget *main_stack = GTK_WIDGET(gtk_builder_get_object(application->builder, "main-stack"));
    g_signal_connect(main_stack, "notify::visible-child",
                     G_CALLBACK(on_main_stack_notify_visible_child), application);
    on_main_stack_notify_visible_child(GTK_STACK(main_stack), NULL, application);

    update_labels(application);

    g_signal_connect(application->monitor, "changed",
                     G_CALLBACK(on_power_monitor_changed),
                     application);

    gtk_widget_show(application->window);
}

static void
gbb_application_class_init(GbbApplicationClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);
    GApplicationClass *application_class = G_APPLICATION_CLASS(class);

    application_class->activate = gbb_application_activate;

    gobject_class->finalize = gbb_application_finalize;
}

static void
gbb_application_init(GbbApplication *application)
{
    application->monitor = gbb_power_monitor_new();
    application->system_state = gbb_system_state_new();

    char *folder_path = g_build_filename(g_get_user_data_dir(), PACKAGE_NAME, "logs", NULL);
    application->log_folder = g_file_new_for_path(folder_path);
    g_free(folder_path);

    application->player = GBB_EVENT_PLAYER(gbb_remote_player_new("GNOME Battery Bench"));
    g_signal_connect(application->player, "ready",
                     G_CALLBACK(on_player_ready), application);
    g_signal_connect(application->player, "finished",
                     G_CALLBACK(on_player_finished), application);
}

GbbApplication *
gbb_application_new(void)
{
    return g_object_new (GBB_TYPE_APPLICATION,
                         "application-id", "org.gnome.BatteryBench",
                         "flags", G_APPLICATION_FLAGS_NONE,
                         NULL);
}
