#define _POSIX_C_SOURCE 200809L

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <glib/gstdio.h>
#include <gdk/gdk.h>

#define GDK_DISABLE_DEPRECATION_WARNINGS

/* =========================================================================
 * 1. DATA MODEL & STRUCTURE DEFINITIONS
 * ========================================================================= */

enum {
    COL_NAME,          
    COL_TYPE,          
    COL_ICON_NAME,     
    COL_EXPANDER_BTN,  
    COL_MODE,          
    COL_UID,           
    COL_GID,           
    COL_MTIME_SEC,     
    COL_MTIME_NSEC,    
    COL_SOURCE_PATH,   
    NUM_COLS           
};

typedef struct {
    gchar *name;
    gchar *path;
    guint mode;
    guint uid;
    guint gid;
    gint64 mtime_sec;
    guint32 mtime_nsec;
    gboolean valid;
} RootMetaData;

typedef struct {
    GtkTreeRowReference *parent_ref; 
    gint position;                    
    gchar *name;
    gchar *type;
    gchar *icon_name;
    gchar *expander_btn;
    guint mode;
    guint uid;
    guint gid;
    gint64 mtime_sec;
    guint32 mtime_nsec;
    gchar *source_path;
} UndoItem;

typedef struct {
    GtkWidget *window;
    GtkWidget *btn_select_dir;
    GtkWidget *btn_delete;
    GtkWidget *btn_undo;
    GtkWidget *btn_export;

    GtkTreeView *tree_left;
    GtkTreeView *tree_right;
    GtkTreeStore *store_left;
    GtkTreeStore *store_right;

    GtkWidget *status_label;
    GtkWidget *new_folder_entry;
    GtkWidget *progress_bar;

    GList *undo_stack;                
    RootMetaData root_meta;           

    // Asynchronous task context
    GSubprocess *current_process;
    GDataInputStream *process_stdout;
    gchar *tmp_exclude_file;
    gboolean is_exporting;
} AppWidgets;

/* =========================================================================
 * 2. STYLING (CSS)
 * ========================================================================= */

const char *GNOME_LIGHT_CSS = 
    "window { background-color: #ededed; font-family: 'Cantarell', sans-serif; font-size: 13px; }"
    "headerbar { background-color: #f6f6f6; border-bottom: 1px solid #dcdcdc; padding: 6px 12px; }"
    ".pane-card { background-color: #f6f6f6; border: 1px solid #dcdcdc; border-radius: 12px; padding: 10px; }"
    ".pane-header { font-size: 13px; font-weight: bold; color: #2e2e2e; margin-bottom: 6px; }"
    "treeview { background-color: #ffffff; border: 1px solid #dcdcdc; border-radius: 8px; padding: 4px; color: #1e1e1e; }"
    "treeview:selected, treeview:selected:focus { background-color: #3584e4; color: #ffffff; }"
    "button.suggested-action { background-color: #3584e4; color: #ffffff; border-radius: 6px; }"
    "button.destructive-action { background-color: #e01b24; color: #ffffff; border-radius: 6px; }"
    "button.destructive-action:hover { background-color: #c01c28; color: #ffffff; }"
    "progressbar { margin-left: 8px; margin-right: 8px; }";

/* =========================================================================
 * 3. HELPER & UTILITY FUNCTIONS
 * ========================================================================= */

static int count_store_nodes_recursive(GtkTreeModel *model, GtkTreeIter *parent) {
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);
    int count = 0;

    while (valid) {
        count++;
        count += count_store_nodes_recursive(model, &iter); 
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    return count;
}

static void update_status_bar(AppWidgets *app) {
    if (!app->status_label) return;

    int left_count = count_store_nodes_recursive(GTK_TREE_MODEL(app->store_left), NULL);
    int right_count = count_store_nodes_recursive(GTK_TREE_MODEL(app->store_right), NULL);
    int diff = left_count - right_count;

    gchar *status_str = g_strdup_printf(
        "Source: <b>%d</b> items  |  Staging: <b>%d</b> items  |  Excluded: <b>%d</b> items",
        left_count, right_count, diff
    );
    gtk_label_set_markup(GTK_LABEL(app->status_label), status_str);
    g_free(status_str);
}

