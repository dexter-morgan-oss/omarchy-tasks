/*
 * tasks-gui — sleek 3-column kanban for Omarchy (GTK4 · C, native & fast).
 *
 *  To Do · In Progress · Done
 *  - click anywhere in a column's empty space -> inline add box right there
 *  - drag any card between columns (or within one) to move / reorder;
 *    dropping into Done marks it done, dragging it back out reopens it
 *  - priority = title colour (+ small dot), click the dot to cycle
 *  - dark blue/black theme, no icons, minimal chrome
 *
 * Storage: $XDG_DATA_HOME/tasks/tasks.json (shared with the `tasks` CLI)
 *
 * Build:
 *   gcc -O2 $(pkg-config --cflags gtk4 jansson) tasks-gui.c \
 *       -o tasks-gui $(pkg-config --libs gtk4 jansson)
 */

#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define APP_ID  "org.omarchy.tasks"
#define VERSION "1.1.0"   /* keep in sync with tasks.py */

/* ---- model ------------------------------------------------------------ */

#define S_TODO 0
#define S_PROG 1
#define S_DONE 2

#define P_HIGH 0
#define P_MED  1
#define P_LOW  2

static const char *STATUS_KEYS[3] = { "todo", "progress", "done" };
static const char *STATUS_TITLES[3] = { "To Do", "In Progress", "Done" };
static const char *PRIO_KEYS[3] = { "high", "med", "low" };
static const char *BADGE_CLASS[3] = { "badge-todo", "badge-progress",
                                      "badge-done" };

/* r,g,b,a in 0..1 */
static const double COL_COLORS[3][4] = {
    { 0.43, 0.63, 1.00, 1.0 }, /* todo     blue  */
    { 0.94, 0.69, 0.30, 1.0 }, /* progress amber */
    { 0.37, 0.82, 0.48, 1.0 }, /* done     green */
};
static const double PRIO_COLORS[3][4] = {
    { 0.94, 0.31, 0.35, 1.0 }, /* high red   */
    { 0.55, 0.60, 0.72, 1.0 }, /* med  gray  */
    { 0.30, 0.78, 0.40, 1.0 }, /* low  green */
};

typedef struct App App;

/* Palette parsed from Omarchy's colors.toml. Every field is heap-allocated
   (theme_clear frees them); `ok` is TRUE only when at least background and
   foreground were found, so a broken/missing file falls back to the built-in
   palette. */
typedef struct {
    gboolean ok;
    gboolean light;              /* mode = "light" */
    gchar *background, *foreground, *accent, *muted, *selection;
    gchar *light_fg, *bright_fg;
    gchar *red, *green, *yellow, *orange, *cyan, *blue, *magenta;
} Theme;

/* One undoable snapshot of the whole store (used by Delete / Clear done). */
typedef struct {
    gchar *json;
    gchar *label;
} UndoState;

typedef struct Task {
    gint64 id;
    char *title;
    gint prio;       /* P_* */
    gint status;     /* S_* */
    char *due;       /* NULL or "YYYY-MM-DD" */
    char *created;
    GtkWidget *row;  /* current listbox row widget or NULL */
    App *app;
} Task;

typedef struct Store {
    GArray *tasks;   /* array of Task* in display order */
    gint next_id;
} Store;

typedef struct Column {
    App *app;
    gint status;
    GtkWidget *panel;
    GtkWidget *scroller;
    GtkWidget *listbox;
    GtkWidget *empty;
    GtkWidget *count;
    GtkWidget *add_row;    /* NULL or the inline add row */
    GtkWidget *add_entry;
    gboolean add_settling; /* a deferred commit/cancel is queued */
    gdouble press_x, press_y;
} Column;

struct App {
    GtkApplication *gapp;
    Store store;
    GtkWidget *win;
    Column *col[3];
    GtkWidget *search;
    gboolean search_active;
    GPtrArray *popovers;
    char *data_path;
    gboolean selftest;
    gint selftest_fail;
    /* config.ini */
    gboolean follow_theme;
    gboolean notify_due;
    gchar *font_family;
    gint font_size;
    gchar *config_path;
    /* live theme */
    Theme theme;
    gchar *theme_path;
    GFileMonitor *theme_mon;
    GFileMonitor *theme_dir_mon;
    /* undo stack (Delete / Clear done) */
    GPtrArray *undo;
    /* most recent day a due notification was considered */
    gchar *last_due_notify;
    /* rotating shortcut ticker at the bottom */
    GtkWidget *hint_stack;
    GtkWidget *hint_a;
    GtkWidget *hint_b;
    gint hint_idx;
    gboolean hint_flip;
    guint hint_timer;
};

/* ---- small helpers ---------------------------------------------------- */

static gint prio_index(const char *s) {
    for (gint i = 0; i < 3; i++)
        if (s && !g_strcmp0(s, PRIO_KEYS[i])) return i;
    return P_MED;
}

static gint status_index(const char *s) {
    for (gint i = 0; i < 3; i++)
        if (s && !g_strcmp0(s, STATUS_KEYS[i])) return i;
    return S_TODO;
}

static void task_free(Task *t) {
    if (!t) return;
    g_free(t->title);
    g_free(t->due);
    g_free(t->created);
    g_free(t);
}

static void store_free(Store *s) {
    if (!s || !s->tasks) return;
    for (guint i = 0; i < s->tasks->len; i++)
        task_free(g_array_index(s->tasks, Task *, i));
    g_array_free(s->tasks, TRUE);
    s->tasks = NULL;
}

static Task *find_task(Store *s, gint64 id) {
    for (guint i = 0; s && s->tasks && i < s->tasks->len; i++) {
        Task *t = g_array_index(s->tasks, Task *, i);
        if (t->id == id) return t;
    }
    return NULL;
}

/* Parse a date token into y/m/d. Accepts the extended form "YYYY-MM-DD"
   and the compact "YYYYMMDD"; the whole token must be consumed, so trailing
   junk ("2026-02-03x") is rejected. Non-padded fields ("2026-1-5") are
   accepted and normalised by the caller, matching what the CLI accepts. */
static gboolean parse_dmy(const char *s, gint *y, gint *m, gint *d) {
    if (!s || !*s) return FALSE;
    gint yy, mm, dd, used = 0;
    if (sscanf(s, "%d-%d-%d%n", &yy, &mm, &dd, &used) == 3 &&
        used == (gint)strlen(s)) {
        *y = yy; *m = mm; *d = dd;
        return TRUE;
    }
    if (sscanf(s, "%4d%2d%2d%n", &yy, &mm, &dd, &used) == 3 &&
        used == (gint)strlen(s)) {
        *y = yy; *m = mm; *d = dd;
        return TRUE;
    }
    return FALSE;
}

/* True for a real calendar date (rejects 2026-02-31, month 13, ...). */
static gboolean valid_date(const char *s) {
    if (!s) return FALSE;
    g_autofree gchar *t = g_strstrip(g_strdup(s));
    gint y = 0, m = 0, d = 0;
    return parse_dmy(t, &y, &m, &d) &&
           g_date_valid_dmy((guint)d, (guint)m, (guint)y);
}

static gchar *today_iso(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    gchar buf[16];
    g_snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900,
               tm.tm_mon + 1, tm.tm_mday);
    return g_strdup(buf);
}

/* Resolve a due token to a normalised "YYYY-MM-DD", or NULL to clear the
   due date (empty, "off", "none", "-", or anything unrecognised). The
   keywords are case-insensitive to match the CLI's parse_due(). */
static gchar *parse_due_token(const char *tok) {
    if (!tok) return NULL;
    g_autofree gchar *t = g_strstrip(g_strdup(tok));
    gchar *out = NULL;
    if (g_ascii_strcasecmp(t, "today") == 0) {
        out = today_iso();
    } else if (g_ascii_strcasecmp(t, "tomorrow") == 0) {
        time_t tt = time(NULL) + 86400;
        struct tm tm;
        localtime_r(&tt, &tm);
        gchar buf[16];
        g_snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900,
                   tm.tm_mon + 1, tm.tm_mday);
        out = g_strdup(buf);
    } else {
        gint y = 0, m = 0, d = 0;
        if (parse_dmy(t, &y, &m, &d) &&
            g_date_valid_dmy((guint)d, (guint)m, (guint)y)) {
            gchar buf[16];
            g_snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
            out = g_strdup(buf);
        }
    }
    return out;
}

/* quick line: "!ship v1 tomorrow" / ".low task" */
static void parse_quick(const char *text, char **title_out, gint *prio_out,
                        char **due_out) {
    gchar *buf = g_strstrip(g_strdup(text));
    gchar *s = buf;
    gint prio = P_MED;
    if (*s == '!') { prio = P_HIGH; s++; }
    else if (*s == '.') { prio = P_LOW; s++; }
    s = g_strstrip(s);
    char *due = NULL;
    char *sp = strrchr(s, ' ');
    if (sp) {
        char *tok = g_strdup(sp + 1);
        due = parse_due_token(tok);
        g_free(tok);
        if (due) *sp = '\0';
    }
    s = g_strstrip(s);
    *title_out = g_strdup(s);
    *prio_out = prio;
    *due_out = due;
    g_free(buf);
}

static gboolean id_in_use(App *a, gint id) {
    for (guint i = 0; i < a->store.tasks->len; i++)
        if (g_array_index(a->store.tasks, Task *, i)->id == id) return TRUE;
    return FALSE;
}

/* Hand out a free id. Guards against a stale next_id (e.g. the CLI appended a
   task with a higher id while this instance was open) rather than trusting the
   counter blindly and producing duplicate ids. */
static gint alloc_id(App *a) {
    gint id = a->store.next_id > 0 ? a->store.next_id : 1;
    while (id_in_use(a, id)) id++;
    a->store.next_id = id + 1;
    return id;
}

static Task *task_new(App *a, const char *title, gint prio, const char *due,
                      gint status) {
    Task *t = g_new0(Task, 1);
    t->id = alloc_id(a);
    t->title = g_strdup(title);
    t->prio = prio;
    t->status = status;
    t->due = due ? g_strdup(due) : NULL;
    t->created = today_iso();
    t->app = a;
    return t;
}

/* ---- storage (jansson) ------------------------------------------------ */

/* Append every task in `root` to a->store.tasks and set next_id. The caller
   must pass an empty tasks array and is responsible for freeing any previous
   tasks — load_store() does this at startup, and undo swaps in a fresh array
   before rebuilding the UI (so old Task* are freed only after their rows have
   been torn down). */
static void store_append_json(App *a, json_t *root) {
    json_t *nid = json_object_get(root, "next_id");
    a->store.next_id = json_is_integer(nid) ? (gint)json_integer_value(nid) : 1;
    if (a->store.next_id < 1) a->store.next_id = 1;
    json_t *tasks = json_object_get(root, "tasks");
    if (!json_is_array(tasks)) return;
    size_t i;
    json_t *j;
    json_array_foreach(tasks, i, j) {
        Task *t = g_new0(Task, 1);
        t->id = (gint64)json_integer_value(json_object_get(j, "id"));
        const char *title = json_string_value(json_object_get(j, "title"));
        t->title = g_strdup(title ? title : "(untitled)");
        const char *st = json_string_value(json_object_get(j, "status"));
        if (!st)
            st = json_is_true(json_object_get(j, "done")) ? "done" : "todo";
        t->status = status_index(st);
        t->prio = prio_index(json_string_value(json_object_get(j, "priority")));
        t->due = g_strdup(json_string_value(json_object_get(j, "due")));
        const char *cre = json_string_value(json_object_get(j, "created"));
        t->created = cre ? g_strdup(cre) : today_iso();
        t->app = a;
        g_array_append_val(a->store.tasks, t);
        /* never hand out an id that is already taken, even if the file's
           next_id was stale (e.g. another writer appended meanwhile) */
        if (t->id >= a->store.next_id) a->store.next_id = t->id + 1;
    }
}

/* Serialise the store exactly as it is written to disk (shared by save_store
   and the undo snapshots). */
static json_t *store_to_json(App *a) {
    json_t *root = json_object();
    json_t *tasks = json_array();
    gint maxid = 0;
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *t = g_array_index(a->store.tasks, Task *, i);
        if (t->id > maxid) maxid = t->id;
    }
    gint nid = a->store.next_id > maxid ? a->store.next_id : maxid + 1;
    json_object_set_new(root, "next_id", json_integer(nid));
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *t = g_array_index(a->store.tasks, Task *, i);
        json_t *o = json_object();
        json_object_set_new(o, "id", json_integer(t->id));
        json_object_set_new(o, "title", json_string(t->title));
        json_object_set_new(o, "done", json_boolean(t->status == S_DONE));
        json_object_set_new(o, "status", json_string(STATUS_KEYS[t->status]));
        json_object_set_new(o, "priority", json_string(PRIO_KEYS[t->prio]));
        json_object_set_new(o, "due",
                            t->due ? json_string(t->due) : json_null());
        json_object_set_new(o, "created", json_string(t->created));
        json_array_append_new(tasks, o);
    }
    json_object_set_new(root, "tasks", tasks);
    return root;
}

/* Move an unreadable store aside so the next save cannot silently clobber it.
   Returns the backup path (caller frees) or NULL on failure. */
static gchar *backup_corrupt(App *a) {
    if (!g_file_test(a->data_path, G_FILE_TEST_EXISTS)) return NULL;
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    gchar *dest = g_strdup_printf("%s.corrupt-%s", a->data_path, stamp);
    if (g_rename(a->data_path, dest) != 0) {
        g_free(dest);
        return NULL;
    }
    return dest;
}

static gboolean load_store(App *a) {
    json_error_t jerr;
    json_t *root = json_load_file(a->data_path, JSON_PRESERVE_ORDER, &jerr);
    if (!root) {
        if (!g_file_test(a->data_path, G_FILE_TEST_EXISTS)) return TRUE;
        g_autofree gchar *bak = backup_corrupt(a);
        if (bak)
            g_warning("tasks: unreadable store backed up to %s (%s)",
                      bak, jerr.text);
        else
            g_warning("tasks: json parse error: %s", jerr.text);
        return FALSE;
    }
    store_append_json(a, root);
    json_decref(root);
    return TRUE;
}

static gboolean save_store(App *a) {
    json_t *root = store_to_json(a);
    g_autofree gchar *dir = g_path_get_dirname(a->data_path);
    g_mkdir_with_parents(dir, 0700);
    gchar *text = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    if (!text) return FALSE;

    /* Write a sibling temp file, flush it to disk, then rename it over the
       store. rename(2) is atomic, so a crash (or full disk) mid-write can
       never leave a truncated tasks.json behind. */
    g_autofree gchar *tmp = g_strconcat(a->data_path, ".tmpXXXXXX", NULL);
    gint fd = g_mkstemp_full(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { g_free(text); return FALSE; }
    gboolean ok = TRUE;
    gsize len = strlen(text), off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = FALSE;
            break;
        }
        off += (gsize)n;
    }
    if (ok && fsync(fd) != 0) ok = FALSE;
    if (close(fd) != 0) ok = FALSE;
    g_free(text);
    if (!ok) { g_unlink(tmp); return FALSE; }
    if (g_rename(tmp, a->data_path) != 0) { g_unlink(tmp); return FALSE; }
    return TRUE;
}


/* The stylesheet is installed by css_apply()/theme_apply() further down, once
   the config and the active Omarchy palette have been read. */

/* ---- dot widgets ------------------------------------------------------ */

typedef struct { const double *color; } DotData;

static void dot_draw(GtkDrawingArea *da, cairo_t *cr, gint w, gint h,
                     gpointer ud) {
    (void)da;
    const double *c = ((DotData *)ud)->color;
    cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
    double r = (w < h ? w : h) / 2.0 - 1.0;
    if (r < 1.2) r = 1.2;
    cairo_arc(cr, w / 2.0, h / 2.0, r, 0, 2 * G_PI);
    cairo_fill(cr);
}

static GtkWidget *make_dot(const double *color) {
    DotData *d = g_new0(DotData, 1);
    d->color = color;
    GtkWidget *da = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), dot_draw, d, g_free);
    gtk_widget_set_size_request(da, 8, 8);
    gtk_widget_set_valign(da, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(da), "dotdata", d);
    return da;
}

static void dot_set_color(GtkWidget *da, const double *color) {
    DotData *d = g_object_get_data(G_OBJECT(da), "dotdata");
    if (d) {
        d->color = color;
        gtk_widget_queue_draw(da);
    }
}
/* ---- forward declarations --------------------------------------------- */

static void rebuild_all(App *a);
static void rebuild_column(Column *col);
static void create_add_row(Column *col, gint index);
static void cancel_add_row(Column *col);
static void commit_add(Column *col, gboolean recreate);
static void settle_add_row(Column *col);
static void change_status(App *a, Task *t, gint dst);
static void move_task(App *a, Task *t, gint dst, gdouble x, gdouble y);
static void relocate_row(App *a, Task *t, gint src, gint dst);
static void update_counts(App *a);
static void refresh_row(App *a, Task *t);
static GtkWidget *build_row(App *a, Task *t);
static void delete_task(App *a, Task *t);
static void close_all_popovers(App *a);
static gboolean matches_query(App *a, Task *t);
static void clear_search(App *a, gboolean do_rebuild);
static gboolean save_store(App *a);
static void rename_activated(GtkEntry *entry, gpointer ud);
static gboolean rename_key(GtkEventControllerKey *c, guint k, guint kc,
                           GdkModifierType st, gpointer ud);
static void rename_focus_left(GtkEventControllerFocus *fc, gpointer ud);
static void apply_due(App *a, Task *t, const char *val);

/* ==== config: ~/.config/omarchy-tasks/config.ini ======================== */

