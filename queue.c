#include "queue.h"
#include <threads.h>
#include <stdatomic.h>
#include <stdlib.h>

// Thread queue structure to manage threads in FIFO order.
struct ThreadQueue {
    struct ThreadElement *head;
    struct ThreadElement *tail;
    atomic_ulong waiting_count;
};

// Represents an individual thread in the queue.
struct ThreadElement {
    thrd_t id; // Thread identifier
    struct ThreadElement *next; // Pointer to the next thread in the queue
    cnd_t cnd_thread; // Condition variable for the thread
    bool terminated; // Flag indicating if the thread has been terminated
    int waiting_on; // Condition or reason the thread is waiting on
};

// Data queue structure for managing data elements in FIFO order.
struct DataQueue {
    struct DataElement *head; // Head of the queue
    struct DataElement *tail; // Tail of the queue
    atomic_ulong queue_size; // Number of elements in the queue
    atomic_ulong visited_count; // Count of elements that have been visited
    atomic_ulong enqueued_count; // Total count of elements enqueued over time
    mtx_t data_queue_lock; // Mutex for protecting access to the data queue
};

// Represents an individual data element in the queue.
struct DataElement {
    struct DataElement *next; // Next element in the queue
    int index; // Index or identifier for the data element
    void *data; // Pointer to the actual data
};

static struct ThreadQueue thread_queue; // Global thread queue
static struct DataQueue data_queue; // Global data queue
static thrd_t terminator; // Thread used for termination signaling


void free_all_data_elements(void);
void destroy_thread_queue(void);
struct DataElement *create_element(void *data);
void add_element_to_data_queue(struct DataElement *new_element);
void add_element_to_empty_data_queue(struct DataElement *new_element);
void add_element_to_nonempty_data_queue(struct DataElement *new_element);
bool current_thread_should_sleep(void);
void thread_enqueue(void);
void thread_dequeue(void);
void add_element_to_thread_queue(struct ThreadElement *new_element);
void add_element_to_empty_thread_queue(struct ThreadElement *new_element);
void add_element_to_nonempty_thread_queue(struct ThreadElement *new_element);
struct ThreadElement *create_thread_element(void);
int get_current_first_waiting_on(void);

// Initializes both the data queue and thread queue.
void initQueue(void) {
    data_queue.head = NULL;
    thread_queue.head = NULL;
    data_queue.tail = NULL;
    thread_queue.tail = NULL;
    atomic_store(&data_queue.queue_size, 0);
    atomic_store(&thread_queue.waiting_count, 0);
    atomic_store(&data_queue.visited_count, 0);
    atomic_store(&data_queue.enqueued_count, 0);
    mtx_init(&data_queue.data_queue_lock, mtx_plain);
}

// Cleans up and destroys both queues.
void destroyQueue(void) {
    mtx_lock(&data_queue.data_queue_lock);
    free_all_data_elements();
    destroy_thread_queue();
    mtx_unlock(&data_queue.data_queue_lock);
    mtx_destroy(&data_queue.data_queue_lock);
}

// Frees all data elements in the data queue.
void free_all_data_elements(void) {
    struct DataElement *current;
    while ((current = data_queue.head) != NULL) {
        data_queue.head = current->next;
        free(current->data); // Assuming 'data' needs to be freed.
        free(current);
    }
    data_queue.tail = NULL;
    atomic_store(&data_queue.queue_size, 0);
    atomic_store(&data_queue.visited_count, 0);
    atomic_store(&data_queue.enqueued_count, 0);
}

// Signals all threads in the thread queue for termination and clears the queue.
void destroy_thread_queue(void) {
    terminator = thrd_current();
    while (thread_queue.head != NULL) {
        struct ThreadElement *current = thread_queue.head;
        current->terminated = true;
        cnd_signal(&current->cnd_thread);
        // Removing the element from the queue
        thread_queue.head = current->next;
        free(current);
    }
    thread_queue.tail = NULL;
    atomic_store(&thread_queue.waiting_count, 0);
}

// Enqueues a new data element.
void enqueue(void *element_data) {
    struct DataElement *new_element = create_element(element_data);
    mtx_lock(&data_queue.data_queue_lock);
    if (data_queue.tail) {
        data_queue.tail->next = new_element;
    } else {
        data_queue.head = new_element;
    }
    data_queue.tail = new_element;
    atomic_fetch_add(&data_queue.queue_size, 1);
    atomic_fetch_add(&data_queue.enqueued_count, 1);
    mtx_unlock(&data_queue.data_queue_lock);

    // Signal the first waiting thread if any.
    if (atomic_load(&thread_queue.waiting_count) > 0) {
        mtx_lock(&data_queue.data_queue_lock);
        if (thread_queue.head) {
            cnd_signal(&thread_queue.head->cnd_thread);
        }
        mtx_unlock(&data_queue.data_queue_lock);
    }
}

// Create and return a new data element with provided data.
struct DataElement *create_element(void *data) {
    struct DataElement *element = (struct DataElement *)malloc(sizeof(struct DataElement));
    element->data = data;
    element->next = NULL;
    element->index = atomic_load(&data_queue.enqueued_count);
    return element;
}

// Attempt to dequeue an element without blocking.
bool tryDequeue(void **element) {
    mtx_lock(&data_queue.data_queue_lock);
    if (!data_queue.head) {
        mtx_unlock(&data_queue.data_queue_lock);
        return false;
    }

    struct DataElement *dequeued_element = data_queue.head;
    *element = dequeued_element->data;
    data_queue.head = dequeued_element->next;
    if (!data_queue.head) {
        data_queue.tail = NULL;
    }
    atomic_fetch_sub(&data_queue.queue_size, 1);
    atomic_fetch_add(&data_queue.visited_count, 1);
    free(dequeued_element);
    mtx_unlock(&data_queue.data_queue_lock);
    return true;
}

// Returns the current number of elements in the queue.
size_t size(void) {
    return atomic_load(&data_queue.queue_size);
}

// Returns the current number of threads waiting.
size_t waiting(void) {
    return atomic_load(&thread_queue.waiting_count);
}

// Returns the number of elements that have been visited.
size_t visited(void) {
    return atomic_load(&data_queue.visited_count);
}

// Note: Implementations for `dequeue`, `thread_enqueue`, `add_element_to_thread_queue`, and related threading logic were implied but not fully detailed. They should manage thread waiting, waking, and safe removal from the thread queue based on the condition variables and atomic operations.
