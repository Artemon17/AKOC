#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NET_MAX_CLIENTS 64
#define NET_MAX_QUEUE (8U * 1024U * 1024U)
#define NET_MAX_SNAPSHOT (8U * 1024U * 1024U)

typedef struct {
    int fd;
    int monitor;
    int handshake_done;
    char input[32];
    size_t input_used;
    char *out;
    size_t capacity;
    size_t used;
    size_t sent;
} NetClient;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} TextBuffer;

static int listen_fd = -1;
static NetClient clients[NET_MAX_CLIENTS];

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void client_close(NetClient *client) {
    if (client->fd >= 0) close(client->fd);
    free(client->out);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static int queue_text(NetClient *client, const char *text, size_t len) {
    if (client->fd < 0 || !text) return -1;

    if (client->sent > 0) {
        size_t left = client->used - client->sent;
        memmove(client->out, client->out + client->sent, left);
        client->used = left;
        client->sent = 0;
    }

    if (len > NET_MAX_QUEUE - client->used) return -1;

    size_t needed = client->used + len;
    if (needed > client->capacity) {
        size_t new_capacity = client->capacity ? client->capacity : 4096;

        while (new_capacity < needed) {
            if (new_capacity > NET_MAX_QUEUE / 2) {
                new_capacity = NET_MAX_QUEUE;
                break;
            }
            new_capacity *= 2;
        }

        if (new_capacity < needed) return -1;

        char *new_out = realloc(client->out, new_capacity);
        if (!new_out) return -1;

        client->out = new_out;
        client->capacity = new_capacity;
    }

    memcpy(client->out + client->used, text, len);
    client->used += len;
    return 0;
}

static void flush_client(NetClient *client) {
    while (client->fd >= 0 && client->sent < client->used) {
        ssize_t n = send(client->fd,
                         client->out + client->sent,
                         client->used - client->sent,
                         MSG_NOSIGNAL);

        if (n > 0) {
            client->sent += (size_t)n;
            continue;
        }

        if (n < 0 && errno == EINTR) continue;

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;

        client_close(client);
        return;
    }

    if (client->fd >= 0) {
        client->used = 0;
        client->sent = 0;
    }
}

static void read_client_command(NetClient *client) {
    if (client->fd < 0 || client->handshake_done) return;

    char buffer[128];

    for (;;) {
        ssize_t n = recv(client->fd, buffer, sizeof(buffer), 0);

        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char ch = buffer[i];

                if (ch == '\n') {
                    client->input[client->input_used] = '\0';

                    if (client->input_used > 0 &&
                        client->input[client->input_used - 1] == '\r') {
                        client->input[client->input_used - 1] = '\0';
                    }

                    client->monitor =
                        strcmp(client->input, "MONITOR") == 0;

                    client->handshake_done = 1;
                    return;
                }

                if (client->input_used + 1 < sizeof(client->input)) {
                    client->input[client->input_used++] = ch;
                } else {
                    client->handshake_done = 1;
                    return;
                }
            }

            continue;
        }

        if (n == 0) {
            client_close(client);
            return;
        }

        if (errno == EINTR) continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        client_close(client);
        return;
    }
}

static int text_buffer_reserve(TextBuffer *buffer, size_t extra) {
    if (buffer->failed) return -1;

    if (extra > NET_MAX_SNAPSHOT - buffer->length) {
        buffer->failed = 1;
        return -1;
    }

    size_t needed = buffer->length + extra + 1;

    if (needed <= buffer->capacity) return 0;

    size_t new_capacity = buffer->capacity ? buffer->capacity : 4096;

    while (new_capacity < needed) {
        if (new_capacity > NET_MAX_SNAPSHOT / 2) {
            new_capacity = NET_MAX_SNAPSHOT + 1;
            break;
        }
        new_capacity *= 2;
    }

    if (new_capacity < needed) {
        buffer->failed = 1;
        return -1;
    }

    char *new_data = realloc(buffer->data, new_capacity);
    if (!new_data) {
        buffer->failed = 1;
        return -1;
    }

    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return 0;
}