static void update_undo_button(AppWidgets *app) {
    if (app->is_exporting) return;

    guint count = g_list_length(app->undo_stack);
    char *label_text;
    if (count > 0) {
        label_text = g_strdup_printf("Undo (%u)", count);
    } else {
        label_text = g_strdup("Undo");
    }
    gtk_button_set_label(GTK_BUTTON(app->btn_undo), label_text);
    gtk_widget_set_sensitive(app->btn_undo, count > 0);
    g_free(label_text);
}

static void free_root_meta(RootMetaData *meta) {
    if (meta->name) g_free(meta->name);
    if (meta->path) g_free(meta->path);
    meta->name = NULL;
    meta->path = NULL;
    meta->valid = FALSE;
}

static void free_undo_item(UndoItem *item) {
    if (!item) return;
    if (item->parent_ref) {
        gtk_tree_row_reference_free(item->parent_ref);
    }
    g_free(item->name);
    g_free(item->type);
    g_free(item->icon_name);
    g_free(item->expander_btn);
    g_free(item->source_path);
    g_free(item);
}

static void set_ui_busy_state(AppWidgets *app, gboolean busy) {
    app->is_exporting = busy;
    gtk_widget_set_sensitive(app->btn_select_dir, !busy);
    gtk_widget_set_sensitive(app->btn_delete, !busy);
    gtk_widget_set_sensitive(app->btn_export, !busy && app->root_meta.valid);
    
    if (busy) {
        gtk_widget_set_sensitive(app->btn_undo, FALSE);
        gtk_widget_set_visible(app->progress_bar, TRUE);
    } else {
        update_undo_button(app);
        gtk_widget_set_visible(app->progress_bar, FALSE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    }
}

/* =========================================================================
 * 4. FILE SYSTEM SCANNING & METADATA EXTRACTION
 * ========================================================================= */

void populate_directory_recursive(GtkTreeStore *store, GtkTreeIter *parent, GFile *file) {
    GFileEnumerator *enumerator = g_file_enumerate_children(
        file,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_ICON ","
        G_FILE_ATTRIBUTE_UNIX_MODE ","
        G_FILE_ATTRIBUTE_UNIX_UID ","
        G_FILE_ATTRIBUTE_UNIX_GID ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
        G_FILE_QUERY_INFO_NONE,
        NULL,
        NULL
    );

    if (!enumerator) return;

    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(enumerator, NULL, NULL)) != NULL) {
        const char *name = g_file_info_get_name(info);
        GFileType file_type = g_file_info_get_file_type(info);
        
        GFile *child_file = g_file_get_child(file, name);
        char *child_path = g_file_get_path(child_file);

        guint32 mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
        guint32 uid  = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID);
        guint32 gid  = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID);

        GDateTime *mtime = g_file_info_get_modification_date_time(info);
        gint64 mtime_sec = 0;
        guint32 mtime_nsec = 0;
        if (mtime) {
            mtime_sec = g_date_time_to_unix(mtime);
            mtime_nsec = g_date_time_get_microsecond(mtime) * 1000;
            g_date_time_unref(mtime);
        }

        if (mode == 0) {
            mode = (file_type == G_FILE_TYPE_DIRECTORY) ? 0750 : 0640;
        }

        const char *type_str = (file_type == G_FILE_TYPE_DIRECTORY) ? "Folder" : "File";
        const char *icon_str = (file_type == G_FILE_TYPE_DIRECTORY) ? "folder-symbolic" : "text-x-generic-symbolic";
        const char *btn_str  = (file_type == G_FILE_TYPE_DIRECTORY) ? "[ + ]" : "";

        GtkTreeIter iter;
        gtk_tree_store_append(store, &iter, parent);
        gtk_tree_store_set(store, &iter,
                           COL_NAME, name,
                           COL_TYPE, type_str,
                           COL_ICON_NAME, icon_str,
                           COL_EXPANDER_BTN, btn_str,
                           COL_MODE, mode,
                           COL_UID, uid,
                           COL_GID, gid,
                           COL_MTIME_SEC, mtime_sec,
                           COL_MTIME_NSEC, mtime_nsec,
                           COL_SOURCE_PATH, child_path,
                           -1);

        if (file_type == G_FILE_TYPE_DIRECTORY) {
            populate_directory_recursive(store, &iter, child_file);
        }

        g_free(child_path);
        g_object_unref(child_file);
        g_object_unref(info);
    }

    g_file_enumerator_close(enumerator, NULL, NULL);
    g_object_unref(enumerator);
}

