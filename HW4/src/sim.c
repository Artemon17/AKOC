#define _POSIX_C_SOURCE 200809L

#include "sim.h"
#include "fire.h"
#include "sensors.h"
#include "messages.h"
#include "center.h"
#include "teams.h"
#include "net.h"

#include <string.h>

static int count_burning(const World *w) {
    int n = w->map->rows * w->map->cols;
    int c = 0;

    for (int i = 0; i < n; i++)
        if (w->state[i] == ST_BURNING) c++;

    return c;
}

static int count_flammable_safe(const World *w) {
    int n = w->map->rows * w->map->cols;
    int c = 0;

    for (int i = 0; i < n; i++) {
        Terrain t = w->map->cells[i];

        if (t == TERRAIN_WATER || t == TERRAIN_FIREBREAK)
            continue;

        FireState s = w->state[i];

        if (s == ST_SAFE || s == ST_EXTINGUISHED)
            c++;
    }

    return c;
}

SimResult sim_finished(const World *w) {
    if (count_burning(w) == 0)
        return RESULT_EXTINGUISHED;

    if (count_flammable_safe(w) == 0)
        return RESULT_TERRITORY_EXHAUSTED;

    if (!w->cfg->end_unlimited && w->tick >= w->cfg->end_max_ticks)
        return RESULT_TICK_LIMIT;

    return RESULT_RUNNING;
}

void sim_tick(World *w) {
    w->tick++;

    sensors_observe(w);
    teams_observe(w);

    messages_tick(w);
    center_tick(w);
    teams_move(w);
    teams_extinguish(w);
    fire_spread(w);

    net_server_poll();
    net_publish_state(w);
    net_server_poll();
}

const char *sim_result_name(SimResult r) {
    switch (r) {
        case RESULT_RUNNING: return "RUNNING";
        case RESULT_EXTINGUISHED: return "EXTINGUISHED";
        case RESULT_TERRITORY_EXHAUSTED: return "TERRITORY_EXHAUSTED";
        case RESULT_TICK_LIMIT: return "TICK_LIMIT";
        case RESULT_INTERRUPTED: return "INTERRUPTED";
    }

    return "?";
}

SimResult sim_run(World *w) {
    SimResult r = sim_finished(w);

    while (r == RESULT_RUNNING) {
        sim_tick(w);
        r = sim_finished(w);
    }

    return r;
}