static int text_buffer_append(TextBuffer *buffer,
                              const char *data,
                              size_t length) {
    if (text_buffer_reserve(buffer, length) != 0) return -1;

    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int text_buffer_appendf(TextBuffer *buffer,
                               const char *format,
                               ...) {
    if (buffer->failed) return -1;

    va_list args;
    va_list copy;

    va_start(args, format);
    va_copy(copy, args);

    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        buffer->failed = 1;
        return -1;
    }

    if (text_buffer_reserve(buffer, (size_t)needed) != 0) {
        va_end(args);
        return -1;
    }

    vsnprintf(buffer->data + buffer->length,
              buffer->capacity - buffer->length,
              format,
              args);

    va_end(args);

    buffer->length += (size_t)needed;
    return 0;
}

static char terrain_glyph(Terrain terrain) {
    switch (terrain) {
        case TERRAIN_GRASS: return '.';
        case TERRAIN_TREE: return 't';
        case TERRAIN_WATER: return 'w';
        case TERRAIN_FIREBREAK: return '_';
    }

    return '?';
}

static char team_glyph(int id) {
    if (id >= 0 && id <= 9) return (char)('0' + id);
    if (id >= 10 && id < 36) return (char)('A' + id - 10);
    return 'T';
}

static char cell_glyph(const World *w, int r, int c) {
    size_t index = (size_t)r * (size_t)w->map->cols + (size_t)c;

    if (w->occupant) {
        switch (w->occupant[index]) {
            case OCC_STATION:
                return 'S';

            case OCC_SENSOR:
                return 's';

            case OCC_TEAM:
                if (w->team_id)
                    return team_glyph(w->team_id[index]);
                return 'T';

            case OCC_NONE:
                break;
        }
    }

    switch (w->state[index]) {
        case ST_BURNING:
            return 'F';

        case ST_EXTINGUISHED:
            return 'x';

        case ST_BURNT:
            return '#';

        case ST_SAFE:
            break;
    }

    return terrain_glyph(w->map->cells[index]);
}

static void collect_stats(const World *w,
                          int stats[15]) {
    memset(stats, 0, 15 * sizeof(stats[0]));

    size_t cells = (size_t)w->map->rows * (size_t)w->map->cols;

    for (size_t i = 0; i < cells; i++) {
        switch (w->state[i]) {
            case ST_BURNING:
                stats[0]++;
                break;

            case ST_SAFE:
                stats[1]++;
                break;

            case ST_EXTINGUISHED:
                stats[2]++;
                break;

            case ST_BURNT:
                stats[3]++;
                break;
        }
    }

    for (int i = 0; i < w->teams_count; i++) {
        switch (w->teams[i].state) {
            case TEAM_IDLE:
                stats[4]++;
                break;

            case TEAM_MOVING:
                stats[5]++;
                break;

            case TEAM_EXTINGUISHING:
                stats[6]++;
                break;

            case TEAM_RETURNING:
                stats[7]++;
                break;
        }
    }

    for (int i = 0; i < w->tasks.count; i++) {
        switch (w->tasks.items[i].state) {
            case TASK_PENDING:
                stats[8]++;
                break;

            case TASK_ASSIGNED:
                stats[9]++;
                break;

            case TASK_IN_PROGRESS:
                stats[10]++;
                break;

            case TASK_DONE:
                stats[11]++;
                break;

            case TASK_CANCELLED:
                stats[12]++;
                break;
        }
    }

    stats[13] = w->in_flight.count;
    stats[14] = w->inbox.count;
}