/* Read follow_theme / notify_due from an INI file. A missing file or key
   keeps the default (both TRUE). A file without a section header is treated
   as [general], so a bare "notify_due = false" line still works. */
static void config_load(App *a, const gchar *path) {
    a->follow_theme = TRUE;
    a->notify_due = TRUE;
    g_free(a->font_family);
    a->font_family = g_strdup("JetBrainsMono Nerd Font");
    a->font_size = 13;
    g_autofree gchar *data = NULL;
    gsize len = 0;
    if (!path || !g_file_get_contents(path, &data, &len, NULL)) return;
    g_autofree gchar *wrapped = strchr(data, '[')
        ? g_strdup(data)
        : g_strconcat("[general]\n", data, NULL);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_data(kf, wrapped, (gsize)-1, G_KEY_FILE_NONE,
                                  NULL)) {
        GError *e = NULL;
        gboolean v = g_key_file_get_boolean(kf, "general", "follow_theme", &e);
        if (e) g_clear_error(&e); else a->follow_theme = v;
        v = g_key_file_get_boolean(kf, "general", "notify_due", &e);
        if (e) g_clear_error(&e); else a->notify_due = v;
        gchar *f = g_key_file_get_string(kf, "general", "font", &e);
        if (e) g_clear_error(&e);
        else if (f && *f) { g_free(a->font_family); a->font_family = f; f = NULL; }
        g_free(f);
        gint sz = g_key_file_get_integer(kf, "general", "font_size", &e);
        if (e) g_clear_error(&e);
        else if (sz >= 8 && sz <= 32) a->font_size = sz;
    }
    g_key_file_unref(kf);
}

/* ---- theme: follow the active Omarchy theme ---------------------------- */

static void theme_clear(Theme *t) {
    g_free(t->background); g_free(t->foreground); g_free(t->accent);
    g_free(t->muted);      g_free(t->selection);
    g_free(t->light_fg);   g_free(t->bright_fg);
    g_free(t->red);  g_free(t->green); g_free(t->yellow); g_free(t->orange);
    g_free(t->cyan); g_free(t->blue);  g_free(t->magenta);
    memset(t, 0, sizeof *t);
}

/* colors.toml is a flat TOML table of `key = "value"` pairs. */
static void theme_parse(Theme *t, const gchar *path) {
    theme_clear(t);
    g_autofree gchar *data = NULL;
    gsize len = 0;
    if (!path || !g_file_get_contents(path, &data, &len, NULL)) return;
    g_auto(GStrv) lines = g_strsplit(data, "\n", -1);
#define TSET(f) do { g_free(t->f); t->f = g_strdup(val); } while (0)
    for (gint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') continue;
        gchar *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        gchar *key = g_strstrip(line);
        gchar *val = g_strstrip(eq + 1);
        if (*val == '"' || *val == '\'') {
            gchar q = *val++;
            gchar *end = strrchr(val, q);
            if (end) *end = '\0';
        }
        if      (!strcmp(key, "background"))        TSET(background);
        else if (!strcmp(key, "foreground"))        TSET(foreground);
        else if (!strcmp(key, "accent"))            TSET(accent);
        else if (!strcmp(key, "muted"))             TSET(muted);
        else if (!strcmp(key, "selection"))         TSET(selection);
        else if (!strcmp(key, "light_foreground"))  TSET(light_fg);
        else if (!strcmp(key, "bright_foreground")) TSET(bright_fg);
        else if (!strcmp(key, "red"))               TSET(red);
        else if (!strcmp(key, "green"))             TSET(green);
        else if (!strcmp(key, "yellow"))            TSET(yellow);
        else if (!strcmp(key, "orange"))            TSET(orange);
        else if (!strcmp(key, "cyan"))              TSET(cyan);
        else if (!strcmp(key, "blue"))              TSET(blue);
        else if (!strcmp(key, "magenta"))           TSET(magenta);
        else if (!strcmp(key, "mode"))
            t->light = (g_ascii_strcasecmp(val, "light") == 0);
    }
#undef TSET
    t->ok = (t->background != NULL && t->foreground != NULL);
}

static gboolean hex_rgb(const gchar *s, gint *r, gint *g, gint *b) {
    if (!s) return FALSE;
    if (*s == '#') s++;
    if (strlen(s) < 6) return FALSE;
    gint rr, gg, bb;
    if (sscanf(s, "%2x%2x%2x", &rr, &gg, &bb) != 3) return FALSE;
    *r = rr; *g = gg; *b = bb;
    return TRUE;
}

/* "#rrggbb" + alpha -> "rgba(r, g, b, a)"; whole-string fallback on failure. */
static gchar *hex_rgba(const gchar *hex, gdouble alpha) {
    gint r, g, b;
    if (!hex_rgb(hex, &r, &g, &b)) return g_strdup("rgba(0,0,0,0)");
    return g_strdup_printf("rgba(%d, %d, %d, %.2f)", r, g, b, alpha);
}

/* Linear blend of two hex colours (t = 0 keeps `a`). */
static gchar *mix_hex(const gchar *a, const gchar *b, gdouble t) {
    gint ar, ag, ab, br, bg, bb;
    if (!hex_rgb(a, &ar, &ag, &ab) || !hex_rgb(b, &br, &bg, &bb))
        return g_strdup(a ? a : "#000000");
    return g_strdup_printf("#%02x%02x%02x",
                           (gint)(ar + (br - ar) * t),
                           (gint)(ag + (bg - ag) * t),
                           (gint)(ab + (bb - ab) * t));
}

/* Build the whole stylesheet. A parsed palette (`t`, t->ok) themes it; a NULL
   palette falls back to the built-in dark-blue one. `font`/`base` come from
   config so family and type scale can be tuned without a rebuild. Every element
   size derives from `base`, keeping the scale coherent (base, +1, -1, -2). */
static gchar *css_build(const Theme *t, const gchar *font, gint base) {
    gboolean themed = t && t->ok;
    const gchar *fg     = themed && t->foreground ? t->foreground : "#cbd6ea";
    const gchar *bg     = themed && t->background ? t->background : "#040814";
    const gchar *accent = themed ? (t->accent ? t->accent
                          : (t->blue ? t->blue : "#5b8cff")) : "#5b8cff";
    const gchar *bright = fg;
    const gchar *red    = themed && t->red   ? t->red   : "#ff5c66";
    const gchar *green  = themed && t->green ? t->green : "#63cf7d";
    const gchar *yellow = themed ? (t->yellow ? t->yellow
                                    : (t->orange ? t->orange : "#f2b455"))
                                 : "#f2b455";
    const gchar *family = font && *font ? font : "JetBrainsMono Nerd Font";
    if (base < 8) base = 8;
    if (base > 32) base = 32;
    gint s_title = base + 1;                  /* task titles            */
    gint s_small = base > 10 ? base - 1 : base;
    gint s_tiny  = base > 11 ? base - 2 : base - 1;
    /* Surfaces and muted text blend the theme's own bg toward its fg, so the
       result stays readable for dark AND light themes. Never derive a surface
       from the foreground alone: a white fg (e.g. Vantablack) would otherwise
       yield white panels, making white task text invisible on them. */
    g_autofree gchar *muted     = mix_hex(fg, bg, 0.40);
    g_autofree gchar *med       = mix_hex(fg, bg, 0.45);
    g_autofree gchar *win_bg    = hex_rgba(bg, 0.72);
    g_autofree gchar *head_bg   = hex_rgba(mix_hex(bg, fg, 0.03), 0.55);
    g_autofree gchar *panel_bg  = hex_rgba(mix_hex(bg, fg, 0.07), 0.52);
    g_autofree gchar *entry_bg  = hex_rgba(mix_hex(bg, fg, 0.05), 0.66);
    g_autofree gchar *border    = hex_rgba(accent, 0.11);
    g_autofree gchar *border2   = hex_rgba(accent, 0.35);
    g_autofree gchar *sel_bg    = hex_rgba(accent, 0.24);
    g_autofree gchar *hover_bg  = hex_rgba(fg, 0.05);
    g_autofree gchar *hover2    = hex_rgba(fg, 0.08);
    g_autofree gchar *drop_bg   = hex_rgba(accent, 0.16);
    g_autofree gchar *badge_hi  = hex_rgba(accent, 0.18);
    g_autofree gchar *badge_pr  = hex_rgba(yellow, 0.16);
    g_autofree gchar *badge_do  = hex_rgba(green, 0.16);

    GString *s = g_string_new("* { outline: none; }\n");
    g_string_append_printf(s,
        "window, window.background { background-color: %s;\n"
        "         background-image: none; box-shadow: none; color: %s;\n"
        "         font-family: \"%s\", \"JetBrainsMono NF\", monospace;\n"
        "         font-size: %dpx; }\n", win_bg, fg, family, base);
    g_string_append_printf(s,
        "headerbar { background: %s; background-image: none;\n"
        "            border-bottom: 1px solid %s; color: %s; }\n",
        head_bg, border, muted);
    g_string_append_printf(s,
        ".app-title { font-size: %dpx; font-weight: 700; letter-spacing: 4px; color: %s; }\n",
        base, bright);
    g_string_append_printf(s,
        ".panel { background-color: %s; border: 1px solid %s; border-radius: 16px;\n"
        "         box-shadow: inset 0 0 0 1px rgba(255,255,255,0.02); }\n",
        panel_bg, border);
    g_string_append_printf(s,
        ".column-title { font-size: %dpx; font-weight: 700; letter-spacing: 2.2px; color: %s; }\n",
        s_small, muted);
    g_string_append_printf(s,
        ".badge { font-size: %dpx; font-weight: 700; border-radius: 999px; padding: 1px 8px; min-width: 20px; }\n"
        ".badge-todo     { color:%s; background:%s; }\n"
        ".badge-progress { color:%s; background:%s; }\n"
        ".badge-done     { color:%s; background:%s; }\n",
        s_tiny, accent, badge_hi, yellow, badge_pr, green, badge_do);
    g_string_append_printf(s, ".hint-label { font-size: %dpx; color: %s; }\n",
                           s_small, muted);
    g_string_append_printf(s,
        "scrolledwindow, scrolledwindow > viewport, overlay, listbox, list {\n"
        "    background: none; box-shadow: none; border: none; }\n"
        "listbox.row-list { background: none; }\n"
        "row { background: none; border-radius: 12px; }\n"
        "row:selected { background-color: %s; border-radius: 12px; }\n"
        "row:hover:not(:selected) { background-color: %s; border-radius: 12px; }\n",
        sel_bg, hover_bg);
    g_string_append_printf(s,
        ".task-box { border-radius: 11px; transition: background-color 130ms ease; }\n"
        ".task-box:hover { background-color: %s; }\n"
        ".task-title { font-size: %dpx; color: %s; }\n"
        ".task-title.prio-high { color: %s; font-weight: 600; }\n"
        ".task-title.prio-med  { color: %s; }\n"
        ".task-title.prio-low  { color: %s; }\n"
        ".task-done .task-title { opacity: 0.40; text-decoration: line-through; }\n"
        ".due-label { font-size: %dpx; color: %s; }\n"
        ".due-overdue { color: %s; font-weight: 700; }\n",
        hover2, s_title, fg, red, med, green, s_tiny, muted, red);
    g_string_append_printf(s,
        "entry { background-color: %s; border-color: %s; color: %s; border-radius: 10px; }\n"
        "entry:focus { border-color: %s; }\n"
        ".add-entry { border: 1px dashed %s; background: %s; }\n"
        ".drop-target { background-color: %s; border-radius: 12px; }\n"
        ".pop-label { font-size: %dpx; color: %s; }\n",
        entry_bg, border2, fg, accent, border2, hover_bg, drop_bg, s_small, muted);
    return g_string_free(s, FALSE);
}

static GtkCssProvider *g_css_provider = NULL;

/* Install/replace the app stylesheet (one provider, reloaded in place). */
static void css_apply(const gchar *css) {
    GdkDisplay *d = gdk_display_get_default();
    if (!d) return;
    if (!g_css_provider) {
        g_css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            d, GTK_STYLE_PROVIDER(g_css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_string(g_css_provider, css);
}

/* Ask GTK for the matching variant so built-in controls sit well inside it. */
static void prefer_dark(gboolean want) {
    GtkSettings *st = gtk_settings_get_default();
    if (!st) return;
    GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(st),
        "gtk-application-prefer-dark-theme");
    if (pspec) g_object_set(st, "gtk-application-prefer-dark-theme", want, NULL);
}

/* The live Omarchy palette when enabled + parsed, else the built-in one. */
static void theme_apply(App *a) {
    gboolean themed = a->follow_theme && a->theme.ok;
    g_autofree gchar *css = css_build(themed ? &a->theme : NULL,
                                      a->font_family, a->font_size);
    css_apply(css);
    prefer_dark(!(themed && a->theme.light));
}

static void theme_reload(App *a) {
    theme_parse(&a->theme, a->theme_path);
    theme_apply(a);
}

static gboolean on_theme_changed(GFileMonitor *m, GFile *f, GFile *o,
                                 GFileMonitorEvent ev, gpointer ud) {
    (void)m; (void)f; (void)o; (void)ev;
    theme_reload(ud);
    return TRUE;
}

/* Watch the file and its folder: Omarchy rewrites colors.toml in place on a
   theme change, but a fresh copy can also replace the whole file. */
static void theme_watch(App *a) {
    if (!a->theme_path) return;
    GFile *file = g_file_new_for_path(a->theme_path);
    a->theme_mon = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (a->theme_mon)
        g_signal_connect(a->theme_mon, "changed",
                         G_CALLBACK(on_theme_changed), a);
    g_object_unref(file);
    g_autofree gchar *dir = g_path_get_dirname(a->theme_path);
    GFile *dfile = g_file_new_for_path(dir);
    a->theme_dir_mon = g_file_monitor_directory(dfile, G_FILE_MONITOR_NONE,
                                                NULL, NULL);
    if (a->theme_dir_mon)
        g_signal_connect(a->theme_dir_mon, "changed",
                         G_CALLBACK(on_theme_changed), a);
    g_object_unref(dfile);
}

/* ---- undo (Delete / Clear done) ---------------------------------------- */

static void undo_state_free(gpointer p) {
    UndoState *u = p;
    if (!u) return;
    g_free(u->json);
    g_free(u->label);
    g_free(u);
}

/* Snapshot the store *before* a destructive change. Capped so a long session
   can't grow without bound. */
static void undo_push(App *a, const gchar *label) {
    if (!a->undo) a->undo = g_ptr_array_new_with_free_func(undo_state_free);
    UndoState *u = g_new0(UndoState, 1);
    json_t *root = store_to_json(a);
    u->json = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    u->label = g_strdup(label);
    g_ptr_array_add(a->undo, u);
    while (a->undo->len > 30)
        g_ptr_array_remove_index(a->undo, 0);
}

static void undo_clear(App *a) {
    if (!a->undo) return;
    g_ptr_array_set_size(a->undo, 0);
}

/* Restore the most recent snapshot. The live task array is swapped out and
   only freed after rebuild_all() has cleared the rows that pointed at it. */
static gboolean undo_perform(App *a) {
    if (!a->undo || a->undo->len == 0) return FALSE;
    UndoState *u = g_ptr_array_steal_index(a->undo, a->undo->len - 1);
    json_error_t err;
    json_t *root = json_loads(u->json, 0, &err);
    if (!root) {
        undo_state_free(u);
        return FALSE;
    }
    close_all_popovers(a);
    GArray *old = a->store.tasks;
    a->store.tasks = g_array_new(FALSE, FALSE, sizeof(Task *));
    store_append_json(a, root);
    json_decref(root);
    rebuild_all(a);
    for (guint i = 0; i < old->len; i++)
        task_free(g_array_index(old, Task *, i));
    g_array_free(old, TRUE);
    save_store(a);
    undo_state_free(u);
    return TRUE;
}

/* ---- due-date notifications -------------------------------------------- */

/* Every not-done task due today or earlier. */
static void due_tasks(App *a, GPtrArray *out) {
    g_autofree gchar *today = today_iso();
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *t = g_array_index(a->store.tasks, Task *, i);
        if (t->status == S_DONE || !t->due) continue;
        if (g_strcmp0(t->due, today) <= 0) g_ptr_array_add(out, t);
    }
}

/* Human summary: up to three titles, then "and N more". Sets *count. */
static gchar *due_body(App *a, gint *count) {
    GPtrArray *due = g_ptr_array_new();
    due_tasks(a, due);
    gint n = (gint)due->len;
    if (count) *count = n;
    GString *s = g_string_new(NULL);
    for (gint i = 0; i < n && i < 3; i++) {
        Task *t = g_ptr_array_index(due, i);
        if (i) g_string_append(s, ", ");
        g_string_append(s, t->title);
    }
    if (n > 3) g_string_append_printf(s, " and %d more", n - 3);
    g_ptr_array_free(due, TRUE);
    return g_string_free(s, FALSE);
}

/* At most one summary notification per calendar day. Returns TRUE only when a
   notification was actually sent (the selftest suppresses sending). */
static gboolean notify_due(App *a) {
    if (!a->notify_due) return FALSE;
    g_autofree gchar *today = today_iso();
    if (g_strcmp0(a->last_due_notify, today) == 0) return FALSE;
    g_free(a->last_due_notify);
    a->last_due_notify = g_strdup(today);
    gint n = 0;
    g_autofree gchar *body = due_body(a, &n);
    if (n == 0 || a->selftest) return FALSE;
    GNotification *note =
        g_notification_new(n == 1 ? "Task due" : "Tasks due");
    g_notification_set_body(note, body);
    g_application_send_notification(G_APPLICATION(a->gapp), "due", note);
    g_object_unref(note);
    return TRUE;
}

static gboolean notify_due_timer(gpointer ud) {
    notify_due(ud);
    return G_SOURCE_CONTINUE;
}

