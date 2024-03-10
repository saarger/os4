#include "queue.h"
#include <threads.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct ThreadElement
{
    thrd_t thread_id;
    struct ThreadElement *next_element;
    cnd_t condition_variable;
    bool is_terminated;
    int wait_count;
} ThreadElement;

typedef struct ThreadQueue
{
    ThreadElement *head;
    ThreadElement *tail;
    atomic_ulong num_waiting_threads;
} ThreadQueue;

typedef struct DataElement
{
    struct DataElement *next_element;
    int data_index;
    void *data_payload;
} DataElement;

typedef struct DataQueue
{
    DataElement *head;
    DataElement *tail;
    atomic_ulong size_of_queue;
    atomic_ulong num_visited_elements;
    atomic_ulong num_enqueued_elements;
    mtx_t lock;
} DataQueue;

static ThreadQueue threads_queue;
static DataQueue data_queue;
static thrd_t cleanup_thread;

void release_all_data_elements(void);
void dismantle_thread_queue(void);
DataElement *new_data_element(void *payload);
void queue_data_element(DataElement *element);
void queue_to_empty_data_queue(DataElement *element);
void queue_to_non_empty_data_queue(DataElement *element);
bool should_thread_wait(void);
void place_thread_in_queue(void);
void remove_thread_from_queue(void);
void queue_thread_element(ThreadElement *element);
void add_to_empty_thread_queue(ThreadElement *element);
void add_to_nonempty_thread_queue(ThreadElement *element);
ThreadElement *generate_thread_element(void);
int fetch_first_thread_wait_count(void);

void initQueue(void)
{
    data_queue.head = NULL;
    data_queue.tail = NULL;
    threads_queue.head = NULL;
    threads_queue.tail = NULL;
    atomic_store(&data_queue.size_of_queue, 0);
    atomic_store(&threads_queue.num_waiting_threads, 0);
    atomic_store(&data_queue.num_visited_elements, 0);
    atomic_store(&data_queue.num_enqueued_elements, 0);
    mtx_init(&data_queue.lock, mtx_plain);
}

void destroyQueue(void)
{
    mtx_lock(&data_queue.lock);
    release_all_data_elements();
    dismantle_thread_queue();
    mtx_unlock(&data_queue.lock);
    mtx_destroy(&data_queue.lock);
}

void release_all_data_elements(void)
{
    DataElement *current_head;
    while (data_queue.head)
    {
        current_head = data_queue.head;
        data_queue.head = current_head->next_element;
        free(current_head);
    }
    data_queue.tail = NULL;
    atomic_store(&data_queue.num_visited_elements, 0);
    atomic_store(&data_queue.size_of_queue, 0);
    atomic_store(&data_queue.num_enqueued_elements, 0);
}

void dismantle_thread_queue(void)
{
    cleanup_thread = thrd_current();
    while (threads_queue.head)
    {
        threads_queue.head->is_terminated = true;
        cnd_signal(&threads_queue.head->condition_variable);
    }
    threads_queue.tail = NULL;
    atomic_store(&threads_queue.num_waiting_threads, 0);
}

void enqueue(void *data_payload)
{
    mtx_lock(&data_queue.lock);
    DataElement *element = new_data_element(data_payload);
    queue_data_element(element);
    mtx_unlock(&data_queue.lock);

    if (atomic_load(&data_queue.size_of_queue) > 0 && atomic_load(&threads_queue.num_waiting_threads) > 0)
    {
        cnd_signal(&threads_queue.head->condition_variable);
    }
}

DataElement *new_data_element(void *payload)
{
    DataElement *element = (DataElement *)malloc(sizeof(DataElement));
    element->data_payload = payload;
    element->next_element = NULL;
    element->data_index = atomic_fetch_add(&data_queue.num_enqueued_elements, 1);
    return element;
}

void queue_data_element(DataElement *element)
{
    atomic_load(&data_queue.size_of_queue) == 0 ? queue_to_empty_data_queue(element) : queue_to_non_empty_data_queue(element);
}

void queue_to_empty_data_queue(DataElement *element)
{
    data_queue.head = element;
    data_queue.tail = element;
    atomic_fetch_add(&data_queue.size_of_queue, 1);
    atomic_fetch_add(&data_queue.num_enqueued_elements, 1);
}

void queue_to_non_empty_data_queue(DataElement *element)
{
    data_queue.tail->next_element = element;
    data_queue.tail = element;
    atomic_fetch_add(&data_queue.size_of_queue, 1);
    atomic_fetch_add(&data_queue.num_enqueued_elements, 1);
}

