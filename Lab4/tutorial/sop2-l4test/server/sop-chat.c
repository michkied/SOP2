#include "l4-common.h"
#include <pthread.h>

#define BACKLOG_SIZE 10
#define MAX_CLIENT_COUNT 4
#define MAX_EVENTS 10

#define NAME_OFFSET 0
#define NAME_SIZE 64
#define MESSAGE_OFFSET NAME_SIZE
#define MESSAGE_SIZE 448
#define BUFF_SIZE (NAME_SIZE + MESSAGE_SIZE)

volatile sig_atomic_t do_work = 1;

// typedef struct {
//     int fd;
//     char* key;
//     int* client_fds;
// } new_connection_args_t;

void usage(char *program_name) {
    fprintf(stderr, "USAGE: %s port key\n", program_name);
    exit(EXIT_FAILURE);
}

void sigint_handler(int sig) { (void)sig; do_work = 0; }

void add_client(int epoll_descriptor, struct epoll_event event, char* key, int* client_fds) {
    char data[BUFF_SIZE];
    ssize_t size;

    int client_socket = add_new_client(event.data.fd);
    if ((size = bulk_read(client_socket, data, BUFF_SIZE)) < 0)
        ERR("read:");

    int free_spot = -1;
    for (int i = 0; i < MAX_CLIENT_COUNT; i++) {
        if (client_fds[i] == -1) {
            free_spot = i;
            break;
        }
    }

    if (size != BUFF_SIZE || free_spot == -1) {
        if (TEMP_FAILURE_RETRY(close(client_socket)) < 0)
            ERR("close");
        return;
    }

    printf("> %s - %s\n", key, data+NAME_SIZE);

    if (strncmp(key, data+NAME_SIZE, MESSAGE_SIZE)) {
        if (TEMP_FAILURE_RETRY(close(client_socket)) < 0)
            ERR("close");
        return;
    }

    if (bulk_write(client_socket, data, BUFF_SIZE) < 0) {
        if (errno == EPIPE) {
            if (TEMP_FAILURE_RETRY(close(client_socket)) < 0)
                ERR("close");
            return;
        }
        ERR("write:");
    }

    client_fds[free_spot] = client_socket;

    struct epoll_event client_event;
    client_event.events = EPOLLIN;
    client_event.data.fd = client_socket;
    if (epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, client_socket, &client_event) == -1)
    {
        ERR("epoll_ctl: listen_sock");
    }
}

void disconnect_client(int* client_fds, int client_fd) {
    for (int i = 0; i < MAX_CLIENT_COUNT; i++) {
        if (client_fds[i] == client_fd) {
            client_fds[i] = -1;
            break;
        }
    }
    if (TEMP_FAILURE_RETRY(close(client_fd)) < 0)
        ERR("close");
}

void print_connected(int* client_fds) {
    printf("Currently connected:");
    for (int i = 0; i < MAX_CLIENT_COUNT; i++) {
        if (client_fds[i] == -1) continue;
        printf("    %d", client_fds[i]);
    }
    putc('\n', stdout);
}

void* server_work(int fd, char* key, int* client_fds) {
//void* server_work(void *ptr) {
    // new_connection_args_t *args = (new_connection_args_t*)ptr;

    int epoll_descriptor;
    if ((epoll_descriptor = epoll_create1(0)) < 0)
    {
        ERR("epoll_create:");
    }
    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = fd;
    if (epoll_ctl(epoll_descriptor, EPOLL_CTL_ADD, fd, &event) == -1)
    {
        ERR("epoll_ctl: listen_sock");
    }

    int nfds, client_fd;
    char data[BUFF_SIZE];
    ssize_t size;
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);
    while (do_work)
    {
        if ((nfds = epoll_pwait(epoll_descriptor, events, MAX_EVENTS, -1, &oldmask)) <= 0) {
            if (errno == EINTR)
                continue;
            ERR("epoll_pwait");
        }

        for (int n = 0; n < nfds; n++)
        {
            if (events[n].data.fd == fd) {
                add_client(epoll_descriptor, events[n], key, client_fds);
                continue;
            }

            client_fd = events[n].data.fd;

            if ((size = bulk_read(client_fd, data, BUFF_SIZE)) < 0)
                ERR("read:");

            if (size != BUFF_SIZE) {
                printf("Disconnected\n");
                disconnect_client(client_fds, client_fd);
                print_connected(client_fds);
                continue;
            }

            printf("%.*s: %s\n", NAME_SIZE, data, data+MESSAGE_OFFSET);

            for (int i = 0; i < MAX_CLIENT_COUNT; i++) {
                if (client_fds[i] == -1 || client_fds[i] == client_fd) continue;
                if (bulk_write(client_fds[i], data, BUFF_SIZE) < 0) {
                    if (errno == EPIPE || errno == ECONNRESET) {
                        disconnect_client(client_fds, client_fd);
                        print_connected(client_fds);
                        continue;
                    }
                    ERR("write:");
                }
            }
        }
    }
    for (int i = 0; i < MAX_CLIENT_COUNT; i++) {
        if (client_fds[i] == -1) continue;
        if (TEMP_FAILURE_RETRY(close(client_fds[i])) < 0) continue;
    }
    if (TEMP_FAILURE_RETRY(close(epoll_descriptor)) < 0)
        ERR("close");
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        usage(argv[0]);
    }

    uint16_t port = atoi(argv[1]);
    if (port == 0){
        usage(argv[0]);
    }

    char *key = argv[2];

    int client_fds[MAX_CLIENT_COUNT];
    for (int i = 0; i < MAX_CLIENT_COUNT; i++) {
        client_fds[i] = -1;
    }

    if (sethandler(SIG_IGN, SIGPIPE))
        ERR("Seting SIGPIPE:");
    if (sethandler(sigint_handler, SIGINT))
        ERR("Seting SIGINT:");

    int fdTcp = bind_tcp_socket(port, BACKLOG_SIZE);
    int new_flags = fcntl(fdTcp, F_GETFL) | O_NONBLOCK;
    fcntl(fdTcp, F_SETFL, new_flags);

    // new_connection_args_t con_args;
    // con_args.client_fds = client_fds;
    // con_args.fd = fdTcp;
    // con_args.key = key;

    //pthread_t tid;

    // pthread_create(&tid, NULL, server_work, &con_args);
    // pthread_join(tid, NULL);
    server_work(fdTcp, key, client_fds);

    return EXIT_SUCCESS;
}