static int build_snapshot(const World *w, TextBuffer *buffer) {
    if (!w || !w->map || !w->map->cells ||
        !w->state || !w->occupant) {
        return -1;
    }

    int stats[15];
    collect_stats(w, stats);

    if (text_buffer_appendf(
            buffer,
            "@FRAME %d %d %d %d %d\n",
            w->tick,
            w->map->rows,
            w->map->cols,
            w->teams_count,
            w->tasks.count) != 0) {
        return -1;
    }

    if (text_buffer_appendf(
            buffer,
            "@STATS %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
            stats[0], stats[1], stats[2], stats[3],
            stats[4], stats[5], stats[6], stats[7],
            stats[8], stats[9], stats[10], stats[11], stats[12],
            stats[13], stats[14]) != 0) {
        return -1;
    }

    if (text_buffer_appendf(
            buffer,
            "@INFO %d %d %d %d\n",
            w->sensors_count,
            w->in_flight.count + w->inbox.count,
            w->seen_count,
            w->next_message_id) != 0) {
        return -1;
    }

    for (int r = 0; r < w->map->rows; r++) {
        if (text_buffer_appendf(buffer, "@MAP %d ", r) != 0)
            return -1;

        for (int c = 0; c < w->map->cols; c++) {
            char glyph = cell_glyph(w, r, c);
            if (text_buffer_append(buffer, &glyph, 1) != 0)
                return -1;
        }

        if (text_buffer_append(buffer, "\n", 1) != 0)
            return -1;
    }

    for (int i = 0; i < w->teams_count; i++) {
        const FireTeam *team = &w->teams[i];

        if (text_buffer_appendf(
                buffer,
                "@TEAM %d %d %d %d %d %d %d %.3f\n",
                team->id,
                team->r,
                team->c,
                (int)team->state,
                team->target_r,
                team->target_c,
                team->target_task_id,
                team->progress) != 0) {
            return -1;
        }
    }

    for (int i = 0; i < w->tasks.count; i++) {
        const Task *task = &w->tasks.items[i];

        if (text_buffer_appendf(
                buffer,
                "@TASK %d %d %d %d %d %d %d\n",
                task->id,
                task->r,
                task->c,
                (int)task->state,
                task->assigned_team_id,
                task->created_tick,
                task->completed_tick) != 0) {
            return -1;
        }
    }

    if (text_buffer_append(buffer, "@END\n", 5) != 0)
        return -1;

    return 0;
}

int net_server_start(const char *ip, int port) {
    if (listen_fd >= 0) return -1;
    if (!ip || port < 1 || port > 65535) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, NET_MAX_CLIENTS) < 0 ||
        set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    listen_fd = fd;

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        memset(&clients[i], 0, sizeof(clients[i]));
        clients[i].fd = -1;
    }

    return 0;
}

void net_server_poll(void) {
    if (listen_fd < 0) return;

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (set_nonblocking(fd) < 0) {
            close(fd);
            continue;
        }

        int slot = -1;

        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            close(fd);
            continue;
        }

        clients[slot].fd = fd;
        clients[slot].monitor = 0;
        clients[slot].handshake_done = 0;
        clients[slot].input_used = 0;
        clients[slot].out = NULL;
        clients[slot].capacity = 0;
        clients[slot].used = 0;
        clients[slot].sent = 0;

        static const char hello[] =
            "Подключено к серверу симуляции лесного пожара\n";

        if (queue_text(&clients[slot], hello, sizeof(hello) - 1) != 0)
            client_close(&clients[slot]);
    }

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) continue;

        read_client_command(&clients[i]);

        if (clients[i].fd >= 0)
            flush_client(&clients[i]);
    }
}

void net_publish(int tick, const char *message) {
    if (listen_fd < 0 || !message) return;

    char line[2048];

    int n = snprintf(line, sizeof(line),
                     "[ТАКТ %d] %s\n", tick, message);

    if (n < 0) return;

    size_t length = (size_t)n;
    if (length >= sizeof(line)) length = sizeof(line) - 1;

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 &&
            queue_text(&clients[i], line, length) != 0) {
            client_close(&clients[i]);
        }
    }
}

void net_publish_state(const World *w) {
    if (listen_fd < 0 || !w) return;

    int has_monitor = 0;

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && clients[i].monitor) {
            has_monitor = 1;
            break;
        }
    }

    if (!has_monitor) return;

    TextBuffer snapshot = {0};

    if (build_snapshot(w, &snapshot) != 0) {
        free(snapshot.data);
        return;
    }

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (clients[i].fd < 0 || !clients[i].monitor)
            continue;

        if (queue_text(&clients[i], snapshot.data, snapshot.length) != 0)
            client_close(&clients[i]);
    }

    free(snapshot.data);
}

void net_server_stop(void) {
    if (listen_fd < 0) return;

    static const char stop_message[] =
        "Сервер симуляции завершает работу\n";

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        NetClient *client = &clients[i];

        if (client->fd < 0) continue;

        client->used = 0;
        client->sent = 0;

        size_t offset = 0;
        size_t length = sizeof(stop_message) - 1;

        while (offset < length) {
            ssize_t n = send(client->fd,
                             stop_message + offset,
                             length - offset,
                             MSG_NOSIGNAL);

            if (n > 0) {
                offset += (size_t)n;
                continue;
            }

            if (n < 0 && errno == EINTR) continue;
            break;
        }

        shutdown(client->fd, SHUT_WR);
        client_close(client);
    }

    close(listen_fd);
    listen_fd = -1;
}