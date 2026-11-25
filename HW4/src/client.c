#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_to_server(const char *host, const char *port) {
    struct addrinfo hints, *result = NULL, *it;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "Не удалось определить адрес %s: %s\n",
                host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (it = result; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Использование: %s IP_СЕРВЕРА ПОРТ\n", argv[0]);
        return 2;
    }

    int fd = connect_to_server(argv[1], argv[2]);
    if (fd < 0) {
        fprintf(stderr, "Не удалось подключиться к %s:%s: %s\n",
                argv[1], argv[2], strerror(errno));
        return 1;
    }

    printf("Подключено к %s:%s. Ожидание событий...\n", argv[1], argv[2]);
    fflush(stdout);

    char buffer[2048];
    for (;;) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            if (fwrite(buffer, 1, (size_t)n, stdout) != (size_t)n) break;
            fflush(stdout);
            continue;
        }
        if (n == 0) {
            printf("Соединение с сервером закрыто.\n");
            break;
        }
        if (errno == EINTR) continue;
        perror("recv");
        break;
    }

    close(fd);
    return 0;
}