static const char *query(App *a) {
    return (a->search_active && a->search) ? gtk_editable_get_text(
        GTK_EDITABLE(a->search)) : "";
}

static gboolean matches_query(App *a, Task *t) {
    if (!a->search_active) return TRUE;
    const char *q = query(a);
    if (!*q) return TRUE;
    /* casefold rather than g_ascii_strdown so non-ASCII titles match too */
    g_autofree gchar *hay = g_utf8_casefold(t->title, -1);
    g_autofree gchar *needle = g_utf8_casefold(q, -1);
    return strstr(hay, needle) != NULL;
}

/* ---- store / array helpers --------------------------------------------- */

static gint count_status(App *a, gint status) {
    gint n = 0;
    for (guint i = 0; i < a->store.tasks->len; i++)
        if (g_array_index(a->store.tasks, Task *, i)->status == status) n++;
    return n;
}

/* insertion point right after the last task of a column */
static gint group_end(App *a, gint status) {
    guint end = a->store.tasks->len;
    for (guint i = 0; i < a->store.tasks->len; i++)
        if (g_array_index(a->store.tasks, Task *, i)->status == status) end = i + 1;
    return (gint)end;
}

static gint array_index_of(App *a, Task *t) {
    for (guint i = 0; i < a->store.tasks->len; i++)
        if (g_array_index(a->store.tasks, Task *, i) == t) return (gint)i;
    return -1;
}

static void array_remove(App *a, Task *t) {
    gint i = array_index_of(a, t);
    if (i >= 0) g_array_remove_index(a->store.tasks, (guint)i);
}

static void array_insert_at(App *a, gint idx, Task *t) {
    if (idx < 0) idx = 0;
    if (idx > (gint)a->store.tasks->len) idx = (gint)a->store.tasks->len;
    g_array_insert_val(a->store.tasks, (guint)idx, t);
}

static Task *next_same_status(App *a, Task *t, gint status) {
    gint i = array_index_of(a, t);
    for (gint k = i + 1; k < (gint)a->store.tasks->len; k++) {
        Task *x = g_array_index(a->store.tasks, Task *, k);
        if (x->status == status) return x;
    }
    return NULL;
}

/* index of t among the visible rows present in the target column listbox */
static gint visible_index_of(App *a, Task *t, gint status) {
    gint cnt = 0;
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *x = g_array_index(a->store.tasks, Task *, i);
        if (x == t) break;
        if (x->status == status && x->row && matches_query(a, x)) cnt++;
    }
    return cnt;
}

static gint visible_rows(Column *col) {
    gint n = 0;
    GtkWidget *w = gtk_widget_get_first_child(col->listbox);
    while (w) {
        if (GTK_IS_LIST_BOX_ROW(w)) n++;
        w = gtk_widget_get_next_sibling(w);
    }
    return n;
}

/* ---- popovers ----------------------------------------------------------- */

static void popover_closed(GtkPopover *pop, gpointer ud) {
    GPtrArray *arr = ud;
    if (arr) {
        /* drop the pointer so a later close_all_popovers can't hit a freed
           widget when the user dismissed this popover on their own */
        for (guint i = 0; i < arr->len; i++) {
            if (g_ptr_array_index(arr, i) == pop) {
                g_ptr_array_remove_index(arr, i);
                break;
            }
        }
    }
    GtkWidget *p = gtk_widget_get_parent(GTK_WIDGET(pop));
    if (p) gtk_widget_unparent(GTK_WIDGET(pop));
}

static void popover_show(App *a, GtkWidget *pop, GtkWidget *anchor) {
    gtk_widget_set_parent(pop, anchor);
    gtk_popover_popup(GTK_POPOVER(pop));
    g_signal_connect(pop, "closed", G_CALLBACK(popover_closed), a->popovers);
    g_ptr_array_add(a->popovers, pop);
}

/* Closing a popover from inside one of its children's signals (a menu item's
   "clicked", the due entry's "activate") unparents the very widget GTK is
   mid-emission on. Detach the list now and let an idle do the popdown. */
static gboolean popovers_close_idle(gpointer ud) {
    GPtrArray *list = ud; /* holds a ref on each popover */
    for (guint i = 0; i < list->len; i++) {
        GtkWidget *pop = g_ptr_array_index(list, i);
        if (gtk_widget_get_parent(pop))
            gtk_popover_popdown(GTK_POPOVER(pop)); /* closed → unparent */
        g_object_unref(pop);
    }
    g_ptr_array_free(list, TRUE);
    return G_SOURCE_REMOVE;
}

static void close_all_popovers(App *a) {
    if (!a->popovers || a->popovers->len == 0) return;
    GPtrArray *list = g_ptr_array_new();
    for (guint i = 0; i < a->popovers->len; i++)
        g_ptr_array_add(list, g_object_ref(g_ptr_array_index(a->popovers, i)));
    /* clear now so a popover opened right after this call isn't swept away */
    g_ptr_array_set_size(a->popovers, 0);
    g_idle_add(popovers_close_idle, list);
}

/* ---- due popover -------------------------------------------------------- */

static void apply_due(App *a, Task *t, const char *val) {
    g_free(t->due);
    t->due = NULL;
    if (val && *val) t->due = parse_due_token(val);
    save_store(a);
    refresh_row(a, t);
    close_all_popovers(a);
}

typedef struct { App *a; Task *t; GtkWidget *entry; } DueCtx;

static DueCtx *due_ctx_new(DueCtx *src, GtkWidget *entry) {
    DueCtx *c = g_new0(DueCtx, 1);
    c->a = src->a;
    c->t = src->t;
    c->entry = entry;
    return c;
}

static void due_ctx_notify(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

/* GTK calls signal handlers as (emitting_widget, user_data); the context is
   the *second* argument, not the first. */
static void due_btn_set(GtkWidget *w, gpointer ud) {
    (void)w;
    DueCtx *c = ud;
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(c->entry));
    apply_due(c->a, c->t, text ? text : "");
}

static void open_due_popover(App *a, Task *t, GtkWidget *anchor) {
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *hint = gtk_label_new("today · tomorrow · YYYY-MM-DD · off");
    gtk_widget_add_css_class(hint, "pop-label");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), "due date…");
    if (t->due) gtk_editable_set_text(GTK_EDITABLE(e), t->due);
    GtkWidget *b_set = gtk_button_new_with_label("Set");
    gtk_widget_set_halign(b_set, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), hint);
    gtk_box_append(GTK_BOX(box), e);
    gtk_box_append(GTK_BOX(box), b_set);
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    DueCtx *base = g_new0(DueCtx, 1);
    base->a = a;
    base->t = t;
    base->entry = e;
    g_signal_connect_data(b_set, "clicked", G_CALLBACK(due_btn_set),
                          due_ctx_new(base, e), (GClosureNotify)due_ctx_notify, 0);
    g_signal_connect_data(e, "activate", G_CALLBACK(due_btn_set),
                          due_ctx_new(base, e), (GClosureNotify)due_ctx_notify, 0);
    g_free(base);
    popover_show(a, pop, anchor);
    gtk_widget_grab_focus(e);
}

/* The context menu is still mapped while its "Due date" item runs; mapping the
   due popover right then asks GDK for a second grabbing popup under a non top
   most parent, which it refuses ("Tried to map a grabbing popup with a
   non-top most parent"). The half-taken grab then swallows every later click
   and key in the window, so the app looks frozen. Close the menu first and do
   the open in the next idle, once the menu is really gone. */
typedef struct {
    App *a;
    Task *t;
    GtkWidget *row; /* weak pointer: NULLed if the row is destroyed first */
} DueOpenReq;

static gboolean open_due_idle(gpointer ud) {
    DueOpenReq *r = ud;
    if (r->row) {
        open_due_popover(r->a, r->t, r->row);
        g_object_remove_weak_pointer(G_OBJECT(r->row), (gpointer *)&r->row);
    }
    g_free(r);
    return G_SOURCE_REMOVE;
}

/* ---- inline rename (double-click / right-click → Rename) ------------------ */

/* swap the inline editor back to the original row content */
static void rename_restore(App *a, GtkWidget *row) {
    g_object_set_data(G_OBJECT(row), "rename-pending", NULL);
    GtkWidget *e = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    if (!GTK_IS_ENTRY(e)) {
        g_object_set_data(G_OBJECT(row), "rename-box", NULL);
        g_object_set_data(G_OBJECT(row), "rename-task", NULL);
        return;
    }
    GtkWidget *old = g_object_get_data(G_OBJECT(row), "rename-box");
    Task *t = g_object_get_data(G_OBJECT(row), "rename-task");
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), old);
    if (t) refresh_row(a, t);
    if (old) g_object_unref(old); /* drop our keep-alive ref */
    g_object_set_data(G_OBJECT(row), "rename-box", NULL);
    g_object_set_data(G_OBJECT(row), "rename-task", NULL);
}

/* Enter / Escape / losing focus must NOT tear the editor down from inside the
   entry's own signal: destroying the focused entry mid-emission corrupts GTK's
   focus + accessibility bookkeeping, which surfaces as stray criticals, a
   runaway focus-fixup loop (100% CPU / window won't respond) or a segfault
   reading the freed entry buffer. Capture what we need now, finish in an idle. */
typedef struct {
    App *a;
    GtkWidget *row;  /* weak pointer: NULLed if the row is destroyed first */
    gchar *text;     /* non-NULL => commit this title (empty leaves it as-is) */
} RenameReq;

static gboolean rename_settle_idle(gpointer ud) {
    RenameReq *r = ud;
    if (r->row) {
        Task *t = g_object_get_data(G_OBJECT(r->row), "rename-task");
        if (t && r->text && *r->text) {
            g_free(t->title);
            t->title = g_strdup(r->text);
            save_store(r->a);
        }
        rename_restore(r->a, r->row);
        g_object_remove_weak_pointer(G_OBJECT(r->row), (gpointer *)&r->row);
    }
    g_free(r->text);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static void rename_settle(App *a, GtkWidget *row, gboolean commit) {
    if (!row) return;
    if (g_object_get_data(G_OBJECT(row), "rename-pending")) return;
    GtkWidget *e = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    if (!GTK_IS_ENTRY(e)) return;
    RenameReq *r = g_new0(RenameReq, 1);
    r->a = a;
    r->row = row;
    if (commit)
        r->text = g_strstrip(
            g_strdup(gtk_editable_get_text(GTK_EDITABLE(e))));
    g_object_set_data(G_OBJECT(row), "rename-pending", GINT_TO_POINTER(1));
    g_object_add_weak_pointer(G_OBJECT(row), (gpointer *)&r->row);
    /* the selftest drives handlers directly (not from a main loop), so it
       needs the work done inline */
    if (a->selftest)
        rename_settle_idle(r);
    else
        g_idle_add(rename_settle_idle, r);
}

static void start_rename(App *a, Task *t) {
    GtkWidget *row = t->row;
    if (!row) return;
    if (GTK_IS_ENTRY(gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row)))) return;
    GtkWidget *old = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), t->title);
    g_object_ref(old); /* keep the content alive while the editor is shown */
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), e);
    g_object_set_data(G_OBJECT(row), "rename-box", old);
    g_object_set_data(G_OBJECT(row), "rename-task", t);

    g_signal_connect(e, "activate", G_CALLBACK(rename_activated), a);
    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(rename_key), a);
    gtk_widget_add_controller(e, GTK_EVENT_CONTROLLER(k));
    GtkEventController *fc = gtk_event_controller_focus_new();
    g_signal_connect(fc, "leave", G_CALLBACK(rename_focus_left), a);
    gtk_widget_add_controller(e, GTK_EVENT_CONTROLLER(fc));

    gtk_widget_grab_focus(e);
    gtk_editable_select_region(GTK_EDITABLE(e), 0, -1);
}

static void rename_activated(GtkEntry *entry, gpointer ud) {
    App *a = ud;
    GtkWidget *row = gtk_widget_get_parent(GTK_WIDGET(entry));
    if (row) rename_settle(a, row, TRUE);
}

static gboolean rename_key(GtkEventControllerKey *c, guint k, guint kc,
                           GdkModifierType st, gpointer ud) {
    (void)kc; (void)st;
    App *a = ud;
    if (k != GDK_KEY_Escape) return FALSE;
    GtkWidget *e = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(c));
    GtkWidget *row = e ? gtk_widget_get_parent(e) : NULL;
    if (row) rename_settle(a, row, FALSE);
    return TRUE;
}

static void rename_focus_left(GtkEventControllerFocus *fc, gpointer ud) {
    (void)fc;
    App *a = ud;
    GtkWidget *e =
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(fc));
    GtkWidget *row = e ? gtk_widget_get_parent(e) : NULL;
    if (row) rename_settle(a, row, TRUE);
}

/* ---- row context menu --------------------------------------------------- */

/* Opening the inline editor right as the context menu pops down lets the
   popdown steal focus from the new entry, which fires its "leave" handler and
   commits the rename the user never got to type. Close the menu first and open
   the editor in the next idle, once the menu is really gone (same fix as the
   context-menu → Due date path). */
typedef struct {
    App *a;
    Task *t;
    GtkWidget *row; /* weak pointer: NULLed if the row is destroyed first */
} RenameOpenReq;

static gboolean start_rename_idle(gpointer ud) {
    RenameOpenReq *r = ud;
    if (r->row) {
        start_rename(r->a, r->t);
        g_object_remove_weak_pointer(G_OBJECT(r->row), (gpointer *)&r->row);
    }
    g_free(r);
    return G_SOURCE_REMOVE;
}

static void row_menu_rename(GtkButton *b, gpointer ud) {
    (void)b;
    Task *t = ud;
    close_all_popovers(t->app);
    if (!t->row) return;
    RenameOpenReq *r = g_new0(RenameOpenReq, 1);
    r->a = t->app;
    r->t = t;
    r->row = t->row;
    g_object_add_weak_pointer(G_OBJECT(t->row), (gpointer *)&r->row);
    g_idle_add(start_rename_idle, r);
}

static void row_menu_due(GtkButton *b, gpointer ud) {
    (void)b;
    Task *t = ud;
    GtkWidget *anchor = t->row;
    close_all_popovers(t->app);
    /* open in the next idle: the menu above is still mapped right now (see
       open_due_idle) and mapping over it would freeze input in the window */
    DueOpenReq *r = g_new0(DueOpenReq, 1);
    r->a = t->app;
    r->t = t;
    r->row = anchor;
    g_object_add_weak_pointer(G_OBJECT(anchor), (gpointer *)&r->row);
    g_idle_add(open_due_idle, r);
}

static void row_menu_delete(GtkButton *b, gpointer ud) {
    (void)b;
    App *a = ((Task *)ud)->app;
    delete_task(a, ud);
}

static void open_row_menu(App *a, Task *t, GtkWidget *anchor) {
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *b_rename = gtk_button_new_with_label("Rename");
    GtkWidget *b_due = gtk_button_new_with_label("Due date");
    GtkWidget *b_del = gtk_button_new_with_label("Delete");
    gtk_button_set_has_frame(GTK_BUTTON(b_rename), FALSE);
    gtk_button_set_has_frame(GTK_BUTTON(b_due), FALSE);
    gtk_button_set_has_frame(GTK_BUTTON(b_del), FALSE);
    gtk_widget_set_halign(b_rename, GTK_ALIGN_START);
    gtk_widget_set_halign(b_due, GTK_ALIGN_START);
    gtk_widget_set_halign(b_del, GTK_ALIGN_START);
    gtk_widget_add_css_class(b_del, "destructive-action");
    gtk_box_append(GTK_BOX(box), b_rename);
    gtk_box_append(GTK_BOX(box), b_due);
    gtk_box_append(GTK_BOX(box), b_del);
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    g_signal_connect(b_rename, "clicked", G_CALLBACK(row_menu_rename), t);
    g_signal_connect(b_due, "clicked", G_CALLBACK(row_menu_due), t);
    g_signal_connect(b_del, "clicked", G_CALLBACK(row_menu_delete), t);
    popover_show(a, pop, anchor);
}

/* ---- row widget --------------------------------------------------------- */

static void refresh_row(App *a, Task *t) {
    (void)a;
    GtkWidget *row = t->row;
    if (!row) return;
    GtkWidget *title = g_object_get_data(G_OBJECT(row), "title");
    GtkWidget *due = g_object_get_data(G_OBJECT(row), "due");
    GtkWidget *dot = g_object_get_data(G_OBJECT(row), "dot");

    if (title) {
        gtk_label_set_text(GTK_LABEL(title), t->title);
        gtk_widget_remove_css_class(title, "prio-high");
        gtk_widget_remove_css_class(title, "prio-med");
        gtk_widget_remove_css_class(title, "prio-low");
        gtk_widget_add_css_class(title, t->prio == P_HIGH   ? "prio-high"
                                        : t->prio == P_LOW  ? "prio-low"
                                                            : "prio-med");
    }
    if (dot) dot_set_color(dot, PRIO_COLORS[t->prio]);

    g_autofree gchar *td = today_iso();
    gboolean overdue = t->due && t->status != S_DONE && strcmp(t->due, td) < 0;
    if (t->due) {
        gtk_label_set_text(GTK_LABEL(due), t->due);
        gtk_widget_set_visible(due, TRUE);
        if (overdue) gtk_widget_add_css_class(due, "due-overdue");
        else gtk_widget_remove_css_class(due, "due-overdue");
    } else {
        gtk_widget_set_visible(due, FALSE);
    }

    GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    if (GTK_IS_BOX(box)) {
        if (t->status == S_DONE) gtk_widget_add_css_class(box, "task-done");
        else gtk_widget_remove_css_class(box, "task-done");
    }
}

