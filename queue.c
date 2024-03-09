#include "queue.h"
#include <threads.h>
#include <stdatomic.h>
#include <stdlib.h>

struct ThreadQueue
{
    struct ThreadElement *head;
    struct ThreadElement *tail;
    atomic_ulong waiting_count;
};

struct ThreadElement
{
    thrd_t id;
    struct ThreadElement *next;
    // Each thread has its own condition variable so we can signal it independently.
    cnd_t cnd_thread;
    bool terminated;
    int waiting_on;
};

// We implement the queue as a linked list, saving its head and tail.
struct DataQueue
{
    struct DataElement *head;
    struct DataElement *tail;
    atomic_ulong queue_size;
    atomic_ulong visited_count;
    atomic_ulong enqueued_count;
    mtx_t data_queue_lock;
};

struct DataElement
{
    struct DataElement *next;
    int index;
    void *data;
};

/*
    We keep track of the threads in order of sleep time in order to
    always signal the oldest one and thus maintain the FIFO order between them.
*/
static struct ThreadQueue thread_queue;
static struct DataQueue data_queue;
static thrd_t terminator;

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

void initQueue(void)
{
    data_queue.head = NULL;
    thread_queue.head = NULL;
    data_queue.tail = NULL;
    thread_queue.tail = NULL;
    data_queue.queue_size = 0;
    thread_queue.waiting_count = 0;
    data_queue.visited_count = 0;
    data_queue.enqueued_count = 0;
    mtx_init(&data_queue.data_queue_lock, mtx_plain);
}

void destroyQueue(void)
{
    mtx_lock(&data_queue.data_queue_lock);
    free_all_data_elements();
    destroy_thread_queue();
    mtx_unlock(&data_queue.data_queue_lock);
    mtx_destroy(&data_queue.data_queue_lock);
}

void free_all_data_elements(void)
{
    struct DataElement *prev_head;
    while (data_queue.head != NULL)
    {
        prev_head = data_queue.head;
        data_queue.head = prev_head->next;
        free(prev_head);
    }
    // Resetting the fields is not strictly necessary, but just for good measure.
    data_queue.tail = NULL;
    data_queue.visited_count = 0;
    data_queue.queue_size = 0;
    data_queue.enqueued_count = 0;
}

void destroy_thread_queue(void)
{
    terminator = thrd_current();
    while (thread_queue.head != NULL)
    {
        thread_queue.head->terminated = true;
        cnd_signal(&thread_queue.head->cnd_thread);
    }
    thread_queue.tail = NULL;
    thread_queue.waiting_count = 0;
}

void enqueue(void *element_data)
{
    mtx_lock(&data_queue.data_queue_lock);
    struct DataElement *new_element = create_element(element_data);
    add_element_to_data_queue(new_element);
    mtx_unlock(&data_queue.data_queue_lock);

    if (data_queue.queue_size > 0 && thread_queue.waiting_count > 0)
    {
        cnd_signal(&thread_queue.head->cnd_thread);
    }
}

struct DataElement *create_element(void *data)
{
    // We assume malloc does not fail, as per the instructions.
    struct DataElement *element = (struct DataElement *)malloc(sizeof(struct DataElement));
    element->data = data;
    element->next = NULL;
    element->index = data_queue.enqueued_count;
    return element;
}

void add_element_to_data_queue(struct DataElement *new_element)
{
    data_queue.queue_size == 0 ? add_element_to_empty_data_queue(new_element) : add_element_to_nonempty_data_queue(new_element);
}

void add_element_to_empty_data_queue(struct DataElement *new_element)
{
    data_queue.head = new_element;
    data_queue.tail = new_element;
    data_queue.queue_size++;
    data_queue.enqueued_count++;
}

void add_element_to_nonempty_data_queue(struct DataElement *new_element)
{
    data_queue.tail->next = new_element;
    data_queue.tail = new_element;
    data_queue.queue_size++;
    data_queue.enqueued_count++;
}

