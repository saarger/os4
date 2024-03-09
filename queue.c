#include "queue.h"
#include <stdlib.h>
#include <threads.h>
#include <stdatomic.h>

typedef struct Node {
    void* data;
    struct Node* next;
} Node;

typedef struct Queue {
    Node* head;
    Node* tail;
    mtx_t lock;
    cnd_t not_empty;
    atomic_size_t size;
    atomic_size_t waiting;
    atomic_size_t visited;
} Queue;

static Queue queue;

void initQueue(void) {
    queue.head = NULL;
    queue.tail = NULL;
    atomic_store(&queue.size, 0);
    atomic_store(&queue.waiting, 0);
    atomic_store(&queue.visited, 0);
    mtx_init(&queue.lock, mtx_plain);
    cnd_init(&queue.not_empty);
}

void destroyQueue(void) {
    Node* temp;
    while (queue.head != NULL) {
        temp = queue.head;
        queue.head = queue.head->next;
        free(temp);
    }
    queue.tail = NULL;
    mtx_destroy(&queue.lock);
    cnd_destroy(&queue.not_empty);
}

void enqueue(void* item) {
    Node* new_node = malloc(sizeof(Node));
    new_node->data = item;
    new_node->next = NULL;

    mtx_lock(&queue.lock);
    if (queue.tail != NULL) {
        queue.tail->next = new_node;
    } else {
        queue.head = new_node;
    }
    queue.tail = new_node;
    atomic_fetch_add(&queue.size, 1);
    cnd_signal(&queue.not_empty);
    mtx_unlock(&queue.lock);
}

void* dequeue(void) {
    mtx_lock(&queue.lock);
    while (queue.head == NULL) {
        atomic_fetch_add(&queue.waiting, 1);
        cnd_wait(&queue.not_empty, &queue.lock);
        atomic_fetch_sub(&queue.waiting, 1);
    }
    Node* temp = queue.head;
    void* data = temp->data;
    queue.head = queue.head->next;
    if (queue.head == NULL) {
        queue.tail = NULL;
    }
    free(temp);
    atomic_fetch_sub(&queue.size, 1);
    atomic_fetch_add(&queue.visited, 1);
    mtx_unlock(&queue.lock);
    return data;
}

bool tryDequeue(void** item) {
    if (mtx_trylock(&queue.lock) == thrd_success) {
        if (queue.head == NULL) {
            mtx_unlock(&queue.lock);
            return false;
        }
        Node* temp = queue.head;
        *item = temp->data;
        queue.head = queue.head->next;
        if (queue.head == NULL) {
            queue.tail = NULL;
        }
        free(temp);
        atomic_fetch_sub(&queue.size, 1);
        atomic_fetch_add(&queue.visited, 1);
        mtx_unlock(&queue.lock);
        return true;
    }
    return false;
}

size_t size(void) {
    return atomic_load(&queue.size);
}

size_t waiting(void) {
    return atomic_load(&queue.waiting);
}

size_t visited(void) {
    return atomic_load(&queue.visited);
}
