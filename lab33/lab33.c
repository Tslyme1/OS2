#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/eventfd.h>

#include "cache.h"
#include "picohttpparser.h"
#include "task_queue.h"
#include "sync_pipe.h"

#define ERROR_INVALID_ARGS 1
#define ERROR_PORT_CONVERSATION 2
#define ERROR_ALLOC 3
#define ERROR_SOCKET_INIT 4
#define ERROR_BIND 5
#define ERROR_LISTEN 6
#define ERROR_PIPE_OPEN 7
#define ERROR_SIG_HANDLER_INIT 8
#define ERROR_TASK_QUEUE_INIT 9
#define ERROR_INIT_SYNC_PIPE 10
#define ERROR_INIT_EVENT_FD 11

#define TIMEOUT 1200
#define START_REQUEST_SIZE BUFSIZ
#define START_RESPONSE_SIZE BUFSIZ
#define LISTEN_NUM 16

int THREAD_POOL_SIZE = 5;

pthread_mutex_t thread_pool_size_mutex;
bool valid_thread_pool_size_mutex = false;

int REAL_THREAD_POOL_SIZE = 0;
task_queue_t *task_queue = NULL;
bool valid_task_queue = false;


pthread_t *tids = NULL;
bool *is_created_thread = NULL;
bool valid_threads_info = false;

bool is_stop = false;

int WRITE_STOP_FD = -1;
int READ_STOP_FD = -1;

void destroyPollFds(struct pollfd *poll_fds, int *poll_last_index) {
    //fprintf(stderr, "destroying poll_fds...\n");
    for (int i = 0; i < *poll_last_index; i++) {
        if (poll_fds[i].fd > 0) {
            close(poll_fds[i].fd);
            poll_fds[i].fd = -1;
        }
    }
    free(poll_fds);
    *poll_last_index = -1;
}

void removeFromPollFds(struct pollfd * poll_fds, int *poll_last_index, int fd) {
    if (fd < 0) {
        return;
    }
    /*fprintf(stderr, "need to remove fd %d from poll_fds\npoll_fds:\n", fd);
    for (int i = 0; i < *poll_last_index; i++) {
        fprintf(stderr, "poll_fds[%d].fd = %d\n", i, poll_fds[i].fd);
    }*/
    int i;
    for (i = 0; i < *poll_last_index; i++) {
        if (poll_fds[i].fd == fd) {
            close(poll_fds[i].fd);
            poll_fds[i].fd = -1;
            poll_fds[i].events = 0;
            poll_fds[i].revents = 0;
            break;
        }
    }
    if (i == *poll_last_index - 1) {
        *poll_last_index -= 1;
    }
    for (i = (int)*poll_last_index - 1; i > 0; i--) {
        if (poll_fds[i].fd == -1) {
            *poll_last_index -= 1;
        }
        else {
            break;
        }
    }
}

cache_list_t *cache_list = NULL;
bool valid_cache = false;
pthread_rwlockattr_t rw_lock_attr;
bool valid_rw_lock_attr = false;

void destroyRwLockAttr() {
    if (valid_rw_lock_attr) {
        pthread_rwlockattr_destroy(&rw_lock_attr);
        valid_rw_lock_attr = false;
    }
}

void destroyCacheList() {
    destroyList(cache_list);
    valid_cache = false;
}

typedef struct client {
    char *request;
    cache_t *cache_record;
    size_t REQUEST_SIZE;
    size_t request_index;

    int write_response_index;
    int thread_fd;

    int fd;
} client_t;

void destroyClients(client_t *clients, size_t *CLIENTS_SIZE, struct pollfd *poll_fds, int *poll_last_index) {
    //fprintf(stderr, "destroying clients...\n");
    for (int i = 0; i < *CLIENTS_SIZE; i++) {
        if (clients[i].request != NULL) {
            free(clients[i].request);
            clients[i].request = NULL;
        }
        if (clients[i].fd != -1) {
            removeFromPollFds(poll_fds, poll_last_index, clients[i].fd);
            clients[i].fd = -1;
        }
        clients[i].thread_fd = -1;
        clients[i].REQUEST_SIZE = 0;
        clients[i].request_index = 0;
        clients[i].cache_record = NULL;
        clients[i].write_response_index = -1;
    }
    //fprintf(stderr, "free clients\n");
    free(clients);
    *CLIENTS_SIZE = 0;
}

typedef struct server {
    cache_t *cache_record;
    int write_request_index;
    int fd;
} server_t;

void destroyServers(server_t *servers, size_t *SERVERS_SIZE, struct pollfd *poll_fds, int *poll_last_index) {
    //fprintf(stderr, "destroying servers...\n");
    for (int i = 0; i < *SERVERS_SIZE; i++) {
        if (servers[i].fd != -1) {
            removeFromPollFds(poll_fds, poll_last_index, servers[i].fd);
            servers[i].cache_record->server_index = -1;
            servers[i].cache_record = NULL;
            servers[i].fd = -1;
        }
    }
    //fprintf(stderr, "free servers...\n");
    free(servers);
    *SERVERS_SIZE = 0;
}

void cleanUp() {
    fprintf(stderr, "\ncleaning up...\n");
    if (!is_stop) {
        is_stop = true;
        sync_pipe_notify(REAL_THREAD_POOL_SIZE);
        for (int i = 0; i < THREAD_POOL_SIZE; i++) {
            if (is_created_thread[i]) {
                pthread_join(tids[i], NULL);
            }
        }
    }
    if (WRITE_STOP_FD != -1) {
        close(WRITE_STOP_FD);
        WRITE_STOP_FD = -1;
    }
    if (valid_cache) {
        destroyCacheList();
    }
    if (valid_rw_lock_attr) {
        destroyRwLockAttr();
    }
    if (valid_task_queue) {
        destroyTaskQueue(task_queue);
        valid_task_queue = false;
    }
    if (valid_threads_info) {
        free(is_created_thread);
        free(tids);
        valid_threads_info = false;
    }
    if (valid_thread_pool_size_mutex) {
        pthread_mutex_destroy(&thread_pool_size_mutex);
        valid_thread_pool_size_mutex = false;
    }
    sync_pipe_close();

}

