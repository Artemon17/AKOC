#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>

#define LINE_SIZE (4U * 1024U * 1024U)
#define MAX_CELLS 2000000
#define MAX_TEAMS 10000
#define MAX_TASKS 100000
#define MAX_EVENTS 8
#define EVENT_SIZE 512
#define MAX_DISPLAY_TASKS 20

typedef struct {
    int burning;
    int safe;
    int extinguished;
    int burnt;

    int team_idle;
    int team_moving;
    int team_extinguishing;
    int team_returning;

    int task_pending;
    int task_assigned;
    int task_in_progress;
    int task_done;
    int task_cancelled;

    int messages_in_flight;
    int messages_inbox;
} MonitorStats;

typedef struct {
    int id;
    int r;
    int c;
    int state;
    int target_r;
    int target_c;
    int target_task_id;
    double progress;
} MonitorTeam;

typedef struct {
    int id;
    int r;
    int c;
    int state;
    int assigned_team_id;
    int created_tick;
    int completed_tick;
} MonitorTask;

typedef struct {
    int tick;
    int rows;
    int cols;

    char *map;

    MonitorTeam *teams;
    int teams_count;
    int teams_used;

    MonitorTask *tasks;
    int tasks_count;
    int tasks_used;

    MonitorStats stats;

    int sensors_count;
    int messages_count;
    int seen_messages;
    int next_message_id;

    int valid;
} MonitorFrame;

typedef struct {
    char lines[MAX_EVENTS][EVENT_SIZE];
    int count;
} MonitorEvents;

static char line_buffer[LINE_SIZE];

static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &result);

    if (rc != 0) {
        fprintf(stderr,
                "Не удалось определить адрес %s: %s\n",
                host,
                gai_strerror(rc));
        return -1;
    }

    int fd = -1;

    for (it = result; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);

        if (fd < 0)
            continue;

        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd,
                         data + sent,
                         length - sent,
                         MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;

        return -1;
    }

    return 0;
}

static void frame_clear(MonitorFrame *frame) {
    free(frame->map);
    free(frame->teams);
    free(frame->tasks);

    memset(frame, 0, sizeof(*frame));
}

static void frame_begin(MonitorFrame *frame,
                        int tick,
                        int rows,
                        int cols,
                        int teams_count,
                        int tasks_count) {
    frame_clear(frame);

    if (rows <= 0 || cols <= 0 ||
        teams_count < 0 || tasks_count < 0) {
        return;
    }

    if ((size_t)rows > MAX_CELLS / (size_t)cols)
        return;

    if (teams_count > MAX_TEAMS || tasks_count > MAX_TASKS)
        return;

    size_t stride = (size_t)cols + 1;

    if ((size_t)rows > SIZE_MAX / stride)
        return;

    size_t map_size = (size_t)rows * stride;

    frame->map = malloc(map_size);
    if (!frame->map) {
        frame_clear(frame);
        return;
    }

    if (teams_count > 0) {
        frame->teams = calloc((size_t)teams_count,
                              sizeof(*frame->teams));

        if (!frame->teams) {
            frame_clear(frame);
            return;
        }
    }

    if (tasks_count > 0) {
        frame->tasks = calloc((size_t)tasks_count,
                              sizeof(*frame->tasks));

        if (!frame->tasks) {
            frame_clear(frame);
            return;
        }
    }

    for (int r = 0; r < rows; r++) {
        char *row = frame->map + (size_t)r * stride;
        memset(row, '.', (size_t)cols);
        row[cols] = '\0';
    }

    frame->tick = tick;
    frame->rows = rows;
    frame->cols = cols;
    frame->teams_count = teams_count;
    frame->tasks_count = tasks_count;
    frame->valid = 1;
}

static void add_event(MonitorEvents *events, const char *line) {
    if (!line || !line[0])
        return;

    if (events->count == MAX_EVENTS) {
        memmove(events->lines,
                events->lines + 1,
                (MAX_EVENTS - 1) * sizeof(events->lines[0]));

        events->count--;
    }

    snprintf(events->lines[events->count],
         sizeof(events->lines[events->count]),
         "%.*s",
         EVENT_SIZE - 1,
         line);

    events->count++;
}

static const char *team_state_name(int state) {
    switch (state) {
        case 0: return "ожидает";
        case 1: return "движется";
        case 2: return "тушит";
        case 3: return "возвращается";
        default: return "неизвестно";
    }
}

static const char *task_state_name(int state) {
    switch (state) {
        case 0: return "ожидает";
        case 1: return "назначена";
        case 2: return "выполняется";
        case 3: return "завершена";
        case 4: return "отменена";
        default: return "неизвестно";
    }
}

