/* The console's built-in apps: see appview.h */
#include "appview.h"
#include "music.h"
#include "player.h"
#include <string.h>

const AppView *app_find(const char *id)
{
    static const AppView *const all[] = { &MONITOR_APP, &STORE_APP, &MUSIC_APP };
    size_t i;
    for (i = 0; id && i < sizeof(all) / sizeof(all[0]); i++) if (!strcmp(all[i]->id, id)) return all[i];
    return NULL;
}

static const FileHandler HANDLERS[] = {
    { "music", player_handles, music_play_list },
};

const FileHandler *app_file_handler(const char *name)
{
    size_t i;
    for (i = 0; name && i < sizeof(HANDLERS) / sizeof(HANDLERS[0]); i++) if (HANDLERS[i].handles(name)) return &HANDLERS[i];
    return NULL;
}
