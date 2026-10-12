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
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define APP_ID  "org.omarchy.tasks"
#define VERSION "3.0.0"

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

static gboolean valid_date(const char *s) {
    gint y, m, d;
    return s && sscanf(s, "%d-%d-%d", &y, &m, &d) == 3 && y >= 1900 &&
           m >= 1 && m <= 12 && d >= 1 && d <= 31;
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

static gchar *parse_due_token(const char *tok) {
    if (g_strcmp0(tok, "today") == 0) return today_iso();
    if (g_strcmp0(tok, "tomorrow") == 0) {
        time_t t = time(NULL) + 86400;
        struct tm tm;
        localtime_r(&t, &tm);
        gchar buf[16];
        g_snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900,
                   tm.tm_mon + 1, tm.tm_mday);
        return g_strdup(buf);
    }
    if (valid_date(tok)) return g_strdup(tok);
    return NULL;
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

static Task *task_new(App *a, const char *title, gint prio, const char *due,
                      gint status) {
    Task *t = g_new0(Task, 1);
    t->id = a->store.next_id++;
    t->title = g_strdup(title);
    t->prio = prio;
    t->status = status;
    t->due = due ? g_strdup(due) : NULL;
    t->created = today_iso();
    t->app = a;
    return t;
}

/* ---- storage (jansson) ------------------------------------------------ */

static gboolean load_store(App *a) {
    json_error_t jerr;
    json_t *root = json_load_file(a->data_path, JSON_PRESERVE_ORDER, &jerr);
    if (!root) {
        if (!g_file_test(a->data_path, G_FILE_TEST_EXISTS)) return TRUE;
        g_warning("tasks: json parse error: %s", jerr.text);
        return FALSE;
    }
    a->store.next_id = (gint)json_integer_value(json_object_get(root, "next_id"));
    if (a->store.next_id < 1) a->store.next_id = 1;
    json_t *tasks = json_object_get(root, "tasks");
    if (json_is_array(tasks)) {
        size_t i;
        json_t *j;
        json_array_foreach(tasks, i, j) {
            Task *t = g_new0(Task, 1);
            t->id = json_integer_value(json_object_get(j, "id"));
            const char *title = json_string_value(json_object_get(j, "title"));
            t->title = g_strdup(title ? title : "(untitled)");
            const char *status_s = json_string_value(json_object_get(j, "status"));
            if (!status_s)
                status_s = json_is_true(json_object_get(j, "done")) ? "done" : "todo";
            t->status = status_index(status_s);
            const char *prio = json_string_value(json_object_get(j, "priority"));
            t->prio = prio_index(prio);
            const char *due = json_string_value(json_object_get(j, "due"));
            t->due = due ? g_strdup(due) : NULL;
            const char *cre = json_string_value(json_object_get(j, "created"));
            t->created = g_strdup(cre ? cre : today_iso());
            t->app = a;
            g_array_append_val(a->store.tasks, t);
        }
    }
    json_decref(root);
    return TRUE;
}

static gboolean save_store(App *a) {
    json_t *root = json_object();
    json_t *tasks = json_array();
    json_object_set_new(root, "next_id", json_integer(a->store.next_id));
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
    g_autofree gchar *dir = g_path_get_dirname(a->data_path);
    g_mkdir_with_parents(dir, 0700);
    gboolean ok = json_dump_file(root, a->data_path, JSON_INDENT(2)) == 0;
    json_decref(root);
    return ok;
}

/* ---- theme: dark blue/black, minimal ---------------------------------- */

static const char *THEME_CSS =
    "* { outline: none; }\n"
    "window, window.background { background-color: rgba(4, 8, 20, 0.72);\n"
    "         background-image: none; box-shadow: none; color: #cbd6ea;\n"
    "         font-family: \"JetBrainsMono Nerd Font\", \"JetBrainsMono NF\", monospace;\n"
    "         font-size: 13px; }\n"
    "headerbar { background: rgba(7, 12, 30, 0.55); background-image: none;\n"
    "            border-bottom: 1px solid rgba(120,155,255,0.10);\n"
    "            color: #95a8cb; }\n"
    ".app-title { font-size: 13px; font-weight: 700; letter-spacing: 4px;\n"
    "             color: #adc0e6; }\n"
    ".panel { background-color: rgba(14, 24, 52, 0.52);\n"
    "         border: 1px solid rgba(130,165,255,0.11);\n"
    "         border-radius: 16px;\n"
    "         box-shadow: inset 0 0 0 1px rgba(255,255,255,0.02); }\n"
    ".column-title { font-size: 12px; font-weight: 700; letter-spacing: 2.2px;\n"
    "                color: #8ba0c8; }\n"
    ".badge { font-size: 11px; font-weight: 700; border-radius: 999px;\n"
    "         padding: 1px 8px; min-width: 20px; }\n"
    ".badge-todo     { color:#7aa2ff; background:rgba(90,140,255,0.18); }\n"
    ".badge-progress { color:#f2b455; background:rgba(240,176,77,0.16); }\n"
    ".badge-done     { color:#5fd07a; background:rgba(95,208,122,0.16); }\n"
    ".hint-label { font-size: 12px; color: #5f7093; }\n"
    "scrolledwindow, scrolledwindow > viewport, overlay, listbox, list {\n"
    "    background: none; box-shadow: none; border: none; }\n"
    "listbox.row-list { background: none; }\n"
    "row { background: none; border-radius: 12px; }\n"
    "row:selected { background-color: rgba(90,140,255,0.24); border-radius: 12px; }\n"
    "row:hover:not(:selected) { background-color: rgba(130,170,255,0.05);\n"
    "                           border-radius: 12px; }\n"
    ".task-box { border-radius: 11px; transition: background-color 130ms ease; }\n"
    ".task-box:hover { background-color: rgba(130,170,255,0.08); }\n"
    ".task-title { font-size: 14px; color: #e9eefb; }\n"
    ".task-title.prio-high { color: #ff5c66; font-weight: 600; }\n"
    ".task-title.prio-med  { color: #c2ccdf; }\n"
    ".task-title.prio-low  { color: #63cf7d; }\n"
    ".task-done .task-title { opacity: 0.40; text-decoration: line-through; }\n"
    ".due-label { font-size: 11px; color: #66779a; }\n"
    ".due-overdue { color: #ff5c66; font-weight: 700; }\n"
    "entry { background-color: rgba(6,11,26,0.66);\n"
    "        border-color: rgba(70,100,170,0.35); color: #eaf1ff;\n"
    "        border-radius: 10px; }\n"
    "entry:focus { border-color: #5b8cff; }\n"
    ".add-entry { border: 1px dashed rgba(85,115,195,0.55);\n"
    "             background: rgba(120,160,255,0.03); }\n"
    ".drop-target { background-color: rgba(90,140,255,0.16); border-radius: 12px; }\n"
    ".pop-label { font-size: 12px; color: #93a6c8; }\n";

static void apply_css(void) {
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, THEME_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ask GTK for the dark variant so controls sit well inside our dark theme */
static void prefer_dark(void) {
    GtkSettings *st = gtk_settings_get_default();
    GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(st),
                                                     "gtk-application-prefer-dark-theme");
    if (pspec) g_object_set(st, "gtk-application-prefer-dark-theme", TRUE, NULL);
}

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

static const char *query(App *a) {
    return (a->search_active && a->search) ? gtk_editable_get_text(
        GTK_EDITABLE(a->search)) : "";
}

static gboolean matches_query(App *a, Task *t) {
    if (!a->search_active) return TRUE;
    const char *q = query(a);
    if (!*q) return TRUE;
    g_autofree gchar *tl = g_ascii_strdown(t->title, -1);
    g_autofree gchar *ql = g_ascii_strdown(q, -1);
    return strstr(tl, ql) != NULL;
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

static void due_btn_set(DueCtx *c) {
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

static void row_menu_rename(GtkButton *b, gpointer ud) {
    (void)b;
    Task *t = ud;
    close_all_popovers(t->app);
    start_rename(t->app, t);
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
    GtkWidget *row = t->row;
    GtkWidget *lb_widget = row ? gtk_widget_get_parent(row) : NULL;
    if (row && lb_widget) gtk_list_box_remove(GTK_LIST_BOX(lb_widget), row);
    array_remove(a, t);
    save_store(a);
    task_free(t);
    update_counts(a);
}

static void clear_done(App *a) {
    close_all_popovers(a);
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
        "Clear completed tasks? This removes everything in the Done column "
        "and cannot be undone.");
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

static gboolean on_list_key(GtkEventControllerKey *c, guint k, guint kc,
                            GdkModifierType st, gpointer ud) {
    (void)c; (void)kc; (void)st;
    Column *col = ud;
    App *a = col->app;
    GtkWidget *fw = gtk_root_get_focus(GTK_ROOT(a->win));
    if (GTK_IS_ENTRY(fw)) return FALSE;
    GtkWidget *row = GTK_WIDGET(gtk_list_box_get_selected_row(
        GTK_LIST_BOX(col->listbox)));
    if (!row) return FALSE;
    Task *t = g_object_get_data(G_OBJECT(row), "task");
    if (!t) return FALSE;
    if (k == GDK_KEY_Delete || k == GDK_KEY_BackSpace) {
        delete_task(a, t);
        return TRUE;
    }
    if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter || k == GDK_KEY_space) {
        change_status(a, t, t->status == S_DONE ? S_TODO : S_DONE);
        return TRUE;
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
        for (gint i = 0; i < 3; i++) {
            GtkWidget *row = GTK_WIDGET(gtk_list_box_get_selected_row(
                GTK_LIST_BOX(a->col[i]->listbox)));
            if (row) {
                Task *t = g_object_get_data(G_OBJECT(row), "task");
                if (t) {
                    delete_task(a, t);
                    return TRUE;
                }
            }
        }
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

    /* listbox keys (delete / toggle) */
    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_list_key), col);
    gtk_widget_add_controller(col->listbox, GTK_EVENT_CONTROLLER(k));
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

static void build_window(App *a) {
    prefer_dark();
    apply_css();
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
    for (gint i = 0; i < 60; i++) {
        while (g_main_context_iteration(NULL, FALSE))
            ;
        g_usleep(1500);
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
        close_all_popovers(a);
        for (gint i = 0; i < 40; i++)
            while (g_main_context_iteration(NULL, FALSE))
                ;
        CHECK(a->popovers->len == 0, "popovers closed after due menu");
    } else {
        g_print("SKIP due-popover grab check (window not mapped)\n");
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
    a->data_path = g_build_filename(g_get_user_data_dir(), "tasks",
                                    "tasks.json", NULL);
    a->store.tasks = g_array_new(FALSE, FALSE, sizeof(Task *));
    load_store(a);
    a->popovers = g_ptr_array_new();
    build_window(a);
    rebuild_all(a);
    gtk_window_present(GTK_WINDOW(a->win));
    if (getenv("TASKS_SELFTEST")) {
        a->selftest = TRUE;
        g_timeout_add(250, selftest_timer, a);
    }
#ifdef TASKS_GEOM
    g_timeout_add(500, debug_geom_timer, a);
#endif
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    App a;
    memset(&a, 0, sizeof a);
    a.gapp = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(a.gapp, "activate", G_CALLBACK(on_activate), &a);
    gint status = g_application_run(G_APPLICATION(a.gapp), argc, argv);
    g_object_unref(a.gapp);
    store_free(&a.store);
    if (a.popovers) g_ptr_array_free(a.popovers, TRUE);
    g_free(a.data_path);
    if (a.selftest) return a.selftest_fail > 0 ? 1 : 0;
    return status;
}
