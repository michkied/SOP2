#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "macros.h"

// Use those names to cooperate with $make clean-resources target.
#define SHMEM_SEMAPHORE_NAME "/sop-shmem-sem"
#define SHMEM_NAME "/sop-shmem"

// Values of this function is in range (0,1]
double func(double x)
{
    usleep(2000);
    return exp(-x * x);
}

/**
 * It counts hit points by Monte Carlo method.
 * Use it to process one batch of computation.
 * @param N Number of points to randomize
 * @param a Lower bound of integration
 * @param b Upper bound of integration
 * @return Number of points which was hit.
 */
int randomize_points(int N, float a, float b)
{
    int i = 0;
    for (; i < N; ++i)
    {
        double rand_x = ((double)rand() / RAND_MAX) * (b - a) + a;
        double rand_y = ((double)rand() / RAND_MAX);
        double real_y = func(rand_x);

        if (rand_y <= real_y)
            i++;
    }
    return i;
}

/**
 * This function calculates approxiamtion of integral from counters of hit and total points.
 * @param total_randomized_points Number of total randomized points.
 * @param hit_points NUmber of hit points.
 * @param a Lower bound of integration
 * @param b Upper bound of integration
 * @return The approximation of intergal
 */
double summarize_calculations(uint64_t total_randomized_points, uint64_t hit_points, float a, float b)
{
    return (b - a) * ((double)hit_points / (double)total_randomized_points);
}

/**
 * This function locks mutex and can sometime die (it has 2% chance to die).
 * It cannot die if lock would return an error.
 * It doesn't handle any errors. It's users responsibility.
 * Use it only in STAGE 4.
 *
 * @param mtx Mutex to lock
 * @return Value returned from pthread_mutex_lock.
 */
int random_death_lock(pthread_mutex_t* mtx)
{
    int ret = pthread_mutex_lock(mtx);
    if (ret)
        return ret;

    // 2% chance to die
    if (!(rand() % 50))
        abort();
    return ret;
}

typedef struct
{
    int procNum;
    pthread_mutex_t procNumMtx;
    int a, b;
    pthread_mutex_t dataMtx;
    uint64_t total, hits;
} sharedDatA_t;

typedef struct
{
    int running;
    pthread_mutex_t mutex;
    sigset_t old_mask, new_mask;
} status_t;

void usage(char* argv[])
{
    printf("%s a b N - calculating integral with multiple processes\n", argv[0]);
    printf("a - Start of segment for integral (default: -1)\n");
    printf("b - End of segment for integral (default: 1)\n");
    printf("N - Size of batch to calculate before rport to shared memory (default: 1000)\n");
    exit(EXIT_FAILURE);
}

void robustLock(pthread_mutex_t* mutex, sharedDatA_t* shmem)
{
    int err;
    if ((err = random_death_lock(mutex)) != 0)
    {
        if (err == EOWNERDEAD)
        {
            shmem->procNum--;
            pthread_mutex_consistent(mutex);
        }
        else
        {
            ERR("pthread_mutex_lock");
        }
    }
}