/* priority dot cycles high → med → low (title colour follows) */
static void on_dot_clicked(GtkGestureClick *g, gint np, gdouble x, gdouble y,
                           gpointer ud) {
    (void)np; (void)x; (void)y;
    App *a = ud;
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    Task *t = g_object_get_data(G_OBJECT(widget), "task");
    if (!t) return;
    t->prio = (t->prio + 1) % 3;
    save_store(a);
    refresh_row(a, t);
}

static void on_row_right_click(GtkGestureClick *g, gint np, gdouble x, gdouble y,
                               gpointer ud) {
    (void)np; (void)x; (void)y;
    App *a = ud;
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    GtkWidget *row = gtk_widget_get_parent(widget);
    Task *t = g_object_get_data(G_OBJECT(row), "task");
    if (!t) return;
    gtk_list_box_select_row(GTK_LIST_BOX(gtk_widget_get_parent(row)), GTK_LIST_BOX_ROW(row));
    open_row_menu(a, t, row);
}

static void on_row_double_click(GtkGestureClick *g, gint np, gdouble x, gdouble y,
                                gpointer ud) {
    if (np != 2) return;
    (void)x; (void)y;
    App *a = ud;
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    GtkWidget *row = gtk_widget_get_parent(widget);
    Task *t = g_object_get_data(G_OBJECT(row), "task");
    if (!t) return;
    start_rename(a, t);
}

/* Clicking a card selects it AND moves keyboard focus to its listbox. Without
   the focus move, an inline add entry keeps focus after a task is added, so
   the window-level Delete handler treats the key as "typed into a field" and
   ignores it — the reason Done/other cards felt undeletable from the keyboard.
   Selecting also clears the other columns so "the selected card" is unambiguous. */
static void focus_listbox_row(App *a, GtkWidget *row) {
    if (!row || !GTK_IS_LIST_BOX_ROW(row)) return;
    GtkWidget *lb = gtk_widget_get_parent(row);
    if (!GTK_IS_LIST_BOX(lb)) return;
    for (gint i = 0; i < 3; i++)
        if (a->col[i]->listbox != lb)
            gtk_list_box_unselect_all(GTK_LIST_BOX(a->col[i]->listbox));
    gtk_list_box_select_row(GTK_LIST_BOX(lb), GTK_LIST_BOX_ROW(row));
    gtk_widget_grab_focus(lb);
}

static void on_row_press(GtkGestureClick *g, gint np, gdouble x, gdouble y,
                         gpointer ud) {
    (void)np; (void)x; (void)y;
    GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    focus_listbox_row(ud, gtk_widget_get_parent(box));
}

static GdkContentProvider *on_drag_prepare(GtkDragSource *src, gdouble x,
                                           gdouble y, gpointer ud) {
    (void)src; (void)x; (void)y;
    GValue v = G_VALUE_INIT;
    g_value_init(&v, G_TYPE_POINTER);
    g_value_set_pointer(&v, ud);
    GdkContentProvider *p = gdk_content_provider_new_for_value(&v);
    g_value_unset(&v);
    return p;
}

static GtkWidget *build_row(App *a, Task *t) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "task-row");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box, "task-box");
    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    GtkWidget *title = gtk_label_new(t->title);
    gtk_widget_add_css_class(title, "task-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    /* shrink rather than force the whole column (and so the window) wider than
       the tile the compositor gives us */
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    GtkWidget *due = gtk_label_new("");
    gtk_widget_add_css_class(due, "due-label");
    gtk_label_set_xalign(GTK_LABEL(due), 0.0);
    gtk_widget_set_halign(due, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), title);
    gtk_box_append(GTK_BOX(vbox), due);

    GtkWidget *dot = make_dot(PRIO_COLORS[t->prio]);
    g_object_set_data(G_OBJECT(dot), "task", t);
    GtkGesture *dg = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dg), GDK_BUTTON_PRIMARY);
    g_signal_connect(dg, "released", G_CALLBACK(on_dot_clicked), a);
    gtk_widget_add_controller(dot, GTK_EVENT_CONTROLLER(dg));

    gtk_box_append(GTK_BOX(box), vbox);
    gtk_box_append(GTK_BOX(box), dot);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    g_object_set_data(G_OBJECT(row), "task", t);
    g_object_set_data(G_OBJECT(row), "title", title);
    g_object_set_data(G_OBJECT(row), "due", due);
    g_object_set_data(G_OBJECT(row), "dot", dot);

    /* drag the whole row (the priority dot keeps its own click) */
    GtkDragSource *ds = gtk_drag_source_new();
    gtk_drag_source_set_actions(ds, GDK_ACTION_MOVE);
    g_signal_connect(ds, "prepare", G_CALLBACK(on_drag_prepare), t);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(ds));

    /* right-click menu */
    GtkGesture *rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    g_signal_connect(rc, "released", G_CALLBACK(on_row_right_click), a);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(rc));

    /* double-click rename */
    GtkGesture *dc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dc), GDK_BUTTON_PRIMARY);
    g_signal_connect(dc, "released", G_CALLBACK(on_row_double_click), a);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(dc));

    /* primary press selects the card and focuses its column (see on_row_press) */
    GtkGesture *sel = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(sel), GDK_BUTTON_PRIMARY);
    g_signal_connect(sel, "pressed", G_CALLBACK(on_row_press), a);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(sel));

    t->row = row;
    refresh_row(a, t);
    return row;
}

/* ---- status changes / drops ---------------------------------------------- */

static void settle_add_row(Column *col) {
    if (!col->add_row) return;
    const gchar *txt = gtk_editable_get_text(GTK_EDITABLE(col->add_entry));
    gboolean empty = TRUE;
    for (const gchar *p = txt; p && *p; p++)
        if (!g_ascii_isspace(*p)) { empty = FALSE; break; }
    if (empty)
        cancel_add_row(col);
    else
        commit_add(col, FALSE);
}

static void change_status(App *a, Task *t, gint dst) {
    if (t->status == dst) return;
    settle_add_row(a->col[dst]);
    gint src = t->status;
    t->status = dst;
    save_store(a);
    relocate_row(a, t, src, dst);
    update_counts(a);
}

static void move_task(App *a, Task *t, gint dst, gdouble x, gdouble y) {
    (void)x;
    settle_add_row(a->col[dst]);
    gint src = t->status;
    Column *dc = a->col[dst];
    GtkWidget *lb = dc->listbox;

    Task *before = NULL;
    GtkWidget *row_at =
        GTK_WIDGET(gtk_list_box_get_row_at_y(GTK_LIST_BOX(lb), (gint)y));
    if (row_at && row_at != t->row) {
        Task *nt = g_object_get_data(G_OBJECT(row_at), "task");
        if (nt) {
            graphene_point_t p0 = GRAPHENE_POINT_INIT(0, 0), pr;
            if (gtk_widget_compute_point(row_at, lb, &p0, &pr)) {
                gdouble mid = pr.y + gtk_widget_get_height(row_at) / 2.0;
                if (y > mid) nt = next_same_status(a, nt, dst);
                if (nt == t) nt = next_same_status(a, nt, dst);
            }
            before = (nt == t) ? NULL : nt;
        }
    }

    array_remove(a, t);
    gint idx;
    if (before) {
        gint bi = array_index_of(a, before);
        idx = (bi >= 0) ? bi : group_end(a, dst);
    } else {
        idx = group_end(a, dst);
    }
    array_insert_at(a, idx, t);
    t->status = dst;
    save_store(a);
    relocate_row(a, t, src, dst);
    update_counts(a);
}

static void relocate_row(App *a, Task *t, gint src, gint dst) {
    GtkWidget *row = t->row;
    if (!row) {
        rebuild_all(a);
        return;
    }
    Column *sc = a->col[src];
    Column *dc = a->col[dst];
    /* the listbox is the only ref-holder; keep the row alive across the move.
       Always remove first — even for a within-column reorder — because
       gtk_list_box_insert() rejects an already-parented child (and logs a
       critical that can wedge GTK's list/gesture state). */
    g_object_ref(row);
    gtk_list_box_remove(GTK_LIST_BOX(sc->listbox), row);
    if (!matches_query(a, t)) {
        t->row = NULL;
        g_object_unref(row);
        rebuild_all(a);
        return;
    }
    close_all_popovers(a);
    refresh_row(a, t);
    gint idx = visible_index_of(a, t, dst);
    gtk_list_box_insert(GTK_LIST_BOX(dc->listbox), row, idx);
    g_object_unref(row);
    update_counts(a);
}

/* ---- drop targets -------------------------------------------------------- */

static GdkDragAction on_drop_motion(GtkDropTarget *target, gdouble x,
                                    gdouble y, gpointer ud) {
    (void)target; (void)x; (void)y;
    Column *col = ud;
    if (!gtk_widget_has_css_class(col->listbox, "drop-target"))
        gtk_widget_add_css_class(col->listbox, "drop-target");
    return GDK_ACTION_MOVE;
}

static void on_drop_leave(GtkDropTarget *target, gpointer ud) {
    (void)target;
    Column *col = ud;
    gtk_widget_remove_css_class(col->listbox, "drop-target");
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, gdouble x,
                        gdouble y, gpointer ud) {
    (void)target;
    Column *col = ud;
    App *a = col->app;
    gtk_widget_remove_css_class(col->listbox, "drop-target");
    Task *t = g_value_get_pointer(value);
    if (!t || !find_task(&a->store, t->id)) return FALSE;
    move_task(a, t, col->status, x, y);
    return TRUE;
}

/* ---- inline add rows ------------------------------------------------------ */

static void create_add_row(Column *col, gint index);
static void cancel_add_row(Column *col) {
    if (!col->add_row) return;
    gtk_list_box_remove(GTK_LIST_BOX(col->listbox), col->add_row);
    col->add_row = NULL;
    col->add_entry = NULL;
}

static void commit_add(Column *col, gboolean recreate) {
    App *a = col->app;
    if (!col->add_row) return;
    const gchar *raw = gtk_editable_get_text(GTK_EDITABLE(col->add_entry));
    gchar *s = g_strstrip(g_strdup(raw ? raw : ""));
    if (!*s) {
        g_free(s);
        cancel_add_row(col);
        return;
    }
    char *title;
    gint prio;
    char *due;
    parse_quick(s, &title, &prio, &due);
    g_free(s);
    if (!*title) {
        g_free(title);
        g_free(due);
        cancel_add_row(col);
        return;
    }

    gint apos = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(col->add_row));
    /* map listbox position (add row excluded) to an array insertion point */
    gint arraypos = 0, vis = 0;
    for (guint i = 0; i < a->store.tasks->len; i++) {
        if (vis == apos) break;
        Task *t = g_array_index(a->store.tasks, Task *, i);
        if (t->status == col->status && t->row && matches_query(a, t)) vis++;
        arraypos++;
    }

    Task *t = task_new(a, title, prio, due, col->status);
    g_free(title);
    g_free(due);
    array_insert_at(a, arraypos, t);
    save_store(a);

    cancel_add_row(col);
    if (a->search_active) {
        clear_search(a, TRUE);
    } else {
        rebuild_column(col);
    }
    update_counts(a);
    if (recreate) {
        gint idx = apos;
        if (idx > visible_rows(col)) idx = visible_rows(col);
        create_add_row(col, idx);
    }
}

/* The inline add entry destroys itself when committed/cancelled. Doing that
   from inside the entry's own signal (activate / key / focus-leave) tears the
   widget down mid-emission and corrupts GTK's focus + accessibility state, so
   always defer the settle to an idle callback. */
static gboolean deferred_commit(gpointer ud) {
    Column *col = ud;
    col->add_settling = FALSE;
    commit_add(col, TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean deferred_cancel(gpointer ud) {
    Column *col = ud;
    col->add_settling = FALSE;
    cancel_add_row(col);
    return G_SOURCE_REMOVE;
}

static gboolean deferred_settle(gpointer ud) {
    Column *col = ud;
    col->add_settling = FALSE;
    settle_add_row(col);
    return G_SOURCE_REMOVE;
}

static void schedule_add(GSourceFunc fn, Column *col) {
    if (col->add_settling) return;
    col->add_settling = TRUE;
    g_idle_add(fn, col);
}

static void on_add_key(GtkEventControllerKey *c, guint k, guint kc,
                       GdkModifierType st, gpointer ud) {
    (void)c; (void)kc; (void)st;
    if (k == GDK_KEY_Escape) {
        Column *col = ud;
        schedule_add(deferred_cancel, col);
    }
}

static void on_add_activate(GtkEntry *entry, gpointer ud) {
    (void)entry;
    schedule_add(deferred_commit, (Column *)ud);
}

static void on_add_focus_left(GtkEventControllerFocus *fc, gpointer ud) {
    (void)fc;
    Column *col = ud;
    if (!col->add_row || !col->add_entry) return;
    schedule_add(deferred_settle, col);
}

static void create_add_row(Column *col, gint index) {
    if (col->add_row) return;
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *e = gtk_entry_new();
    gtk_widget_add_css_class(e, "add-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), "add a task…");
    gtk_widget_set_margin_top(e, 5);
    gtk_widget_set_margin_bottom(e, 5);
    gtk_widget_set_margin_start(e, 4);
    gtk_widget_set_margin_end(e, 4);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), e);
    gtk_list_box_insert(GTK_LIST_BOX(col->listbox), row, index);
    col->add_row = row;
    col->add_entry = e;

    g_signal_connect(e, "activate", G_CALLBACK(on_add_activate), col);
    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_add_key), col);
    gtk_widget_add_controller(e, GTK_EVENT_CONTROLLER(k));
    GtkEventController *fc = gtk_event_controller_focus_new();
    g_signal_connect(fc, "leave", G_CALLBACK(on_add_focus_left), col);
    gtk_widget_add_controller(e, GTK_EVENT_CONTROLLER(fc));

    gtk_widget_grab_focus(e);
}

/* click anywhere empty in a column → add box there */
static void on_col_press(GtkGestureClick *g, gint np, gdouble x, gdouble y,
                         gpointer ud) {
    (void)g; (void)np;
    Column *col = ud;
    col->press_x = x;
    col->press_y = y;
}

static gint add_index_for_y(Column *col, gdouble ly) {
    gint idx = 0;
    GtkWidget *w = gtk_widget_get_first_child(col->listbox);
    while (w) {
        if (GTK_IS_LIST_BOX_ROW(w)) {
            graphene_point_t p0 = GRAPHENE_POINT_INIT(0, 0), pr;
            if (gtk_widget_compute_point(w, col->listbox, &p0, &pr)) {
                if (pr.y + gtk_widget_get_height(w) > ly) return idx;
            }
            idx++;
        }
        w = gtk_widget_get_next_sibling(w);
    }
    return idx;
}

static void on_col_released(GtkGestureClick *g, gint np, gdouble x, gdouble y,
                            gpointer ud) {
    (void)g;
    if (np != 1) return;
    Column *col = ud;
    if (hypot(x - col->press_x, y - col->press_y) > 8.0) return;
    graphene_point_t pin = GRAPHENE_POINT_INIT(x, y), pout;
    if (gtk_widget_compute_point(col->scroller, col->listbox, &pin, &pout)) {
        gdouble ly = pout.y;
        GtkWidget *row_at = GTK_WIDGET(gtk_list_box_get_row_at_y(
            GTK_LIST_BOX(col->listbox), (gint)ly));
        if (row_at) return; /* clicked on a row */
        gint idx = add_index_for_y(col, ly);
        if (idx < 0) idx = visible_rows(col);
        create_add_row(col, idx);
    } else {
        create_add_row(col, visible_rows(col));
    }
}

/* ---- delete / clear done -------------------------------------------------- */

static void delete_task(App *a, Task *t) {
    close_all_popovers(a);
    {
        g_autofree gchar *label = g_strdup_printf("Delete \"%s\"", t->title);
        undo_push(a, label);
    }
    GtkWidget *row = t->row;
    GtkWidget *lb_widget = row ? gtk_widget_get_parent(row) : NULL;
    if (row && lb_widget) gtk_list_box_remove(GTK_LIST_BOX(lb_widget), row);
    array_remove(a, t);
    save_store(a);
    task_free(t);
    update_counts(a);
}

static void clear_done(App *a) {
    if (count_status(a, S_DONE) == 0) return;
    close_all_popovers(a);
    undo_push(a, "Clear completed");
    gint i = (gint)a->store.tasks->len - 1;
    while (i >= 0) {
        Task *t = g_array_index(a->store.tasks, Task *, (guint)i);
        if (t->status == S_DONE) {
            if (t->row) {
                GtkWidget *lb = gtk_widget_get_parent(t->row);
                if (lb) gtk_list_box_remove(GTK_LIST_BOX(lb), t->row);
            }
            array_remove(a, t);
            task_free(t);
        }
        i--;
    }
    save_store(a);
    update_counts(a);
}

static void on_clear_answered(GObject *obj, GAsyncResult *res, gpointer ud) {
    App *a = ud;
    GError *err = NULL;
    gint btn = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(obj), res, &err);
    if (err) {
        g_error_free(err);
        return;
    }
    if (btn == 1) clear_done(a);
}

static void confirm_clear(App *a) {
    if (!count_status(a, S_DONE)) return;
    GtkAlertDialog *d = gtk_alert_dialog_new(
        "Clear completed tasks? This removes everything in the Done column. "
        "Press Ctrl+Z (or the Undo button) to bring them back.");
    const char *buttons[] = { "Cancel", "Clear", NULL };
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_choose(d, GTK_WINDOW(a->win), NULL, on_clear_answered, a);
}

/* ---- counts / rebuild ---------------------------------------------------- */

