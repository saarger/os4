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

// Initialize the queue
void initQueue(void) {
    head = tail = NULL;
    mtx_init(&queue_lock, mtx_plain);
    cnd_init(&queue_not_empty);
    atomic_store(&queue_size, 0);
    atomic_store(&queue_waiting, 0);
    atomic_store(&queue_visited, 0);
}

// Destroy the queue
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
    atomic_store(&queue_size, 0);
    atomic_store(&queue_waiting, 0);
    atomic_store(&queue_visited, 0);
}

// Enqueue an item
void enqueue(void* item) {
    Node* new_node = malloc(sizeof(Node));
    if (!new_node) return; // Handle malloc failure

    new_node->data = item;
    new_node->next = NULL;

    mtx_lock(&queue_lock);
    if (tail == NULL) {
        head = tail = new_node;
    } else {
        tail->next = new_node;
        tail = new_node;
    }
    atomic_fetch_add_explicit(&queue_size, 1, memory_order_release);
    cnd_broadcast(&queue_not_empty);
    mtx_unlock(&queue_lock);
}

// Dequeue an item
void* dequeue(void) {
    mtx_lock(&queue_lock);
    while (head == NULL) {
        atomic_fetch_add_explicit(&queue_waiting, 1, memory_order_relaxed);
        cnd_wait(&queue_not_empty, &queue_lock);
        atomic_fetch_sub_explicit(&queue_waiting, 1, memory_order_relaxed);
    }
    Node* temp = head;
    void* data = temp->data;
    head = head->next;
    if (head == NULL) tail = NULL;
    free(temp);
    atomic_fetch_sub_explicit(&queue_size, 1, memory_order_release);
    atomic_fetch_add_explicit(&queue_visited, 1, memory_order_relaxed);
    mtx_unlock(&queue_lock);
    return data;
}

// Try to dequeue an item without blocking
bool tryDequeue(void** item) {
    if (mtx_trylock(&queue_lock) == thrd_success) {
        if (head == NULL) {
            mtx_unlock(&queue_lock);
            return false;
        }
        Node* temp = head;
        *item = temp->data;
        head = head->next;
        if (head == NULL) tail = NULL;
        free(temp);
        atomic_fetch_sub_explicit(&queue_size, 1, memory_order_release);
        atomic_fetch_add_explicit(&queue_visited, 1, memory_order_relaxed);
        mtx_unlock(&queue_lock);
        return true;
    }
    return false;
}

// Get the current size of the queue
size_t size(void) {
    return atomic_load_explicit(&queue_size, memory_order_acquire);
}

// Get the number of waiting threads
size_t waiting(void) {
    return atomic_load_explicit(&queue_waiting, memory_order_acquire);
}

// Get the number of visited items
size_t visited(void) {
    return atomic_load_explicit(&queue_visited, memory_order_acquire);
}