sharedDatA_t* initMem(int a, int b)
{
    sem_t* shmem_init_sem;
    shmem_init_sem = sem_open(SHMEM_SEMAPHORE_NAME, O_CREAT, S_IRUSR | S_IWUSR, 1);
    if (shmem_init_sem == SEM_FAILED)
        ERR("sem_open");

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
        ERR("clock_gettime");

    ts.tv_sec += 10;
    int s = 0;
    while ((s = sem_timedwait(shmem_init_sem, &ts)) == -1 && errno == EINTR)
        continue;

    if (s == -1)
    {
        if (errno == ETIMEDOUT)
            ERR("sem_timedwait() timed out\n");
        else
            ERR("sem_timedwait");
    }

    int initialize = 1;
    int shmem_fd;
    shmem_fd = shm_open(SHMEM_NAME, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (shmem_fd == -1)
    {
        if (errno != EEXIST)
        {
            sem_post(shmem_init_sem);
            ERR("shm_open");
        }

        shmem_fd = shm_open(SHMEM_NAME, O_RDWR, S_IRUSR | S_IWUSR);
        if (shmem_fd == -1)
        {
            sem_post(shmem_init_sem);
            ERR("shm_open");
        }
        initialize = 0;
    }

    if (initialize && ftruncate(shmem_fd, sizeof(sharedDatA_t)) == -1)
    {
        shm_unlink(SHMEM_NAME);
        sem_post(shmem_init_sem);
        ERR("ftruncate");
    }

    sharedDatA_t* shmem = mmap(NULL, sizeof(sharedDatA_t), PROT_READ | PROT_WRITE, MAP_SHARED, shmem_fd, 0);
    if (shmem == MAP_FAILED)
    {
        if (initialize)
            shm_unlink(SHMEM_NAME);
        sem_post(shmem_init_sem);
        ERR("mmap");
    }

    if (initialize)
    {
        errno = 0;

        pthread_mutexattr_t mtxAttr;
        pthread_mutexattr_init(&mtxAttr);
        pthread_mutexattr_setpshared(&mtxAttr, PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_setrobust(&mtxAttr, PTHREAD_MUTEX_ROBUST);
        pthread_mutex_init(&shmem->procNumMtx, &mtxAttr);
        pthread_mutex_init(&shmem->dataMtx, &mtxAttr);

        shmem->procNum = 1;
        shmem->hits = 0;
        shmem->total = 0;
        shmem->a = a;
        shmem->b = b;

        if (errno)
        {
            shm_unlink(SHMEM_NAME);
            sem_post(shmem_init_sem);
            ERR("shmem init");
        }
    }
    else
    {
        pthread_mutex_lock(&shmem->procNumMtx);
        shmem->procNum++;
        pthread_mutex_unlock(&shmem->procNumMtx);
    }
    sem_post(shmem_init_sem);
    return shmem;
}

void* sigHandler(void* args)
{
    status_t* status = (status_t*)args;
    int signo;
    if (sigwait(&status->new_mask, &signo))
        ERR("sigwait failed.");
    if (signo != SIGINT)
    {
        ERR("unexpected signal");
    }

    pthread_mutex_lock(&status->mutex);
    status->running = 0;
    pthread_mutex_unlock(&status->mutex);
    return NULL;
}

void initSigHandler(status_t* status)
{
    sigemptyset(&status->new_mask);
    sigaddset(&status->new_mask, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &status->new_mask, &status->old_mask))
        ERR("SIG_BLOCK error");

    pthread_t sighandling_thread;
    pthread_create(&sighandling_thread, NULL, sigHandler, &status);
}

void finalise(sharedDatA_t* shmem)
{
    int processesLeft;
    robustLock(&shmem->procNumMtx, shmem);
    processesLeft = --shmem->procNum;
    pthread_mutex_unlock(&shmem->procNumMtx);

    if (processesLeft == 0)
    {
        double result = summarize_calculations(shmem->total, shmem->hits, shmem->a, shmem->b);
        printf("Result: %lf\n", result);
        shm_unlink(SHMEM_NAME);
    }
}

void calculateBatches(int N, int a, int b, sharedDatA_t* shmem, status_t* status)
{
    uint64_t hits;
    while (status->running)
    {
        hits = (uint64_t)randomize_points(N, a, b);
        robustLock(&shmem->dataMtx, shmem);
        shmem->hits += hits;
        shmem->total += (uint64_t)N;
        printf("Total: %lu, Hits: %lu\n", shmem->total, shmem->hits);
        pthread_mutex_unlock(&shmem->dataMtx);
    }
}

int main(int argc, char* argv[])
{
    if (argc < 4)
        usage(argv);
    int a = atoi(argv[1]);
    int b = atoi(argv[2]);
    int N = atoi(argv[3]);
    if (0 >= N)
        usage(argv);

    sharedDatA_t* shmem = initMem(a, b);
    if (a != shmem->a || b != shmem->b)
    {
        puts("a and b are already set. Using previous values...");
        a = shmem->a;
        b = shmem->b;
    }

    robustLock(&shmem->procNumMtx, shmem);
    printf("Currently running processes: %d\n", shmem->procNum);
    pthread_mutex_unlock(&shmem->procNumMtx);

    pthread_mutex_t statusMtx = PTHREAD_MUTEX_INITIALIZER;
    status_t status;
    status.running = 1;
    status.mutex = statusMtx;

    sigemptyset(&status.new_mask);
    sigaddset(&status.new_mask, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &status.new_mask, &status.old_mask))
        ERR("SIG_BLOCK error");

    pthread_t sighandling_thread;
    pthread_create(&sighandling_thread, NULL, sigHandler, &status);

    calculateBatches(N, a, b, shmem, &status);

    finalise(shmem);
    return EXIT_SUCCESS;
}