static void update_col_counts(App *a, Column *col) {
    gint vis = 0;
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *t = g_array_index(a->store.tasks, Task *, i);
        if (t->status == col->status && t->row && matches_query(a, t)) vis++;
    }
    gchar buf[16];
    g_snprintf(buf, sizeof buf, "%d", vis);
    gtk_label_set_text(GTK_LABEL(col->count), buf);
    gtk_widget_set_visible(col->empty, vis == 0);
    gtk_label_set_text(GTK_LABEL(col->empty),
                       a->search_active ? "no matches" : "click to add");
}

static void update_counts(App *a) {
    for (gint i = 0; i < 3; i++) update_col_counts(a, a->col[i]);
}

static void clear_column_rows(Column *col) {
    GtkWidget *w;
    while ((w = gtk_widget_get_first_child(col->listbox)) != NULL) {
        if (GTK_IS_LIST_BOX_ROW(w)) {
            Task *t = g_object_get_data(G_OBJECT(w), "task");
            if (t) t->row = NULL;
        }
        gtk_list_box_remove(GTK_LIST_BOX(col->listbox), w);
    }
}

static void rebuild_column(Column *col) {
    App *a = col->app;
    if (col->add_row) cancel_add_row(col);
    clear_column_rows(col);
    gint vis = 0;
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *t = g_array_index(a->store.tasks, Task *, i);
        if (t->status == col->status && matches_query(a, t)) {
            GtkWidget *row = build_row(a, t);
            gtk_list_box_insert(GTK_LIST_BOX(col->listbox), row, vis++);
        }
    }
    update_col_counts(a, col);
}

static void rebuild_all(App *a) {
    for (gint i = 0; i < 3; i++)
        if (a->col[i]->add_row) cancel_add_row(a->col[i]);
    for (gint i = 0; i < 3; i++) rebuild_column(a->col[i]);
}

/* ---- search -------------------------------------------------------------- */

static void clear_search(App *a, gboolean do_rebuild) {
    if (!a->search_active) return;
    a->search_active = FALSE;
    if (a->search) {
        if (gtk_editable_get_text(GTK_EDITABLE(a->search))[0])
            gtk_editable_set_text(GTK_EDITABLE(a->search), "");
        gtk_widget_set_visible(a->search, FALSE);
    }
    if (do_rebuild) rebuild_all(a);
}

/* open / close the search bar (Ctrl+F or the search entry's own Esc) */
static void set_search(App *a, gboolean on) {
    if (on) {
        a->search_active = TRUE;
        gtk_widget_set_visible(a->search, TRUE);
        gtk_widget_grab_focus(a->search);
    } else {
        clear_search(a, TRUE);
    }
}

static void on_search_changed(GtkSearchEntry *e, gpointer ud) {
    (void)e;
    App *a = ud;
    rebuild_all(a);
}

static void on_search_stop(GtkSearchEntry *e, gpointer ud) {
    (void)e;
    App *a = ud;
    set_search(a, FALSE);
}

/* ---- keyboard ------------------------------------------------------------ */

/* Delete the selected card in whichever column has one. Routed through the
   window rather than a single listbox so it keeps working when focus sits
   outside the listbox (e.g. right after the inline add entry is dismissed,
   which is why Done cards felt undeletable from the keyboard). */
static gboolean delete_selected_task(App *a) {
    /* Prefer the column that holds keyboard focus, so the visibly-active card
       wins even if another column still carries a stale selection. */
    GtkWidget *fw = gtk_root_get_focus(GTK_ROOT(a->win));
    if (fw) {
        for (gint i = 0; i < 3; i++) {
            GtkWidget *lb = a->col[i]->listbox;
            if (fw != lb && !gtk_widget_is_ancestor(fw, lb)) continue;
            GtkWidget *row = GTK_WIDGET(
                gtk_list_box_get_selected_row(GTK_LIST_BOX(lb)));
            Task *t = row ? g_object_get_data(G_OBJECT(row), "task") : NULL;
            if (t) {
                delete_task(a, t);
                return TRUE;
            }
        }
    }
    for (gint i = 0; i < 3; i++) {
        GtkWidget *row = GTK_WIDGET(gtk_list_box_get_selected_row(
            GTK_LIST_BOX(a->col[i]->listbox)));
        if (!row) continue;
        Task *t = g_object_get_data(G_OBJECT(row), "task");
        if (t) {
            delete_task(a, t);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean on_window_key(GtkEventControllerKey *c, guint k, guint kc,
                              GdkModifierType st, gpointer ud) {
    (void)c; (void)kc;
    App *a = ud;
    if (k == GDK_KEY_F11) {
        if (gtk_window_is_fullscreen(GTK_WINDOW(a->win)))
            gtk_window_unfullscreen(GTK_WINDOW(a->win));
        else
            gtk_window_fullscreen(GTK_WINDOW(a->win));
        return TRUE;
    }
    if ((st & GDK_CONTROL_MASK) && k == GDK_KEY_z) {
        undo_perform(a);
        return TRUE;
    }
    if ((st & GDK_CONTROL_MASK) && k == GDK_KEY_n) {
        Column *todo = a->col[S_TODO];
        if (!todo->add_row) create_add_row(todo, 0);
        return TRUE;
    }
    if ((st & GDK_CONTROL_MASK) && (st & GDK_SHIFT_MASK) && k == GDK_KEY_d) {
        confirm_clear(a);
        return TRUE;
    }
    if ((st & GDK_CONTROL_MASK) && k == GDK_KEY_f) {
        set_search(a, !a->search_active);
        return TRUE;
    }
    if ((st & GDK_CONTROL_MASK) && k == GDK_KEY_d) {
        return delete_selected_task(a);
    }
    /* plain Delete/Backspace removes the selected card in any column, unless
       a text field (add/search/rename) has focus and should get the key */
    if ((k == GDK_KEY_Delete || k == GDK_KEY_BackSpace) && !(st & GDK_CONTROL_MASK)) {
        GtkWidget *fw = gtk_root_get_focus(GTK_ROOT(a->win));
        if (fw && GTK_IS_EDITABLE(fw)) return FALSE;
        return delete_selected_task(a);
    }
    return FALSE;
}

static void on_done_header_clear(GtkGestureClick *g, gint np, gdouble x, gdouble y,
                                 gpointer ud) {
    (void)g; (void)np; (void)x; (void)y;
    confirm_clear(ud);
}

/* ---- column / window construction ---------------------------------------- */

static void attach_column_controllers(App *a, Column *col) {
    (void)a;
    /* drop target: whole listbox is a target */
    GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_POINTER, GDK_ACTION_MOVE);
    g_signal_connect(dt, "motion", G_CALLBACK(on_drop_motion), col);
    g_signal_connect(dt, "leave", G_CALLBACK(on_drop_leave), col);
    g_signal_connect(dt, "drop", G_CALLBACK(on_drop), col);
    gtk_widget_add_controller(col->listbox, GTK_EVENT_CONTROLLER(dt));

    /* click anywhere empty in the column body -> add box there */
    GtkGesture *cg = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(cg), GDK_BUTTON_PRIMARY);
    g_signal_connect(cg, "pressed", G_CALLBACK(on_col_press), col);
    g_signal_connect(cg, "released", G_CALLBACK(on_col_released), col);
    gtk_widget_add_controller(col->scroller, GTK_EVENT_CONTROLLER(cg));
    /* card keyboard actions (Delete) are handled window-wide in on_window_key */
}

static Column *build_column(App *a, gint status) {
    Column *col = g_new0(Column, 1);
    col->app = a;
    col->status = status;

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(panel, "panel");
    gtk_widget_set_hexpand(panel, TRUE);
    gtk_widget_set_vexpand(panel, TRUE);
    gtk_widget_set_margin_top(panel, 2);
    gtk_widget_set_margin_bottom(panel, 2);
    gtk_widget_set_margin_start(panel, 4);
    gtk_widget_set_margin_end(panel, 4);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_margin_top(hbox, 10);
    gtk_widget_set_margin_bottom(hbox, 4);
    gtk_widget_set_margin_start(hbox, 12);
    gtk_widget_set_margin_end(hbox, 12);

    GtkWidget *dot = make_dot(COL_COLORS[status]);
    GtkWidget *tlabel = gtk_label_new(STATUS_TITLES[status]);
    gtk_widget_add_css_class(tlabel, "column-title");
    gtk_label_set_xalign(GTK_LABEL(tlabel), 0.0);
    gtk_widget_set_halign(tlabel, GTK_ALIGN_START);
    gtk_widget_set_hexpand(tlabel, TRUE);
    GtkWidget *count = gtk_label_new("0");
    gtk_widget_add_css_class(count, "badge");
    gtk_widget_add_css_class(count, BADGE_CLASS[status]);
    gtk_box_append(GTK_BOX(hbox), dot);
    gtk_box_append(GTK_BOX(hbox), tlabel);
    gtk_box_append(GTK_BOX(hbox), count);

    if (status == S_DONE) {
        GtkGesture *cg = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(cg),
                                      GDK_BUTTON_SECONDARY);
        g_signal_connect(cg, "pressed", G_CALLBACK(on_done_header_clear), a);
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(cg));
        gtk_widget_set_tooltip_text(hbox, "Right-click to clear completed");
    }

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(listbox, "row-list");
    gtk_widget_set_vexpand(listbox, TRUE);
    /* focusable so clicking a card can take keyboard focus and Delete works */
    gtk_widget_set_focusable(listbox, TRUE);

    GtkWidget *empty = gtk_label_new("click to add");
    gtk_widget_add_css_class(empty, "hint-label");
    /* Keep the listbox always mapped, so its drop target stays alive even
       when the column is empty; overlay the hint instead of swapping the
       listbox out via a GtkStack (which hid the drop target). */
    gtk_widget_set_can_target(empty, FALSE);
    gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(empty, FALSE);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), listbox);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), empty);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), overlay);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);

    gtk_box_append(GTK_BOX(panel), hbox);
    gtk_box_append(GTK_BOX(panel), scroller);

    col->panel = panel;
    col->scroller = scroller;
    col->listbox = listbox;
    col->empty = empty;
    col->count = count;

    attach_column_controllers(a, col);
    return col;
}

/* ---- rotating shortcut ticker -------------------------------------------- */

/* Shown one at a time at the bottom, loading-screen style, so the board stays
   uncluttered. Keep each tip self-contained: it must read well on its own. */
static const char *HINTS[] = {
    "Ctrl + N   new task at the top of To Do",
    "Ctrl + F   search across all columns",
    "Ctrl + Z   undo the last delete or clear",
    "Ctrl + Shift + D   clear all completed tasks",
    "Delete  —  remove the card you clicked",
    "Drag a card between columns to move or complete it",
    "Right-click a card for rename, due date, priority, delete",
    "Click the coloured dot to cycle priority: high / med / low",
    "F11  fullscreen",
};
#define HINT_COUNT (gint)(sizeof(HINTS) / sizeof(HINTS[0]))

/* Advance the ticker one step. Split out from the timer callback so tests can
   drive it deterministically. */
static void hint_show_next(App *a) {
    if (!a->hint_stack) return;
    GtkWidget *target = a->hint_flip ? a->hint_a : a->hint_b;
    gtk_label_set_text(GTK_LABEL(target), HINTS[a->hint_idx]);
    gtk_stack_set_visible_child(GTK_STACK(a->hint_stack), target);
    a->hint_flip = !a->hint_flip;
    a->hint_idx = (a->hint_idx + 1) % (gint)G_N_ELEMENTS(HINTS);
}

static GtkWidget *make_hint_label(const gchar *text) {
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_add_css_class(lbl, "hint-label");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(lbl, 10);
    gtk_widget_set_margin_end(lbl, 10);
    gtk_widget_set_margin_bottom(lbl, 8);
    return lbl;
}

/* Rotate only while the window is focused, so a background board isn't
   animating forever. */
static gboolean hint_tick(gpointer ud) {
    App *a = ud;
    if (a->win && GTK_IS_WINDOW(a->win) &&
        gtk_window_is_active(GTK_WINDOW(a->win)))
        hint_show_next(a);
    return G_SOURCE_CONTINUE;
}

static void build_window(App *a) {
    theme_apply(a);
    GtkWidget *win = gtk_application_window_new(a->gapp);
    a->win = win;
    gtk_window_set_title(GTK_WINDOW(win), "tasks");
    gtk_window_set_default_size(GTK_WINDOW(win), 980, 620);
    /* keep the floor low enough that Hyprland can still tile us into a small
       slot; a large minimum made the client render wider than its surface */
    gtk_widget_set_size_request(GTK_WIDGET(win), 480, 320);

    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hb), TRUE);
    GtkWidget *appname = gtk_label_new("TASKS");
    gtk_widget_add_css_class(appname, "app-title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), appname);
    gtk_window_set_titlebar(GTK_WINDOW(win), hb);

    GtkWidget *search = gtk_search_entry_new();
    a->search = search;
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search),
                                          "Search all columns…");
    gtk_widget_set_visible(search, FALSE);
    gtk_widget_set_margin_top(search, 6);
    gtk_widget_set_margin_start(search, 10);
    gtk_widget_set_margin_end(search, 10);
    g_signal_connect(search, "search-changed", G_CALLBACK(on_search_changed), a);
    g_signal_connect(search, "stop-search", G_CALLBACK(on_search_stop), a);

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand(main, TRUE);
    gtk_widget_set_vexpand(main, TRUE);
    gtk_widget_set_margin_top(main, 6);
    gtk_widget_set_margin_bottom(main, 10);
    gtk_widget_set_margin_start(main, 8);
    gtk_widget_set_margin_end(main, 8);
    for (gint i = 0; i < 3; i++) {
        Column *col = build_column(a, i);
        a->col[i] = col;
        gtk_box_append(GTK_BOX(main), col->panel);
    }

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(root), search);
    gtk_box_append(GTK_BOX(root), main);

    /* rotating shortcut ticker: one quiet tip, crossfading to the next, so the
       bottom edge stays calm instead of listing every key at once. */
    GtkWidget *hints = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(hints),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(hints), 450);
    gtk_widget_add_css_class(hints, "hints");
    a->hint_a = make_hint_label(HINTS[0]);
    a->hint_b = make_hint_label("");
    gtk_stack_add_child(GTK_STACK(hints), a->hint_a);
    gtk_stack_add_child(GTK_STACK(hints), a->hint_b);
    gtk_stack_set_visible_child(GTK_STACK(hints), a->hint_a);
    a->hint_stack = hints;
    a->hint_idx = 1;          /* HINTS[0] already shown */
    a->hint_flip = FALSE;     /* next tip goes to the hidden label, hint_b */
    gtk_widget_set_margin_bottom(hints, 6);
    gtk_box_append(GTK_BOX(root), hints);
    a->hint_timer = g_timeout_add_seconds(6, hint_tick, a);

    gtk_window_set_child(GTK_WINDOW(win), root);

    GtkEventController *wk = gtk_event_controller_key_new();
    g_signal_connect(wk, "key-pressed", G_CALLBACK(on_window_key), a);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(wk));
}

/* ---- selftest ------------------------------------------------------------ */

/* Count GDK's "grabbing popup" warnings so the context-menu → Due date path
   can be asserted to open cleanly (the old code mapped the due popover while
   the menu still held the grab, leaving the window frozen). */
static gint selftest_popup_warnings = 0;
static void selftest_log_handler(const gchar *domain, GLogLevelFlags level,
                                 const gchar *message, gpointer ud) {
    (void)domain; (void)level; (void)ud;
    if (message && strstr(message, "grabbing popup"))
        selftest_popup_warnings++;
}

