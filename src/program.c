#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <llhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h> // kody bledow (perror)
#include <unistd.h> // unix commands
#include <arpa/inet.h> // struktury sieciowe


// KONFIGURACJA BACKEND
struct backend { const char *host; const char *ip; const uint16_t port; };

static struct backend backends[] = {
    {"panel.local", "127.0.0.1", 9090},
    {"api.local", "127.0.0.1", 9091},
    {NULL, NULL, 0}
};

// KONFIGURACJA CONNECTIONS
struct conn {
    struct bufferevent *client_bev;
    struct bufferevent *backend_bev;

    llhttp_t parser;
    llhttp_settings_t settings;

    struct evbuffer *pending; // dane czekające na połączenie z backendem

    char host[256];
    bool reading_host;

    bool closing;
    bool close_after_write;    
};

// można dodać niedługo
// enum conn_state { READING, PROXYING, CLOSING, ... };

static void conn_free(struct conn *c) {
    if (!c) return;

    if (c->client_bev) {
        bufferevent_free(c->client_bev);
        c->client_bev = NULL;
    }

    if (c->backend_bev) {
        bufferevent_free(c->backend_bev);
        c->backend_bev = NULL;
    }

    if (c->pending) {
        evbuffer_free(c->pending);
        c->pending = NULL;
    }

    free(c);
}

// LLHTTP CALLBACK 
static int on_header_field(llhttp_t *p, const char *at, size_t n)
{
    struct conn *c = p->data;

    // Czy to pole "Host"?
    c->reading_host = false;

    if (n == 4 && strncasecmp(at, "host", 4) == 0) {
        c->reading_host = true;
    }
        
    return 0;
}

// LLHTTP CALLBACK
static int on_header_value(llhttp_t *p, const char *at, size_t n)
{
    struct conn *c = p->data;

    if (!c->reading_host) return 0; // Ignoruj inne nagłówki niż "Host"

    // zakres len wynosi [n ; sizeof(c->host) - 1]
    size_t len = n < sizeof(c->host) - 1 ? n : sizeof(c->host) - 1;
    memcpy(c->host, at, len);
    c->host[len] = '\0'; // dodaj null terminator

    return 0;
}

// BACKEND PRZYSLAL ODPOWIEDZ
static void backend_read_cb(struct bufferevent *bev, void *ctx) 
{
    struct conn *c = ctx;

    // Zero-copy [backend -> client]
    evbuffer_add_buffer(
        bufferevent_get_output(c->client_bev),
        bufferevent_get_input(bev)
    );
}

// STATUS POLACZENIA Z BACKENDEM
static void backend_event_cb(struct bufferevent *bev, short what, void *ctx)
{
    struct conn *c = ctx;
    if (what & BEV_EVENT_CONNECTED) {
        printf("[BACKEND] połączono\n");
        evbuffer_add_buffer(
            bufferevent_get_output(c->backend_bev),
            c->pending
        );
        return;
    }
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        printf("[BACKEND] rozłączono\n");
        bufferevent_free(c->backend_bev);
        c->backend_bev = NULL;
    }
}

// POLACZENIE Z BACKENDEM
static void connect_to_backend(struct conn *c, const char *ip, uint16_t port) 
{
    struct event_base *base = bufferevent_get_base(c->client_bev);

    c->backend_bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(c->backend_bev, backend_read_cb, NULL, backend_event_cb, c);
    bufferevent_enable(c->backend_bev, EV_READ | EV_WRITE);

    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port); // zamienia z little-endian na big-endian
    inet_pton(AF_INET, ip, &sin.sin_addr); // str -> bin

    bufferevent_socket_connect(c->backend_bev,
                               (struct sockaddr *)&sin, sizeof(sin));
    printf("[BACKEND] łącze z %s:%d\n", ip, port);
}

// LLHTTP CALLBACK (CO ROBIMY PO PRZESŁANIU REQUESTA)
static int on_message_complete(llhttp_t *p) 
{
    struct conn *c = p->data;

    // Usuwa port ':xxxx' 
    char *colon = strchr(c->host, ':');
    if (colon) *colon = '\0';
  
    for (int i = 0; backends[i].host != NULL; i++) {
        if (strcasecmp(c->host, backends[i].host) == 0) {
            printf("[ROUTER] %s -> %s:%d\n", c->host, backends[i].ip, backends[i].port);
            connect_to_backend(c, backends[i].ip, backends[i].port);
            return 0;
        }
    }

    printf("[ROUTER] brak backendu dla: %s\n", c->host);

    const char *resp =
        "HTTP/1.1 502 Bad Gateway\r\n"
        "Content-Length: 12\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Bad Gateway!";

    bufferevent_write(c->client_bev, resp, strlen(resp));
    // zwolnienie dopiero po wyslaniu http code error
    c->close_after_write = true;

    return 0;
}

