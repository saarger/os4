#include "queue.h"
#include <threads.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct ThreadNode {
    thrd_t thread_id;
    struct ThreadNode *next_node;
    cnd_t condition_var;
    bool is_terminated;
    int wait_condition;
} ThreadNode;

typedef struct DataNode {
    struct DataNode *next_node;
    int data_index;
    void *data_pointer;
} DataNode;

typedef struct ThreadQueue {
    ThreadNode *front;
    ThreadNode *rear;
    atomic_ulong count_waiting;
} ThreadQueue;

typedef struct DataQueue {
    DataNode *front;
    DataNode *rear;
    atomic_ulong size;
    atomic_ulong count_visited;
    atomic_ulong count_enqueued;
    mtx_t lock;
} DataQueue;

ThreadQueue threads_waiting;
DataQueue data_in_queue;
static thrd_t terminator_thread;

void release_all_data_nodes(void);
void dismantle_thread_queue(void);
DataNode *generate_data_node(void *data);
void insert_data_node(DataNode *node);
void insert_into_empty_data_queue(DataNode *node);
void insert_into_filled_data_queue(DataNode *node);
bool should_thread_wait(void);
void enqueue_thread(void);
void dequeue_thread(void);
void insert_thread_node(ThreadNode *node);
void insert_into_empty_thread_queue(ThreadNode *node);
void insert_into_filled_thread_queue(ThreadNode *node);
ThreadNode *generate_thread_node(void);
int fetch_first_thread_wait_condition(void);

void initQueue(void) {
    data_in_queue.front = NULL;
    threads_waiting.front = NULL;
    data_in_queue.rear = NULL;
    threads_waiting.rear = NULL;
    atomic_store(&data_in_queue.size, 0);
    atomic_store(&threads_waiting.count_waiting, 0);
    atomic_store(&data_in_queue.count_visited, 0);
    atomic_store(&data_in_queue.count_enqueued, 0);
    mtx_init(&data_in_queue.lock, mtx_plain);
}

void destroyQueue(void) {
    mtx_lock(&data_in_queue.lock);
    release_all_data_nodes();
    dismantle_thread_queue();
    mtx_unlock(&data_in_queue.lock);
    mtx_destroy(&data_in_queue.lock);
}

void release_all_data_nodes(void) {
    DataNode *current_node;
    while ((current_node = data_in_queue.front) != NULL) {
        data_in_queue.front = current_node->next_node;
        free(current_node);
    }
    data_in_queue.rear = NULL;
    atomic_store(&data_in_queue.size, 0);
    atomic_store(&data_in_queue.count_visited, 0);
    atomic_store(&data_in_queue.count_enqueued, 0);
}

void dismantle_thread_queue(void) {
    terminator_thread = thrd_current();
    while (threads_waiting.front != NULL) {
        threads_waiting.front->is_terminated = true;
        cnd_signal(&threads_waiting.front->condition_var);
    }
    threads_waiting.rear = NULL;
    atomic_store(&threads_waiting.count_waiting, 0);
}

void enqueue(void *element_data) {
    mtx_lock(&data_in_queue.lock);
    DataNode *new_node = generate_data_node(element_data);
    insert_data_node(new_node);
    mtx_unlock(&data_in_queue.lock);

    if (atomic_load(&data_in_queue.size) > 0 && atomic_load(&threads_waiting.count_waiting) > 0) {
        cnd_signal(&threads_waiting.front->condition_var);
    }
}

DataNode *generate_data_node(void *data) {
    DataNode *node = (DataNode *)malloc(sizeof(DataNode));
    node->data_pointer = data;
    node->next_node = NULL;
    node->data_index = atomic_fetch_add(&data_in_queue.count_enqueued, 1);
    return node;
}

void insert_data_node(DataNode *node) {
    if (atomic_load(&data_in_queue.size) == 0) {
        insert_into_empty_data_queue(node);
    } else {
        insert_into_filled_data_queue(node);
    }
}

void insert_into_empty_data_queue(DataNode *node) {
    data_in_queue.front = node;
    data_in_queue.rear = node;
    atomic_fetch_add(&data_in_queue.size, 1);
}

void insert_into_filled_data_queue(DataNode *node) {
    data_in_queue.rear->next_node = node;
    data_in_queue.rear = node;
    atomic_fetch_add(&data_in_queue.size, 1);
}

