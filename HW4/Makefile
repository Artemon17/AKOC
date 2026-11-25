
CC       = gcc
CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -O2 -g -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS  =

SRC_DIR   = src
BUILD_DIR = build

TARGET         = $(BUILD_DIR)/fire
CLIENT_TARGET  = $(BUILD_DIR)/fire-client
MONITOR_TARGET = $(BUILD_DIR)/fire-monitor

SERVER_SRCS = $(filter-out $(SRC_DIR)/client.c $(SRC_DIR)/monitor.c,$(wildcard $(SRC_DIR)/*.c))
SERVER_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SERVER_SRCS))

all: $(TARGET) $(CLIENT_TARGET) $(MONITOR_TARGET)

# Сервер симуляции.
$(TARGET): $(SERVER_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

# Обычный клиент.
$(CLIENT_TARGET): $(SRC_DIR)/client.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# Комплексный клиент.
$(MONITOR_TARGET): $(SRC_DIR)/monitor.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# Компиляция объектных файлов сервера.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET)

client: $(CLIENT_TARGET)

monitor: $(MONITOR_TARGET)

-include $(SERVER_OBJS:.o=.d)

.PHONY: all clean run client monitor
