/* SPDX-License-Identifier: Zlib */

#ifdef GTKOSXAPPLICATION
#include <gtkosxapplication.h>
#include "osx-utils.h"
#endif

#include <girara/settings.h>
#include <girara/log.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include "zathura.h"
#include "plugin.h"
#include "utils.h"
#ifdef WITH_SYNCTEX
#include "dbus-interface.h"
#include "synctex.h"
#endif

/* Unix socket path for single-instance IPC */
#define SOCKET_PATH_TEMPLATE "/tmp/zathura-%s.sock"

/* Application data structure */
typedef struct {
  GtkApplication* app;
  gchar* config_dir;
  gchar* data_dir;
  gchar* cache_dir;
  gchar* plugin_path;
  gchar* synctex_editor;
  gchar* password;
  gchar* mode;
  gchar* bookmark_name;
  gchar* search_string;
  gint page_number;
  Window embed;
  char** argv;
  gboolean files_opened;  /* Flag to track if files were opened at startup */
} ZathuraAppData;

static ZathuraAppData* app_data = NULL;
static int ipc_socket_fd = -1;
static GIOChannel* ipc_channel = NULL;
#ifdef GTKOSXAPPLICATION
static GtkAccelGroup* menu_accel_group = NULL;
#endif

/* Forward declaration */
static zathura_t* create_zathura_window(const char* filepath);

/* Get socket path for current user */
static char* get_socket_path(void) {
  const char* user = g_get_user_name();
  return g_strdup_printf(SOCKET_PATH_TEMPLATE, user);
}

/* Try to connect to existing instance and send file path */
static bool try_send_to_existing(const char* filepath) {
  g_autofree char* socket_path = get_socket_path();

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    return false;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(sock);
    return false;
  }

  /* Send file path to existing instance */
  const char* path_to_send = filepath ? filepath : "";
  ssize_t len = strlen(path_to_send);
  ssize_t written = write(sock, path_to_send, len + 1);  /* Include null terminator */
  close(sock);

  return written == len + 1;
}

/* Handle incoming connection on IPC socket */
static gboolean on_ipc_connection(GIOChannel* source, GIOCondition condition, gpointer data) {
  (void)condition;
  (void)data;

  int listen_fd = g_io_channel_unix_get_fd(source);
  int client_fd = accept(listen_fd, NULL, NULL);
  if (client_fd < 0) {
    return TRUE;
  }

  /* Read file path from client */
  char buffer[PATH_MAX + 1];
  ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
  close(client_fd);

  if (n <= 0) {
    return TRUE;
  }
  buffer[n] = '\0';

  /* Open new window with the file */
  if (strlen(buffer) > 0) {
    girara_debug("IPC: Opening new window for '%s'", buffer);
    create_zathura_window(buffer);
  } else {
    girara_debug("IPC: Opening new empty window");
    create_zathura_window(NULL);
  }

  return TRUE;
}