static RootMetaData fetch_root_metadata(GFile *folder) {
    RootMetaData meta = {0};
    GFileInfo *info = g_file_query_info(
        folder,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_UNIX_MODE ","
        G_FILE_ATTRIBUTE_UNIX_UID ","
        G_FILE_ATTRIBUTE_UNIX_GID ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
        G_FILE_QUERY_INFO_NONE,
        NULL,
        NULL
    );

    if (info) {
        meta.name = g_strdup(g_file_info_get_name(info));
        meta.path = g_file_get_path(folder);
        meta.mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
        meta.uid  = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID);
        meta.gid  = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID);

        GDateTime *mtime = g_file_info_get_modification_date_time(info);
        if (mtime) {
            meta.mtime_sec = g_date_time_to_unix(mtime);
            meta.mtime_nsec = g_date_time_get_microsecond(mtime) * 1000;
            g_date_time_unref(mtime);
        }
        meta.valid = TRUE;
        g_object_unref(info);
    }
    return meta;
}

/* =========================================================================
 * 5. STAGING, UNDO, & EXCLUSION CALCULATION
 * ========================================================================= */

static void collect_exclusions_recursive(GtkTreeModel *model_left, GtkTreeIter *parent_left,
                                          GtkTreeModel *model_right, GtkTreeIter *parent_right,
                                          const char *rel_prefix, GPtrArray *exclude_list) {
    GtkTreeIter iter_left;
    gboolean valid_left = gtk_tree_model_iter_children(model_left, &iter_left, parent_left);

    while (valid_left) {
        gchar *name_left = NULL;
        gchar *type_left = NULL;
        gtk_tree_model_get(model_left, &iter_left,
                           COL_NAME, &name_left,
                           COL_TYPE, &type_left,
                           -1);

        gchar *rel_path = rel_prefix ? g_build_filename(rel_prefix, name_left, NULL) : g_strdup(name_left);

        GtkTreeIter iter_right;
        gboolean found = FALSE;
        gboolean valid_right = (model_right != NULL) && gtk_tree_model_iter_children(model_right, &iter_right, parent_right);

        while (valid_right) {
            gchar *name_right = NULL;
            gtk_tree_model_get(model_right, &iter_right, COL_NAME, &name_right, -1);
            if (g_strcmp0(name_left, name_right) == 0) {
                found = TRUE;
                g_free(name_right);
                break;
            }
            g_free(name_right);
            valid_right = gtk_tree_model_iter_next(model_right, &iter_right);
        }

        if (!found) {
            g_ptr_array_add(exclude_list, g_strdup_printf("/%s", rel_path));
        } else {
            if (g_strcmp0(type_left, "Folder") == 0) {
                collect_exclusions_recursive(model_left, &iter_left, model_right, &iter_right, rel_path, exclude_list);
            }
        }

        g_free(name_left);
        g_free(type_left);
        g_free(rel_path);

        valid_left = gtk_tree_model_iter_next(model_left, &iter_left);
    }
}

