#include "ofl_app.h"
#include <glib.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

static void setup_portable_environment(void) {
#ifdef __APPLE__
  char exe_path[2048];
  uint32_t size = sizeof(exe_path);
  if (_NSGetExecutablePath(exe_path, &size) == 0) {
    char *dir = g_path_get_dirname(exe_path); // .../Contents/MacOS
    char *schemas = g_build_filename(dir, "..", "Resources", "share", "glib-2.0", "schemas", NULL);
    if (g_file_test(schemas, G_FILE_TEST_IS_DIR)) {
      g_setenv("GSETTINGS_SCHEMA_DIR", schemas, TRUE);
    }
    g_free(schemas);
    g_free(dir);
  }
#elif defined(_WIN32)
  char exe_path[MAX_PATH];
  if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) > 0) {
    char *dir = g_path_get_dirname(exe_path);
    char *schemas = g_build_filename(dir, "share", "glib-2.0", "schemas", NULL);
    if (g_file_test(schemas, G_FILE_TEST_IS_DIR)) {
      g_setenv("GSETTINGS_SCHEMA_DIR", schemas, TRUE);
    }
    g_free(schemas);
    g_free(dir);
  }
#endif
}

int main(int argc, char **argv) {
  setup_portable_environment();
  return ofl_app_run(argc, argv);
}