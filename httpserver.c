#include "asgn2_helper_funcs.h"
#include "connection.h"
#include "debug.h"
#include "response.h"
#include "request.h"
#include "rwlock.h"
#include "queue.h"
#include "linked.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/file.h>
#define OPTIONS "t:"

//queues/threads
queue_t *queue;
pthread_mutex_t mutex;
int t = 4; //default threads

//lock implemention through linked list
struct LinkedList *locker = NULL;

//functions declarations
void handle_connection(int);
void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unsupported(conn_t *);
void handle_file_open_error(const char *uri, int errnum, const Response_t **res);
void handle_file_info(
    int fd, const char *uri, const Response_t **res, size_t *fsize, bool *existed);
rwlock_t *LockIt(LinkedList **front, const char *uri);

//helper function
void *worker_thread() {
    while (1) {
        uintptr_t connfd = 0;
        queue_pop(queue, (void **) &connfd);
        handle_connection(connfd);
        close(connfd);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        warnx("wrong arguments: %s port_num", argv[0]);
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int opt;
    while ((opt = getopt(argc, argv, OPTIONS)) != -1) {
        switch (opt) {
        case 't': t = atoi(optarg); break;
        default: exit(EXIT_FAILURE);
        }
    }

    char *endptr;
    size_t port = (size_t) strtoull(argv[optind], &endptr, 10);
    if (endptr && *endptr != '\0') {
        warnx("invalid port number: %s", argv[optind]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    listener_init(&sock, port);

    queue = queue_new(t);

    //vincent section
    pthread_t threads[t];
    for (int i = 0; i < t; i++) {
        pthread_create(&threads[i], NULL, (void *(*) (void *) ) worker_thread, (void *) threads[i]);
    }

    //vincent section
    while (1) {
        uintptr_t connfd = listener_accept(&sock);
        queue_push(queue, (void *) connfd);
    }

    return EXIT_SUCCESS;
}

rwlock_t *LockIt(LinkedList **front, const char *uri) {
    pthread_mutex_lock(&mutex);

    //acquire lock if it exists
    rwlock_t *Newlock = acquire(*front, uri);

    //if lock doesn't exist create it
    if (Newlock == NULL) {
        Newlock = rwlock_new(N_WAY, 1);
        LinkedList *node = malloc(sizeof(LinkedList));
        if (node != NULL) {
            node->uri = strdup(uri);
            node->lock = Newlock;
            node->nextList = *front;
            *front = node;
        }
    }
    pthread_mutex_unlock(&mutex);
    return Newlock;
}

void handle_connection(int connfd) {
    conn_t *conn = conn_new(connfd);

    const Response_t *res = conn_parse(conn);

    if (res != NULL) {
        conn_send_response(conn, res);
    } else {
        const Request_t *req = conn_get_request(conn);
        if (req == &REQUEST_GET) {
            handle_get(conn);
        } else if (req == &REQUEST_PUT) {
            handle_put(conn);
        } else {
            handle_unsupported(conn);
        }
    }
    conn_delete(&conn);
}

void handle_file_open_error(const char *uri, int errnum, const Response_t **res) {
    debug("%s: %d", uri, errnum);
    if (errnum == EACCES) {
        *res = &RESPONSE_FORBIDDEN;
    } else if (errnum == ENOENT) {
        *res = &RESPONSE_NOT_FOUND;
    } else {
        *res = &RESPONSE_INTERNAL_SERVER_ERROR;
    }
}

void handle_file_info(
    int fd, const char *uri, const Response_t **res, size_t *fsize, bool *existed) {
    struct stat properties;

    if (fstat(fd, &properties) != 0 || properties.st_size < 0) {
        close(fd);
        *res = NULL;
    }

    *fsize = (size_t) properties.st_size;
    *existed = access(uri, F_OK) == 0;

    if (properties.st_mode == S_IFDIR) {
        *res = &RESPONSE_FORBIDDEN;
    }
}

void handle_get(conn_t *conn) {
    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;
    size_t fsize;
    bool existed;

    rwlock_t *get_lock = LockIt(&locker, uri);
    reader_lock(get_lock);
    pthread_mutex_lock(&mutex);
    int fd = open(uri, O_RDONLY);
    if (fd < 0) {
        handle_file_open_error(uri, errno, &res);
        goto out;
    }

    handle_file_info(fd, uri, &res, &fsize, &existed);

    pthread_mutex_unlock(&mutex);

    res = conn_send_file(conn, fd, fsize);
    if (res == NULL && existed) {
        res = &RESPONSE_OK;
    }

    char *id = conn_get_header(conn, "Request-Id");
    fprintf(stderr, "%s,%s,%d,%s\n", "GET", uri, response_get_code(res), (id == NULL) ? "0" : id);
    reader_unlock(acquire(locker, uri));
    close(fd);
    return;

out:
    conn_send_response(conn, res);
}

void handle_unsupported(conn_t *conn) {
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
}

void handle_put(conn_t *conn) {
    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;
    bool existed = access(uri, F_OK) == 0;

    rwlock_t *put_lock = LockIt(&locker, uri);
    writer_lock(put_lock);

    int fd = open(uri, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        handle_file_open_error(uri, errno, &res);
        goto out;
    }

    res = conn_recv_file(conn, fd);

    if (res == NULL && existed) {
        res = &RESPONSE_OK;
    } else if (res == NULL && !existed) {
        res = &RESPONSE_CREATED;
    }

    char *id = conn_get_header(conn, "Request-Id");
    fprintf(stderr, "%s,%s,%d,%s\n", "PUT", uri, response_get_code(res), (id == NULL) ? "0" : id);
    writer_unlock(acquire(locker, uri));
    close(fd);

out:
    conn_send_response(conn, res);
}
