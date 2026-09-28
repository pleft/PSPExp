/* The console's built-in apps: see appview.h */
#include "appview.h"
#include <string.h>

const AppView *app_find(const char *id)
{
    static const AppView *const all[] = { &MONITOR_APP, &STORE_APP };
    size_t i;
    for (i = 0; id && i < sizeof(all) / sizeof(all[0]); i++) if (!strcmp(all[i]->id, id)) return all[i];
    return NULL;
}