void* dequeue(void)
{
    mtx_lock(&data_queue.lock);
    while (should_thread_wait())
    {
        place_thread_in_queue();
        ThreadElement *current_thread = threads_queue.tail;
        cnd_wait(&current_thread->condition_variable, &data_queue.lock);
        if (current_thread->is_terminated)
        {
            ThreadElement *former_head = threads_queue.head;
            threads_queue.head = former_head->next_element;
            free(former_head);
            thrd_join(cleanup_thread, NULL);
        }
        if (data_queue.head && fetch_first_thread_wait_count() <= data_queue.head->data_index)
        {
            remove_thread_from_queue();
        }
    }

    DataElement *element_to_dequeue = data_queue.head;
    data_queue.head = element_to_dequeue->next_element;
    if (!data_queue.head)
    {
        data_queue.tail = NULL;
    }
    atomic_fetch_sub(&data_queue.size_of_queue, 1);
    atomic_fetch_add(&data_queue.num_visited_elements, 1);
    mtx_unlock(&data_queue.lock);
    void *data = element_to_dequeue->data_payload;
    free(element_to_dequeue);
    return data;
}

bool should_thread_wait(void)
{
    if (atomic_load(&data_queue.size_of_queue) == 0)
    {
        return true;
    }
    if (atomic_load(&threads_queue.num_waiting_threads) <= atomic_load(&data_queue.size_of_queue))
    {
        return false;
    }
    int wait_count = fetch_first_thread_wait_count();
    return wait_count > data_queue.head->data_index;
}

int fetch_first_thread_wait_count(void)
{
    ThreadElement *element = threads_queue.head;
    while (element)
    {
        if (thrd_equal(thrd_current(), element->thread_id))
        {
            return element->wait_count;
        }
        element = element->next_element;
    }
    return -1;
}

void place_thread_in_queue(void)
{
    ThreadElement *element = generate_thread_element();
    queue_thread_element(element);
}

void remove_thread_from_queue(void)
{
    ThreadElement *element_to_remove = threads_queue.head;
    threads_queue.head = threads_queue.head->next_element;
    free(element_to_remove);
    if (!threads_queue.head)
    {
        threads_queue.tail = NULL;
    }
    atomic_fetch_sub(&threads_queue.num_waiting_threads, 1);
}

void queue_thread_element(ThreadElement *element)
{
    atomic_load(&threads_queue.num_waiting_threads) == 0 ? add_to_empty_thread_queue(element) : add_to_nonempty_thread_queue(element);
}

void add_to_empty_thread_queue(ThreadElement *element)
{
    threads_queue.head = element;
    threads_queue.tail = element;
    atomic_fetch_add(&threads_queue.num_waiting_threads, 1);
}

void add_to_nonempty_thread_queue(ThreadElement *element)
{
    threads_queue.tail->next_element = element;
    threads_queue.tail = element;
    atomic_fetch_add(&threads_queue.num_waiting_threads, 1);
}

ThreadElement *generate_thread_element(void)
{
    ThreadElement *element = (ThreadElement *)malloc(sizeof(ThreadElement));
    element->thread_id = thrd_current();
    element->next_element = NULL;
    element->is_terminated = false;
    cnd_init(&element->condition_variable);
    element->wait_count = atomic_load(&data_queue.num_enqueued_elements) + atomic_load(&threads_queue.num_waiting_threads);
    return element;
}

bool tryDequeue(void **payload)
{
    mtx_lock(&data_queue.lock);
    while (atomic_load(&data_queue.size_of_queue) == 0 || !data_queue.head)
    {
        mtx_unlock(&data_queue.lock);
        return false;
    }
    DataElement *element_to_dequeue = data_queue.head;
    data_queue.head = element_to_dequeue->next_element;
    if (!data_queue.head)
    {
        data_queue.tail = NULL;
    }
    atomic_fetch_sub(&data_queue.size_of_queue, 1);
    atomic_fetch_add(&data_queue.num_visited_elements, 1);
    mtx_unlock(&data_queue.lock);
    *payload = element_to_dequeue->data_payload;
    free(element_to_dequeue);
    return true;
}

size_t size(void)
{
    return atomic_load(&data_queue.size_of_queue);
}

size_t waiting(void)
{
    return atomic_load(&threads_queue.num_waiting_threads);
}

size_t visited(void)
{
    return atomic_load(&data_queue.num_visited_elements);
}
