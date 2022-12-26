#include <pthread.h>
#include <stdbool.h>

typedef struct task_queue_node {
    int arg;
    struct task_queue_node *next;
} task_queue_node_t;

typedef struct task_queue {
    task_queue_node_t *head;
    task_queue_node_t *tail;
    pthread_mutex_t queue_mutex;
} task_queue_t;

task_queue_t *initTaskQueue();
void destroyTaskQueue(task_queue_t *queue);
int submitTask(task_queue_t *queue, int arg);
int popTask(task_queue_t *queue);
void printTaskQueue(task_queue_t *queue);