static void selftest_run(App *a) {
    g_print("--- tasks-gui selftest ---\n");
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        gboolean b = (cond);                                                  \
        if (!b) a->selftest_fail++;                                           \
        g_print("%s %s\n", b ? "PASS" : "FAIL", name);                        \
    } while (0)

    /* flush pending layout */
    for (gint i = 0; i < 40; i++) {
        while (g_main_context_iteration(NULL, FALSE))
            ;
    }

    /* 1. add in To Do via the inline add row path */
    Column *todo = a->col[S_TODO];
    create_add_row(todo, 0);
    CHECK(todo->add_row != NULL, "add row created");
    gtk_editable_set_text(GTK_EDITABLE(todo->add_entry), "!selftest tomorrow");
    commit_add(todo, FALSE);
    CHECK(a->store.tasks->len == 1, "store: 1 task");
    Task *t1 = g_array_index(a->store.tasks, Task *, 0);
    CHECK(t1->prio == P_HIGH, "priority high");
    CHECK(t1->status == S_TODO, "status todo");
    CHECK(t1->due != NULL, "due tomorrow");
    CHECK(t1->row != NULL, "row built");
    CHECK(count_status(a, S_TODO) == 1, "todo count 1");

    /* 2. drag to In Progress (empty target -> appended) */
    move_task(a, t1, S_PROG, 5, 100000);
    CHECK(t1->status == S_PROG, "drag to progress");
    CHECK(t1->row != NULL, "row survives move");
    CHECK(count_status(a, S_PROG) == 1, "progress count 1");
    CHECK(count_status(a, S_TODO) == 0, "todo count 0");

    /* 3. move -> Done */
    change_status(a, t1, S_DONE);
    CHECK(t1->status == S_DONE, "move to done");
    CHECK(gtk_widget_get_parent(t1->row) == a->col[S_DONE]->listbox,
          "done row reparented");

    /* 4. move back -> Todo */
    change_status(a, t1, S_TODO);
    CHECK(t1->status == S_TODO, "move back to todo");

    /* 5. a second task in progress, then drop t1 above it (reorder) */
    Column *prog = a->col[S_PROG];
    create_add_row(prog, 0);
    gtk_editable_set_text(GTK_EDITABLE(prog->add_entry), "second task");
    commit_add(prog, FALSE);
    Task *t2 = NULL;
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *x = g_array_index(a->store.tasks, Task *, i);
        if (x->status == S_PROG) { t2 = x; break; }
    }
    CHECK(t2 != NULL && t2 != t1, "second task in progress");

    /* wait until the window has actually been allocated (Wayland mapping can
       lag a few frames) so the reorder geometry check is meaningful */
    gint h = 0;
    for (gint i = 0; i < 80 && h == 0; i++) {
        for (gint j = 0; j < 10; j++)
            while (g_main_context_iteration(NULL, FALSE))
                ;
        if (h == 0) g_usleep(10000); /* let the compositor map us */
        h = gtk_widget_get_height(t2->row);
    }
    if (h > 0) {
        move_task(a, t1, S_PROG, 5, 2); /* very top of t2's row */
        gint i1 = array_index_of(a, t1);
        gint i2 = array_index_of(a, t2);
        CHECK(i1 >= 0 && i2 >= 0 && i1 < i2, "dropped above sibling");
    } else {
        g_print("SKIP reorder (rows not realised, h=%d mapped=%d lbh=%d)\n",
                h, gtk_widget_get_mapped(a->win),
                gtk_widget_get_height(a->col[S_PROG]->listbox));
    }

    /* 6. search filters */
    GtkSearchEntry *se = a->search ? GTK_SEARCH_ENTRY(a->search) : NULL;
    if (se) {
        a->search_active = TRUE;
        gtk_widget_set_visible(a->search, TRUE);
        gtk_editable_set_text(GTK_EDITABLE(se), t2->title);
        rebuild_all(a);
        gint vis = 0;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *t = g_array_index(a->store.tasks, Task *, i);
            if (t->row && matches_query(a, t)) vis++;
        }
        CHECK(vis == 1, "search shows one row");
        /* case folding must not be ASCII-only */
        Task ut;
        memset(&ut, 0, sizeof ut);
        ut.title = (char *)"Caf\xc3\xa9 \xc3\x80\xc3\x89\xc3\x8e";
        gtk_editable_set_text(GTK_EDITABLE(se), "caf\xc3\xa9");
        CHECK(matches_query(a, &ut), "search casefolds accented titles");
        gtk_editable_set_text(GTK_EDITABLE(se), "CAF\xc3\x89");
        CHECK(matches_query(a, &ut), "search matches uppercase accented text");
        clear_search(a, TRUE);
        CHECK(!a->search_active, "search cleared");
    }

    /* 7. rename via inline editor */
    start_rename(a, t2);
    GtkWidget *ren = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(t2->row));
    CHECK(GTK_IS_ENTRY(ren), "rename editor open");
    gtk_editable_set_text(GTK_EDITABLE(ren), "renamed task");
    rename_activated(GTK_ENTRY(ren), a);
    CHECK(g_strcmp0(t2->title, "renamed task") == 0, "rename committed");

    /* 8. due apply + display, then clear */
    apply_due(a, t2, "2026-01-01");
    CHECK(t2->due && g_strcmp0(t2->due, "2026-01-01") == 0, "due applied");
    GtkWidget *dlabel = g_object_get_data(G_OBJECT(t2->row), "due");
    CHECK(dlabel && gtk_widget_get_visible(dlabel), "due shown");
    apply_due(a, t2, NULL);
    CHECK(t2->due == NULL, "due cleared");

    /* 9. delete */
    delete_task(a, t2);
    CHECK(a->store.tasks->len == 1, "store has one after delete");
    gint vis = 0;
    for (guint i = 0; i < a->store.tasks->len; i++)
        if (g_array_index(a->store.tasks, Task *, i)->row) vis++;
    CHECK(vis == 1, "one visible row left");

    /* 10. persistence round trip */
    json_error_t je;
    json_t *r = json_load_file(a->data_path, 0, &je);
    if (r) {
        CHECK(json_array_size(json_object_get(r, "tasks")) == 1,
              "json has one task");
        json_t *t = json_array_get(json_object_get(r, "tasks"), 0);
        const char *st = json_string_value(json_object_get(t, "status"));
        const char *expect = STATUS_KEYS[t1->status];
        CHECK(st && g_strcmp0(st, expect) == 0, "json status persisted");
        CHECK(json_is_false(json_object_get(t, "done")), "json done synced");
        json_decref(r);
    } else {
        CHECK(FALSE, "json reloadable");
    }
    /* 11. widget-level interactions (the paths a real click/drag triggers) */

    /* 11a. a card moved to Done carries done=true and a priority title colour */
    Column *td = a->col[S_TODO];
    create_add_row(td, 0);
    gtk_editable_set_text(GTK_EDITABLE(td->add_entry), "done card");
    commit_add(td, FALSE);
    Task *tc = NULL;
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *x = g_array_index(a->store.tasks, Task *, i);
        if (g_strcmp0(x->title, "done card") == 0) tc = x;
    }
    CHECK(tc != NULL, "done-card task created");
    if (tc) {
        change_status(a, tc, S_DONE);
        CHECK(tc->status == S_DONE, "move to done -> status done");
        CHECK(gtk_widget_get_parent(tc->row) == a->col[S_DONE]->listbox,
              "done row reparented to Done");
        CHECK(gtk_widget_has_css_class(
                  g_object_get_data(G_OBJECT(tc->row), "title"),
                  tc->prio == P_HIGH   ? "prio-high"
                  : tc->prio == P_LOW  ? "prio-low"
                                       : "prio-med"),
              "title carries a priority colour class");
    }

    /* 11b. drop handler -> To Do card lands in In Progress */
    Column *tp = a->col[S_TODO];
    create_add_row(tp, 0);
    gtk_editable_set_text(GTK_EDITABLE(tp->add_entry), "drag me");
    commit_add(tp, FALSE);
    Task *tdrag = NULL;
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *x = g_array_index(a->store.tasks, Task *, i);
        if (g_strcmp0(x->title, "drag me") == 0) tdrag = x;
    }
    CHECK(tdrag != NULL, "drag task created");
    if (tdrag) {
        GValue v = G_VALUE_INIT;
        g_value_init(&v, G_TYPE_POINTER);
        g_value_set_pointer(&v, tdrag);
        on_drop(NULL, &v, 5, 100000, a->col[S_PROG]);
        g_value_unset(&v);
        CHECK(tdrag->status == S_PROG, "drop handler -> in progress");
        CHECK(gtk_widget_get_parent(tdrag->row) ==
                  a->col[S_PROG]->listbox,
              "dropped row reparented to In Progress");
    }

    /* 11c. typing + Enter adds exactly one task (no duplicate rows) */
    Column *tn = a->col[S_TODO];
    create_add_row(tn, 0);
    gtk_editable_set_text(GTK_EDITABLE(tn->add_entry), "dupecheck");
    on_add_activate(GTK_ENTRY(tn->add_entry), tn);
    /* the commit is deferred to an idle; pump until it lands (or give up) */
    for (gint i = 0; i < 200; i++) {
        while (g_main_context_iteration(NULL, FALSE))
            ;
        gint seen = 0;
        for (guint j = 0; j < a->store.tasks->len; j++)
            if (g_strcmp0(g_array_index(a->store.tasks, Task *, j)->title,
                          "dupecheck") == 0) seen++;
        if (seen > 0) break;
        g_usleep(1000);
    }
    gint ndup = 0;
    for (guint i = 0; i < a->store.tasks->len; i++)
        if (g_strcmp0(g_array_index(a->store.tasks, Task *, i)->title,
                      "dupecheck") == 0)
            ndup++;
    CHECK(ndup == 1, "Enter adds exactly one task");

    /* 11d. dragging a card into Done marks it completed (persisted) */
    Column *tf = a->col[S_TODO];
    create_add_row(tf, 0);
    gtk_editable_set_text(GTK_EDITABLE(tf->add_entry), "finish me");
    commit_add(tf, FALSE);
    Task *tfin = NULL;
    for (guint i = 0; i < a->store.tasks->len; i++) {
        Task *x = g_array_index(a->store.tasks, Task *, i);
        if (g_strcmp0(x->title, "finish me") == 0) tfin = x;
    }
    CHECK(tfin != NULL, "done-drop task created");
    if (tfin) {
        GValue v2 = G_VALUE_INIT;
        g_value_init(&v2, G_TYPE_POINTER);
        g_value_set_pointer(&v2, tfin);
        on_drop(NULL, &v2, 5, 100000, a->col[S_DONE]);
        g_value_unset(&v2);
        CHECK(tfin->status == S_DONE, "drop -> done");
        CHECK(gtk_widget_get_parent(tfin->row) == a->col[S_DONE]->listbox,
              "done drop reparents the row");
        json_error_t je2;
        json_t *r2 = json_load_file(a->data_path, 0, &je2);
        if (r2) {
            json_t *jt;
            size_t idx;
            json_array_foreach (json_object_get(r2, "tasks"), idx, jt) {
                if (json_integer_value(json_object_get(jt, "id")) == tfin->id) {
                    CHECK(json_is_true(json_object_get(jt, "done")),
                          "dropped-to-done persists done=true");
                    CHECK(g_strcmp0(
                              json_string_value(json_object_get(jt, "status")),
                              "done") == 0, "dropped-to-done status=done");
                }
            }
            json_decref(r2);
        }
    }

    /* 11e. row context menu → Due date must not fight over the popup grab.
       The menu and the due popover are both grabbing popups; opening the due
       one while the menu is still mapped made GDK refuse the grab and froze
       all input in the window. The open is now deferred to the next idle. */
    if (tc && tc->row && gtk_widget_get_mapped(a->win)) {
        selftest_popup_warnings = 0;
        g_log_set_default_handler(selftest_log_handler, NULL);
        open_row_menu(a, tc, tc->row);
        if (a->popovers->len) {
            GtkWidget *mpop =
                g_ptr_array_index(a->popovers, a->popovers->len - 1);
            GtkWidget *mchild = gtk_popover_get_child(GTK_POPOVER(mpop));
            GtkWidget *bdue =
                gtk_widget_get_next_sibling(gtk_widget_get_first_child(mchild));
            g_signal_emit_by_name(bdue, "clicked");
        }
        for (gint i = 0; i < 40; i++)
            while (g_main_context_iteration(NULL, FALSE))
                ;
        CHECK(selftest_popup_warnings == 0,
              "due popover opens without a grabbing-popup warning");
        CHECK(a->popovers->len == 1, "due popover is the only popup open");
        /* Exercising the popover's Set callback must neither crash nor lose
           the date. The callback runs as signal(widget, user_data); getting
           that signature wrong read the widget as the context and segfaulted
           on every Set / Enter. */
        if (a->popovers->len == 1) {
            GtkWidget *dpop = g_ptr_array_index(a->popovers, 0);
            GtkWidget *dbox = gtk_popover_get_child(GTK_POPOVER(dpop));
            GtkWidget *dentry =
                gtk_widget_get_next_sibling(gtk_widget_get_first_child(dbox));
            gtk_editable_set_text(GTK_EDITABLE(dentry), "2026-03-03");
            g_signal_emit_by_name(dentry, "activate");
            for (gint i = 0; i < 40; i++)
                while (g_main_context_iteration(NULL, FALSE))
                    ;
            CHECK(g_strcmp0(tc->due, "2026-03-03") == 0,
                  "due committed from popover entry");
        }
        close_all_popovers(a);
        for (gint i = 0; i < 40; i++)
            while (g_main_context_iteration(NULL, FALSE))
                ;
        CHECK(a->popovers->len == 0, "popovers closed after due menu");
    } else {
        g_print("SKIP due-popover grab check (window not mapped)\n");
    }

    /* 12. pure parsing: quick-add markers and due tokens */
    {
        g_autofree gchar *today = today_iso();
        CHECK(g_strcmp0(parse_due_token("today"), today) == 0,
              "parse_due_token today");
        CHECK(g_strcmp0(parse_due_token("TODAY"), today) == 0,
              "parse_due_token TODAY (case-insensitive)");
        g_autofree gchar *tom = parse_due_token("tomorrow");
        CHECK(tom != NULL && g_strcmp0(tom, today) != 0,
              "parse_due_token tomorrow");
        CHECK(parse_due_token("off") == NULL, "parse_due_token off clears");
        CHECK(parse_due_token("none") == NULL, "parse_due_token none clears");
        CHECK(parse_due_token("-") == NULL, "parse_due_token dash clears");
        CHECK(parse_due_token("") == NULL, "parse_due_token empty clears");
        CHECK(parse_due_token("garbage") == NULL,
              "parse_due_token garbage rejected");
        CHECK(parse_due_token("2026-13-01") == NULL,
              "parse_due_token month 13 rejected");
        CHECK(parse_due_token("2026-02-31") == NULL,
              "parse_due_token feb 31 rejected");
        CHECK(parse_due_token("2026-01-05x") == NULL,
              "parse_due_token trailing junk rejected");
        g_autofree gchar *norm = parse_due_token("2026-1-5");
        CHECK(norm != NULL && g_strcmp0(norm, "2026-01-05") == 0,
              "parse_due_token normalises YYYY-M-D");
        CHECK(valid_date("2024-02-29"), "valid_date accepts leap day");
        CHECK(!valid_date("2026-02-29"), "valid_date rejects non-leap day");

        char *title = NULL; gint pp = -1; char *dd = NULL;
        parse_quick("!ship v1 tomorrow", &title, &pp, &dd);
        CHECK(pp == P_HIGH && g_strcmp0(title, "ship v1") == 0 && dd != NULL,
              "parse_quick: !priority + due token stripped");
        g_free(title); g_free(dd);
        title = NULL; pp = -1; dd = NULL;
        parse_quick(".low task", &title, &pp, &dd);
        CHECK(pp == P_LOW && g_strcmp0(title, "low task") == 0 && dd == NULL,
              "parse_quick: .priority marker stripped");
        g_free(title); g_free(dd);
        title = NULL; pp = -1; dd = NULL;
        parse_quick("plain title", &title, &pp, &dd);
        CHECK(pp == P_MED && g_strcmp0(title, "plain title") == 0 && dd == NULL,
              "parse_quick: defaults to medium, keeps title");
        g_free(title); g_free(dd);
    }

    /* 13. clicking the priority dot cycles high→med→low and repaints */
    {
        Column *tc2 = a->col[S_TODO];
        create_add_row(tc2, 0);
        gtk_editable_set_text(GTK_EDITABLE(tc2->add_entry), "cycle me");
        commit_add(tc2, FALSE);
        Task *tcy = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "cycle me") == 0) tcy = x;
        }
        CHECK(tcy != NULL, "cycle task created");
        if (tcy) {
            GtkWidget *dot = g_object_get_data(G_OBJECT(tcy->row), "dot");
            GtkGesture *gg = gtk_gesture_click_new();
            gtk_widget_add_controller(dot, GTK_EVENT_CONTROLLER(gg));
            gint before = tcy->prio;
            on_dot_clicked(GTK_GESTURE_CLICK(gg), 1, 0, 0, a);
            CHECK(tcy->prio == (before + 1) % 3, "dot click cycles priority");
            on_dot_clicked(GTK_GESTURE_CLICK(gg), 1, 0, 0, a);
            CHECK(tcy->prio == (before + 2) % 3, "dot click cycles again");
            on_dot_clicked(GTK_GESTURE_CLICK(gg), 1, 0, 0, a);
            CHECK(tcy->prio == before, "dot click wraps around");
            GtkWidget *tl = g_object_get_data(G_OBJECT(tcy->row), "title");
            const gchar *cls = tcy->prio == P_HIGH   ? "prio-high"
                               : tcy->prio == P_LOW  ? "prio-low"
                                                     : "prio-med";
            CHECK(gtk_widget_has_css_class(tl, cls),
                  "title colour follows priority");
            delete_task(a, tcy);
        }
    }

    /* 14. list keyboard: Delete removes in every column; Return does nothing */
    {
        /* (a) Return must NOT move a card (completion is drag-only) */
        Column *tl = a->col[S_TODO];
        create_add_row(tl, 0);
        gtk_editable_set_text(GTK_EDITABLE(tl->add_entry), "kbd item");
        commit_add(tl, FALSE);
        Task *tk = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "kbd item") == 0) tk = x;
        }
        CHECK(tk != NULL, "keyboard task created");
        if (tk) {
            gtk_list_box_select_row(GTK_LIST_BOX(tl->listbox),
                                    GTK_LIST_BOX_ROW(tk->row));
            gboolean h = on_window_key(NULL, GDK_KEY_Return, 0, 0, a);
            CHECK(!h && tk->status == S_TODO,
                  "Return does not move a card");
        }

        /* (b) a card created directly in Done: Delete removes it */
        Column *tdn = a->col[S_DONE];
        create_add_row(tdn, 0);
        gtk_editable_set_text(GTK_EDITABLE(tdn->add_entry), "kbd done");
        commit_add(tdn, FALSE);
        Task *tr = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "kbd done") == 0) tr = x;
        }
        CHECK(tr != NULL && tr->status == S_DONE, "done-column add lands done");
        if (tr) {
            gtk_list_box_select_row(GTK_LIST_BOX(tdn->listbox),
                                    GTK_LIST_BOX_ROW(tr->row));
            gtk_window_set_focus(GTK_WINDOW(a->win), tdn->listbox);
            guint before = a->store.tasks->len;
            gboolean h = on_window_key(NULL, GDK_KEY_Delete, 0, 0, a);
            CHECK(h && a->store.tasks->len == before - 1,
                  "Delete removes a card in Done");
        }

        /* (c) a fresh To Do card: Delete removes it */
        create_add_row(tl, 0);
        gtk_editable_set_text(GTK_EDITABLE(tl->add_entry), "kbd del");
        commit_add(tl, FALSE);
        Task *tkd = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "kbd del") == 0) tkd = x;
        }
        CHECK(tkd != NULL, "delete-target task created");
        if (tkd) {
            gtk_list_box_select_row(GTK_LIST_BOX(tl->listbox),
                                    GTK_LIST_BOX_ROW(tkd->row));
            gtk_window_set_focus(GTK_WINDOW(a->win), tl->listbox);
            guint before = a->store.tasks->len;
            gboolean h = on_window_key(NULL, GDK_KEY_Delete, 0, 0, a);
            CHECK(h && a->store.tasks->len == before - 1,
                  "Delete removes selected task");
        }

        /* (d) clicking a card moves focus off the inline add entry and leaves
           only that card selected, so Delete removes the clicked card rather
           than a stale selection in another column (the reported bug). */
        Column *tp = a->col[S_PROG];
        create_add_row(tl, 0);
        gtk_editable_set_text(GTK_EDITABLE(tl->add_entry), "focus todo");
        commit_add(tl, FALSE);
        create_add_row(tp, 0);
        gtk_editable_set_text(GTK_EDITABLE(tp->add_entry), "focus prog");
        commit_add(tp, FALSE);
        Task *ftodo = NULL, *fprog = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "focus todo") == 0) ftodo = x;
            if (g_strcmp0(x->title, "focus prog") == 0) fprog = x;
        }
        CHECK(ftodo && fprog, "focus-delete targets created");
        if (ftodo && fprog) {
            gtk_list_box_select_row(GTK_LIST_BOX(tl->listbox),
                                    GTK_LIST_BOX_ROW(ftodo->row));
            focus_listbox_row(a, fprog->row);   /* as if the user clicked it */
            CHECK(gtk_list_box_get_selected_row(GTK_LIST_BOX(tl->listbox)) == NULL,
                  "clicking a card clears the other column's selection");
            guint before = a->store.tasks->len;
            gboolean h = on_window_key(NULL, GDK_KEY_Delete, 0, 0, a);
            gboolean kept_todo = FALSE;
            for (guint i = 0; i < a->store.tasks->len; i++) {
                Task *x = g_array_index(a->store.tasks, Task *, i);
                if (g_strcmp0(x->title, "focus todo") == 0) kept_todo = TRUE;
            }
            CHECK(h && a->store.tasks->len == before - 1 && kept_todo,
                  "Delete removes the clicked card, not a stale selection");
            delete_task(a, ftodo);              /* clean up remaining card */
        }
    }

    /* 15. window keyboard shortcuts */
    {
        if (a->col[S_TODO]->add_row) cancel_add_row(a->col[S_TODO]);
        gboolean h = on_window_key(NULL, GDK_KEY_n, 0, GDK_CONTROL_MASK, a);
        CHECK(h && a->col[S_TODO]->add_row != NULL, "Ctrl+N opens add row");
        cancel_add_row(a->col[S_TODO]);
        h = on_window_key(NULL, GDK_KEY_f, 0, GDK_CONTROL_MASK, a);
        CHECK(h && a->search_active, "Ctrl+F opens search");
        h = on_window_key(NULL, GDK_KEY_f, 0, GDK_CONTROL_MASK, a);
        CHECK(h && !a->search_active, "Ctrl+F closes search");

        Column *tx = a->col[S_TODO];
        create_add_row(tx, 0);
        gtk_editable_set_text(GTK_EDITABLE(tx->add_entry), "ctl-d item");
        commit_add(tx, FALSE);
        Task *tcd = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "ctl-d item") == 0) tcd = x;
        }
        CHECK(tcd != NULL, "Ctrl+D target created");
        if (tcd) {
            gtk_list_box_select_row(GTK_LIST_BOX(tx->listbox),
                                    GTK_LIST_BOX_ROW(tcd->row));
            guint before = a->store.tasks->len;
            h = on_window_key(NULL, GDK_KEY_d, 0, GDK_CONTROL_MASK, a);
            CHECK(h && a->store.tasks->len == before - 1,
                  "Ctrl+D deletes selected task");
        }
    }

    /* 15b. rotating shortcut ticker */
    {
        CHECK(a->hint_stack != NULL, "ticker stack built");
        if (a->hint_stack) {
            CHECK(gtk_stack_get_visible_child(GTK_STACK(a->hint_stack)) ==
                      a->hint_a &&
                  g_strcmp0(gtk_label_get_text(GTK_LABEL(a->hint_a)),
                            HINTS[0]) == 0,
                  "ticker starts on the first tip");
            hint_show_next(a);
            CHECK(gtk_stack_get_visible_child(GTK_STACK(a->hint_stack)) ==
                      a->hint_b &&
                  g_strcmp0(gtk_label_get_text(GTK_LABEL(a->hint_b)),
                            HINTS[1]) == 0,
                  "ticker crossfades to the next tip");
            hint_show_next(a);
            CHECK(gtk_stack_get_visible_child(GTK_STACK(a->hint_stack)) ==
                      a->hint_a &&
                  g_strcmp0(gtk_label_get_text(GTK_LABEL(a->hint_a)),
                            HINTS[2]) == 0,
                  "ticker alternates labels");
            /* one full lap shows every tip (bitmask over the tip list) */
            gint n = (gint)G_N_ELEMENTS(HINTS);
            guint seen = 0;
            for (gint i = 0; i < n; i++) {
                hint_show_next(a);
                GtkWidget *v =
                    gtk_stack_get_visible_child(GTK_STACK(a->hint_stack));
                const gchar *txt = gtk_label_get_text(GTK_LABEL(v));
                for (gint k = 0; k < n; k++)
                    if (g_strcmp0(txt, HINTS[k]) == 0) seen |= (1u << k);
            }
            CHECK(seen == ((1u << n) - 1), "ticker cycles through every tip");
            CHECK(a->hint_timer != 0, "ticker timer is armed");
        }
    }

    /* 16. blank / marker-only quick adds are ignored */
    {
        Column *tb = a->col[S_TODO];
        guint before = a->store.tasks->len;
        create_add_row(tb, 0);
        gtk_editable_set_text(GTK_EDITABLE(tb->add_entry), "   ");
        commit_add(tb, FALSE);
        CHECK(a->store.tasks->len == before && tb->add_row == NULL,
              "whitespace-only add ignored");
        create_add_row(tb, 0);
        gtk_editable_set_text(GTK_EDITABLE(tb->add_entry), "!");
        commit_add(tb, FALSE);
        CHECK(a->store.tasks->len == before && tb->add_row == NULL,
              "priority-marker-only add ignored");
    }

    /* 17. unicode + very long titles survive a round trip */
    {
        const char *uni = "café — naïve ✓ 日本語 😀";
        Column *tu = a->col[S_TODO];
        create_add_row(tu, 0);
        gtk_editable_set_text(GTK_EDITABLE(tu->add_entry), uni);
        commit_add(tu, FALSE);
        Task *tuni = NULL, *tbig = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, uni) == 0) tuni = x;
        }
        CHECK(tuni != NULL, "unicode title stored");
        g_autofree gchar *big = g_strnfill(1000, 'x');
        create_add_row(tu, 0);
        gtk_editable_set_text(GTK_EDITABLE(tu->add_entry), big);
        commit_add(tu, FALSE);
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, big) == 0) tbig = x;
        }
        CHECK(tbig != NULL && strlen(tbig->title) == 1000,
              "1000-character title stored intact");
        if (tuni) delete_task(a, tuni);
        if (tbig) delete_task(a, tbig);
    }

    /* 18. a past due date shows the overdue colour; a done task clears it */
    {
        Column *to = a->col[S_TODO];
        create_add_row(to, 0);
        gtk_editable_set_text(GTK_EDITABLE(to->add_entry), "overdue item");
        commit_add(to, FALSE);
        Task *tov = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "overdue item") == 0) tov = x;
        }
        CHECK(tov != NULL, "overdue task created");
        if (tov) {
            apply_due(a, tov, "2000-01-01");
            GtkWidget *due = g_object_get_data(G_OBJECT(tov->row), "due");
            CHECK(due && gtk_widget_has_css_class(due, "due-overdue"),
                  "past due -> overdue class");
            change_status(a, tov, S_DONE);
            GtkWidget *due2 = g_object_get_data(G_OBJECT(tov->row), "due");
            CHECK(due2 && !gtk_widget_has_css_class(due2, "due-overdue"),
                  "done task clears overdue class");
            delete_task(a, tov);
        }
    }

    /* 19. clear_done removes completed tasks and persists */
    {
        Column *td = a->col[S_TODO];
        create_add_row(td, 0);
        gtk_editable_set_text(GTK_EDITABLE(td->add_entry), "clear me");
        commit_add(td, FALSE);
        Task *tcl = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "clear me") == 0) tcl = x;
        }
        if (tcl) change_status(a, tcl, S_DONE);
        guint before = a->store.tasks->len;
        gint nd = count_status(a, S_DONE);
        CHECK(nd >= 1, "done task present before clear");
        clear_done(a);
        CHECK(count_status(a, S_DONE) == 0, "clear_done empties Done");
        CHECK(a->store.tasks->len == before - (guint)nd,
              "clear_done removes exactly the done tasks");
    }

    /* 20. every persisted task carries the CLI/GUI contract fields */
    {
        json_error_t jc;
        json_t *rj = json_load_file(a->data_path, 0, &jc);
        CHECK(rj != NULL, "store serialises to json");
        if (rj) {
            CHECK(json_is_integer(json_object_get(rj, "next_id")),
                  "json next_id is an integer");
            json_t *arr = json_object_get(rj, "tasks");
            CHECK(json_is_array(arr), "json tasks is an array");
            gboolean ok = json_is_array(arr);
            size_t i;
            json_t *o;
            json_array_foreach(arr, i, o) {
                if (!json_is_integer(json_object_get(o, "id"))) ok = FALSE;
                if (!json_is_string(json_object_get(o, "title"))) ok = FALSE;
                if (!json_is_boolean(json_object_get(o, "done"))) ok = FALSE;
                const char *st = json_string_value(json_object_get(o, "status"));
                if (!st || (strcmp(st, "todo") && strcmp(st, "progress") &&
                            strcmp(st, "done"))) ok = FALSE;
                const char *pr = json_string_value(json_object_get(o, "priority"));
                if (!pr || (strcmp(pr, "high") && strcmp(pr, "med") &&
                            strcmp(pr, "low"))) ok = FALSE;
                json_t *du = json_object_get(o, "due");
                if (!json_is_null(du) && !json_is_string(du)) ok = FALSE;
                if (!json_is_string(json_object_get(o, "created"))) ok = FALSE;
            }
            CHECK(ok, "task contract: id/title/done/status/priority/due/created");
            json_decref(rj);
        }
    }

    /* 20b. storage safety: atomic save, id collisions, corrupt backup */
    {
        save_store(a);
        g_autofree gchar *sdir = g_path_get_dirname(a->data_path);
        g_autofree gchar *sbase = g_path_get_basename(a->data_path);
        g_autofree gchar *spref = g_strconcat(sbase, ".tmp", NULL);
        GDir *sd = g_dir_open(sdir, 0, NULL);
        gboolean leftover = FALSE;
        const gchar *sname;
        while (sd && (sname = g_dir_read_name(sd)))
            if (g_str_has_prefix(sname, spref)) leftover = TRUE;
        if (sd) g_dir_close(sd);
        CHECK(!leftover, "atomic save leaves no temp file behind");

        if (a->store.tasks->len > 0) {
            gint taken = g_array_index(a->store.tasks, Task *, 0)->id;
            gint saved = a->store.next_id;
            a->store.next_id = taken;      /* pretend the counter went stale */
            gint got = alloc_id(a);
            CHECK(got != taken, "alloc_id skips an id already in use");
            a->store.next_id = saved;
        }

        g_autofree gchar *cdir = g_dir_make_tmp("tasks-corrupt-XXXXXX", NULL);
        g_autofree gchar *cfile = g_build_filename(cdir, "tasks.json", NULL);
        g_file_set_contents(cfile, "{ broken", -1, NULL);
        gchar *saved_path = a->data_path;
        a->data_path = g_strdup(cfile);
        backup_corrupt(a);
        g_autofree gchar *cbase = g_path_get_basename(cfile);
        g_autofree gchar *cpat = g_strdup_printf("%s.corrupt-*", cbase);
        GPatternSpec *ps = g_pattern_spec_new(cpat);
        gboolean found = FALSE;
        GDir *cd = g_dir_open(cdir, 0, NULL);
        while (cd && (sname = g_dir_read_name(cd)))
            if (g_pattern_spec_match_string(ps, sname)) found = TRUE;
        if (cd) g_dir_close(cd);
        g_pattern_spec_free(ps);
        CHECK(found && !g_file_test(cfile, G_FILE_TEST_EXISTS),
              "corrupt store is quarantined, not overwritten");
        g_free(a->data_path);
        a->data_path = saved_path;
    }

    /* 21. inline rename: Esc discards, focus-leave commits */
    {
        Column *trn = a->col[S_TODO];
        create_add_row(trn, 0);
        gtk_editable_set_text(GTK_EDITABLE(trn->add_entry), "rename me");
        commit_add(trn, FALSE);
        Task *tr = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "rename me") == 0) tr = x;
        }
        CHECK(tr != NULL, "rename-target created");
        if (tr) {
            start_rename(a, tr);
            GtkWidget *e =
                gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(tr->row));
            CHECK(GTK_IS_ENTRY(e), "rename editor opens");
            gtk_editable_set_text(GTK_EDITABLE(e), "discard this");
            GtkEventController *kc = gtk_event_controller_key_new();
            gtk_widget_add_controller(e, kc);
            gboolean h = rename_key(GTK_EVENT_CONTROLLER_KEY(kc), GDK_KEY_Escape,
                                    0, 0, a);
            CHECK(h && g_strcmp0(tr->title, "rename me") == 0,
                  "Esc discards the rename");

            start_rename(a, tr);
            e = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(tr->row));
            gtk_editable_set_text(GTK_EDITABLE(e), "committed name");
            GtkEventController *fc = gtk_event_controller_focus_new();
            gtk_widget_add_controller(e, fc);
            rename_focus_left(GTK_EVENT_CONTROLLER_FOCUS(fc), a);
            CHECK(g_strcmp0(tr->title, "committed name") == 0,
                  "focus-leave commits the rename");
            delete_task(a, tr);
        }
    }

    /* 22. row context menu: Rename and Delete items */
    {
        Column *tm = a->col[S_TODO];
        create_add_row(tm, 0);
        gtk_editable_set_text(GTK_EDITABLE(tm->add_entry), "menu item");
        commit_add(tm, FALSE);
        Task *tmi = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "menu item") == 0) tmi = x;
        }
        CHECK(tmi != NULL, "menu-target created");
        if (tmi) {
            open_row_menu(a, tmi, tmi->row);
            CHECK(a->popovers->len == 1, "row menu opened");
            GtkWidget *pop = g_ptr_array_index(a->popovers,
                                               a->popovers->len - 1);
            GtkWidget *box = gtk_popover_get_child(GTK_POPOVER(pop));
            GtkWidget *bren = gtk_widget_get_first_child(box);
            g_signal_emit_by_name(bren, "clicked");
            for (gint i = 0; i < 40; i++)
                while (g_main_context_iteration(NULL, FALSE))
                    ;
            GtkWidget *e =
                gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(tmi->row));
            CHECK(GTK_IS_ENTRY(e), "menu Rename opens the editor");
            GtkEventController *kc = gtk_event_controller_key_new();
            gtk_widget_add_controller(e, kc);
            rename_key(GTK_EVENT_CONTROLLER_KEY(kc), GDK_KEY_Escape, 0, 0, a);

            open_row_menu(a, tmi, tmi->row);
            pop = g_ptr_array_index(a->popovers, a->popovers->len - 1);
            box = gtk_popover_get_child(GTK_POPOVER(pop));
            GtkWidget *bdel = gtk_widget_get_last_child(box);
            guint before = a->store.tasks->len;
            g_signal_emit_by_name(bdel, "clicked");
            CHECK(a->store.tasks->len == before - 1,
                  "menu Delete removes the task");
        }
    }

    /* 23. config.ini parsing: defaults, [general], bare keys */
    {
        gboolean saved_follow = a->follow_theme, saved_notify = a->notify_due;
        g_autofree gchar *cdir = g_dir_make_tmp("tasks-cfg-XXXXXX", NULL);
        g_autofree gchar *cp = g_build_filename(cdir, "config.ini", NULL);
        g_autofree gchar *cnone = g_build_filename(cdir, "absent.ini", NULL);
        config_load(a, cnone);
        CHECK(a->follow_theme && a->notify_due,
              "config: defaults when file absent");
        g_file_set_contents(cp,
            "[general]\nfollow_theme = false\nnotify_due = false\n", -1, NULL);
        config_load(a, cp);
        CHECK(!a->follow_theme && !a->notify_due,
              "config: [general] booleans read");
        g_file_set_contents(cp, "follow_theme = true\nnotify_due = false\n",
                            -1, NULL);
        config_load(a, cp);
        CHECK(a->follow_theme && !a->notify_due,
              "config: section-less file still works");
        gchar *saved_font = g_strdup(a->font_family);
        gint saved_size = a->font_size;
        config_load(a, cp);
        CHECK(g_strcmp0(a->font_family, "JetBrainsMono Nerd Font") == 0 &&
              a->font_size == 13, "config: font defaults (Omarchy mono)");
        g_file_set_contents(cp,
            "[general]\nfont = Fira Code\nfont_size = 16\n", -1, NULL);
        config_load(a, cp);
        CHECK(g_strcmp0(a->font_family, "Fira Code") == 0 && a->font_size == 16,
              "config: font family and size read");
        g_file_set_contents(cp, "[general]\nfont_size = 999\n", -1, NULL);
        config_load(a, cp);
        CHECK(a->font_size == 13, "config: out-of-range font size ignored");
        g_free(a->font_family);
        a->font_family = saved_font;
        a->font_size = saved_size;
        a->follow_theme = saved_follow;
        a->notify_due = saved_notify;
        g_remove(cp);
        g_remove(cnone);
        g_rmdir(cdir);
    }

    /* 24. Omarchy colors.toml parsing + generated stylesheet */
    {
        g_autofree gchar *tdir = g_dir_make_tmp("tasks-theme-XXXXXX", NULL);
        g_autofree gchar *tp = g_build_filename(tdir, "colors.toml", NULL);
        g_autofree gchar *tmiss = g_build_filename(tdir, "missing.toml", NULL);
        const gchar *tomo =
            "mode = \"dark\"\n"
            "background = \"#101418\"\n"
            "foreground = \"#e6e6e6\"\n"
            "accent = \"#7aa2f7\"\n"
            "muted = \"#565f89\"\n"
            "red = \"#f7768e\"\n"
            "green = \"#9ece6a\"\n"
            "yellow = \"#e0af68\"\n";
        g_file_set_contents(tp, tomo, -1, NULL);
        Theme th;
        memset(&th, 0, sizeof th);
        theme_parse(&th, tp);
        CHECK(th.ok, "theme: colors.toml parsed");
        CHECK(g_strcmp0(th.background, "#101418") == 0, "theme: background");
        CHECK(g_strcmp0(th.foreground, "#e6e6e6") == 0, "theme: foreground");
        CHECK(g_strcmp0(th.accent, "#7aa2f7") == 0, "theme: accent");
        CHECK(g_strcmp0(th.red, "#f7768e") == 0, "theme: red");
        CHECK(!th.light, "theme: dark mode detected");
        g_autofree gchar *tcss = css_build(&th, "JetBrainsMono Nerd Font", 13);
        CHECK(strstr(tcss, "#e6e6e6") != NULL, "theme CSS: uses foreground");
        CHECK(strstr(tcss, "rgba(16, 20, 24, 0.72)") != NULL,
              "theme CSS: translucent window background");
        CHECK(strstr(tcss, "#7aa2f7") != NULL, "theme CSS: accent colour");
        CHECK(strstr(tcss, "#f7768e") != NULL,
              "theme CSS: high-priority red");
        g_autofree gchar *fcss = css_build(&th, "Fira Code", 20);
        CHECK(strstr(fcss, "\"Fira Code\"") != NULL,
              "theme CSS: custom font family applied");
        CHECK(strstr(fcss, "font-size: 20px") != NULL,
              "theme CSS: custom base font size applied");
        CHECK(strstr(fcss, "font-size: 21px") != NULL,
              "theme CSS: task titles scale with base");
        /* Regression: surfaces must blend bg->fg, never be derived from a
           light foreground, or a white-fg theme (e.g. Vantablack) yields
           white panels and white task text becomes invisible. */
        CHECK(strstr(tcss, "rgba(30, 34, 38, 0.52)") != NULL,
              "theme CSS: panel blended from background");
        Theme twhite;
        memset(&twhite, 0, sizeof twhite);
        twhite.background = g_strdup("#000000");
        twhite.foreground = g_strdup("#ffffff");
        twhite.ok = TRUE;
        g_autofree gchar *tcss2 = css_build(&twhite, "JetBrainsMono Nerd Font", 13);
        CHECK(strstr(tcss2, "rgba(17, 17, 17, 0.52)") != NULL,
              "theme CSS: dark panel when foreground is white");
        CHECK(strstr(tcss2, "rgba(255, 255, 255, 0.52)") == NULL,
              "theme CSS: panel never derived from a white foreground");
        theme_clear(&twhite);

        /* apply the fixture palette for real, then put the live app theme back */
#if GTK_CHECK_VERSION(4, 12, 0)
        gboolean saved_follow = a->follow_theme;
        Theme saved_theme = a->theme;
        a->theme = th;                /* steal the parsed fixture */
        a->follow_theme = TRUE;
        theme_apply(a);
        gchar *applied =
            g_css_provider ? gtk_css_provider_to_string(g_css_provider) : NULL;
        CHECK(applied && strstr(applied, "rgb(230,230,230)") != NULL,
              "theme: palette installed into the live stylesheet");
        g_free(applied);
        th = a->theme;               /* give it back so we can free it */
        theme_clear(&th);
        a->theme = saved_theme;
        a->follow_theme = saved_follow;
        theme_apply(a);              /* restore the real stylesheet */
#else
        theme_clear(&th);
#endif

        theme_parse(&th, tmiss);
        CHECK(!th.ok, "theme: missing file falls back to built-in");
        theme_clear(&th);
        g_remove(tp);
        g_remove(tmiss);
        g_rmdir(tdir);
    }

    /* 25. undo: Delete and Clear done restore the store */
    {
        undo_clear(a);
        Column *tu = a->col[S_TODO];
        create_add_row(tu, 0);
        gtk_editable_set_text(GTK_EDITABLE(tu->add_entry), "undo-delete me");
        commit_add(tu, FALSE);
        Task *ut = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "undo-delete me") == 0) ut = x;
        }
        CHECK(ut != NULL, "undo: delete target created");
        gint64 uid = ut ? ut->id : 0;
        guint before = a->store.tasks->len;
        delete_task(a, ut);
        CHECK(a->store.tasks->len == before - 1, "undo: delete removed task");
        CHECK(a->undo && a->undo->len == 1,
              "undo: snapshot queued after delete");
        CHECK(undo_perform(a), "undo: delete restored");
        CHECK(a->store.tasks->len == before, "undo: task count restored");
        Task *uback = find_task(&a->store, uid);
        CHECK(uback != NULL && g_strcmp0(uback->title, "undo-delete me") == 0,
              "undo: deleted task is back");
        CHECK(!a->undo || a->undo->len == 0,
              "undo: stack empty after restore");
        CHECK(!undo_perform(a), "undo: no-op on empty stack");
        if (uback) delete_task(a, uback);

        undo_clear(a);
        create_add_row(a->col[S_DONE], 0);
        gtk_editable_set_text(GTK_EDITABLE(a->col[S_DONE]->add_entry),
                              "undo-clear me");
        commit_add(a->col[S_DONE], FALSE);
        Task *ct = NULL;
        for (guint i = 0; i < a->store.tasks->len; i++) {
            Task *x = g_array_index(a->store.tasks, Task *, i);
            if (g_strcmp0(x->title, "undo-clear me") == 0) ct = x;
        }
        CHECK(ct != NULL && ct->status == S_DONE, "undo: done task created");
        gint64 cid = ct ? ct->id : 0;
        gint dones_before = count_status(a, S_DONE);
        clear_done(a);
        CHECK(count_status(a, S_DONE) == 0, "undo: clear-done emptied column");
        CHECK(undo_perform(a), "undo: clear-done restored");
        CHECK(count_status(a, S_DONE) == dones_before,
              "undo: done column restored");
        Task *cback = find_task(&a->store, cid);
        CHECK(cback != NULL && cback->status == S_DONE,
              "undo: cleared task is back");
        if (cback) delete_task(a, cback);
        undo_clear(a);
    }

    /* 26. due notifications: selection, body, once-a-day suppression */
    {
        undo_clear(a);
        for (gint c = 0; c < 3; c++) clear_column_rows(a->col[c]);
        for (guint i = 0; i < a->store.tasks->len; i++)
            task_free(g_array_index(a->store.tasks, Task *, i));
        g_array_set_size(a->store.tasks, 0);

        g_autofree gchar *today = today_iso();
        Task *d1 = task_new(a, "zeta overdue", P_MED, "2000-01-01", S_TODO);
        Task *d2 = task_new(a, "alpha today", P_MED, today, S_TODO);
        Task *d3 = task_new(a, "omega future", P_MED, "2999-12-31", S_TODO);
        Task *d4 = task_new(a, "done overdue", P_MED, "2000-01-01", S_DONE);
        g_array_append_val(a->store.tasks, d1);
        g_array_append_val(a->store.tasks, d2);
        g_array_append_val(a->store.tasks, d3);
        g_array_append_val(a->store.tasks, d4);
        rebuild_all(a);

        GPtrArray *due = g_ptr_array_new();
        due_tasks(a, due);
        CHECK(due->len == 2, "due: only open tasks due today/overdue");
        g_ptr_array_free(due, TRUE);

        gint dn = 0;
        g_autofree gchar *dbody = due_body(a, &dn);
        CHECK(dn == 2, "due: summary count is 2");
        CHECK(strstr(dbody, "zeta overdue") && strstr(dbody, "alpha today") &&
                  !strstr(dbody, "omega future") &&
                  !strstr(dbody, "done overdue"),
              "due: body lists the open overdue/due-today tasks");

        g_free(a->last_due_notify);
        a->last_due_notify = NULL;
        gboolean saved_notify = a->notify_due;
        a->notify_due = TRUE;
        CHECK(!notify_due(a), "due: notification suppressed in selftest");
        CHECK(g_strcmp0(a->last_due_notify, today) == 0,
              "due: remembers the day it notified");
        a->notify_due = saved_notify;

        for (gint c = 0; c < 3; c++) clear_column_rows(a->col[c]);
        for (guint i = 0; i < a->store.tasks->len; i++)
            task_free(g_array_index(a->store.tasks, Task *, i));
        g_array_set_size(a->store.tasks, 0);
        rebuild_all(a);
        undo_clear(a);
    }

    g_print("--- %s (%d failures) ---\n",
            a->selftest_fail ? "FAILURES" : "selftest OK", a->selftest_fail);