void *dequeue(void)
{
    mtx_lock(&data_queue.data_queue_lock);
    // This loop blocks as required
    while (current_thread_should_sleep())
    {
        thread_enqueue();
        struct ThreadElement *current = thread_queue.tail;
        cnd_wait(&current->cnd_thread, &data_queue.data_queue_lock);
        if (current->terminated)
        {
            struct ThreadElement *prev_head;
            prev_head = thread_queue.head;
            thread_queue.head = prev_head->next;
            // Prevents runaway threads upon destruction
            free(prev_head);
            thrd_join(terminator, NULL);
        }
        if (data_queue.head && get_current_first_waiting_on() <= data_queue.head->index)
        {
            thread_dequeue();
        }
    }

    struct DataElement *dequeued_data = data_queue.head;
    data_queue.head = dequeued_data->next;
    if (data_queue.head == NULL)
    {
        data_queue.tail = NULL;
    }
    data_queue.queue_size--;
    data_queue.visited_count++;
    mtx_unlock(&data_queue.data_queue_lock);
    void *data = dequeued_data->data;
    free(dequeued_data);
    return data;
}

bool current_thread_should_sleep(void)
{
    if (data_queue.queue_size == 0)
    {
        return true;
    }
    if (thread_queue.waiting_count <= data_queue.queue_size)
    {
        return false;
    }
    int first_waiting_on = get_current_first_waiting_on();
    return first_waiting_on > data_queue.head->index;
}

int get_current_first_waiting_on(void)
{
    struct ThreadElement *thread_element = thread_queue.head;
    while (thread_element != NULL)
    {
        if (thrd_equal(thrd_current(), thread_element->id))
        {
            return thread_element->waiting_on;
        }
        thread_element = thread_element->next;
    }
    return -1;
}

void thread_enqueue(void)
{
    struct ThreadElement *new_thread_element = create_thread_element();
    add_element_to_thread_queue(new_thread_element);
}

void thread_dequeue(void)
{
    struct ThreadElement *dequeued_thread = thread_queue.head;
    thread_queue.head = thread_queue.head->next;
    free(dequeued_thread);
    if (thread_queue.head == NULL)
    {
        thread_queue.tail = NULL;
    }
    thread_queue.waiting_count--;
}

void add_element_to_thread_queue(struct ThreadElement *new_element)
{
    thread_queue.waiting_count == 0 ? add_element_to_empty_thread_queue(new_element) : add_element_to_nonempty_thread_queue(new_element);
}

void add_element_to_empty_thread_queue(struct ThreadElement *new_element)
{
    thread_queue.head = new_element;
    thread_queue.tail = new_element;
    thread_queue.waiting_count++;
}

void add_element_to_nonempty_thread_queue(struct ThreadElement *new_element)
{
    thread_queue.tail->next = new_element;
    thread_queue.tail = new_element;
    thread_queue.waiting_count++;
}

struct ThreadElement *create_thread_element(void)
{
    struct ThreadElement *thread_element = (struct ThreadElement *)malloc(sizeof(struct ThreadElement));
    thread_element->id = thrd_current();
    thread_element->next = NULL;
    thread_element->terminated = false;
    cnd_init(&thread_element->cnd_thread);
    thread_element->waiting_on = data_queue.enqueued_count + thread_queue.waiting_count;
    return thread_element;
}

bool tryDequeue(void **element)
{
    mtx_lock(&data_queue.data_queue_lock);
    while (data_queue.queue_size == 0 || data_queue.head == NULL)
    {
        mtx_unlock(&data_queue.data_queue_lock);
        return false;
    }
    struct DataElement *dequeued_data = data_queue.head;
    data_queue.head = dequeued_data->next;
    if (data_queue.head == NULL)
    {
        data_queue.tail = NULL;
    }
    data_queue.queue_size--;
    data_queue.visited_count++;
    mtx_unlock(&data_queue.data_queue_lock);
    *element = (void *)dequeued_data->data;
    free(dequeued_data);
    return true;
}

size_t size(void)
{
    return data_queue.queue_size;
}

size_t waiting(void)
{
    return thread_queue.waiting_count;
}

size_t visited(void)
{
    return data_queue.visited_count;
}