// RESETOWANIE STANU PARSERA
static void reset_parser(struct conn *c) __attribute__((unused));
static void reset_parser(struct conn *c) {
    if (!c) return;
    // Re-inicjalizacja parsera – przywracanie stanu początkowego
    // http-request = żądania klienta
    llhttp_init(&c->parser, HTTP_REQUEST, &c->settings);
    c->parser.data = c;
}

// INICJALIZAJA PARSERA LLHTTP
static void init_parser(struct conn *c) 
{
    llhttp_settings_init(&c->settings);
    c->settings.on_header_field = on_header_field;
    c->settings.on_header_value = on_header_value;
    c->settings.on_message_complete = on_message_complete;
    llhttp_init(&c->parser, HTTP_REQUEST, &c->settings);
    c->parser.data = c;
}

static void client_write_cb(struct bufferevent *bev, void *ctx) {
    struct conn *c = ctx;

    if (c->close_after_write &&
        evbuffer_get_length(bufferevent_get_output(bev)) == 0) {
        c->closing = true;
    }
}

// KLIENT PRZYSLAL DANE
static void client_read_cb(struct bufferevent *bev, void *ctx) 
{
    struct conn *c = ctx;
    char buf[4096];
    size_t n;

    while ((n = bufferevent_read(bev, buf, sizeof(buf))) > 0) {
        enum llhttp_errno err = llhttp_execute(&c->parser, buf, n);

        // [idea]
        // można zrobić żeby zwracało 400 Bad Request zamiast zamykania połaczenia od razu

        if (err != HPE_OK) {
            fprintf(stderr, "[HTTP ERROR] %s\n", llhttp_errno_name(err));
            
            // zamkniecie klienta i backend
            bufferevent_disable(bev, EV_READ | EV_WRITE);
            c->closing = true;

            return;
        }

        // sklejamy request w 1 calosc
        evbuffer_add(c->pending, buf, n);
    }
}

// KLIENT ZAMNKNAL POLACZENIE
static void client_event_cb(struct bufferevent *bev, short what, void *ctx) 
{
    struct conn *c = ctx;

    if (what & BEV_EVENT_EOF) {
        printf("[CLIENT] połączenie zamknięte przez klienta\n");
    } else if (what & BEV_EVENT_ERROR) {
        printf("[CLIENT] błąd: %s\n", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    } else if (what & BEV_EVENT_TIMEOUT) {
        printf("[CLIENT] timeout\n");
    }

    // Zamykamy połączenie po EOF / błedzie / timeout
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {

        //reset_parser(c);        
        
        // zwalnianie struktur
        conn_free(c);
    }
}

// NOWE POLACZENIE PRZYCHODZACE DO PROXY
static void accept_cb(struct evconnlistener *listener,
                      evutil_socket_t fd,
                      struct sockaddr *sa,
                      int socklen,
                      void *arg)
{
    struct event_base *base = arg; // base przez ctx.

    char ip[16] = "?"; 
    evutil_inet_ntop(AF_INET, &((struct sockaddr_in*)sa)->sin_addr, ip, sizeof(ip));
                    
    printf("[ACCEPT] nowe połączenie od %s\n", ip);

    // Nowe połączenie zajmuje miejsce na stercie
    struct conn *c = calloc(1, sizeof(*c));
    c->pending = evbuffer_new();
    init_parser(c);

    struct bufferevent *bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    c->client_bev = bev;

    bufferevent_setcb(bev, client_read_cb, client_write_cb, client_event_cb, c); // ustawienie callback
    bufferevent_enable(bev, EV_READ | EV_WRITE); // wlaczamy odczyt i zapis
}

// MAIN
int main(void)
{
    struct event_base *base = event_base_new();
    if (!base) return 1;

    // Nasłuchiwanie
    struct sockaddr_in sin = {.sin_family = AF_INET, .sin_port = htons(8081), .sin_addr.s_addr = INADDR_ANY };

    struct evconnlistener *listener = evconnlistener_new_bind(
        base,
        accept_cb,
        base,       // base jako argument do 'accept_cb'
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
        -1, // albo SOMAXCONN
        (struct sockaddr*)&sin,
        sizeof(sin)
    );

    if (!listener) {
        fprintf(stderr, "evconnlistener_new_bind failed \n");
        event_base_free(base);
        return 1;
    }

    printf("Serwer reverse-proxy na porcie 8081\n");

    event_base_dispatch(base);

    // Tu nigdy nie dojdziemy
    evconnlistener_free(listener);
    event_base_free(base);

    return 0;
}