/* Start listening for IPC connections */
static bool start_ipc_listener(void) {
  g_autofree char* socket_path = get_socket_path();

  /* Remove stale socket file if it exists */
  unlink(socket_path);

  ipc_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ipc_socket_fd < 0) {
    girara_warning("Failed to create IPC socket: %s", strerror(errno));
    return false;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (bind(ipc_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    girara_warning("Failed to bind IPC socket: %s", strerror(errno));
    close(ipc_socket_fd);
    ipc_socket_fd = -1;
    return false;
  }

  if (listen(ipc_socket_fd, 5) < 0) {
    girara_warning("Failed to listen on IPC socket: %s", strerror(errno));
    close(ipc_socket_fd);
    unlink(socket_path);
    ipc_socket_fd = -1;
    return false;
  }

  /* Set non-blocking */
  int flags = fcntl(ipc_socket_fd, F_GETFL, 0);
  fcntl(ipc_socket_fd, F_SETFL, flags | O_NONBLOCK);

  /* Add to GLib main loop */
  ipc_channel = g_io_channel_unix_new(ipc_socket_fd);
  g_io_add_watch(ipc_channel, G_IO_IN, on_ipc_connection, NULL);

  girara_debug("IPC listener started on %s", socket_path);
  return true;
}

/* Cleanup IPC socket */
static void cleanup_ipc(void) {
  if (ipc_channel != NULL) {
    g_io_channel_shutdown(ipc_channel, FALSE, NULL);
    g_io_channel_unref(ipc_channel);
    ipc_channel = NULL;
  }
  if (ipc_socket_fd >= 0) {
    close(ipc_socket_fd);
    ipc_socket_fd = -1;
  }
  g_autofree char* socket_path = get_socket_path();
  unlink(socket_path);
}

/* Handle synctex forward synchronization */
#ifdef WITH_SYNCTEX
static int run_synctex_forward(const char* synctex_fwd, const char* filename, int synctex_pid) {
  g_autoptr(GFile) file = g_file_new_for_commandline_arg(filename);
  if (file == NULL) {
    girara_error("Unable to handle argument '%s'.", filename);
    return -1;
  }

  g_autofree char* real_path = g_file_get_path(file);
  if (real_path == NULL) {
    girara_error("Failed to determine path for '%s'", filename);
    return -1;
  }

  int line                    = 0;
  int column                  = 0;
  g_autofree char* input_file = NULL;
  if (synctex_parse_input(synctex_fwd, &input_file, &line, &column) == false) {
    girara_error("Failed to parse argument to --synctex-forward.");
    return -1;
  }

  const int ret = zathura_dbus_synctex_position(real_path, input_file, line, column, synctex_pid);
  if (ret == -1) {
    /* D-Bus or SyncTeX failed */
    girara_error("Got no usable data from SyncTeX or D-Bus failed in some way.");
  }

  return ret;
}
#endif

static zathura_t* create_zathura_window(const char* filepath) {
  /* create zathura session */
  zathura_t* zathura = zathura_create();
  if (zathura == NULL) {
    return NULL;
  }

  zathura_set_xid(zathura, app_data->embed);
  zathura_set_config_dir(zathura, app_data->config_dir);
  zathura_set_data_dir(zathura, app_data->data_dir);
  zathura_set_cache_dir(zathura, app_data->cache_dir);
  zathura_set_plugin_dir(zathura, app_data->plugin_path);
  zathura_set_argv(zathura, app_data->argv);

  /* Init zathura */
  if (zathura_init(zathura) == false) {
    zathura_free(zathura);
    return NULL;
  }

  /* Register window with GtkApplication so it knows to keep running */
  if (app_data->app != NULL && zathura->ui.session != NULL && zathura->ui.session->gtk.window != NULL) {
    gtk_application_add_window(app_data->app, GTK_WINDOW(zathura->ui.session->gtk.window));
#ifdef GTKOSXAPPLICATION
    /* Disable tabbing for this window */
    osx_disable_window_tabbing(zathura->ui.session->gtk.window);
    /* Add menu accelerator group to window for Cmd+N shortcut */
    if (menu_accel_group != NULL) {
      gtk_window_add_accel_group(GTK_WINDOW(zathura->ui.session->gtk.window), menu_accel_group);
    }
#endif
  }

  if (app_data->synctex_editor != NULL) {
    girara_setting_set(zathura->ui.session, "synctex-editor-command", app_data->synctex_editor);
  }

  /* Open document if provided */
  if (filepath != NULL) {
    gint page = app_data->page_number;
    if (page > 0) {
      --page;
    }
    document_open_idle(zathura, filepath, app_data->password, page,
                       app_data->mode, NULL, app_data->bookmark_name, app_data->search_string);
  }

  return zathura;
}

#ifdef GTKOSXAPPLICATION
static GtkosxApplication* osx_app = NULL;

/* Track zathura instances for reusing empty windows */
static GList* zathura_instances = NULL;

/* Handle macOS file open events (from Finder, open command, etc.) */
static gboolean on_osx_open_file(GtkosxApplication* app, gchar* path, gpointer user_data) {
  (void)app;
  (void)user_data;

  girara_debug("macOS open file request: %s", path ? path : "(null)");

  if (path != NULL && strlen(path) > 0) {
    /* Check if there's an empty window we can reuse */
    for (GList* l = zathura_instances; l != NULL; l = l->next) {
      zathura_t* z = (zathura_t*)l->data;
      if (z != NULL && !zathura_has_document(z)) {
        girara_debug("Reusing empty window for file");
        /* Open document in existing empty window */
        document_open_idle(z, path, NULL, 0, NULL, NULL, NULL, NULL);
        return TRUE;
      }
    }

    /* No empty window, create a new one */
    girara_debug("Creating new window for file");
    zathura_t* zathura = create_zathura_window(path);
    if (zathura != NULL) {
      zathura_instances = g_list_append(zathura_instances, zathura);
      return TRUE;  /* File handled successfully */
    }
  }
  return FALSE;  /* File not handled */
}

/* Action callback for new window */
static void on_new_window_action(GSimpleAction* action, GVariant* parameter, gpointer user_data) {
  (void)action;
  (void)parameter;
  (void)user_data;
  g_application_activate(G_APPLICATION(app_data->app));
}

/* Called on application startup to set up macOS menu */
static void on_startup(GtkApplication* app, gpointer user_data) {
  (void)user_data;

  /* Disable macOS automatic window tabbing (prevents tabs showing in fullscreen) */
  osx_disable_automatic_tabbing();

  osx_app = g_object_new(GTKOSX_TYPE_APPLICATION, NULL);
  gtkosx_application_set_use_quartz_accelerators(osx_app, TRUE);

  /* Connect to macOS-specific file open signal */
  g_signal_connect(osx_app, "NSApplicationOpenFile",
                   G_CALLBACK(on_osx_open_file), NULL);

  /* Create action for new window */
  GSimpleAction* new_window_action = g_simple_action_new("new-window", NULL);
  g_signal_connect(new_window_action, "activate", G_CALLBACK(on_new_window_action), NULL);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(new_window_action));

  /* Set Cmd+N accelerator for the action */
  const gchar* new_window_accels[] = { "<Primary>n", NULL };
  gtk_application_set_accels_for_action(app, "app.new-window", new_window_accels);

  /* Create macOS menu bar */
  GtkWidget* menubar = gtk_menu_bar_new();

  /* File menu */
  GtkWidget* file_menu = gtk_menu_new();
  GtkWidget* file_item = gtk_menu_item_new_with_label("File");
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);

  /* New Window item with Cmd+N label */
  GtkWidget* new_window_item = gtk_menu_item_new_with_label("New Window		⌘N");
  g_signal_connect(new_window_item, "activate", G_CALLBACK(on_new_window_action), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), new_window_item);

  gtk_widget_show_all(menubar);
  gtkosx_application_set_menu_bar(osx_app, GTK_MENU_SHELL(menubar));
  gtkosx_application_ready(osx_app);

  const gchar* id = gtkosx_application_get_bundle_id();
  if (id != NULL) {
    girara_debug("Bundle ID: %s", id);
  }
}
#endif

