#include "queue.h"
#include <threads.h>
#include <stdatomic.h>
#include <stdlib.h>

// Thread queue structure holds information about threads.
typedef struct ThreadNode {
    thrd_t threadID;
    struct ThreadNode *next;
    cnd_t threadCond;
    bool hasExited;
    int waitPosition;
} ThreadNode;

typedef struct ThreadQueue {
    ThreadNode *front;
    ThreadNode *rear;
    atomic_ulong waitingThreads;
} ThreadQueue;

// Data queue structure for storing queued elements.
typedef struct DataNode {
    struct DataNode *next;
    int position;
    void *data;
} DataNode;

typedef struct DataQueue {
    DataNode *front;
    DataNode *rear;
    atomic_ulong currentSize;
    atomic_ulong processedItems;
    atomic_ulong addedItems;
    mtx_t lock;
} DataQueue;

static ThreadQueue threadQueue;
static DataQueue dataQueue;
static thrd_t terminatorThread;

void clearDataQueue(void);
void clearThreadQueue(void);
DataNode *allocateDataNode(void *data);
void appendDataNode(DataNode *node);
void handleEmptyDataQueue(DataNode *node);
void handleNonEmptyDataQueue(DataNode *node);
bool shouldThreadYield(void);
void enqueueThread(void);
void dequeueThread(void);
void appendThreadNode(ThreadNode *node);
void handleEmptyThreadQueue(ThreadNode *node);
void handleNonEmptyThreadQueue(ThreadNode *node);
ThreadNode *allocateThreadNode(void);
int getCurrentThreadWaitTarget(void);

void initQueue(void) {
    atomic_store(&dataQueue.currentSize, 0);
    atomic_store(&threadQueue.waitingThreads, 0);
    atomic_store(&dataQueue.processedItems, 0);
    atomic_store(&dataQueue.addedItems, 0);
    dataQueue.front = dataQueue.rear = NULL;
    threadQueue.front = threadQueue.rear = NULL;
    mtx_init(&dataQueue.lock, mtx_plain);
}

void destroyQueue(void) {
    mtx_lock(&dataQueue.lock);
    clearDataQueue();
    clearThreadQueue();
    mtx_unlock(&dataQueue.lock);
    mtx_destroy(&dataQueue.lock);
}

void clearDataQueue(void) {
    while (dataQueue.front) {
        DataNode *temp = dataQueue.front;
        dataQueue.front = dataQueue.front->next;
        free(temp);
    }
    dataQueue.rear = NULL;
    atomic_store(&dataQueue.currentSize, 0);
    atomic_store(&dataQueue.processedItems, 0);
    atomic_store(&dataQueue.addedItems, 0);
}

void clearThreadQueue(void) {
    terminatorThread = thrd_current();
    while (threadQueue.front) {
        threadQueue.front->hasExited = true;
        cnd_signal(&threadQueue.front->threadCond);
        threadQueue.front = threadQueue.front->next;
    }
    threadQueue.rear = NULL;
    atomic_store(&threadQueue.waitingThreads, 0);
}

void enqueue(void *data) {
    mtx_lock(&dataQueue.lock);
    DataNode *node = allocateDataNode(data);
    appendDataNode(node);
    mtx_unlock(&dataQueue.lock);

    if (atomic_load(&dataQueue.currentSize) > 0 && atomic_load(&threadQueue.waitingThreads) > 0) {
        cnd_signal(&threadQueue.front->threadCond);
    }
}

DataNode *allocateDataNode(void *data) {
    DataNode *node = (DataNode *)malloc(sizeof(DataNode));
    node->data = data;
    node->next = NULL;
    node->position = atomic_fetch_add(&dataQueue.addedItems, 1);
    return node;
}

void appendDataNode(DataNode *node) {
    if (atomic_load(&dataQueue.currentSize) == 0) {
        handleEmptyDataQueue(node);
    } else {
        handleNonEmptyDataQueue(node);
    }
}

void handleEmptyDataQueue(DataNode *node) {
    dataQueue.front = dataQueue.rear = node;
    atomic_fetch_add(&dataQueue.currentSize, 1);
}

void handleNonEmptyDataQueue(DataNode *node) {
    dataQueue.rear->next = node;
    dataQueue.rear = node;
    atomic_fetch_add(&dataQueue.currentSize, 1);
}

void* dequeue(void) {
    mtx_lock(&dataQueue.lock);
    while (shouldThreadYield()) {
        enqueueThread();
        ThreadNode *currentThread = threadQueue.rear;
        cnd_wait(&currentThread->threadCond, &dataQueue.lock);
        if (currentThread->hasExited) {
            free(currentThread);
            thrd_join(terminatorThread, NULL);
        }
        if (dataQueue.front && getCurrentThreadWaitTarget() <= dataQueue.front->position) {
            dequeueThread();
        }
    }

    DataNode *node = dataQueue.front;
    dataQueue.front = dataQueue.front->next;
    if (!dataQueue.front) dataQueue.rear = NULL;
    void *data = node->data;
    free(node);
    atomic_fetch_sub(&dataQueue.currentSize, 1);
    atomic_fetch_add(&dataQueue.processedItems, 1);
    mtx_unlock(&dataQueue.lock);
    return data;
}

bool shouldThreadYield(void) {
    if (atomic_load(&dataQueue.currentSize) == 0) return true;
    return false;
}

// Keep remaining function implementations and other supporting functions as is, focusing on ensuring logical flow and readability.
