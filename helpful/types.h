#ifndef TYPES_H
#define TYPES_H

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <event2/buffer.h>
#include <llhttp.h>
#include <stdbool.h>


#define LISTEN_PORT 8080
#define MAX_HOST_LEN 256
#define MAX_METHOD_LEN 16
#define MAX_URL_LEN 512
#define MAX_REQUEST_SIZE (2*1024*1024)


struct backend {
    const char *friendly;
    const char *host;
    const char *ip;
    uint16_t port;
};

extern struct backend backends[];

struct client_state {
    struct bufferevent *client_bev;
    struct bufferevent *backend_bev;

    llhttp_t parser;
    llhttp_settings_t settings;

    struct evbuffer *full_request;
    char host[MAX_HOST_LEN];
    char method[MAX_METHOD_LEN];
    char url[MAX_URL_LEN];
    bool keep_alive;
    bool request_complete;
};

#endif