void initRwLockAttr() {
    if (!valid_rw_lock_attr) {
        pthread_rwlockattr_init(&rw_lock_attr);
        pthread_rwlockattr_setkind_np(&rw_lock_attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
        valid_rw_lock_attr = true;
    }
}

void initEmptyServer(server_t *servers, size_t i) {
    servers[i].fd = -1;
    servers[i].cache_record = NULL;
    servers[i].write_request_index = -1;
}

server_t *initServers(size_t SERVERS_SIZE) {
    server_t *servers = (server_t *) calloc(SERVERS_SIZE, sizeof(server_t));
    if (servers == NULL) {
        fprintf(stderr, "failed to alloc memory for servers\n");
        return NULL;
    }
    for (size_t i = 0; i < SERVERS_SIZE; i++) {
        initEmptyServer(servers, i);
    }
    return servers;
}

void reallocServers(server_t **servers, size_t *SERVERS_SIZE) {
    size_t prev_size = *SERVERS_SIZE;
    *SERVERS_SIZE *= 2;
    *servers = realloc(*servers, *SERVERS_SIZE * sizeof(server_t));
    for (size_t i = prev_size; i < *SERVERS_SIZE; i++) {
        initEmptyServer(*servers, i);
    }
}

int findFreeServer(server_t **servers, size_t *SERVERS_SIZE, int server_fd) {
    if (server_fd < 0) {
        return -1;
    }
    for (int i = 0; i < *SERVERS_SIZE; i++) {
        if ((*servers)[i].fd == -1) {
            (*servers)[i].fd = server_fd;
            return i;
        }
    }
    size_t prev_size = *SERVERS_SIZE;
    reallocServers(servers, SERVERS_SIZE);
    (*servers)[prev_size].fd = server_fd;
    return (int)prev_size;
}

int findServerByFd(server_t *servers, const size_t *SERVERS_SIZE, int fd) {
    if (fd < 0) {
        return -1;
    }
    for (int i = 0; i < *SERVERS_SIZE; i++) {
        if (servers[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

void initEmptyClient(client_t *clients, size_t i) {
    clients[i].fd = -1;
    clients[i].thread_fd = -1;
    clients[i].request_index = 0;
    clients[i].request = NULL;
    clients[i].REQUEST_SIZE = 0;

    clients[i].cache_record = NULL;
    clients[i].write_response_index = -1;
}

client_t *initClients(size_t CLIENTS_SIZE) {
    client_t *clients = (client_t *) calloc(CLIENTS_SIZE, sizeof(client_t));
    if (clients == NULL) {
        fprintf(stderr, "failed to alloc memory for clients\n");
        return NULL;
    }
    for (size_t i = 0; i < CLIENTS_SIZE; i++) {
        initEmptyClient(clients, i);
    }
    return clients;
}

void reallocClients(client_t **clients, size_t *CLIENTS_SIZE) {
    size_t prev_size = *CLIENTS_SIZE;
    *CLIENTS_SIZE *= 2;
    *clients = realloc(*clients, *CLIENTS_SIZE * sizeof(client_t));
    for (size_t i = prev_size; i < *CLIENTS_SIZE; i++) {
        initEmptyClient(*clients, i);
    }
}

int findFreeClient(client_t **clients, size_t *CLIENTS_SIZE, int client_fd) {
    if (client_fd < 0) {
        return -1;
    }
    for (int i = 0; i < *CLIENTS_SIZE; i++) {
        if ((*clients)[i].fd == -1) {
            (*clients)[i].fd = client_fd;
            return i;
        }
    }
    size_t prev_size = *CLIENTS_SIZE;
    reallocClients(clients, CLIENTS_SIZE);
    (*clients)[prev_size].fd = client_fd;
    return (int)prev_size;
}

int findClientByFd(client_t *clients, const size_t *CLIENTS_SIZE, int fd) {
    if (fd < 0) {
        return -1;
    }
    for (int i = 0; i < *CLIENTS_SIZE; i++) {
        if (clients[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

void initEmptyCacheRecord(cache_t *record) {
    if (record == NULL) {
        return;
    }
    record->request = NULL;
    record->response = NULL;
    record->response_index = 0;
    record->RESPONSE_SIZE = 0;
    record->subscribers = NULL;
    record->full = false;
    record->url = NULL;
    record->URL_LEN = 0;
    record->SUBSCRIBERS_SIZE = 0;
    record->server_index = -1;
    record->private = true;
    pthread_rwlock_init(&record->rw_lock, &rw_lock_attr);
    record->valid_rw_lock = true;
    pthread_mutex_init(&record->subs_mutex, NULL);
    record->valid_subs_mutex = true;
    record->valid = false;
}

void initCacheList() {
    cache_list = initList();
    if (cache_list == NULL) {
        cleanUp();
        return;
    }
    valid_cache = true;
}

struct pollfd *initPollFds(size_t POLL_TABLE_SIZE, int *poll_last_index) {
    struct pollfd *poll_fds = (struct pollfd *)calloc(POLL_TABLE_SIZE, sizeof(struct pollfd));
    if (poll_fds == NULL) {
        fprintf(stderr, "failed to alloc memory for poll_fds\n");
        return NULL;
    }
    for (int i = 0; i < POLL_TABLE_SIZE; i++) {
        poll_fds[i].fd = -1;
    }
    *poll_last_index = 0;
    return poll_fds;
}

void reallocPollFds(struct pollfd **poll_fds, size_t *POLL_TABLE_SIZE) {
    size_t prev_size = *POLL_TABLE_SIZE;
    *POLL_TABLE_SIZE *= 2;
    //fprintf(stderr, "realloc poll_fds, new expected size = %lu\n", *POLL_TABLE_SIZE);
    *poll_fds = realloc(*poll_fds, *POLL_TABLE_SIZE * (sizeof(struct pollfd)));
    for (size_t i = prev_size; i < *POLL_TABLE_SIZE; i++) {
        (*poll_fds)[i].fd = -1;
    }
    /*fprintf(stderr, "**************************************\n");
    for (int i = 0; i < *POLL_TABLE_SIZE; i++) {
        fprintf(stderr, "poll_fds[%d] = %d\n", i, (*poll_fds)[i].fd);
    }
    fprintf(stderr, "**************************************\n");
    */
}

void addFdToPollFds(struct pollfd **poll_fds, int *poll_last_index, size_t *POLL_TABLE_SIZE, int fd, short events) {
    if (fd < 0) {
        return;
    }
    for (int i = 0; i < *poll_last_index; i++) {
        if ((*poll_fds)[i].fd == -1) {
            (*poll_fds)[i].fd = fd;
            (*poll_fds)[i].events = events;
            return;
        }
    }
    if (*poll_last_index >= *POLL_TABLE_SIZE) {
        reallocPollFds(poll_fds, POLL_TABLE_SIZE);
    }
    (*poll_fds)[*poll_last_index].fd = fd;
    (*poll_fds)[*poll_last_index].events = events;
    *poll_last_index += 1;
}

int connectToServerHost(char *hostname, int port) {
    if (hostname == NULL || port < 0) {
        return -1;
    }
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        return -1;
    }

    struct hostent *h = gethostbyname(hostname);
    if (h == NULL) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_port = htons(port);
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr, h->h_addr, h->h_length);

    int connect_res = connect(server_sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
    if (connect_res != 0) {
        perror("connect");
        return -1;
    }
    return server_sock;
}

int initListener(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        cleanUp();
        exit(ERROR_SOCKET_INIT);
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    int bind_res = bind(listen_fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
    if (bind_res != 0) {
        perror("bind");
        close(listen_fd);
        cleanUp();
        exit(ERROR_BIND);
    }

    int listen_res = listen(listen_fd, LISTEN_NUM);
    if (listen_res == -1) {
        perror("listen");
        close(listen_fd);
        cleanUp();
        exit(ERROR_LISTEN);
    }
    return listen_fd;
}

void acceptNewClient(int listen_fd) {
    //fprintf(stderr, "accepting new client...\n");
    int new_client_fd = accept(listen_fd, NULL, NULL);
    if (new_client_fd == -1) {
        perror("new client accept");
        return;
    }
    //fprintf(stderr, "making NONBLOCK...\n");
    int fcntl_res = fcntl(new_client_fd, F_SETFL, O_NONBLOCK);
    if (fcntl_res < 0) {
        perror("make new client nonblock");
        close(new_client_fd);
        return;
    }
    //fprintf(stderr, "accept done, adding client_fd %d to queue\n", new_client_fd);
    submitTask(task_queue, new_client_fd);
    sync_pipe_notify(1);
    //fprintf(stderr, "new client %d accepted\n", index);
}

void changeEventForFd(struct pollfd *poll_fds, const int *poll_last_index, int fd, short new_events) {
    if (fd < 0) {
        return;
    }
    for (int i = 0; i < *poll_last_index; i++) {
        if (poll_fds[i].fd == fd) {
            poll_fds[i].events = new_events;
            return;
        }
    }
}

void removeSubscriber(int thread_fd, cache_t *record) {
    if (record == NULL || thread_fd < 0) {
        return;
    }
    fprintf(stderr, "removing subscriber %d...\n", thread_fd);
    pthread_mutex_lock(&record->subs_mutex);
    for (int i = 0; i < record->SUBSCRIBERS_SIZE; i++) {
        if (record->subscribers[i] == thread_fd) {
            record->subscribers[i] = -1;
        }
    }
    pthread_mutex_unlock(&record->subs_mutex);
}

void disconnectClient(client_t *clients, size_t CLIENTS_SIZE, int client_num, struct pollfd *poll_fds,
        int *poll_last_index) {
    if (client_num < 0 || client_num >= CLIENTS_SIZE) {
        return;
    }
    // fprintf(stderr, "disconnecting client %d...\n", client_num);
    if (clients[client_num].request != NULL) {
        //free(clients[client_num].request);
        //clients[client_num].request = NULL;
        memset(clients[client_num].request, 0, clients[client_num].request_index);
        clients[client_num].request_index = 0;
        //clients[client_num].REQUEST_SIZE = 0;
    }
    if (clients[client_num].cache_record != NULL) {
        //removeSubscriber(clients[client_num].thread_fd, clients[client_num].cache_record);
        clients[client_num].cache_record = NULL;
        clients[client_num].write_response_index = -1;
    }
    if (clients[client_num].fd != -1) {
        removeFromPollFds(poll_fds, poll_last_index, clients[client_num].fd);
        clients[client_num].fd = -1;
    }
    //fprintf(stderr, "disconnect client %d ended\n", client_num);
}

int addSubscriber(int thread_fd, cache_t *record) {
    if (record == NULL || thread_fd < 0 || !record->valid) {
        return -1;
    }
    pthread_mutex_lock(&record->subs_mutex);
    if (record->SUBSCRIBERS_SIZE == 0) {
        record->SUBSCRIBERS_SIZE = 4;
        record->subscribers = (int *) calloc(record->SUBSCRIBERS_SIZE, sizeof(int));
        if (record->subscribers == NULL) {
            pthread_mutex_unlock(&record->subs_mutex);
            return -1;
        }
        for (int i = 0; i < record->SUBSCRIBERS_SIZE; i++) {
            record->subscribers[i] = -1;
        }
    }
    for (int i = 0; i < record->SUBSCRIBERS_SIZE; i++) {
        if (record->subscribers[i] == -1 || record->subscribers[i] == thread_fd) {
            record->subscribers[i] = thread_fd;
            pthread_mutex_unlock(&record->subs_mutex);
            return 0;
        }
    }
    size_t prev_index = record->SUBSCRIBERS_SIZE;
    record->SUBSCRIBERS_SIZE *= 2;
    record->subscribers = realloc(record->subscribers, record->SUBSCRIBERS_SIZE * sizeof(int));
    for (size_t i = prev_index; i < record->SUBSCRIBERS_SIZE; i++) {
        record->subscribers[i] = -1;
    }
    record->subscribers[prev_index] = thread_fd;
    pthread_mutex_unlock(&record->subs_mutex);
    return 0;
}

void notifySubscribers(cache_t *record) {
    pthread_mutex_lock(&record->subs_mutex);
    for (int i = 0; i < record->SUBSCRIBERS_SIZE; i++) {
        if (record->subscribers[i] != -1) {
            uint64_t u = 1;
            write(record->subscribers[i], &u, sizeof(u));
        }
    }
    pthread_mutex_unlock(&record->subs_mutex);
}

void disconnectServer(server_t *servers, size_t SERVERS_SIZE, int server_num, struct pollfd *poll_fds,
        int *poll_last_index) {
    if (server_num < 0 || server_num > SERVERS_SIZE) {
        return;
    }
    servers[server_num].write_request_index = -1;
    if (servers[server_num].cache_record != NULL) {
        servers[server_num].cache_record->server_index = -1;
        notifySubscribers(servers[server_num].cache_record);
        servers[server_num].cache_record = NULL;
    }
    if (servers[server_num].fd != -1) {
        removeFromPollFds(poll_fds, poll_last_index, servers[server_num].fd);
        servers[server_num].fd = -1;
    }
}

void freeCacheRecord(cache_t *record, server_t *servers, size_t SERVERS_SIZE, struct pollfd *poll_fds,
        int *poll_last_index) {
    if (record == NULL) {
        return;
    }
    record->private = true;
    if (record->url != NULL) {
        free(record->url);
        record->url = NULL;
        record->URL_LEN = 0;
    }
    if (record->request != NULL) {
        free(record->request);
        record->request = NULL;
    }
    record->REQUEST_SIZE = 0;
    if (record->response != NULL) {
        free(record->response);
        record->response = NULL;
    }
    record->response_index = 0;
    record->RESPONSE_SIZE = 0;
    if (record->subscribers != NULL) {
        pthread_mutex_lock(&record->subs_mutex);
        free(record->subscribers);
        record->subscribers = NULL;
        record->SUBSCRIBERS_SIZE = 0;
        pthread_mutex_unlock(&record->subs_mutex);
    }
    if (record->valid_subs_mutex) {
        pthread_mutex_destroy(&record->subs_mutex);
        record->valid_subs_mutex = false;
    }
    if (record->server_index != -1) {
        disconnectServer(servers, SERVERS_SIZE, record->server_index, poll_fds, poll_last_index);
    }
    if (record->valid_rw_lock) {
        pthread_rwlock_destroy(&record->rw_lock);
        record->valid_rw_lock = false;
    }
    record->valid = false;
}

void printCacheRecord(cache_t *record) {
    if (record == NULL) {
        fprintf(stderr, "cache record is NULL\n");
        return;
    }
    fprintf(stderr, "cache record:\n");
    if (record->valid) {
        fprintf(stderr, "valid, ");
    }
    else {
        fprintf(stderr, "NOT valid, ");
    }
    if (record->private) {
        fprintf(stderr, "private, ");
    }
    else {
        fprintf(stderr, "public, ");
    }
    if (record->full) {
        fprintf(stderr, "full\n");
    }
    else {
        fprintf(stderr, "NOT full\n");
    }
    fprintf(stderr, "server_index = %d\n", record->server_index);
    fprintf(stderr, "REQ_SIZE = %lu\n", record->REQUEST_SIZE);
    if (record->valid_rw_lock) {
        fprintf(stderr, "rw_lock = valid, ");
    }
    else {
        fprintf(stderr, "rw_lock = NOT valid, ");
    }
    fprintf(stderr, "rsp_ind = %lu, RSP_SIZE = %lu\n", record->response_index, record->RESPONSE_SIZE);
    if (record->valid_subs_mutex) {
        fprintf(stderr, "subs_mutex = valid, ");
        fprintf(stderr, "SUBS_SIZE = %lu\n", record->SUBSCRIBERS_SIZE);
        pthread_mutex_lock(&record->subs_mutex);
        for (int i = 0; i < record->SUBSCRIBERS_SIZE; i++) {
            fprintf(stderr, "%d ", record->subscribers[i]);
        }
        pthread_mutex_unlock(&record->subs_mutex);
        fprintf(stderr, "\n\n");
    }
    else {
        fprintf(stderr, "subs_mutex = NOT valid, ");
        fprintf(stderr, "SUBS_SIZE = %lu\n\n", record->SUBSCRIBERS_SIZE);
    }
}

void printCacheList() {
    pthread_mutex_lock(&cache_list->mutex);
    cache_node_t *list_nodes = cache_list->head;
    fprintf(stderr, "printing cache...\n");
    while (list_nodes != NULL) {
        printCacheRecord(list_nodes->record);
        list_nodes = list_nodes->next;
    }
    pthread_mutex_unlock(&cache_list->mutex);
}

void findAndAddCacheRecord(char *url, size_t url_len, client_t *clients, size_t CLIENTS_SIZE, int client_num,
                           char *host, int REQUEST_SIZE, struct pollfd **poll_fds, int *poll_last_index,
                                   size_t *POLL_TABLE_SIZE, server_t **servers, size_t *SERVERS_SIZE) {
    //fprintf(stderr, "adding client %d to cache record\nwith url: %.*s\n", client_num, (int)url_len, url);
    pthread_mutex_lock(&cache_list->mutex);
    cache_node_t *list_nodes = cache_list->head;
    cache_node_t *prev_node = NULL;
    while (list_nodes != NULL) {
        if (!list_nodes->record->valid) {
            cache_node_t *next_node = list_nodes->next;
            freeCacheRecord(list_nodes->record, *servers, *SERVERS_SIZE, *poll_fds, poll_last_index);
            free(list_nodes->record);
            free(list_nodes);
            list_nodes = next_node;
            continue;
        }
        if (list_nodes->record->URL_LEN == url_len && strncmp(list_nodes->record->url, url, url_len) == 0) {
            printf("getting %s from cache\n", url);
            clients[client_num].cache_record = list_nodes->record;
            addSubscriber(clients[client_num].thread_fd, list_nodes->record);
            clients[client_num].write_response_index = 0;
            pthread_rwlock_rdlock(&clients[client_num].cache_record->rw_lock);
            if (clients[client_num].cache_record->response_index != 0) {
                changeEventForFd(*poll_fds, poll_last_index, clients[client_num].fd, POLLIN | POLLOUT);
            }
            pthread_rwlock_unlock(&clients[client_num].cache_record->rw_lock);
            pthread_mutex_unlock(&cache_list->mutex);
            //fprintf(stderr, "client %d added to existing cache record\n", client_num);
            return;
        }
        pthread_mutex_unlock(&cache_list->mutex);

        pthread_mutex_lock(&cache_list->mutex);
        prev_node = list_nodes;
        list_nodes = list_nodes->next;
    }
    cache_node_t *new_node = (cache_node_t *) calloc(1, sizeof(cache_node_t));
    if (new_node == NULL) {
        pthread_mutex_unlock(&cache_list->mutex);
        fprintf(stderr, "failed to add client %d to cache (can't create new node)\n", client_num);
        disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
        return;
    }
    new_node->record = (cache_t *)calloc(1, sizeof(cache_t));
    if (new_node->record == NULL) {
        pthread_mutex_unlock(&cache_list->mutex);
        fprintf(stderr, "failed to add client %d to cache (can't create new record)\n", client_num);
        free(new_node);
        disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
        return;
    }
    new_node->next = NULL;
    initEmptyCacheRecord(new_node->record);
    new_node->record->url = url;
    new_node->record->URL_LEN = url_len;
    new_node->record->valid = true;
    if (prev_node == NULL) {
        cache_list->head = new_node;
    }
    else {
        prev_node->next = new_node;
    }
    pthread_mutex_unlock(&cache_list->mutex);
    //fprintf(stderr, "node added to list\n");

    new_node->record->request = (char *)calloc(REQUEST_SIZE, sizeof(char));
    if (new_node->record->request == NULL) {
        freeCacheRecord(new_node->record, *servers, *SERVERS_SIZE, *poll_fds, poll_last_index);
        disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
        return;
    }
    memcpy(new_node->record->request, clients[client_num].request, REQUEST_SIZE);
    new_node->record->REQUEST_SIZE = REQUEST_SIZE;
    int server_fd = connectToServerHost(host, 80);
    if (server_fd == -1) {
        fprintf(stderr, "failed to connect to remote host: %s\n", host);
        freeCacheRecord(new_node->record, *servers, *SERVERS_SIZE, *poll_fds, poll_last_index);
        disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
        free(host);
        return;
    }
    int fcntl_res = fcntl(server_fd, F_SETFL, O_NONBLOCK);
    if (fcntl_res < 0) {
        perror("make new server fd nonblock");
        close(server_fd);
        freeCacheRecord(new_node->record, *servers, *SERVERS_SIZE, *poll_fds, poll_last_index);
        disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
        free(host);
        return;
    }
    //fprintf(stderr, "connected to remote host with fd = %d\n", server_fd);
    free(host);
    //fprintf(stderr, "looking for free server record...\n");
    int server_num = findFreeServer(servers, SERVERS_SIZE, server_fd);
    (*servers)[server_num].cache_record = new_node->record;
    (*servers)[server_num].write_request_index = 0;
    addFdToPollFds(poll_fds, poll_last_index, POLL_TABLE_SIZE, server_fd, POLLIN | POLLOUT);

    new_node->record->server_index = server_num;

    clients[client_num].cache_record = new_node->record;
    addSubscriber(clients[client_num].thread_fd, new_node->record);
    clients[client_num].write_response_index = 0;
}

void shiftRequest(client_t *clients, size_t CLIENTS_SIZE, int client_num, int pret) {
    if (client_num < 0 || client_num >= CLIENTS_SIZE || pret < 0 || clients[client_num].fd < 0 ||
        clients[client_num].request == NULL || clients[client_num].request_index == 0) {
        return;
    }
    for (int i = pret; i < clients[client_num].request_index; i++) {
        clients[client_num].request[i] = clients[client_num].request[i - pret];
    }
    memset(&clients[client_num].request[clients[client_num].request_index - pret], 0, pret);
    clients[client_num].request_index -= pret;
}

void readFromClient(client_t *clients, size_t CLIENTS_SIZE, int client_num, struct pollfd **poll_fds,
        int *poll_last_index, size_t *POLL_TABLE_SIZE, server_t **servers, size_t *SERVERS_SIZE) {
    //fprintf(stderr, "read from client %d\n", client_num);
    if (client_num < 0 || client_num > CLIENTS_SIZE || clients[client_num].fd == -1) {
        return;
    }
    char buf[BUFSIZ];
    ssize_t was_read = read(clients[client_num].fd, buf, BUFSIZ);
    if (was_read < 0) {
        fprintf(stderr, "error in client %d ", client_num);
        perror("read");
        disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
        return;
    }
    else if (was_read == 0) {
        // fprintf(stderr, "client %d closed connection\n", client_num);
        disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
        return;
    }
    if (clients[client_num].REQUEST_SIZE == 0) {
        clients[client_num].REQUEST_SIZE = START_REQUEST_SIZE;
        clients[client_num].request = (char *)calloc(clients[client_num].REQUEST_SIZE, sizeof(char));
        if (clients[client_num].request == NULL) {
            fprintf(stderr, "calloc returned NULL\n");
            disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
            return;
        }
    }
    if (clients[client_num].request_index + was_read >= clients[client_num].REQUEST_SIZE) {
        clients[client_num].REQUEST_SIZE *= 2;
        clients[client_num].request = realloc(clients[client_num].request,
                                                    clients[client_num].REQUEST_SIZE * sizeof(char));
    }
    memcpy(&clients[client_num].request[clients[client_num].request_index], buf, was_read);
    clients[client_num].request_index += was_read;
    char *method;
    char *path;
    size_t method_len, path_len;
    int minor_version;
    size_t num_headers = 100;
    struct phr_header headers[num_headers];
    int pret = phr_parse_request(clients[client_num].request, clients[client_num].request_index,
                                 (const char **)&method, &method_len, (const char **)&path, &path_len,
                                 &minor_version, headers, &num_headers, 0);
    if (pret > 0) {
        if (strncmp(method, "GET", method_len) != 0) {
            disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
            return;
        }
        size_t url_len = path_len;
        char *url = calloc(url_len, sizeof(char));
        if (url == NULL) {
            disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
            return;
        }
        memcpy(url, path, path_len);


        char *host = NULL;
        for (size_t i = 0; i < num_headers; i++) {
            if (strncmp(headers[i].name, "Host", 4) == 0) {
                host = calloc(headers[i].value_len + 1, sizeof(char));
                if (host == NULL) {
                    free(url);
                    disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
                    return;
                }
                memcpy(host, headers[i].value, headers[i].value_len);
                break;
            }
        }
        if (host == NULL) {
            free(url);
            disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
            return;
        }
        findAndAddCacheRecord(url, url_len, clients, CLIENTS_SIZE, client_num, host, pret, poll_fds, poll_last_index,
                              POLL_TABLE_SIZE, servers, SERVERS_SIZE);

        shiftRequest(clients, CLIENTS_SIZE, client_num, pret);
        //printCacheRecord(clients[client_num].cache_record);
    }
    else if (pret == -1) {
        disconnectClient(clients, CLIENTS_SIZE, client_num, *poll_fds, poll_last_index);
    }
}


void writeToServer(server_t *servers, size_t SERVERS_SIZE, int server_num, struct pollfd *poll_fds,
        int *poll_last_index) {
    //fprintf(stderr, "write to server %d, SERVERS_SIZE = %lu\n", server_num, SERVERS_SIZE);
    if (server_num < 0 || server_num >= SERVERS_SIZE || servers[server_num].fd == -1) {
        return;
    }
    ssize_t written = write(servers[server_num].fd,
                            &servers[server_num].cache_record->request[servers[server_num].write_request_index],
                            servers[server_num].cache_record->REQUEST_SIZE -
                                servers[server_num].write_request_index);
    //fprintf(stderr, "written %ld to server %d\n", written, server_num);
    if (written < 0) {
        fprintf(stderr, "error in server %d ", server_num);
        perror("write");
        disconnectServer(servers, SERVERS_SIZE, server_num, poll_fds, poll_last_index);
        return;
    }
    servers[server_num].write_request_index += (int)written;
    if (servers[server_num].write_request_index == servers[server_num].cache_record->REQUEST_SIZE) {
        changeEventForFd(poll_fds, poll_last_index, servers[server_num].fd, POLLIN);
    }
}

void readFromServer(server_t *servers, size_t SERVERS_SIZE, int server_num, struct pollfd *poll_fds,
        int *poll_last_index) {
    //fprintf(stderr, "read from server %d\n", server_num);
    if (server_num < 0 || server_num >= SERVERS_SIZE || servers[server_num].fd == -1) {
        return;
    }
    char buf[BUFSIZ];
    
    ssize_t was_read = read(servers[server_num].fd, buf, BUFSIZ);
    //fprintf(stderr, "server %d was_read = %ld\n", server_num, was_read);
    if (was_read < 0) {
        fprintf(stderr, "error in server %d ", server_num);
        perror("read");
        return;
    }
    else if (was_read == 0) {
        servers[server_num].cache_record->full = true;
        pthread_rwlock_wrlock(&servers[server_num].cache_record->rw_lock);

        servers[server_num].cache_record->response = realloc(
                servers[server_num].cache_record->response,
                servers[server_num].cache_record->response_index * sizeof(char));
        servers[server_num].cache_record->RESPONSE_SIZE = servers[server_num].cache_record->response_index;

        pthread_rwlock_unlock(&servers[server_num].cache_record->rw_lock);
        notifySubscribers(servers[server_num].cache_record);
        disconnectServer(servers, SERVERS_SIZE, server_num, poll_fds, poll_last_index);
        return;
    }
    pthread_rwlock_wrlock(&servers[server_num].cache_record->rw_lock);
    if (servers[server_num].cache_record->RESPONSE_SIZE == 0) {
        servers[server_num].cache_record->RESPONSE_SIZE = START_RESPONSE_SIZE;
        servers[server_num].cache_record->response = (char *)calloc(START_RESPONSE_SIZE, sizeof(char));
        if (servers[server_num].cache_record->response == NULL) {
            disconnectServer(servers, SERVERS_SIZE, server_num, poll_fds, poll_last_index);
            pthread_rwlock_unlock(&servers[server_num].cache_record->rw_lock);
            return;
        }
    }
    if (was_read + servers[server_num].cache_record->response_index >=
        servers[server_num].cache_record->RESPONSE_SIZE) {
        servers[server_num].cache_record->RESPONSE_SIZE *= 2;
        servers[server_num].cache_record->response = realloc(
                servers[server_num].cache_record->response,
                servers[server_num].cache_record->RESPONSE_SIZE * sizeof(char));
    }
    memcpy(&servers[server_num].cache_record->response[servers[server_num].cache_record->response_index],
           buf, was_read);
    size_t prev_len = servers[server_num].cache_record->response_index;
    if (prev_len == 0) {
        printf("reading %s from server\n", servers[server_num].cache_record->url);
    }
    servers[server_num].cache_record->response_index += was_read;
    pthread_rwlock_unlock(&servers[server_num].cache_record->rw_lock);
    int minor_version, status;
    char *msg;
    size_t msg_len;
    size_t num_headers = 100;
    struct phr_header headers[num_headers];
    pthread_rwlock_rdlock(&servers[server_num].cache_record->rw_lock);
    int pret = phr_parse_response(servers[server_num].cache_record->response,
                                  servers[server_num].cache_record->response_index,
                                  &minor_version, &status, (const char **)&msg, &msg_len, headers,
                                  &num_headers, prev_len);
    pthread_rwlock_unlock(&servers[server_num].cache_record->rw_lock);
    notifySubscribers(servers[server_num].cache_record);
    
    if (pret > 0) {
        if (status >= 200 && status < 300) {
            servers[server_num].cache_record->private = false;
        }
    }
}


void writeToClient(client_t *clients, size_t CLIENTS_SIZE, int client_num, struct pollfd *poll_fds,
        int *poll_last_index, server_t *servers, size_t SERVERS_SIZE) {
    
    if (client_num < 0 || client_num >= CLIENTS_SIZE || clients[client_num].fd == -1) {
        fprintf(stderr, "invalid client_num %d\n", client_num);
        return;
    }
    if (clients[client_num].cache_record == NULL) {
        fprintf(stderr, "client %d cache record is NULL\n", client_num);
        disconnectClient(clients, CLIENTS_SIZE, client_num, poll_fds, poll_last_index);
        return;
    }
    
    if (clients[client_num].cache_record->server_index == -1 &&
        !clients[client_num].cache_record->full) {
        //fprintf(stderr, "invalid cache record detected by client %d\n", client_num);
        freeCacheRecord(clients[client_num].cache_record, servers, SERVERS_SIZE, poll_fds, poll_last_index);
        disconnectClient(clients, CLIENTS_SIZE, client_num, poll_fds, poll_last_index);
        return;
    }
    //fprintf(stderr, "acquire rdlock by client %d...\n", client_num);
    pthread_rwlock_rdlock(&clients[client_num].cache_record->rw_lock);
    
    ssize_t written = write(clients[client_num].fd,
                            &clients[client_num].cache_record->response[clients[client_num].write_response_index],
                            clients[client_num].cache_record->response_index -
                                clients[client_num].write_response_index);
    pthread_rwlock_unlock(&clients[client_num].cache_record->rw_lock);
    //fprintf(stderr, "written %ld to client %d\n", written, client_num);
    if (written < 0) {
        fprintf(stderr, "error in client %d ", client_num);
        perror("write");
        disconnectClient(clients, CLIENTS_SIZE, client_num, poll_fds, poll_last_index);
        return;
    }
    clients[client_num].write_response_index += (int)written;
    if (clients[client_num].write_response_index == clients[client_num].cache_record->response_index) {
        changeEventForFd(poll_fds, poll_last_index, clients[client_num].fd, POLLIN);
    }

}

static void sigCatch(int sig) {
    if (sig == SIGINT) {
        if (WRITE_STOP_FD != -1) {
            char a = 'a';
            write(WRITE_STOP_FD, &a, 1);
            close(WRITE_STOP_FD);
            WRITE_STOP_FD = -1;
        }
    }
}

void *threadFunc(void *arg) {
    //fprintf(stderr, "starting pooled thread...\n");
    int thread_fd = eventfd(0, EFD_NONBLOCK);
    if (thread_fd < 0) {
        pthread_mutex_lock(&thread_pool_size_mutex);
        REAL_THREAD_POOL_SIZE -= 1;
        pthread_mutex_unlock(&thread_pool_size_mutex);
        pthread_exit((void *) ERROR_INIT_EVENT_FD);
    }
    //fprintf(stderr, "pooled thread: event_fd opened\n");
    size_t POLL_TABLE_SIZE = 8;
    int poll_last_index = -1;
    struct pollfd *poll_fds = initPollFds(POLL_TABLE_SIZE, &poll_last_index);
    if (poll_fds == NULL) {
        pthread_mutex_lock(&thread_pool_size_mutex);
        REAL_THREAD_POOL_SIZE -= 1;
        pthread_mutex_unlock(&thread_pool_size_mutex);
        close(thread_fd);
        pthread_exit((void *) ERROR_ALLOC);
    }
    //fprintf(stderr, "pooled thread: poll_fds created\n");
    addFdToPollFds(&poll_fds, &poll_last_index, &POLL_TABLE_SIZE, get_rfd_spipe(), POLLIN);
    addFdToPollFds(&poll_fds, &poll_last_index, &POLL_TABLE_SIZE, thread_fd, POLLIN);

    size_t CLIENTS_SIZE = 8;
    client_t *clients = initClients(CLIENTS_SIZE);
    if (clients == NULL) {
        pthread_mutex_lock(&thread_pool_size_mutex);
        REAL_THREAD_POOL_SIZE -= 1;
        pthread_mutex_unlock(&thread_pool_size_mutex);
        destroyPollFds(poll_fds, &poll_last_index);
        pthread_exit((void *) ERROR_ALLOC);
    }
    //fprintf(stderr, "pooled thread: clients created\n");
    size_t SERVERS_SIZE = 4;
    server_t *servers = initServers(SERVERS_SIZE);
    if (servers == NULL) {
        pthread_mutex_lock(&thread_pool_size_mutex);
        REAL_THREAD_POOL_SIZE -= 1;
        pthread_mutex_unlock(&thread_pool_size_mutex);
        destroyClients(clients, &CLIENTS_SIZE, poll_fds, &poll_last_index);
        destroyPollFds(poll_fds, &poll_last_index);
        pthread_exit((void *) ERROR_ALLOC);
    }
    //fprintf(stderr, "pooled thread: servers created\n");

    while (!is_stop) {
        //fprintf(stderr, "pooled thread poll()\n");
        int poll_res = poll(poll_fds, poll_last_index, TIMEOUT * 1000);
        if (is_stop) {
            break;
        }
        if (poll_res < 0) {
            perror("poll");
            break;
        }
        else if (poll_res == 0) {
            fprintf(stdout, "proxy timeout\n");
            break;
        }
        int num_handled_fd = 0;
        size_t i = 0;
        size_t prev_last_index = poll_last_index;
        /*fprintf(stderr, "pooled thread: poll_res = %d, poll_last_index = %d\n", poll_res, poll_last_index);
        for (int j = 0; j < prev_last_index; j++) {
            fprintf(stderr, "poll_fds[%d] = %d : ", j, poll_fds[j].fd);
            if (poll_fds[j].revents & POLLIN) {
                fprintf(stderr, "POLLIN ");
            }
            if (poll_fds[j].revents & POLLOUT) {
                fprintf(stderr, "POLLOUT ");
            }
            fprintf(stderr, "\n");
        }*/
        while (num_handled_fd < poll_res && i < prev_last_index && !is_stop) {
            if (poll_fds[i].fd == get_rfd_spipe() && (poll_fds[i].revents & POLLIN)) {
                char s;
                ssize_t read_res = read(poll_fds[i].fd, &s, 1);
                if (read_res == 0) {
                    is_stop = true;
                    break;
                }
                if (read_res > 0) {
                    int new_client_fd = popTask(task_queue);
                    //fprintf(stderr, "preparing to add fd %d to local clients\n", new_client_fd);
                    if (new_client_fd != -1) {
                        int index = findFreeClient(&clients, &CLIENTS_SIZE, new_client_fd);
                        addFdToPollFds(&poll_fds, &poll_last_index, &POLL_TABLE_SIZE, new_client_fd, POLLIN);
                        clients[index].thread_fd = thread_fd;
                        // fprintf(stderr, "client %d added to local poll_fds\n", index);
                        /*for (int j = 0; j < poll_last_index; j++) {
                            fprintf(stderr, "poll_fds[%d].fd = %d waiting: ", j, poll_fds[j].fd);
                            if (poll_fds[j].events & POLLIN) {
                                fprintf(stderr, "POLLIN ");
                            }
                            if (poll_fds[j].events & POLLOUT) {
                                fprintf(stderr, "POLLOUT ");
                            }
                            fprintf(stderr, "\n");
                        }*/
                    }
                }
                num_handled_fd += 1;
                i += 1;
                continue;
            }
            if (poll_fds[i].fd == thread_fd && (poll_fds[i].revents & POLLIN)) {
                uint64_t u;
                read(thread_fd, &u, sizeof(u));
                //ssize_t read_res =
                //fprintf(stderr, "woke up pooled thread because of cache read_res = %ld\n", read_res);
                for (int j = 0; j < CLIENTS_SIZE; j++) {
                    if (clients[j].fd != -1 && clients[j].cache_record != NULL) {
                        pthread_rwlock_rdlock(&clients[j].cache_record->rw_lock);
                        if (clients[j].cache_record->response_index > clients[j].write_response_index) {
                            pthread_rwlock_unlock(&clients[j].cache_record->rw_lock);
                            changeEventForFd(poll_fds, &poll_last_index, clients[j].fd, POLLIN | POLLOUT);
                            if (clients[j].write_response_index < 0) {
                                clients[j].write_response_index = 0;
                            }
                        }
                        else {
                            pthread_rwlock_unlock(&clients[j].cache_record->rw_lock);
                        }

                    }
                }
                num_handled_fd += 1;
                i += 1;
                continue;
            }
            int client_num = findClientByFd(clients, &CLIENTS_SIZE, poll_fds[i].fd);
            int server_num = -1;
            if (client_num == -1) {
                server_num = findServerByFd(servers, &SERVERS_SIZE, poll_fds[i].fd);
            }
            //fprintf(stderr, "poll_fds[%lu].fd = %d, client %d, server %d\n", i, poll_fds[i].fd, client_num, server_num);
            bool handled = false;
            if (poll_fds[i].revents & POLLIN) {
                if (client_num != -1) {
                    readFromClient(clients, CLIENTS_SIZE, client_num, &poll_fds, &poll_last_index, &POLL_TABLE_SIZE,
                                   &servers, &SERVERS_SIZE);
                }
                else if (server_num != -1) {
                    readFromServer(servers, SERVERS_SIZE, server_num, poll_fds, &poll_last_index);
                }
                handled = true;
            }
            if (poll_fds[i].revents & POLLOUT) {
                if (client_num != -1) {
                    writeToClient(clients, CLIENTS_SIZE, client_num, poll_fds, &poll_last_index, servers, SERVERS_SIZE);
                }
                else if (server_num != -1) {
                    writeToServer(servers, SERVERS_SIZE, server_num, poll_fds, &poll_last_index);
                }
                handled = true;
            }
            /*if (client_num == -1 && server_num == -1 && poll_fds[i].fd != thread_fd && poll_fds[i].fd != get_rfd_spipe()) {
                removeFromPollFds(poll_fds, &poll_last_index, poll_fds[i].fd);
            }*/
            if (handled) {
                num_handled_fd += 1;
            }
            i += 1;
        }

    }
    fprintf(stderr, "stopping thread...\n");
    destroyServers(servers, &SERVERS_SIZE, poll_fds, &poll_last_index);
    destroyClients(clients, &CLIENTS_SIZE, poll_fds, &poll_last_index);
    destroyPollFds(poll_fds, &poll_last_index);
    pthread_exit((void *)0);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Error wrong amount of arguments\nexpected:\n1) thread poll size\n2) port\n");
        exit(ERROR_INVALID_ARGS);
    }
    char *invalid_sym;
    errno = 0;
    THREAD_POOL_SIZE = (int)strtol(argv[1], &invalid_sym, 10);
    if (errno != 0 || *invalid_sym != '\0') {
        fprintf(stderr, "Error wrong TREAD_POOL_SIZE\n");
        THREAD_POOL_SIZE = 5;
    }
    errno = 0;
    int port = (int)strtol(argv[2], &invalid_sym, 10);
    if (errno != 0 || *invalid_sym != '\0') {
        fprintf(stderr, "Error wrong port\n");
        exit(ERROR_PORT_CONVERSATION);
    }
    pthread_mutex_init(&thread_pool_size_mutex, NULL);
    valid_thread_pool_size_mutex = true;

    tids = (pthread_t *)calloc(THREAD_POOL_SIZE, sizeof(pthread_t));
    if (tids == NULL) {
        fprintf(stderr, "failed to alloc memory for tids\n");
        exit(ERROR_ALLOC);
    }
    is_created_thread = (bool *) calloc(THREAD_POOL_SIZE, sizeof(bool));
    if (is_created_thread == NULL) {
        fprintf(stderr, "failed to alloc memory for thread creation results\n");
        free(tids);
        exit(ERROR_ALLOC);
    }
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        is_created_thread[i] = false;
    }
    valid_threads_info = true;

    task_queue = initTaskQueue();
    if (task_queue == NULL) {
        fprintf(stderr, "failed to init task queue\n");
        exit(ERROR_TASK_QUEUE_INIT);
    }
    valid_task_queue = true;

    int sync_pipe_res = sync_pipe_init();
    if (sync_pipe_res != 0) {
        fprintf(stderr, "failed to init sync pipe\n");
        cleanUp();
        exit(ERROR_INIT_SYNC_PIPE);
    }

    size_t POLL_TABLE_SIZE = 4;
    int poll_last_index = -1;
    struct pollfd *poll_fds = initPollFds(POLL_TABLE_SIZE, &poll_last_index);
    if (poll_fds == NULL) {
        cleanUp();
        exit(ERROR_ALLOC);
    }
    int pipe_fds[2];
    int pipe_res = pipe(pipe_fds);
    if (pipe_res != 0) {
        perror("pipe:");
        exit(ERROR_PIPE_OPEN);
    }
    READ_STOP_FD = pipe_fds[0];
    WRITE_STOP_FD = pipe_fds[1];
    addFdToPollFds(&poll_fds, &poll_last_index, &POLL_TABLE_SIZE, READ_STOP_FD, POLLIN);

    struct sigaction sig_act = { 0 };
    sig_act.sa_handler = sigCatch;
    sigemptyset(&sig_act.sa_mask);
    int sigact_res = sigaction(SIGINT, &sig_act, NULL);
    if (sigact_res != 0) {
        perror("sigaction");
        cleanUp();
        exit(ERROR_SIG_HANDLER_INIT);
    }

    sigset_t old_set;
    sigset_t thread_set;
    sigemptyset(&thread_set);
    sigaddset(&thread_set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &thread_set, &old_set);
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        int create_res = pthread_create(&tids[i], NULL, threadFunc, NULL);
        if (create_res != 0) {
            is_created_thread[i] = false;
        }
        else {
            pthread_mutex_lock(&thread_pool_size_mutex);
            REAL_THREAD_POOL_SIZE += 1;
            pthread_mutex_unlock(&thread_pool_size_mutex);
            is_created_thread[i] = true;
        }
    }
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);

    initRwLockAttr();
    initCacheList();

    int listen_fd = initListener(port);
    addFdToPollFds(&poll_fds, &poll_last_index, &POLL_TABLE_SIZE, listen_fd, POLLIN);

    while (!is_stop) {
        //fprintf(stderr, "main poll()\n");
        int poll_res = poll(poll_fds, poll_last_index, TIMEOUT * 1000);
        if (poll_res < 0) {
            perror("poll");
            break;
        }
        else if (poll_res == 0) {
            fprintf(stdout, "proxy timeout\n");
            break;
        }
        int num_handled_fd = 0;
        size_t i = 0;
        size_t prev_last_index = poll_last_index;
        /*fprintf(stderr, "main: poll_res = %d, prev_last_index = %lu\n", poll_res, prev_last_index);
        for (int j = 0; j < prev_last_index; j++) {
            fprintf(stderr, "poll_fds[%d] = %d : ", j, poll_fds[j].fd);
            if (poll_fds[j].revents & POLLIN) {
                fprintf(stderr, "POLLIN ");
            }
            if (poll_fds[j].revents & POLLOUT) {
                fprintf(stderr, "POLLOUT ");
            }
            fprintf(stderr, "\n");
        }*/
        while (num_handled_fd < poll_res && i < prev_last_index && !is_stop) {
            //fprintf(stderr, "main: i = %lu, num_handled = %d\n", i, num_handled_fd);
            if (poll_fds[i].fd == READ_STOP_FD && (poll_fds[i].revents & POLLIN)) {
                //fprintf(stderr, "main received stop signal\n");
                removeFromPollFds(poll_fds, &poll_last_index, READ_STOP_FD);
                READ_STOP_FD = -1;
                destroyPollFds(poll_fds, &poll_last_index);
                cleanUp();
                exit(0);
            }
            if (poll_fds[i].fd == listen_fd && (poll_fds[i].revents & POLLIN)) {
                //fprintf(stderr, "acceptNewClient()\n");
                acceptNewClient(listen_fd);
                num_handled_fd += 1;
            }
            i += 1;

            //fprintf(stderr, "poll_fds[%lu].fd = %d, client %d, server %d\n", i, poll_fds[i].fd, client_num, server_num);

        }
        //printCacheList();
    }
    removeFromPollFds(poll_fds, &poll_last_index, READ_STOP_FD);
    READ_STOP_FD = -1;
    destroyPollFds(poll_fds, &poll_last_index);
    cleanUp();
    return 0;
}