static void draw_frame(const MonitorFrame *frame,
                       const MonitorEvents *events) {
    if (!frame->valid)
        return;

    printf("\033[H\033[J");

    printf("ЛЕСНОЙ ПОЖАР — КОМПЛЕКСНЫЙ МОНИТОР\n");
    printf("Такт: %d | Карта: %d x %d\n\n",
           frame->tick,
           frame->rows,
           frame->cols);

    printf("КАРТА\n");

    for (int r = 0; r < frame->rows; r++) {
        const char *row =
            frame->map + (size_t)r * ((size_t)frame->cols + 1);

        printf("%s\n", row);
    }

    printf("\n");
    printf("Обозначения: . — трава, t — деревья, w — вода, ");
    printf("_ — противопожарная полоса\n");
    printf("F — огонь, # — выгоревшая клетка, x — потушенная, ");
    printf("S — станция, s — датчик, цифры/буквы — группы\n");

    printf("\nСТАТИСТИКА ПОЖАРА\n");
    printf("Горит: %d | Безопасные: %d | Потушено: %d | Выгорело: %d\n",
           frame->stats.burning,
           frame->stats.safe,
           frame->stats.extinguished,
           frame->stats.burnt);

    printf("\nПОЖАРНЫЕ ГРУППЫ\n");

    printf("Всего: %d | Ожидают: %d | Движутся: %d | Тушат: %d | Возвращаются: %d\n",
           frame->teams_count,
           frame->stats.team_idle,
           frame->stats.team_moving,
           frame->stats.team_extinguishing,
           frame->stats.team_returning);

    if (frame->teams_used == 0) {
        printf("Нет данных о группах.\n");
    } else {
        for (int i = 0; i < frame->teams_used; i++) {
            const MonitorTeam *team = &frame->teams[i];

            printf("Группа %d: позиция (%d, %d), состояние: %s",
                   team->id,
                   team->r,
                   team->c,
                   team_state_name(team->state));

            if (team->target_task_id >= 0) {
                printf(", цель (%d, %d), задача %d",
                       team->target_r,
                       team->target_c,
                       team->target_task_id);
            }

            printf(", прогресс: %.2f\n", team->progress);
        }
    }

    printf("\nЗАДАЧИ\n");

    printf("Всего: %d | Ожидают: %d | Назначены: %d | Выполняются: %d | Завершены: %d | Отменены: %d\n",
           frame->tasks_count,
           frame->stats.task_pending,
           frame->stats.task_assigned,
           frame->stats.task_in_progress,
           frame->stats.task_done,
           frame->stats.task_cancelled);

    int shown_tasks = frame->tasks_used;

    if (shown_tasks > MAX_DISPLAY_TASKS)
        shown_tasks = MAX_DISPLAY_TASKS;

    if (shown_tasks == 0) {
        printf("Нет данных о задачах.\n");
    } else {
        for (int i = 0; i < shown_tasks; i++) {
            const MonitorTask *task = &frame->tasks[i];

            printf("Задача %d: клетка (%d, %d), состояние: %s",
                   task->id,
                   task->r,
                   task->c,
                   task_state_name(task->state));

            if (task->assigned_team_id >= 0) {
                printf(", группа %d", task->assigned_team_id);
            }

            printf(", создана на такте %d",
                   task->created_tick);

            if (task->completed_tick >= 0) {
                printf(", завершена на такте %d",
                       task->completed_tick);
            }

            printf("\n");
        }
    }

    if (frame->tasks_used > shown_tasks) {
        printf("Показаны первые %d из %d задач.\n",
               shown_tasks,
               frame->tasks_used);
    }

    printf("\nСООБЩЕНИЯ И ДАТЧИКИ\n");
    printf("Датчиков: %d | Сообщений в очередях: %d | Доставлено/обработано: %d | Следующий ID: %d\n",
       frame->sensors_count,
       frame->messages_count,
       frame->seen_messages,
       frame->next_message_id);

    printf("Сообщения в пути: %d | Входящие: %d\n",
           frame->stats.messages_in_flight,
           frame->stats.messages_inbox);

    printf("\nПОСЛЕДНИЕ СОБЫТИЯ\n");

    if (events->count == 0) {
        printf("Событий пока нет.\n");
    } else {
        for (int i = 0; i < events->count; i++)
            printf("%s\n", events->lines[i]);
    }

    printf("\nОжидание следующего обновления...\n");
    fflush(stdout);
}

