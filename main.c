#include <arpa/inet.h>
#include <errno.h>
#include <event2/event.h>
#include <netinet/in.h> // keep this, for FreeBSD
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctype.h>
#include <evdns.h>
#include <sys/ioctl.h>
#include <net/if.h>

#define READ_BUFF_SIZE 128
#define WRITE_BUFF_SIZE ((READ_BUFF_SIZE)*8)
#define MAX_NAME_LENGTH 32
#define LECHAT_PORT 1234

const char send_op[] = ">>>";
const char mv_op[] = "===";

typedef struct peer_ctx_t {
    struct client_ctx_t* client;
    struct peer_ctx_t* next;
    struct peer_ctx_t* prev;

    char name[MAX_NAME_LENGTH];

    evutil_socket_t listener;
    struct event_base* base;
    struct event* read_event;
    struct event* write_event;

    uint8_t read_buff[READ_BUFF_SIZE];
    uint8_t write_buff[WRITE_BUFF_SIZE];

    ssize_t read_buff_used;
    ssize_t write_buff_used;
} peer_ctx_t;

typedef struct client_ctx_t {
    struct peer_ctx_t* peers;

    char name[MAX_NAME_LENGTH];

    evutil_socket_t listener;
    struct event_base* base;
    struct event* accept_event;
    struct event* input_event;

    uint8_t read_buff[READ_BUFF_SIZE];
    ssize_t read_buff_used;
} client_ctx_t;

typedef struct head_msg_t {
    char name[MAX_NAME_LENGTH];
} head_msg_t;

typedef struct privdata_t {
    uint32_t priv_evseen;                    // events previously seen
    struct trace_buffer* priv_trace;         // trace buffer to aid debug (e.g.)
    char name[MAX_NAME_LENGTH];
} privdata_t;


void error(const char* msg) {
    perror(msg);
    exit(1);
}

char* strtrim(char* str) {
    char* end;
    // skip leading whitespace
    while (isspace(*str)) {
        str = str + 1;
    }
    // remove trailing whitespace
    end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) {
        end = end - 1;
    }
    // write null character
    *(end + 1) = '\0';
    return str;
}

// called manually
void on_close(struct peer_ctx_t* ctx) {
    // remove ctx from the linked list
    if (ctx->next == ctx) {
        ctx->client->peers = NULL;
    } else {
        ctx->prev->next = ctx->next;
        ctx->next->prev = ctx->prev;
    }
    if (ctx->client->peers == ctx)
        ctx->client->peers = ctx->next;

    event_del(ctx->read_event);
    event_free(ctx->read_event);

    if (ctx->write_event) {
        event_del(ctx->write_event);
        event_free(ctx->write_event);
    }

    close(ctx->listener);
    free(ctx);
}

// called manually
void on_close_client(struct client_ctx_t* ctx) {
    // close all peer connections
    while (ctx->peers)
        on_close(ctx->peers);

    event_del(ctx->accept_event);
    event_free(ctx->accept_event);

    event_del(ctx->input_event);
    event_free(ctx->input_event);

    close(ctx->listener);
    free(ctx);
}

// called manually
void on_string_received(const char* str, char name[MAX_NAME_LENGTH]) {
    printf("[%s] >>> %s\n", name, str);
}

void on_read(evutil_socket_t socket, short flags, void* arg) {
    peer_ctx_t* ctx = (peer_ctx_t*) arg;

    ssize_t bytes;
    for (;;) {
        // checking for a head message
        struct head_msg_t head_msg;
        if (strlen(ctx->name) == 0) {
            bytes = read(socket, (void*)&head_msg, sizeof(struct head_msg_t));
        } else {
            bytes = read(socket, ctx->read_buff + ctx->read_buff_used, READ_BUFF_SIZE - ctx->read_buff_used);
        }

        if (bytes == 0) {
            printf("[%s has left the chat]\n", ctx->name);
            on_close(ctx);
            return;
        }

        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            printf("[%s read() failed, errno = %d, closing connection]\n", ctx->name, errno);
            on_close(ctx);
            return;
        }

        if (strlen(ctx->name) == 0) {
            strcpy(ctx->name, head_msg.name);
            printf("[%s connected]\n", ctx->name);
            continue;
        }

        break; // read() succeeded
    }

    ssize_t check = ctx->read_buff_used;
    ssize_t check_end = ctx->read_buff_used + bytes;
    ctx->read_buff_used = check_end;

    while (check < check_end) {
        if (ctx->read_buff[check] != '\n') {
            check++;
            continue;
        }

        int length = (int) check;
        ctx->read_buff[length] = '\0';
        if ((length > 0) && (ctx->read_buff[length - 1] == '\r')) {
            ctx->read_buff[length - 1] = '\0';
            length--;
        }

        if (length > 0)
            on_string_received((const char*)ctx->read_buff, ctx->name);

        // shift read_buff (optimize!)
        memmove(ctx->read_buff, ctx->read_buff + check, check_end - check - 1);
        ctx->read_buff_used -= check + 1;
        check_end -= check;
        check = 0;
    }

    if (ctx->read_buff_used == READ_BUFF_SIZE) {
        printf("[%s sent a very long string, closing connection]\n", ctx->name);
        on_close(ctx);
    }
}