#undef CHECK
}

static gboolean selftest_timer(gpointer ud) {
    App *a = ud;
    selftest_run(a);
    g_application_quit(G_APPLICATION(a->gapp));
    return G_SOURCE_REMOVE;
}

/* Load-mode selftest: points at a fixture tasks.json produced elsewhere (the
   CLI / the Python interop tests) and asserts the GUI read it correctly.
   This is the counterpart to selftest_run, which writes its own store. */
static gboolean selftest_load_timer(gpointer ud) {
    App *a = ud;
    g_print("--- tasks-gui load selftest ---\n");
    gint fails = 0;
#define LCHECK(cond, name)                                                    \
    do {                                                                      \
        gboolean b = (cond);                                                  \
        if (!b) fails++;                                                      \
        g_print("%s %s\n", b ? "PASS" : "FAIL", name);                        \
    } while (0)

    for (gint i = 0; i < 40; i++)
        while (g_main_context_iteration(NULL, FALSE))
            ;

    LCHECK(a->store.tasks->len == 4, "loaded 4 tasks");
    LCHECK(a->store.next_id == 5, "next_id restored from json");
    if (a->store.tasks->len == 4) {
        Task *t0 = g_array_index(a->store.tasks, Task *, 0);
        Task *t1 = g_array_index(a->store.tasks, Task *, 1);
        Task *t2 = g_array_index(a->store.tasks, Task *, 2);
        Task *t3 = g_array_index(a->store.tasks, Task *, 3);
        LCHECK(t0->id == 1 && g_strcmp0(t0->title, "cligh") == 0 &&
                  t0->status == S_TODO && t0->prio == P_HIGH &&
                  g_strcmp0(t0->due, "2026-01-01") == 0 &&
                  g_strcmp0(t0->created, "2026-01-01") == 0,
              "task 1: all fields loaded");
        LCHECK(t1->id == 2 && t1->status == S_PROG && t1->prio == P_LOW &&
                  t1->due == NULL,
              "task 2: progress + null due loaded");
        LCHECK(t2->id == 3 && t2->status == S_DONE && t2->prio == P_MED,
              "task 3: done loaded");
        LCHECK(t3->id == 4 && t3->status == S_DONE && t3->prio == P_MED &&
                  t3->due == NULL && g_strcmp0(t3->created, "2026-01-04") == 0,
              "task 4: legacy done-only normalised");
        LCHECK(count_status(a, S_TODO) == 1 && count_status(a, S_PROG) == 1 &&
                  count_status(a, S_DONE) == 2,
              "column counts match loaded store");
        LCHECK(t0->row && t1->row && t2->row && t3->row,
              "rows built for every loaded task");
        LCHECK(gtk_widget_get_parent(t0->row) == a->col[S_TODO]->listbox &&
                  gtk_widget_get_parent(t1->row) == a->col[S_PROG]->listbox &&
                  gtk_widget_get_parent(t2->row) == a->col[S_DONE]->listbox &&
                  gtk_widget_get_parent(t3->row) == a->col[S_DONE]->listbox,
              "rows parented to the right columns");
    }

    /* resave and reload: the contract fields survive a GUI round trip */
    save_store(a);
    json_error_t je;
    json_t *r = json_load_file(a->data_path, 0, &je);
    LCHECK(r != NULL, "resaved store reloads");
    if (r) {
        LCHECK(json_array_size(json_object_get(r, "tasks")) == 4,
               "resave keeps all 4 tasks");
        json_decref(r);
    }

    g_print("--- %s (%d failures) ---\n",
            fails ? "FAILURES" : "load selftest OK", fails);
    a->selftest_fail += fails;
    g_application_quit(G_APPLICATION(a->gapp));
#undef LCHECK
    return G_SOURCE_REMOVE;
}

