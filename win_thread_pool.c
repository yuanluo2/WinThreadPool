#include <stdio.h>
#include <stdlib.h>
#include <Windows.h>
#include <stdint.h>

/* task function prototype. */
typedef void(*taskFunc)(void*);

/* task would be organized as a single linked list. */
struct Task {
    taskFunc task;
    void* taskArgs;
    struct Task* next;
};

struct ThreadPool {
    int8_t running;      /* still running ? */
    int32_t threadNum;   /* worker num. */
    HANDLE* threads;     /* handles to the thread. */
    struct Task* taskQueueHead;
    struct Task* taskQueueTail;
    CRITICAL_SECTION taskQueueMutex;
    CONDITION_VARIABLE taskQueueConditionVariable;
};

/* malloc() wrapper, when out of memory, funcName and lineNum will be printed. */
static void* underlying_safe_malloc(size_t len, const char* funcName, int32_t lineNum) {
    void* ptr;

    if ((ptr = malloc(len)) == NULL) {
        fprintf(stderr, "%s, line: %d: malloc() failed.\n", funcName, lineNum);
        exit(EXIT_FAILURE);
    }

    return ptr;
}

/* this macro will automatically fill the funcName and lineNum. */
#define safe_malloc(len) \
    underlying_safe_malloc(len, __FUNCTION__, __LINE__)

/*
    GetLastError() only get a error num, not error message,
    so this function is for that. Only supports windows platform.
*/
static void print_last_system_error(const char* userDefinedMsg) {
    DWORD lastErrorCode = GetLastError();
    LPVOID lpMsgBuf;

    if (FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        lastErrorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&lpMsgBuf,
        0,
        NULL))
    {
        fprintf(stderr, "%s, %s", userDefinedMsg, (const char*)lpMsgBuf);
        LocalFree(lpMsgBuf);
    }
    else
    {
        fprintf(stderr, "%s, line: %d: FormatMessageA() failed.\n", __FUNCTION__, __LINE__);
        exit(EXIT_FAILURE);
    }
}

/*
* Thread working loop, used inside the thread pool, you don't need to use this.
*/
static DWORD WINAPI thread_pool_work_loop(LPVOID args) {
    struct ThreadPool* pool = (struct ThreadPool*)args;

    for (;;) {
        /* 1. lock. */
        EnterCriticalSection(&(pool->taskQueueMutex));

        /* 2. waits for condition variable. */
        while (pool->taskQueueHead == NULL && pool->running) {
            if (!SleepConditionVariableCS(&(pool->taskQueueConditionVariable), &(pool->taskQueueMutex), INFINITE)) {
                print_last_system_error("SleepConditionVariableCS() failed");
                LeaveCriticalSection(&(pool->taskQueueMutex));
            }
        }

        if (pool->taskQueueHead == NULL && !pool->running) {
            LeaveCriticalSection(&(pool->taskQueueMutex));
            return 0;
        }

        /* 3. pop the head task. */
        struct Task* headTask = pool->taskQueueHead;
        pool->taskQueueHead = pool->taskQueueHead->next;

        /* 4. unlock. */
        LeaveCriticalSection(&(pool->taskQueueMutex));

        /* 5. execute the function. */
        headTask->task(headTask->taskArgs);

        /* 6. clean the resources. */
        free(headTask->taskArgs);
        free(headTask);
    }

    return 0;   /* just return. */
}

/*
* create a thread pool, and create underlying threads by the given num.
*/
static struct ThreadPool* create_thread_pool(int32_t threadNum) {
    struct ThreadPool* pool = (struct ThreadPool*)safe_malloc(sizeof(struct ThreadPool));

    pool->running = 1;
    pool->threadNum = threadNum;
    pool->threads = (HANDLE*)safe_malloc(threadNum * sizeof(HANDLE));
    pool->taskQueueHead = NULL;
    pool->taskQueueTail = NULL;
    InitializeConditionVariable(&(pool->taskQueueConditionVariable));
    InitializeCriticalSection(&(pool->taskQueueMutex));

    int32_t i;
    for (i = 0; i < threadNum; ++i) {
        HANDLE t = CreateThread(NULL,
            0,
            (LPTHREAD_START_ROUTINE)thread_pool_work_loop,
            pool,
            0,
            NULL);

        if (t == NULL) {
            print_last_system_error("CreateThread() failed");
            exit(EXIT_FAILURE);
        }

        pool->threads[i] = t;
    }

    return pool;
}

/*
* let all the threads running until they finished works, then thread pool's running flag would be set to false.
*/
static void thread_pool_join_all(struct ThreadPool* pool) {
    /* pool is NULL or not running, do nothing. */
    if (pool == NULL || !pool->running) {
        return;
    }

    /* lock, then modify the pool->running. */
    EnterCriticalSection(&(pool->taskQueueMutex));
    pool->running = 0;
    LeaveCriticalSection(&(pool->taskQueueMutex));

    /* wake up all threads. */
    WakeAllConditionVariable(&(pool->taskQueueConditionVariable));

    /* let all the threads running until completed. */
    (void)WaitForMultipleObjects(pool->threadNum, pool->threads, 1, INFINITE);
}

/*
* destroy a thread pool.
*/
static void thread_pool_destroy(struct ThreadPool* pool) {
    thread_pool_join_all(pool);

    /* clean the resources. */
    struct Task* temp = NULL;
    while (pool->taskQueueHead != NULL) {
        temp = pool->taskQueueHead->next;
        free(pool->taskQueueHead->taskArgs);   /* free(NULL) is valid if taskArgs is NULL. */
        free(pool->taskQueueHead);

        pool->taskQueueHead = temp;
    }

    free(pool);
}

/*
* add a task to the thread pool.
* param: taskArgs could be NULL, then the param: argsSize is ignored.
*/
static void thread_pool_add_task(struct ThreadPool* pool, taskFunc taskFunc, void* taskArgs, size_t argsSize) {
    struct Task* temp = (struct Task*)safe_malloc(sizeof(struct Task));

    temp->next = NULL;
    temp->task = taskFunc;

    /* if taskArgs is not NULL, then copy it. */
    if (taskArgs != NULL) {
        temp->taskArgs = safe_malloc(argsSize);
        memcpy(temp->taskArgs, taskArgs, argsSize);
    }
    else {
        temp->taskArgs = NULL;   
    }

    EnterCriticalSection(&(pool->taskQueueMutex));

    /* lock, then add a task object. */
    if (pool->taskQueueHead == NULL) {
        pool->taskQueueHead = pool->taskQueueTail = temp;
    }
    else {
        pool->taskQueueTail->next = temp;
        pool->taskQueueTail = pool->taskQueueTail->next;
    }

    LeaveCriticalSection(&(pool->taskQueueMutex));

    /* wake up one thread. */
    WakeConditionVariable(&(pool->taskQueueConditionVariable));
}

static void myFunc(void* args) {
    printf("Hello, thread.\n");
}

static void myFuncDigit(void* args) {
    printf("%d\n", *(int*)args);
}

int main() {
    struct ThreadPool* threadPool = create_thread_pool(4);

    int num = 39;
    thread_pool_add_task(threadPool, myFunc, NULL, sizeof(NULL));
    thread_pool_add_task(threadPool, myFuncDigit, &num, sizeof(num));

    thread_pool_join_all(threadPool);
    thread_pool_destroy(threadPool);
    return 0;
}