void on_write(evutil_socket_t socket, short flags, void* arg) {
    struct peer_ctx_t* ctx = (struct peer_ctx_t*) arg;
    ssize_t bytes;

    for (;;) {
        bytes = write(socket, ctx->write_buff, ctx->write_buff_used);
        if (bytes <= 0) {
            if (errno == EINTR)
                continue;

            on_close(ctx);
            return;
        }

        break; // write() succeeded
    }

    // shift the write_buffer (optimize!)
    memmove(ctx->write_buff, ctx->write_buff + bytes, ctx->write_buff_used - bytes);
    ctx->write_buff_used -= bytes;

    // if there is nothing to send call event_del
    if (ctx->write_buff_used == 0) {
        if (event_del(ctx->write_event) < 0)
            error("event_del() failed");
    }
}

struct peer_ctx_t* add_new_peer(client_ctx_t* client_ctx, evutil_socket_t peer_socket) {
    struct peer_ctx_t* ctx = (struct peer_ctx_t*)malloc(sizeof(peer_ctx_t));
    if (!ctx)
        error("malloc() failed");

    ctx->client = client_ctx;
    if (client_ctx->peers == NULL) {
        client_ctx->peers = ctx;
        ctx->next = ctx;
        ctx->prev = ctx;
    } else {
        ctx->next = client_ctx->peers;
        ctx->prev = client_ctx->peers->prev;
        client_ctx->peers->prev = ctx;
        client_ctx->peers = ctx;
    }

    ctx->base = client_ctx->base;

    ctx->name[0] = '\0';
    ctx->read_buff_used = 0;
    ctx->write_buff_used = 0;

    ctx->listener = peer_socket;
    ctx->read_event = event_new(ctx->base, peer_socket, EV_READ | EV_PERSIST, on_read, (void*)ctx);
    if (!ctx->read_event)
        error("event_new(... EV_READ ...) failed");

    ctx->write_event = event_new(ctx->base, peer_socket, EV_WRITE | EV_PERSIST, on_write, (void*)ctx);
    if (!ctx->write_event)
        error("event_new(... EV_WRITE ...) failed");

    if (event_add(ctx->read_event, NULL) < 0)
        error("event_add(read_event, ...) failed");

    return ctx;
}

void on_accept(evutil_socket_t listen_sock, short flags, void* arg) {
    client_ctx_t* client_ctx = (client_ctx_t*) arg;
    evutil_socket_t new_socket = accept(listen_sock, 0, 0);

    if (new_socket < 0)
        error("accept() failed");

    // make it nonblocking
    if (evutil_make_socket_nonblocking(new_socket) < 0)
        error("evutil_make_socket_nonblocking() failed");

    add_new_peer(client_ctx, new_socket);
}

void send_head_message(peer_ctx_t* peer, const char* name) {
    struct head_msg_t head_message;
    strcpy(head_message.name, name);

    // append data to the buffer
    memcpy(peer->write_buff, &head_message, sizeof(head_message));
    peer->write_buff[sizeof(head_message)] = '\n';
    peer->write_buff_used += sizeof(head_message);

    // add writing event (it's not a problem to call it multiple times)
    if (event_add(peer->write_event, NULL) < 0)
        error("event_add(peer->write_event, ...) failed");
}

int send_message(peer_ctx_t* peer, const char* name, const char* message, size_t len) {
    // check that there is enough space in the write buffer
    if (WRITE_BUFF_SIZE - peer->write_buff_used < len + 1) {
        // if it's not, call on_close being careful with
        // the links in the linked list
        printf("[Unable to send a message to %s. Not enough space in the buffer. "
               "Closing connection]\n", name);
        on_close(peer);
        return 0;
    }

    // append data to the buffer
    memcpy(peer->write_buff + peer->write_buff_used, message, len);
    peer->write_buff[peer->write_buff_used + len] = '\n';
    peer->write_buff_used += len + 1;

    // add writing event (it's not a problem to call it multiple times)
    if (event_add(peer->write_event, NULL) < 0)
        error("event_add(peer->write_event, ...) failed");

    return 1;
}