static void process_line(const char *line,
                         MonitorFrame *frame,
                         MonitorEvents *events) {
    if (!line || !line[0])
        return;

    if (strncmp(line, "@FRAME ", 7) == 0) {
        int tick;
        int rows;
        int cols;
        int teams_count;
        int tasks_count;

        if (sscanf(line,
                   "@FRAME %d %d %d %d %d",
                   &tick,
                   &rows,
                   &cols,
                   &teams_count,
                   &tasks_count) == 5) {
            frame_begin(frame,
                        tick,
                        rows,
                        cols,
                        teams_count,
                        tasks_count);
        }

        return;
    }

    if (strncmp(line, "@STATS ", 7) == 0) {
        int values[15];

        int count = sscanf(
            line,
            "@STATS %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
            &values[0],
            &values[1],
            &values[2],
            &values[3],
            &values[4],
            &values[5],
            &values[6],
            &values[7],
            &values[8],
            &values[9],
            &values[10],
            &values[11],
            &values[12],
            &values[13],
            &values[14]);

        if (count == 15 && frame->valid) {
            frame->stats.burning = values[0];
            frame->stats.safe = values[1];
            frame->stats.extinguished = values[2];
            frame->stats.burnt = values[3];

            frame->stats.team_idle = values[4];
            frame->stats.team_moving = values[5];
            frame->stats.team_extinguishing = values[6];
            frame->stats.team_returning = values[7];

            frame->stats.task_pending = values[8];
            frame->stats.task_assigned = values[9];
            frame->stats.task_in_progress = values[10];
            frame->stats.task_done = values[11];
            frame->stats.task_cancelled = values[12];

            frame->stats.messages_in_flight = values[13];
            frame->stats.messages_inbox = values[14];
        }

        return;
    }

    if (strncmp(line, "@INFO ", 6) == 0) {
        if (frame->valid) {
            sscanf(line,
                   "@INFO %d %d %d %d",
                   &frame->sensors_count,
                   &frame->messages_count,
                   &frame->seen_messages,
                   &frame->next_message_id);
        }

        return;
    }

    if (strncmp(line, "@MAP ", 5) == 0) {
        int row = -1;
        int offset = 0;

        if (!frame->valid)
            return;

        if (sscanf(line, "@MAP %d %n", &row, &offset) != 1)
            return;

        if (row < 0 || row >= frame->rows)
            return;

        const char *source = line + offset;
        size_t length = strlen(source);

        if (length > (size_t)frame->cols)
            length = (size_t)frame->cols;

        char *destination =
            frame->map + (size_t)row * ((size_t)frame->cols + 1);

        memcpy(destination, source, length);
        return;
    }

    if (strncmp(line, "@TEAM ", 6) == 0) {
        if (!frame->valid ||
            frame->teams_used >= frame->teams_count) {
            return;
        }

        MonitorTeam team;

        if (sscanf(line,
                   "@TEAM %d %d %d %d %d %d %d %lf",
                   &team.id,
                   &team.r,
                   &team.c,
                   &team.state,
                   &team.target_r,
                   &team.target_c,
                   &team.target_task_id,
                   &team.progress) == 8) {
            frame->teams[frame->teams_used++] = team;
        }

        return;
    }

    if (strncmp(line, "@TASK ", 6) == 0) {
        if (!frame->valid ||
            frame->tasks_used >= frame->tasks_count) {
            return;
        }

        MonitorTask task;

        if (sscanf(line,
                   "@TASK %d %d %d %d %d %d %d",
                   &task.id,
                   &task.r,
                   &task.c,
                   &task.state,
                   &task.assigned_team_id,
                   &task.created_tick,
                   &task.completed_tick) == 7) {
            frame->tasks[frame->tasks_used++] = task;
        }

        return;
    }

    if (strcmp(line, "@END") == 0) {
        draw_frame(frame, events);
        return;
    }

    add_event(events, line);

    if (strstr(line, "завершает работу"))
        draw_frame(frame, events);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "Использование: %s IP_СЕРВЕРА ПОРТ\n",
                argv[0]);
        return 2;
    }

    int fd = connect_to_server(argv[1], argv[2]);

    if (fd < 0) {
        fprintf(stderr,
                "Не удалось подключиться к %s:%s: %s\n",
                argv[1],
                argv[2],
                strerror(errno));
        return 1;
    }

    if (send_all(fd, "MONITOR\n", 8) != 0) {
        fprintf(stderr,
                "Не удалось зарегистрировать режим монитора: %s\n",
                strerror(errno));
        close(fd);
        return 1;
    }

    MonitorFrame frame = {0};
    MonitorEvents events = {0};

    printf("Комплексный монитор подключён к %s:%s\n",
           argv[1],
           argv[2]);
    printf("Ожидание первого снимка состояния...\n");
    fflush(stdout);

    char buffer[4096];
    size_t used = 0;
    int dropping = 0;

    for (;;) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);

        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char ch = buffer[i];

                if (ch == '\n') {
                    if (!dropping) {
                        line_buffer[used] = '\0';
                        process_line(line_buffer, &frame, &events);
                    }

                    used = 0;
                    dropping = 0;
                    continue;
                }

                if (dropping)
                    continue;

                if (used + 1 < sizeof(line_buffer)) {
                    line_buffer[used++] = ch;
                } else {
                    used = 0;
                    dropping = 1;
                }
            }

            continue;
        }

        if (n == 0) {
            if (used > 0 && !dropping) {
                line_buffer[used] = '\0';
                process_line(line_buffer, &frame, &events);
            }

            printf("\nСервер отключился.\n");

            if (frame.valid) {
                printf("Последний полученный такт: %d\n",
                       frame.tick);
            }

            break;
        }

        if (errno == EINTR)
            continue;

        perror("recv");
        break;
    }

    frame_clear(&frame);
    close(fd);

    return 0;
}