static void perform_remove_selected(AppWidgets *app) {
    if (app->is_exporting) return;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(app->tree_right);
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        GtkTreePath *parent_path = gtk_tree_path_copy(path);

        UndoItem *item = g_new0(UndoItem, 1);

        if (gtk_tree_path_up(parent_path) && gtk_tree_path_get_depth(parent_path) > 0) {
            item->parent_ref = gtk_tree_row_reference_new(model, parent_path);
        }

        int *indices = gtk_tree_path_get_indices(path);
        item->position = indices[gtk_tree_path_get_depth(path) - 1];

        gtk_tree_model_get(model, &iter,
                           COL_NAME, &item->name,
                           COL_TYPE, &item->type,
                           COL_ICON_NAME, &item->icon_name,
                           COL_EXPANDER_BTN, &item->expander_btn,
                           COL_MODE, &item->mode,
                           COL_UID, &item->uid,
                           COL_GID, &item->gid,
                           COL_MTIME_SEC, &item->mtime_sec,
                           COL_MTIME_NSEC, &item->mtime_nsec,
                           COL_SOURCE_PATH, &item->source_path,
                           -1);

        gtk_tree_store_remove(app->store_right, &iter);
        app->undo_stack = g_list_prepend(app->undo_stack, item);

        gtk_tree_path_free(path);
        gtk_tree_path_free(parent_path);

        update_undo_button(app);
        update_status_bar(app);
    }
}

static void on_remove_clicked(GtkButton *btn, gpointer user_data) {
    perform_remove_selected((AppWidgets *)user_data);
}

static gboolean on_tree_key_pressed(GtkEventControllerKey *controller,
                                     guint keyval,
                                     guint keycode,
                                     GdkModifierType state,
                                     gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    if (gtk_widget_has_focus(GTK_WIDGET(app->tree_right))) {
        if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_KP_Delete || keyval == GDK_KEY_BackSpace) {
            perform_remove_selected(app);
            return GDK_EVENT_STOP;
        }
    }

    return GDK_EVENT_PROPAGATE;
}

static void on_undo_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    if (!app->undo_stack || app->is_exporting) return;

    UndoItem *item = (UndoItem *)app->undo_stack->data;
    app->undo_stack = g_list_remove(app->undo_stack, item);

    GtkTreeIter parent_iter;
    GtkTreeIter *p_iter_ptr = NULL;

    if (item->parent_ref) {
        GtkTreePath *parent_path = gtk_tree_row_reference_get_path(item->parent_ref);
        if (parent_path) {
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->store_right), &parent_iter, parent_path)) {
                p_iter_ptr = &parent_iter;
            }
            gtk_tree_path_free(parent_path);
        }
    }

    GtkTreeIter new_iter;
    gtk_tree_store_insert(app->store_right, &new_iter, p_iter_ptr, item->position);
    gtk_tree_store_set(app->store_right, &new_iter,
                       COL_NAME, item->name,
                       COL_TYPE, item->type,
                       COL_ICON_NAME, item->icon_name,
                       COL_EXPANDER_BTN, item->expander_btn,
                       COL_MODE, item->mode,
                       COL_UID, item->uid,
                       COL_GID, item->gid,
                       COL_MTIME_SEC, item->mtime_sec,
                       COL_MTIME_NSEC, item->mtime_nsec,
                       COL_SOURCE_PATH, item->source_path,
                       -1);

    free_undo_item(item);
    update_undo_button(app);
    update_status_bar(app);
}

/* =========================================================================
 * 6. ASYNCHRONOUS EXPORT ENGINE & STREAM PARSER
 * ========================================================================= */

static void read_rsync_output_async(AppWidgets *app);

