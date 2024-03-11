#include "queue.h"
#include <threads.h>
#include <stdatomic.h>
#include <stdlib.h>


// Manages a collection of thread entries, tracking the first and last entries and the count of entries awaiting processing.
struct QueueOfThreads
{
    struct ThreadNode *first;
    struct ThreadNode *last;
    atomic_ulong count_of_waiting_threads;
};

// Represents a single thread entry in the queue, including its unique ID, next node, condition variable, termination status, and wait condition.
struct ThreadNode
{
    thrd_t thread_id;
    struct ThreadNode *next_node;
    // Unique condition variable for each thread to enable specific signaling.
    cnd_t condition_var;
    bool is_terminated;
    int waiting_for;
};

// Queue structure for data elements, including pointers to the first and last elements, and counters for size, processed, and added elements.
struct QueueOfData
{
    struct DataNode *first;
    struct DataNode *last;
    atomic_ulong size;
    atomic_ulong processed_count;
    atomic_ulong added_count;
    mtx_t lock;
};

// Represents a single data element within the data queue, including a pointer to the next element, an index, and the data pointer itself.
struct DataNode
{
    struct DataNode *next_node;
    int data_index;
    void *data_ptr;
};



static struct QueueOfThreads thread_queue;
static struct QueueOfData data_queue;
static thrd_t terminator;


void clear_all_data_nodes(void);
void dismantle_queue_of_threads(void);
struct DataNode *initialize_data_node(void *data);
void insert_node_into_data_queue(struct DataNode *node_to_add);
void insert_node_into_empty_data_queue(struct DataNode *node_to_add);
void insert_node_into_nonempty_data_queue(struct DataNode *node_to_add);
bool should_current_thread_yield(void);
void enqueue_thread_node(void);
void dequeue_thread_node(void);
void insert_node_into_thread_queue(struct ThreadNode *node_to_add);
void insert_node_into_empty_thread_queue(struct ThreadNode *node_to_add);
void insert_node_into_nonempty_thread_queue(struct ThreadNode *node_to_add);
struct ThreadNode *initialize_thread_node(void);
int retrieve_first_waiting_status(void);


void initQueue(void)
{
    // Initialize data queue pointers to null, indicating an empty queue.
    data_queue.first = NULL;
    data_queue.last = NULL;
    // Reset all data queue counters to 0, reflecting an empty state.
    data_queue.size = 0;
    data_queue.processed_count = 0;
    data_queue.added_count = 0;
    // Initialize the mutex lock for data queue operations.
    mtx_init(&data_queue.lock, mtx_plain);
    
    // Initialize thread queue pointers to null, showing no threads are queued.
    thread_queue.first = NULL;
    thread_queue.last = NULL;
    // Reset the count of waiting threads to 0.
    thread_queue.count_of_waiting_threads = 0;
}

void destroyQueue(void)
{
    // Acquire the lock on the data queue to ensure exclusive access.
    mtx_lock(&data_queue.lock);
    // Clear all nodes from the data queue safely.
    clear_all_data_nodes();
    // Dismantle the thread queue, ensuring all thread nodes are properly managed.
    dismantle_queue_of_threads();
    // Release the lock after operations are completed.
    mtx_unlock(&data_queue.lock);
    // Destroy the mutex lock, as the data queue will no longer be in use.
    mtx_destroy(&data_queue.lock);
}

void clear_all_data_nodes(void)
{
    struct DataNode *previous_node;
    while (data_queue.first != NULL)
    {
        previous_node = data_queue.first;
        data_queue.first = previous_node->next_node;
        free(previous_node);
    }
    // Although resetting these fields might not be strictly necessary, it ensures the data queue is in a clean state.
    data_queue.last = NULL;
    data_queue.size = 0;
    data_queue.processed_count = 0;
    data_queue.added_count = 0;
}

void dismantle_queue_of_threads(void)
{
    terminator = thrd_current();
    while (thread_queue.first != NULL)
    {
        thread_queue.first->is_terminated = true;
        cnd_signal(&thread_queue.first->condition_var);
        // Move to the next node to avoid an infinite loop.
        thread_queue.first = thread_queue.first->next_node;
    }
    // Reset the thread queue to a clean state after clearing it.
    thread_queue.last = NULL;
    thread_queue.count_of_waiting_threads = 0;
}

void enqueue(void *element_data)
{
    mtx_lock(&data_queue.lock);
    struct DataNode *new_node = initialize_data_node(element_data);
    insert_node_into_data_queue(new_node);
    mtx_unlock(&data_queue.lock);

    if (data_queue.size > 0 && thread_queue.count_of_waiting_threads > 0)
    {
        // Signaling the first thread in the queue if there are elements in the data queue and waiting threads.
        cnd_signal(&thread_queue.first->condition_var);
    }
}

struct DataNode *initialize_data_node(void *data)
{
    // Assuming malloc succeeds as per instructions.
    struct DataNode *node = (struct DataNode *)malloc(sizeof(struct DataNode));
    node->data_ptr = data;
    node->next_node = NULL;
    node->data_index = data_queue.added_count;
    return node;
}