void on_connect(struct bufferevent* bev, short flag, void* user_data) {
    struct privdata_t* priv = (privdata_t*) user_data;
    do {
        if ((priv->priv_evseen & BEV_EVENT_CONNECTED) == 0) {
            switch (flag & (BEV_EVENT_ERROR | BEV_EVENT_CONNECTED)) {
                case BEV_EVENT_CONNECTED:
                    printf("[Connected to %s]\n", priv->name);
                    break;
                case BEV_EVENT_ERROR:
                    printf("[Failed to connect to %s]\n", priv->name);
                    break;
                case (BEV_EVENT_CONNECTED | BEV_EVENT_ERROR):
                    printf("[Failed to connect to %s]\n", priv->name);
                    break;
                case 0:  // huh? -- the only excuse ...
                    if (flag & BEV_EVENT_TIMEOUT)
                        printf("[%s: Connection timeout expired]\n", priv->name);
                    break;
            }
            break;
        }
        // An error occurred during a bufferevent operation.
        if (flag & BEV_EVENT_ERROR) {
            printf("[Failed to connect to %s]\n", priv->name);
        }
        // A timeout expired on the bufferevent.
        if (flag & BEV_EVENT_TIMEOUT) {
            printf("[%s: Connection timeout expired]\n", priv->name);
        }
        // We finished a requested connection on the bufferevent.
        // NOTE: this should _never_ happen now
        if (flag & BEV_EVENT_CONNECTED) {
            printf("[Connected to %s]\n", priv->name);
        }
    } while (0);

    // remember types we've seen before
    priv->priv_evseen |= flag;
}

evutil_socket_t create_connection(const char* name, struct client_ctx_t* client) {
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(name);
    sin.sin_port = htons(LECHAT_PORT);

    struct bufferevent* buf_ev;
    buf_ev = bufferevent_socket_new(client->base, -1, BEV_OPT_CLOSE_ON_FREE);

    struct privdata_t* data = (privdata_t*)malloc(sizeof(privdata_t));
    strcpy(data->name, name);

    bufferevent_setcb(buf_ev, NULL, NULL, on_connect, data);

    printf("[Connecting to %s...]\n", name);

    int err = bufferevent_socket_connect(buf_ev, (struct sockaddr*)&sin, sizeof(sin));
    if (err < 0) {
        bufferevent_free(buf_ev);
        free(data);
        errno = err;
        printf("[Failed to connect to %s]\n", name);
        return -1;
    }

    return bufferevent_getfd(buf_ev);
}

void on_send(struct client_ctx_t* client, const char* msg, char* send_op_pos) {
    struct peer_ctx_t* peer = client->peers;

    const char* name = strtrim(send_op_pos + strlen(send_op));
    if (strcmp(name, client->name) == 0)
        return;

    // search for an open connection with this recipient
    int sent = 0;
    do {
        if (!peer) break;
        if (strcmp(peer->name, name) != 0) {
            peer = peer->next;
            continue;
        }
        sent = send_message(peer, name, msg, send_op_pos - msg);
        break;
    } while (peer != client->peers);

    if (!sent) {
        // open a new connection with this recipient
        evutil_socket_t new_socket = create_connection(name, client);
        if (new_socket == -1) {
            printf("[Failed to connect to %s]\n", name);
            return;
        }
        peer = add_new_peer(client, new_socket);
        strcpy(peer->name, name);

        send_head_message(peer, client->name);
        send_message(peer, name, msg, send_op_pos - msg);
    }
}

void on_rename(struct client_ctx_t* client, const char* msg, char* mv_op_pos) {
    struct peer_ctx_t* peer = client->peers;

    const char* new_name = strtrim(mv_op_pos + strlen(mv_op));

    char* old_name_raw = (char*)malloc(mv_op_pos - msg + 1);
    memcpy(old_name_raw, msg, mv_op_pos - msg);
    old_name_raw[mv_op_pos - msg] = '\0';
    const char* old_name = strtrim(old_name_raw);

    // search for an open connection with this recipient
    int renamed = 0;
    do {
        if (!peer) break;
        if (strcmp(peer->name, old_name) != 0) {
            peer = peer->next;
            continue;
        }
        strcpy(peer->name, new_name);
        printf("[%s renamed to %s]\n", old_name, new_name);
        renamed = 1;
        break;
    } while (peer != client->peers);

    if (!renamed)
        printf("[No connection to %s found]\n", old_name);

    free(old_name_raw);
}