static void on_rsync_wait_finish(GObject *source, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GError *error = NULL;

    gboolean success = g_subprocess_wait_check_finish(G_SUBPROCESS(source), res, &error);

    set_ui_busy_state(app, FALSE);

    if (app->tmp_exclude_file) {
        g_unlink(app->tmp_exclude_file);
        g_free(app->tmp_exclude_file);
        app->tmp_exclude_file = NULL;
    }

    if (success) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Backup Complete");
        gtk_alert_dialog_set_detail(alert, "All non-excluded data and metadata was transferred successfully.");
        gtk_alert_dialog_show(alert, GTK_WINDOW(app->window));
    } else {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Backup Execution Failed");
        gtk_alert_dialog_set_detail(alert, error ? error->message : "An unknown rsync error occurred.");
        gtk_alert_dialog_show(alert, GTK_WINDOW(app->window));
        if (error) g_error_free(error);
    }

    g_clear_object(&app->current_process);
    g_clear_object(&app->process_stdout);
}

static void on_rsync_line_read(GObject *source, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GError *error = NULL;
    gsize length = 0;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), res, &length, &error);

    if (line) {
        char *percent_ptr = strchr(line, '%');
        if (percent_ptr) {
            char *start = percent_ptr;
            while (start > line && *(start - 1) != ' ') {
                start--;
            }
            int percent = atoi(start);
            if (percent >= 0 && percent <= 100) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), percent / 100.0);
                gchar *text = g_strdup_printf("%d%%", percent);
                gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_bar), text);
                g_free(text);
            }
        }
        g_free(line);

        read_rsync_output_async(app);
    }
}

static void read_rsync_output_async(AppWidgets *app) {
    if (!app->process_stdout) return;
    g_data_input_stream_read_line_async(
        app->process_stdout,
        G_PRIORITY_DEFAULT,
        NULL,
        on_rsync_line_read,
        app
    );
}

static gchar* write_exclusions_to_temp_file(AppWidgets *app) {
    GPtrArray *exclusions = g_ptr_array_new_with_free_func(g_free);
    collect_exclusions_recursive(
        GTK_TREE_MODEL(app->store_left), NULL,
        GTK_TREE_MODEL(app->store_right), NULL,
        NULL, exclusions
    );

    GError *error = NULL;
    gchar *tmp_path = NULL;
    gint fd = g_file_open_tmp("backup_exclude_XXXXXX.txt", &tmp_path, &error);

    if (fd != -1) {
        FILE *f = fdopen(fd, "w");
        if (f) {
            for (guint i = 0; i < exclusions->len; i++) {
                fprintf(f, "%s\n", (char *)g_ptr_array_index(exclusions, i));
            }
            fclose(f);
        } else {
            close(fd);
        }
    } else {
        if (error) g_error_free(error);
    }

    g_ptr_array_free(exclusions, TRUE);
    return tmp_path;
}

static void on_select_folder_finish(GObject *source, GAsyncResult *res, gpointer data) {
    AppWidgets *app_data = (AppWidgets *)data;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, NULL);
    if (folder) {
        gtk_tree_store_clear(app_data->store_left);
        gtk_tree_store_clear(app_data->store_right);

        g_list_free_full(app_data->undo_stack, (GDestroyNotify)free_undo_item);
        app_data->undo_stack = NULL;
        update_undo_button(app_data);

        free_root_meta(&app_data->root_meta);
        app_data->root_meta = fetch_root_metadata(folder);

        populate_directory_recursive(app_data->store_left, NULL, folder);
        populate_directory_recursive(app_data->store_right, NULL, folder);

        gtk_widget_set_sensitive(app_data->btn_delete, TRUE);
        gtk_widget_set_sensitive(app_data->btn_export, TRUE);

        update_status_bar(app_data);
        g_object_unref(folder);
    }
}

static void on_select_directory(GtkButton *btn, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Source Directory");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->window), NULL, on_select_folder_finish, app);
    g_object_unref(dialog);
}

