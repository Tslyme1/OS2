#include "task_queue.h"
#include <malloc.h>

task_queue_t *initTaskQueue() {
    task_queue_t *ret = (task_queue_t *) calloc(1, sizeof(task_queue_t));
    if (ret == NULL) {
        fprintf(stderr, "failed to alloc memory for task_queue\n");
        return NULL;
    }
    pthread_mutex_init(&ret->queue_mutex, NULL);
    ret->head = NULL;
    ret->tail = NULL;
    return ret;
}

void destroyTaskQueue(task_queue_t *queue) {
    if (queue == NULL) {
        return;
    }
    pthread_mutex_lock(&queue->queue_mutex);
    task_queue_node_t *nodes = queue->head;
    while (nodes != NULL) {
        task_queue_node_t *next_node = nodes->next;
        free(nodes);
        nodes = next_node;
    }
    queue->tail = NULL;
    queue->head = NULL;
    pthread_mutex_unlock(&queue->queue_mutex);
    pthread_mutex_destroy(&queue->queue_mutex);
    free(queue);
}

int submitTask(task_queue_t *queue, int arg) {
    if (queue == NULL) {
        fprintf(stderr, "queue is NULL\n");
        return -1;
    }
    task_queue_node_t *new_node = (task_queue_node_t *) calloc(1, sizeof(task_queue_node_t));
    if (new_node == NULL) {
        fprintf(stderr, "failed to alloc memory for node\n");
        return -1;
    }
    new_node->arg = arg;
    new_node->next = NULL;
    pthread_mutex_lock(&queue->queue_mutex);
    if (queue->head == NULL) {
        queue->head = new_node;
        queue->tail = queue->head;
        pthread_mutex_unlock(&queue->queue_mutex);
        return 0;
    }
    queue->tail->next = new_node;
    queue->tail = queue->tail->next;
    pthread_mutex_unlock(&queue->queue_mutex);
    return 0;
}

int popTask(task_queue_t *queue) {
    if (queue == NULL) {
        return -1;
    }
    pthread_mutex_lock(&queue->queue_mutex);
    if (queue->head == queue->tail) {
        if (queue->head == NULL) {
            pthread_mutex_unlock(&queue->queue_mutex);
            return -1;
        }
        else {
            task_queue_node_t *first_node = queue->head;
            queue->head = NULL;
            queue->tail = NULL;
            pthread_mutex_unlock(&queue->queue_mutex);
            int ret = first_node->arg;
            free(first_node);
            return ret;
        }
    }
    else {
        task_queue_node_t *first_node = queue->head;
        queue->head = queue->head->next;
        pthread_mutex_unlock(&queue->queue_mutex);
        int ret = first_node->arg;
        free(first_node);
        return ret;
    }
}

void printTaskQueue(task_queue_t *queue) {
    pthread_mutex_lock(&queue->queue_mutex);

    task_queue_node_t *nodes = queue->head;
    while (nodes != NULL) {
        task_queue_node_t *next_node = nodes->next;
        fprintf(stderr, "in node: task->arg = %d\n", nodes->arg);
        nodes = next_node;
    }

    pthread_mutex_unlock(&queue->queue_mutex);
}