/* Called when the application is activated (no files) */
static void on_activate(GtkApplication* app, gpointer user_data) {
  (void)app;
  (void)user_data;

  /* Don't create a blank window if we already opened files at startup */
  if (app_data != NULL && app_data->files_opened) {
    return;
  }

  /* Create a new empty window */
  zathura_t* zathura = create_zathura_window(NULL);
  if (zathura == NULL) {
    girara_error("Could not create zathura window.");
  }
#ifdef GTKOSXAPPLICATION
  else {
    zathura_instances = g_list_append(zathura_instances, zathura);
  }
#endif
}

/* Called when files are opened */
static void on_open(GtkApplication* app, GFile** files, gint n_files, const gchar* hint, gpointer user_data) {
  (void)app;
  (void)hint;
  (void)user_data;

  for (gint i = 0; i < n_files; i++) {
    g_autofree gchar* filepath = g_file_get_path(files[i]);
    if (filepath != NULL) {
      zathura_t* zathura = create_zathura_window(filepath);
      if (zathura == NULL) {
        girara_error("Could not create zathura window for '%s'.", filepath);
      } else {
        /* Mark that we successfully opened files */
        if (app_data != NULL) {
          app_data->files_opened = TRUE;
        }
#ifdef GTKOSXAPPLICATION
        zathura_instances = g_list_append(zathura_instances, zathura);
#endif
      }
    }
  }
}

