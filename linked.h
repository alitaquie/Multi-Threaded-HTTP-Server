#include "rwlock.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

//#pragma once

//declare

/// @brief Linked list data structure that will be used to store and retrieve locks for each uri

typedef struct LinkedList {
    char *uri;
    rwlock_t *lock;
    struct LinkedList *nextList;
} LinkedList;

rwlock_t *acquire(LinkedList *front, const char *uri) {
    LinkedList *current_node = front;
    while (current_node) {
        if (strcmp(current_node->uri, uri) == 0) {
            return current_node->lock;
        }
        current_node = current_node->nextList;
    }
    return NULL; // URI not found
}