void insert_node_into_data_queue(struct DataNode *node_to_add)
{
    data_queue.size == 0 ? insert_node_into_empty_data_queue(node_to_add) : insert_node_into_nonempty_data_queue(node_to_add);
}

void insert_node_into_empty_data_queue(struct DataNode *node_to_add)
{
    data_queue.first = node_to_add;
    data_queue.last = node_to_add;
    data_queue.size++;
    data_queue.added_count++;
}

void insert_node_into_nonempty_data_queue(struct DataNode *node_to_add)
{
    data_queue.last->next_node = node_to_add;
    data_queue.last = node_to_add;
    data_queue.size++;
    data_queue.added_count++;
}

void *dequeue(void)
{
    mtx_lock(&data_queue.lock);
    // This loop blocks as required
    while (should_current_thread_yield())
    {
        enqueue_thread_node();
        struct ThreadNode *current = thread_queue.last;
        cnd_wait(&current->condition_var, &data_queue.lock);
        if (current->is_terminated)
        {
            struct ThreadNode *previous_first;
            previous_first = thread_queue.first;
            thread_queue.first = previous_first->next_node;
            // Prevents runaway threads upon destruction
            free(previous_first);
            thrd_join(terminator, NULL);
        }
        if (data_queue.first && retrieve_first_waiting_status() <= data_queue.first->data_index)
        {
            dequeue_thread_node();
        }
    }

    struct DataNode *dequeued_node = data_queue.first;
    data_queue.first = dequeued_node->next_node;
    if (data_queue.first == NULL)
    {
        data_queue.last = NULL;
    }
    data_queue.size--;
    data_queue.processed_count++;
    mtx_unlock(&data_queue.lock);
    void *data = dequeued_node->data_ptr;
    free(dequeued_node);
    return data;
}


bool should_current_thread_yield(void)
{
    if (data_queue.size == 0)
    {
        return true;
    }
    if (thread_queue.count_of_waiting_threads <= data_queue.size)
    {
        return false;
    }
    int first_waiting_on = retrieve_first_waiting_status();
    return first_waiting_on > data_queue.first->data_index;
}

int retrieve_first_waiting_status(void)
{
    struct ThreadNode *thread_node = thread_queue.first;
    while (thread_node != NULL)
    {
        if (thrd_equal(thrd_current(), thread_node->thread_id))
        {
            return thread_node->waiting_for;
        }
        thread_node = thread_node->next_node;
    }
    return -1;
}

void enqueue_thread_node(void)
{
    struct ThreadNode *new_thread_node = initialize_thread_node();
    insert_node_into_thread_queue(new_thread_node);
}

void dequeue_thread_node(void)
{
    struct ThreadNode *dequeued_thread = thread_queue.first;
    thread_queue.first = dequeued_thread->next_node;
    free(dequeued_thread);
    if (thread_queue.first == NULL)
    {
        thread_queue.last = NULL;
    }
    thread_queue.count_of_waiting_threads--;
}

void insert_node_into_thread_queue(struct ThreadNode *node_to_add)
{
    thread_queue.count_of_waiting_threads == 0 ? insert_node_into_empty_thread_queue(node_to_add) : insert_node_into_nonempty_thread_queue(node_to_add);
}

void insert_node_into_empty_thread_queue(struct ThreadNode *node_to_add)
{
    thread_queue.first = node_to_add;
    thread_queue.last = node_to_add;
    thread_queue.count_of_waiting_threads++;
}

void insert_node_into_nonempty_thread_queue(struct ThreadNode *node_to_add)
{
    thread_queue.last->next_node = node_to_add;
    thread_queue.last = node_to_add;
    thread_queue.count_of_waiting_threads++;
}

struct ThreadNode *initialize_thread_node(void)
{
    struct ThreadNode *thread_node = (struct ThreadNode *)malloc(sizeof(struct ThreadNode));
    thread_node->thread_id = thrd_current();
    thread_node->next_node = NULL;
    thread_node->is_terminated = false;
    cnd_init(&thread_node->condition_var);
    thread_node->waiting_for = data_queue.added_count + thread_queue.count_of_waiting_threads;
    return thread_node;
}

bool tryDequeue(void **element)
{
    mtx_lock(&data_queue.lock);
    while (data_queue.size == 0 || data_queue.first == NULL)
    {
        mtx_unlock(&data_queue.lock);
        return false;
    }
    struct DataNode *dequeued_node = data_queue.first;
    data_queue.first = dequeued_node->next_node;
    if (data_queue.first == NULL)
    {
        data_queue.last = NULL;
    }
    data_queue.size--;
    data_queue.processed_count++;
    mtx_unlock(&data_queue.lock);
    *element = dequeued_node->data_ptr;
    free(dequeued_node);
    return true;
}

size_t size(void)
{
    return data_queue.size;
}

size_t waiting(void)
{
    return thread_queue.count_of_waiting_threads;
}

size_t visited(void)
{
    return data_queue.processed_count;
}