/* main function */
GIRARA_VISIBLE int main(int argc, char* argv[]) {
  zathura_init_locale();

  /* parse command line arguments */
  g_autofree gchar* config_dir     = NULL;
  g_autofree gchar* data_dir       = NULL;
  g_autofree gchar* cache_dir      = NULL;
  g_autofree gchar* plugin_path    = NULL;
  g_autofree gchar* loglevel       = NULL;
  g_autofree gchar* password       = NULL;
  g_autofree gchar* synctex_editor = NULL;
  g_autofree gchar* synctex_fwd    = NULL;
  g_autofree gchar* mode           = NULL;
  g_autofree gchar* bookmark_name  = NULL;
  g_autofree gchar* search_string  = NULL;
  gboolean forkback                = false;
  gboolean print_version           = false;
  gboolean new_instance            = false;
  gint page_number                 = ZATHURA_PAGE_NUMBER_UNSPECIFIED;
  gint synctex_pid                 = -1;
  Window embed                     = 0;

  GOptionEntry entries[] = {
      {"reparent", 'e', 0, G_OPTION_ARG_INT, &embed, _("Reparents to window specified by xid (X11)"), "xid"},
      {"config-dir", 'c', 0, G_OPTION_ARG_FILENAME, &config_dir, _("Path to the config directory"), "path"},
      {"data-dir", 'd', 0, G_OPTION_ARG_FILENAME, &data_dir, _("Path to the data directory"), "path"},
      {"cache-dir", '\0', 0, G_OPTION_ARG_FILENAME, &cache_dir, _("Path to the cache directory"), "path"},
      {"plugins-dir", 'p', 0, G_OPTION_ARG_STRING, &plugin_path, _("Path to the directories containing plugins"),
       "path"},
      {"fork", '\0', 0, G_OPTION_ARG_NONE, &forkback, _("Fork into the background"), NULL},
      {"new-instance", 'n', 0, G_OPTION_ARG_NONE, &new_instance, _("Start a new instance"), NULL},
      {"password", 'w', 0, G_OPTION_ARG_STRING, &password, _("Document password"), "password"},
      {"page", 'P', 0, G_OPTION_ARG_INT, &page_number, _("Page number to go to"), "number"},
      {"log-level", 'l', 0, G_OPTION_ARG_STRING, &loglevel, _("Log level (debug, info, warning, error)"), "level"},
      {"version", 'v', 0, G_OPTION_ARG_NONE, &print_version, _("Print version information"), NULL},
      {"synctex-editor-command", 'x', 0, G_OPTION_ARG_STRING, &synctex_editor,
       _("Synctex editor (forwarded to the synctex command)"), "cmd"},
      {"synctex-forward", '\0', 0, G_OPTION_ARG_STRING, &synctex_fwd, _("Move to given synctex position"), "position"},
      {"synctex-pid", '\0', 0, G_OPTION_ARG_INT, &synctex_pid, _("Highlight given position in the given process"),
       "pid"},
      {"mode", '\0', 0, G_OPTION_ARG_STRING, &mode, _("Start in a non-default mode"), "mode"},
      {"bookmark", 'b', 0, G_OPTION_ARG_STRING, &bookmark_name, _("Bookmark to go to"), "bookmark"},
      {"find", 'f', 0, G_OPTION_ARG_STRING, &search_string, _("Search for the given phrase and display results"),
       "string"},
      {NULL, '\0', 0, 0, NULL, NULL, NULL},
  };

  GOptionContext* context = g_option_context_new(" [file1] [file2] [...]");
  g_option_context_add_main_entries(context, entries, NULL);

  g_autoptr(GError) error = NULL;
  if (g_option_context_parse(context, &argc, &argv, &error) == false) {
    girara_error("Error parsing command line arguments: %s\n", error->message);
    g_option_context_free(context);

    return -1;
  }
  g_option_context_free(context);

  zathura_set_log_level(loglevel);

#ifdef WITH_SYNCTEX
  /* handle synctex forward synchronization */
  if (synctex_fwd != NULL) {
    if (argc != 2) {
      girara_error("Too many arguments or missing filename while running with "
                   "--synctex-forward");
      return -1;
    }

    int ret = run_synctex_forward(synctex_fwd, argv[1], synctex_pid);
    if (ret > 0) {
      /* Instance found. */
      return 0;
    } else if (ret < 0) {
      /* Error occurred. */
      return -1;
    }

    girara_debug("No instance found. Starting new one.");
  }
#else
  if (synctex_fwd != NULL || synctex_editor != NULL || synctex_pid != -1) {
    girara_error("Built without synctex support, but synctex specific option was specified.");
    return -1;
  }
#endif

  /* check mode */
  if (mode != NULL && g_strcmp0(mode, "presentation") != 0 && g_strcmp0(mode, "fullscreen") != 0) {
    girara_error("Invalid argument for --mode: %s", mode);
    return -1;
  }

  /* Print version */
  if (print_version == true) {
    g_autoptr(zathura_plugin_manager_t) plugin_manager = zathura_plugin_manager_new();
    zathura_plugin_manager_set_dir(plugin_manager, plugin_path);
    zathura_plugin_manager_load(plugin_manager);

    g_autofree char* string = zathura_get_version_string(plugin_manager, false);
    if (string != NULL) {
      fprintf(stdout, "%s\n", string);
    }
    return 0;
  }

  /* Fork into the background if the user really wants to ... */
  if (forkback == true) {
    const pid_t pid = fork();
    if (pid > 0) { /* parent */
      return 0;
    } else if (pid < 0) { /* error */
      girara_error("Could not fork: %s", strerror(errno));
      return -1;
    }

    if (setsid() == -1) {
      girara_error("Could not start new process group: %s", strerror(errno));
      return -1;
    }
  }

  /* Single-instance IPC check (for macOS without D-Bus) */
  if (!new_instance) {
    /* Determine file path from arguments */
    const bool has_dd = argc > 1 && g_strcmp0(argv[1], "--") == 0;
    const int fidx = has_dd ? 2 : 1;
    const char* filepath = (argc > fidx) ? argv[fidx] : NULL;

    /* Convert to absolute path if provided */
    g_autofree char* abs_path = NULL;
    if (filepath != NULL) {
      g_autoptr(GFile) gfile = g_file_new_for_commandline_arg(filepath);
      abs_path = g_file_get_path(gfile);
    }

    /* Try to send to existing instance */
    if (try_send_to_existing(abs_path)) {
      girara_debug("Sent file to existing instance, exiting.");
      return 0;
    }
  }

  /* Set up application data */
  app_data = g_new0(ZathuraAppData, 1);
  app_data->config_dir = g_strdup(config_dir);
  app_data->data_dir = g_strdup(data_dir);
  app_data->cache_dir = g_strdup(cache_dir);
  app_data->plugin_path = g_strdup(plugin_path);
  app_data->synctex_editor = g_strdup(synctex_editor);
  app_data->password = g_strdup(password);
  app_data->mode = g_strdup(mode);
  app_data->bookmark_name = g_strdup(bookmark_name);
  app_data->search_string = g_strdup(search_string);
  app_data->page_number = page_number;
  app_data->embed = embed;
  app_data->argv = argv;

  /* Create GtkApplication */
  GApplicationFlags flags = G_APPLICATION_HANDLES_OPEN;
  if (new_instance) {
    flags |= G_APPLICATION_NON_UNIQUE;
  }

  app_data->app = gtk_application_new("org.pwmt.zathura", flags);
  g_signal_connect(app_data->app, "activate", G_CALLBACK(on_activate), NULL);
  g_signal_connect(app_data->app, "open", G_CALLBACK(on_open), NULL);
#ifdef GTKOSXAPPLICATION
  g_signal_connect(app_data->app, "startup", G_CALLBACK(on_startup), NULL);
#endif

  /* g_option_context_parse has some funny (documented) behavior:
   * * for "-- a b c" you get no -- in argv
   * * for "-- --" you get -- in argv twice
   * * for "-- -a" you get -- in argv
   *
   * So if there is one -- in argv, we need to ignore it. */
  const bool has_double_dash = argc > 1 && g_strcmp0(argv[1], "--") == 0;
  const int file_idx_base    = has_double_dash ? 2 : 1;

  /* Build file list for GApplication */
  GFile** files = NULL;
  int n_files = 0;

  if (argc > file_idx_base) {
    n_files = argc - file_idx_base;
    files = g_new0(GFile*, n_files);
    for (int i = 0; i < n_files; i++) {
      files[i] = g_file_new_for_commandline_arg(argv[file_idx_base + i]);
    }
  }

  /* Register application to check if we're remote */
  g_autoptr(GError) reg_error = NULL;
  if (!g_application_register(G_APPLICATION(app_data->app), NULL, &reg_error)) {
    girara_error("Failed to register application: %s", reg_error->message);
    for (int i = 0; i < n_files; i++) {
      g_object_unref(files[i]);
    }
    g_free(files);
    g_object_unref(app_data->app);
    g_free(app_data);
    return -1;
  }

  int status;
  if (g_application_get_is_remote(G_APPLICATION(app_data->app))) {
    /* Another instance is running via D-Bus - send files to it */
    if (n_files > 0) {
      g_application_open(G_APPLICATION(app_data->app), files, n_files, "");
    } else {
      g_application_activate(G_APPLICATION(app_data->app));
    }
    status = 0;
  } else {
    /* We are the primary instance - start IPC listener for macOS without D-Bus */
    start_ipc_listener();

    if (n_files > 0) {
      on_open(app_data->app, files, n_files, "", NULL);
    }
    status = g_application_run(G_APPLICATION(app_data->app), 0, NULL);

    /* Cleanup IPC when done */
    cleanup_ipc();
  }

  /* Cleanup */
  for (int i = 0; i < n_files; i++) {
    g_object_unref(files[i]);
  }
  g_free(files);

  g_object_unref(app_data->app);
  g_free(app_data->config_dir);
  g_free(app_data->data_dir);
  g_free(app_data->cache_dir);
  g_free(app_data->plugin_path);
  g_free(app_data->synctex_editor);
  g_free(app_data->password);
  g_free(app_data->mode);
  g_free(app_data->bookmark_name);
  g_free(app_data->search_string);
  g_free(app_data);

  return status;
}