void *dequeue(void) {
    mtx_lock(&data_in_queue.lock);
    while (should_thread_wait()) {
        enqueue_thread();
        ThreadNode *current_thread_node = threads_waiting.rear;
        cnd_wait(&current_thread_node->condition_var, &data_in_queue.lock);
        if (current_thread_node->is_terminated) {
            free(threads_waiting.front);
            threads_waiting.front = threads_waiting.front->next_node;
            thrd_join(terminator_thread, NULL);
        }
        if (data_in_queue.front && fetch_first_thread_wait_condition() <= data_in_queue.front->data_index) {
            dequeue_thread();
        }
    }

    DataNode *node_to_return = data_in_queue.front;
    data_in_queue.front = node_to_return->next_node;
    if (data_in_queue.front == NULL) {
        data_in_queue.rear = NULL;
    }
    atomic_fetch_sub(&data_in_queue.size, 1);
    atomic_fetch_add(&data_in_queue.count_visited, 1);
    mtx_unlock(&data_in_queue.lock);
    void *data = node_to_return->data_pointer;
    free(node_to_return);
    return data;
}

bool should_thread_wait(void) {
    if (atomic_load(&data_in_queue.size) == 0) {
        return true;
    }
    if (atomic_load(&threads_waiting.count_waiting) <= atomic_load(&data_in_queue.size)) {
        return false;
    }
    int first_waiting_condition = fetch_first_thread_wait_condition();
    return first_waiting_condition > data_in_queue.front->data_index;
}

int fetch_first_thread_wait_condition(void) {
    for (ThreadNode *node = threads_waiting.front; node != NULL; node = node->next_node) {
        if (thrd_equal(thrd_current(), node->thread_id)) {
            return node->wait_condition;
        }
    }
    return -1; // Not found
}

void enqueue_thread(void) {
    ThreadNode *new_thread_node = generate_thread_node();
    insert_thread_node(new_thread_node);
}

void dequeue_thread(void) {
    ThreadNode *node_to_remove = threads_waiting.front;
    threads_waiting.front = threads_waiting.front->next_node;
    free(node_to_remove);
    if (threads_waiting.front == NULL) {
        threads_waiting.rear = NULL;
    }
    atomic_fetch_sub(&threads_waiting.count_waiting, 1);
}

void insert_thread_node(ThreadNode *node) {
    if (atomic_load(&threads_waiting.count_waiting) == 0) {
        insert_into_empty_thread_queue(node);
    } else {
        insert_into_filled_thread_queue(node);
    }
}

void insert_into_empty_thread_queue(ThreadNode *node) {
    threads_waiting.front = node;
    threads_waiting.rear = node;
    atomic_fetch_add(&threads_waiting.count_waiting, 1);
}

void insert_into_filled_thread_queue(ThreadNode *node) {
    threads_waiting.rear->next_node = node;
    threads_waiting.rear = node;
    atomic_fetch_add(&threads_waiting.count_waiting, 1);
}

ThreadNode *generate_thread_node(void) {
    ThreadNode *node = (ThreadNode *)malloc(sizeof(ThreadNode));
    node->thread_id = thrd_current();
    node->next_node = NULL;
    node->is_terminated = false;
    cnd_init(&node->condition_var);
    node->wait_condition = atomic_load(&data_in_queue.count_enqueued) + atomic_load(&threads_waiting.count_waiting);
    return node;
}

bool tryDequeue(void **element) {
    mtx_lock(&data_in_queue.lock);
    if (atomic_load(&data_in_queue.size) == 0 || data_in_queue.front == NULL) {
        mtx_unlock(&data_in_queue.lock);
        return false;
    }
    DataNode *node_to_return = data_in_queue.front;
    data_in_queue.front = node_to_return->next_node;
    if (data_in_queue.front == NULL) {
        data_in_queue.rear = NULL;
    }
    atomic_fetch_sub(&data_in_queue.size, 1);
    atomic_fetch_add(&data_in_queue.count_visited, 1);
    mtx_unlock(&data_in_queue.lock);
    *element = node_to_return->data_pointer;
    free(node_to_return);
    return true;
}

size_t size(void) {
    return atomic_load(&data_in_queue.size);
}

size_t waiting(void) {
    return atomic_load(&threads_waiting.count_waiting);
}

size_t visited(void) {
    return atomic_load(&data_in_queue.count_visited);
}
