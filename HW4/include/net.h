#ifndef NET_H
#define NET_H

#include "world.h"

int net_server_start(const char *ip, int port);
void net_server_poll(void);
void net_server_stop(void);

void net_publish(int tick, const char *message);
void net_publish_state(const World *w);

#endif