static void on_export_folder_finish(GObject *source, GAsyncResult *res, gpointer data) {
    AppWidgets *app_data = (AppWidgets *)data;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, NULL);
    if (folder && app_data->root_meta.valid) {
        char *base_path = g_file_get_path(folder);
        if (base_path) {
            const char *custom_subfolder = gtk_editable_get_text(GTK_EDITABLE(app_data->new_folder_entry));
            char *export_destination = NULL;

            if (custom_subfolder && strlen(custom_subfolder) > 0) {
                export_destination = g_build_filename(base_path, custom_subfolder, NULL);
            } else if (app_data->root_meta.name) {
                export_destination = g_build_filename(base_path, app_data->root_meta.name, NULL);
            } else {
                export_destination = g_strdup(base_path);
            }

            app_data->tmp_exclude_file = write_exclusions_to_temp_file(app_data);
            char *source_dir_slash = g_strconcat(app_data->root_meta.path, "/", NULL);

            GPtrArray *argv_array = g_ptr_array_new();
            g_ptr_array_add(argv_array, "rsync");
            g_ptr_array_add(argv_array, "-a");
            g_ptr_array_add(argv_array, "-X");
            g_ptr_array_add(argv_array, "-A");
            g_ptr_array_add(argv_array, "-H");
            g_ptr_array_add(argv_array, "-S");
            g_ptr_array_add(argv_array, "--info=progress2");

            if (app_data->tmp_exclude_file) {
                g_ptr_array_add(argv_array, g_strdup_printf("--exclude-from=%s", app_data->tmp_exclude_file));
            }

            g_ptr_array_add(argv_array, source_dir_slash);
            g_ptr_array_add(argv_array, export_destination);
            g_ptr_array_add(argv_array, NULL);

            GError *error = NULL;
            GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
            
            app_data->current_process = g_subprocess_launcher_spawnv(
                launcher,
                (const char * const *)argv_array->pdata,
                &error
            );

            g_object_unref(launcher);

            if (error) {
                GtkAlertDialog *alert = gtk_alert_dialog_new("Process Initialization Error");
                gtk_alert_dialog_set_detail(alert, error->message);
                gtk_alert_dialog_show(alert, GTK_WINDOW(app_data->window));
                g_error_free(error);
            } else {
                set_ui_busy_state(app_data, TRUE);

                GInputStream *stdout_stream = g_subprocess_get_stdout_pipe(app_data->current_process);
                app_data->process_stdout = g_data_input_stream_new(stdout_stream);

                read_rsync_output_async(app_data);

                g_subprocess_wait_check_async(
                    app_data->current_process,
                    NULL,
                    on_rsync_wait_finish,
                    app_data
                );
            }

            if (app_data->tmp_exclude_file) {
                g_free(g_ptr_array_index(argv_array, 7)); 
            }
            g_ptr_array_free(argv_array, TRUE);
            g_free(source_dir_slash);
            g_free(export_destination);
            g_free(base_path);
        }
        g_object_unref(folder);
    }
}

static void on_export_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Destination Directory");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->window), NULL, on_export_folder_finish, app);
    g_object_unref(dialog);
}

/* =========================================================================
 * 7. CUSTOM TREE VIEW CONTROLS & EXPANDER BUTTONS
 * ========================================================================= */

static void toggle_row_expansion(GtkTreeView *tree_view, GtkTreePath *path) {
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;

    if (gtk_tree_model_get_iter(model, &iter, path)) {
        if (gtk_tree_model_iter_has_child(model, &iter)) {
            if (gtk_tree_view_row_expanded(tree_view, path)) {
                gtk_tree_view_collapse_row(tree_view, path);
                gtk_tree_store_set(GTK_TREE_STORE(model), &iter, COL_EXPANDER_BTN, "[ + ]", -1);
            } else {
                gtk_tree_view_expand_row(tree_view, path, FALSE);
                gtk_tree_store_set(GTK_TREE_STORE(model), &iter, COL_EXPANDER_BTN, "[ -- ]", -1);
            }
        }
    }
}