#ifdef TASKS_GEOM
/* Debug-only: dump window-relative geometry of every column + row so an
 * external tool can aim synthetic pointer events. Not built into releases. */
static gboolean debug_geom_timer(gpointer ud) {
    App *a = ud;
    graphene_rect_t r;
    g_print("STORE n=%u search=%d\n", a->store.tasks->len, a->search_active);
    g_print("WINDOW %g %g\n", (double)gtk_widget_get_width(a->win),
            (double)gtk_widget_get_height(a->win));
    for (gint i = 0; i < 3; i++) {
        Column *col = a->col[i];
        if (gtk_widget_compute_bounds(col->panel, a->win, &r))
            g_print("COLUMN %d %.1f %.1f %.1f %.1f\n", i,
                    (double)r.origin.x, (double)r.origin.y,
                    (double)r.size.width, (double)r.size.height);
        gint nrows = 0;
        GtkWidget *w = gtk_widget_get_first_child(col->listbox);
        while (w) { if (GTK_IS_LIST_BOX_ROW(w)) nrows++; w = gtk_widget_get_next_sibling(w); }
        g_print("COLCOUNT %d rows=%d\n", i, nrows);
        w = gtk_widget_get_first_child(col->listbox);
        while (w) {
            if (GTK_IS_LIST_BOX_ROW(w)) {
                Task *t = g_object_get_data(G_OBJECT(w), "task");
                if (t && gtk_widget_compute_bounds(w, a->win, &r))
                    g_print("ROW task=%lld status=%d x=%.1f y=%.1f w=%.1f h=%.1f\n",
                            (long long)t->id, t->status,
                            (double)r.origin.x, (double)r.origin.y,
                            (double)r.size.width, (double)r.size.height);
            }
            w = gtk_widget_get_next_sibling(w);
        }
    }
    g_print("GEOMEND\n");
    return G_SOURCE_CONTINUE;
}
#endif

/* ---- app bootstrap -------------------------------------------------------- */

static void on_activate(GApplication *gapp, gpointer ud) {
    (void)gapp;
    App *a = ud;
    if (a->win) {
        gtk_window_present(GTK_WINDOW(a->win));
        return;
    }
    if (getenv("TASKS_SELFTEST") || getenv("TASKS_SELFTEST_LOAD"))
        a->selftest = TRUE;
    a->config_path = g_build_filename(g_get_user_config_dir(),
                                      "omarchy-tasks", "config.ini", NULL);
    config_load(a, a->config_path);
    a->theme_path = g_build_filename(g_get_user_state_dir(), "omarchy",
                                     "current", "theme", "colors.toml", NULL);
    theme_parse(&a->theme, a->theme_path);
    a->data_path = g_build_filename(g_get_user_data_dir(), "tasks",
                                    "tasks.json", NULL);
    a->store.tasks = g_array_new(FALSE, FALSE, sizeof(Task *));
    load_store(a);
    a->popovers = g_ptr_array_new();
    a->undo = g_ptr_array_new_with_free_func(undo_state_free);
    build_window(a);
    rebuild_all(a);
    theme_watch(a);
    gtk_window_present(GTK_WINDOW(a->win));
    if (a->notify_due) {
        notify_due(a);
        g_timeout_add_seconds(900, notify_due_timer, a); /* re-check 15m */
    }
    if (a->selftest) {
        if (getenv("TASKS_SELFTEST_LOAD"))
            g_timeout_add(250, selftest_load_timer, a);
        else
            g_timeout_add(250, selftest_timer, a);
    }
#ifdef TASKS_GEOM
    g_timeout_add(500, debug_geom_timer, a);
#endif
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--version") == 0 ||
            g_strcmp0(argv[i], "-v") == 0) {
            g_print("tasks-gui %s\n", VERSION);
            return 0;
        }
    }
    App a;
    memset(&a, 0, sizeof a);
    a.gapp = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(a.gapp, "activate", G_CALLBACK(on_activate), &a);
    gint status = g_application_run(G_APPLICATION(a.gapp), argc, argv);
    if (a.hint_timer) g_source_remove(a.hint_timer);
    g_object_unref(a.gapp);
    store_free(&a.store);
    if (a.popovers) g_ptr_array_free(a.popovers, TRUE);
    if (a.undo) g_ptr_array_free(a.undo, TRUE);
    if (a.theme_mon) g_object_unref(a.theme_mon);
    if (a.theme_dir_mon) g_object_unref(a.theme_dir_mon);
    theme_clear(&a.theme);
    g_free(a.font_family);
    g_free(a.data_path);
    g_free(a.config_path);
    g_free(a.theme_path);
    g_free(a.last_due_notify);
    if (a.selftest) return a.selftest_fail > 0 ? 1 : 0;
    return status;
}