// called manually
void on_input_received(const char* str, size_t len, client_ctx_t* ctx) {
    // search for >>> operator
    char* send_op_pos = strstr(str, send_op);
    if (send_op_pos) {
        on_send(ctx, str, send_op_pos);
        return;
    }

    // search for === operator
    char* mv_op_pos = strstr(str, mv_op);
    if (mv_op_pos) {
        on_rename(ctx, str, mv_op_pos);
        return;
    }

    // no operators found
    // ...
}

void on_input(int fd, short flags, void* arg) {
    struct client_ctx_t* ctx = (client_ctx_t*) arg;

    ssize_t bytes;
    for (;;) {
        bytes = read(fd, ctx->read_buff + ctx->read_buff_used, READ_BUFF_SIZE - ctx->read_buff_used);
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            printf("stdin read() failed, errno = %d, closing connection.\n", errno);
            return;
        }
        break; // read() succeeded
    }

    ssize_t check = ctx->read_buff_used;
    ssize_t check_end = ctx->read_buff_used + bytes;
    ctx->read_buff_used = check_end;

    while (check < check_end) {
        if (ctx->read_buff[check] != '\n') {
            check++;
            continue;
        }

        size_t length = (size_t) check;
        ctx->read_buff[length] = '\0';
        if ((length > 0) && (ctx->read_buff[length - 1] == '\r')) {
            ctx->read_buff[length - 1] = '\0';
            length--;
        }

        if (length > 0)
            on_input_received((const char*)ctx->read_buff, length, ctx);

        // shift read_buff (optimize!)
        memmove(ctx->read_buff, ctx->read_buff + check, check_end - check - 1);
        ctx->read_buff_used -= check + 1;
        check_end -= check;
        check = 0;
    }

    if (ctx->read_buff_used == READ_BUFF_SIZE) {
        printf("<<<ERROR>>>: The input is too long. Cannot send it.\n");
    }
}

void run(const char* host, int port) {
    // allocate memory for a client ctx
    client_ctx_t* client_ctx = (client_ctx_t*)malloc(sizeof(client_ctx_t));
    if (!client_ctx)
        error("malloc() failed");

    client_ctx->peers = NULL;

    // create an event base
    struct event_base* base = event_base_new();
    if (!base)
        error("event_base_new() failed");

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(host);

    // create a socket
    client_ctx->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (client_ctx->listener < 0)
        error("socket() failed");

    // make it nonblocking
    if (evutil_make_socket_nonblocking(client_ctx->listener) < 0)
        error("evutil_make_socket_nonblocking() failed");

#ifndef WIN32
    {
        int one = 1;
        setsockopt(client_ctx->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
#endif

    // bind and listen
    if (bind(client_ctx->listener, (struct sockaddr*)&sin, sizeof(sin)) < 0)
        error("bind() failed");

    if (listen(client_ctx->listener, 1000) < 0)
        error("listen() failed");

    client_ctx->base = base;
    client_ctx->read_buff_used = 0;
    strcpy(client_ctx->name, host);

    // create a new accept event
    struct event* accept_event = event_new(base, client_ctx->listener, EV_READ | EV_PERSIST, on_accept, (void*)client_ctx);
    if (!accept_event)
        error("event_new() failed");
    client_ctx->accept_event = accept_event;

    // schedule the execution of accept_event
    if (event_add(accept_event, NULL) < 0)
        error("event_add() failed");

    struct event* input_event = event_new(base, STDIN_FILENO, EV_READ | EV_PERSIST, on_input, (void*)client_ctx);
    if (!input_event)
        error("event_new() failed");
    client_ctx->input_event = input_event;

    // schedule the execution of input_event
    if (event_add(input_event, NULL) < 0)
        error("event_add() failed");

    // run the event dispatching loop
    if (event_base_dispatch(base) < 0)
        error("event_base_dispatch() failed");

    // free allocated resources
    on_close_client(client_ctx);
    event_base_free(base);
}

/*
 * If client will close a connection send() will just return -1
 * instead of killing a process with SIGPIPE.
 */
void ignore_sigpipe() {
    sigset_t msk;
    if(sigemptyset(&msk) < 0)
        error("sigemptyset() failed");

    if(sigaddset(&msk, SIGPIPE) < 0)
        error("sigaddset() failed");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <network_interface>\n", argv[0]);
        exit(1);
    }

    const char* network_interface = argv[1];

    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, network_interface, IFNAMSIZ-1);
    ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);

    const char* host = inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr); // argv[1];

    printf("Starting chat on %s\n", host);
    printf("Type your message in the following format: <message> >>> <recipient>\n");
    printf("Rename a recipient the following way: <recipient> === <name>\n");
    printf("Press Ctrl+C to exit\n");
    printf("--------------------------------------------------------------------\n");

    ignore_sigpipe();
    run(host, LECHAT_PORT);

    return 0;
}