static void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
    toggle_row_expansion(tree_view, path);
}

static void on_row_expanded_event(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *path, gpointer user_data) {
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    gtk_tree_store_set(GTK_TREE_STORE(model), iter, COL_EXPANDER_BTN, "[ -- ]", -1);
}

static void on_row_collapsed_event(GtkTreeView *tree_view, GtkTreeIter *iter, GtkTreePath *path, gpointer user_data) {
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    gtk_tree_store_set(GTK_TREE_STORE(model), iter, COL_EXPANDER_BTN, "[ + ]", -1);
}

static GtkWidget* create_tree_pane(const char *title_text, GtkTreeStore *store, GtkTreeView **out_view) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(card, "pane-card");

    GtkWidget *lbl = gtk_label_new(title_text);
    gtk_widget_add_css_class(lbl, "pane-header");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_append(GTK_BOX(card), lbl);

    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    *out_view = GTK_TREE_VIEW(tree);

    gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(tree), FALSE);
    gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(tree), 16);

    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(tree), TRUE);
    g_signal_connect(tree, "row-activated", G_CALLBACK(on_row_activated), NULL);
    g_signal_connect(tree, "row-expanded", G_CALLBACK(on_row_expanded_event), NULL);
    g_signal_connect(tree, "row-collapsed", G_CALLBACK(on_row_collapsed_event), NULL);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);

    GtkCellRenderer *renderer_btn = gtk_cell_renderer_text_new();
    g_object_set(renderer_btn, "xalign", 0.5, NULL);
    GtkTreeViewColumn *col_btn = gtk_tree_view_column_new_with_attributes("Expand", renderer_btn, "text", COL_EXPANDER_BTN, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col_btn);

    GtkCellRenderer *renderer_icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *renderer_text = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col_name = gtk_tree_view_column_new();
    
    gtk_tree_view_column_set_title(col_name, "Name");
    gtk_tree_view_column_pack_start(col_name, renderer_icon, FALSE);
    gtk_tree_view_column_pack_start(col_name, renderer_text, TRUE);
    gtk_tree_view_column_add_attribute(col_name, renderer_icon, "icon-name", COL_ICON_NAME);
    gtk_tree_view_column_add_attribute(col_name, renderer_text, "text", COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col_name);

    GtkCellRenderer *renderer_type = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col_type = gtk_tree_view_column_new_with_attributes("Type", renderer_type, "text", COL_TYPE, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col_type);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), tree);
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_box_append(GTK_BOX(card), scroll);
    return card;
}

/* =========================================================================
 * 8. MAIN INITIALIZATION & APPLICATION LIFECYCLE
 * ========================================================================= */

static void free_app_widgets(gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    if (!app) return;

    if (app->undo_stack) {
        g_list_free_full(app->undo_stack, (GDestroyNotify)free_undo_item);
        app->undo_stack = NULL;
    }

    free_root_meta(&app->root_meta);

    if (app->tmp_exclude_file) {
        g_unlink(app->tmp_exclude_file);
        g_free(app->tmp_exclude_file);
    }

    if (app->current_process) {
        g_subprocess_force_exit(app->current_process);
        g_object_unref(app->current_process);
    }

    if (app->process_stdout) {
        g_object_unref(app->process_stdout);
    }

    if (app->store_left) g_object_unref(app->store_left);
    if (app->store_right) g_object_unref(app->store_right);

    g_free(app);
}

