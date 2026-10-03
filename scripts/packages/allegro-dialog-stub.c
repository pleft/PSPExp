/* allegro_dialog for a board with no desktop (Open Surge on PSPExp): Allegro's
 * native-dialog add-on needs GTK on Linux, far too heavy for a PSPExp board, and
 * the PSP shows no desktop windows anyway. Message boxes and the text log go to
 * standard error (the console's log); file choosers find nothing. Built by
 * scripts/packages/build-opensurge.sh as liballegro_dialog.so.5.2. */
#include <allegro5/allegro.h>
#include <allegro5/allegro_native_dialog.h>
#include <stdarg.h>
#include <stdio.h>

static bool inited;

bool al_init_native_dialog_addon(void) { inited = true; return true; }
bool al_is_native_dialog_addon_initialized(void) { return inited; }
void al_shutdown_native_dialog_addon(void) { inited = false; }
uint32_t al_get_allegro_native_dialog_version(void) { return ALLEGRO_VERSION_INT; }

int al_show_native_message_box(ALLEGRO_DISPLAY *display, char const *title, char const *heading,
                               char const *text, char const *buttons, int flags)
{
    (void)display; (void)buttons; (void)flags;
    fprintf(stderr, "[allegro dialog] %s: %s: %s\n", title ? title : "", heading ? heading : "", text ? text : "");
    return 0;   /* as if the window were closed */
}

ALLEGRO_FILECHOOSER *al_create_native_file_dialog(char const *initial_path, char const *title, char const *patterns, int mode)
{
    (void)initial_path; (void)title; (void)patterns; (void)mode;
    return NULL;
}
bool al_show_native_file_dialog(ALLEGRO_DISPLAY *display, ALLEGRO_FILECHOOSER *dialog) { (void)display; (void)dialog; return false; }
int al_get_native_file_dialog_count(const ALLEGRO_FILECHOOSER *dialog) { (void)dialog; return 0; }
const char *al_get_native_file_dialog_path(const ALLEGRO_FILECHOOSER *dialog, size_t index) { (void)dialog; (void)index; return NULL; }
void al_destroy_native_file_dialog(ALLEGRO_FILECHOOSER *dialog) { (void)dialog; }

ALLEGRO_TEXTLOG *al_open_native_text_log(char const *title, int flags) { (void)title; (void)flags; return (ALLEGRO_TEXTLOG *)1; }
void al_close_native_text_log(ALLEGRO_TEXTLOG *textlog) { (void)textlog; }
void al_append_native_text_log(ALLEGRO_TEXTLOG *textlog, char const *format, ...)
{
    va_list ap;
    (void)textlog;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
}
ALLEGRO_EVENT_SOURCE *al_get_native_text_log_event_source(ALLEGRO_TEXTLOG *textlog) { (void)textlog; return NULL; }
