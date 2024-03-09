#include "queue.h"
#include <stdlib.h>
#include <threads.h>
#include <stdatomic.h>

typedef struct Node {
    void* data;
    struct Node* next;
} Node;

static Node* volatile head = NULL;
static Node* volatile tail = NULL;
static mtx_t queue_lock;
static cnd_t queue_not_empty;
static atomic_size_t queue_size = 0;
static atomic_size_t queue_waiting = 0;
static atomic_size_t queue_visited = 0;

void initQueue(void) {
    head = tail = NULL;
    mtx_init(&queue_lock, mtx_plain);
    cnd_init(&queue_not_empty);
    queue_size = 0;
    queue_waiting = 0;
    queue_visited = 0;
}

void destroyQueue(void) {
    Node* temp;
    while (head != NULL) {
        temp = head;
        head = head->next;
        free(temp);
    }
    tail = NULL;
    mtx_destroy(&queue_lock);
    cnd_destroy(&queue_not_empty);
}

void enqueue(void* item) {
    Node* new_node = malloc(sizeof(Node));
    new_node->data = item;
    new_node->next = NULL;

    mtx_lock(&queue_lock);
    if (tail == NULL) {
        head = tail = new_node;
    } else {
        tail->next = new_node;
        tail = new_node;
    }
    atomic_fetch_add(&queue_size, 1);
    cnd_signal(&queue_not_empty); // Changed from broadcast to signal
    mtx_unlock(&queue_lock);
}

void* dequeue(void) {
    mtx_lock(&queue_lock);
    while (head == NULL) {
        atomic_fetch_add(&queue_waiting, 1);
        cnd_wait(&queue_not_empty, &queue_lock);
        atomic_fetch_sub(&queue_waiting, 1);
    }
    Node* temp = head;
    void* data = temp->data;
    head = head->next;
    if (head == NULL) {
        tail = NULL;
    }
    free(temp);
    atomic_fetch_sub(&queue_size, 1);
    atomic_fetch_add(&queue_visited, 1);
    mtx_unlock(&queue_lock);
    return data;
}

bool tryDequeue(void** item) {
    if (mtx_trylock(&queue_lock) == thrd_success) {
        if (head == NULL) {
            mtx_unlock(&queue_lock);
            return false;
        }
        Node* temp = head;
        *item = temp->data;
        head = head->next;
        if (head == NULL) {
        tail = NULL;
        }
        free(temp);
        atomic_fetch_sub(&queue_size, 1);
        atomic_fetch_add(&queue_visited, 1);
        mtx_unlock(&queue_lock);
        return true;
    }
    return false;
}

size_t size(void) {
    return atomic_load(&queue_size);
}

size_t waiting(void) {
    return atomic_load(&queue_waiting);
}

size_t visited(void) {
    return atomic_load(&queue_visited);
}