static void activate(GtkApplication *app, gpointer user_data) {
    AppWidgets *widgets = g_new0(AppWidgets, 1);

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, GNOME_LIGHT_CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    widgets->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(widgets->window), "Directory Filter & Exporter");
    gtk_window_set_default_size(GTK_WINDOW(widgets->window), 1100, 650);

    // Destroy context cleanup
    g_object_set_data_full(G_OBJECT(widgets->window), "app_context", widgets, free_app_widgets);

    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(widgets->window), header);

    widgets->btn_select_dir = gtk_button_new_with_label("Open Directory");
    gtk_widget_add_css_class(widgets->btn_select_dir, "suggested-action");
    g_signal_connect(widgets->btn_select_dir, "clicked", G_CALLBACK(on_select_directory), widgets);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), widgets->btn_select_dir);

    widgets->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(widgets->progress_bar), TRUE);
    gtk_widget_set_visible(widgets->progress_bar, FALSE);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), widgets->progress_bar);

    widgets->btn_export = gtk_button_new_with_label("Export…");
    gtk_widget_add_css_class(widgets->btn_export, "suggested-action");
    gtk_widget_set_sensitive(widgets->btn_export, FALSE);
    g_signal_connect(widgets->btn_export, "clicked", G_CALLBACK(on_export_clicked), widgets);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->btn_export);

    widgets->btn_undo = gtk_button_new_with_label("Undo");
    gtk_widget_set_sensitive(widgets->btn_undo, FALSE);
    g_signal_connect(widgets->btn_undo, "clicked", G_CALLBACK(on_undo_clicked), widgets);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->btn_undo);

    widgets->btn_delete = gtk_button_new_with_label("Exclude Selected");
    gtk_widget_add_css_class(widgets->btn_delete, "destructive-action");
    gtk_widget_set_sensitive(widgets->btn_delete, FALSE);
    g_signal_connect(widgets->btn_delete, "clicked", G_CALLBACK(on_remove_clicked), widgets);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), widgets->btn_delete);

    // Create Data Stores
    widgets->store_left = gtk_tree_store_new(NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT,
                                             G_TYPE_INT64, G_TYPE_UINT, G_TYPE_STRING);
    widgets->store_right = gtk_tree_store_new(NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                              G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT,
                                              G_TYPE_INT64, G_TYPE_UINT, G_TYPE_STRING);

    // Layout Assembly
    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(main_vbox, 12);
    gtk_widget_set_margin_end(main_vbox, 12);
    gtk_widget_set_margin_top(main_vbox, 12);
    gtk_widget_set_margin_bottom(main_vbox, 12);

    GtkWidget *panes_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_vexpand(panes_box, TRUE);

    GtkWidget *left_pane = create_tree_pane("Original Source Directory", widgets->store_left, &widgets->tree_left);
    GtkWidget *right_pane = create_tree_pane("Staging Directory (Export Target)", widgets->store_right, &widgets->tree_right);

    gtk_widget_set_hexpand(left_pane, TRUE);
    gtk_widget_set_hexpand(right_pane, TRUE);

    gtk_box_append(GTK_BOX(panes_box), left_pane);
    gtk_box_append(GTK_BOX(panes_box), right_pane);
    gtk_box_append(GTK_BOX(main_vbox), panes_box);

    // Options Bar & Subfolder Input
    GtkWidget *options_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *folder_label = gtk_label_new("Target Subfolder Name (Optional):");
    widgets->new_folder_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(widgets->new_folder_entry), "Defaults to source folder name");

    gtk_box_append(GTK_BOX(options_box), folder_label);
    gtk_box_append(GTK_BOX(options_box), widgets->new_folder_entry);
    gtk_box_append(GTK_BOX(main_vbox), options_box);

    // Bottom Status Bar
    widgets->status_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(widgets->status_label), 0.0);
    update_status_bar(widgets);
    gtk_box_append(GTK_BOX(main_vbox), widgets->status_label);

    // Keyboard Controller for Delete key handling in Right Tree
    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_tree_key_pressed), widgets);
    gtk_widget_add_controller(GTK_WIDGET(widgets->tree_right), key_controller);

    gtk_window_set_child(GTK_WINDOW(widgets->window), main_vbox);
    gtk_window_present(GTK_WINDOW(widgets->window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.example.directory_filter_exporter", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
