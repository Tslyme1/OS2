#include "cache.h"

#include <malloc.h>
#include <string.h>

cache_list_t *initList() {
    cache_list_t *list = (cache_list_t *) calloc(1, sizeof(cache_list_t)); // Thread-safety
    if (list == NULL) {
        perror("Error: calloc returned NULL");
        return NULL;
    }
    pthread_mutex_init(&list->mutex, NULL);
    list->head = NULL;
    return list;
}

void destroyList(cache_list_t *list) {
    if (list == NULL) {
        return;
    }
    pthread_mutex_destroy(&list->mutex);
    while (list->head != NULL) {
        cache_node_t *tmp = list->head->next;
        free(list->head->record);
        free(list->head);
        list->head = tmp;
    }
    free(list);
}
