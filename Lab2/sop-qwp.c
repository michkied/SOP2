#include <asm-generic/errno-base.h>
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <mqueue.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), kill(0, SIGKILL), exit(EXIT_FAILURE))

#define MAX_QUEUE_NAME 256
#define TASK_QUEUE_NAME "/task_queue_%d"
#define RESULT_QUEUE_NAME "/result_queue_%d_%d"

#define MAX_MSG_SIZE 10  // Max message size
#define MAX_MSGS 10      // Queue size

#define MIN_WORKERS 2
#define MAX_WORKERS 20
#define MIN_TIME 100
#define MAX_TIME 5000
#define TASK_TODO 5
#define MS_TO_NS 1000000
#define S_TO_NS 1000000000

#define CHILD_SLEEP_MIN 500
#define CHILD_SLEEP_MAX 2000

volatile sig_atomic_t childrenAlive = 0;

typedef struct task
{
    float a;
    float b;
} task_t;

typedef struct resultM
{
    pid_t pid;
    float result;
} resultM_t;

void usage(const char *name)
{
    fprintf(stderr, "USAGE: %s N T1 T2\n", name);
    fprintf(stderr, "N: %d <= N <= %d - number of workers\n", MIN_WORKERS, MAX_WORKERS);
    fprintf(stderr, "T1, T2: %d <= T1 < T2 <= %d - time range for spawning new tasks\n", MIN_TIME, MAX_TIME);
    exit(EXIT_FAILURE);
}

void sethandler(void (*f)(int, siginfo_t *, void *), int sigNo)
{
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_sigaction = f;
    act.sa_flags = SA_SIGINFO;
    if (-1 == sigaction(sigNo, &act, NULL))
        ERR("sigaction");
}

void sigchld_handler(int sig, siginfo_t *s, void *p)
{
    (void)s;
    (void)p;
    (void)sig;
    pid_t pid;
    for (;;)
    {
        pid = waitpid(0, NULL, WNOHANG);
        if (pid == 0)
            return;
        if (pid <= 0)
        {
            if (errno == ECHILD)
                return;
            ERR("waitpid");
        }
        childrenAlive--;
    }
}

void mq_handler(int sig, siginfo_t *info, void *p)
{
    mqd_t *resultQueue;
    unsigned msg_prio;
    (void)sig;
    (void)p;

    resultQueue = (mqd_t *)info->si_value.sival_ptr;

    static struct sigevent not ;
    not .sigev_notify = SIGEV_SIGNAL;
    not .sigev_signo = SIGRTMIN;
    not .sigev_value.sival_ptr = resultQueue;
    if (mq_notify(*resultQueue, &not ) < 0)
        ERR("mq_notify");

    resultM_t resultMessage;
    if (mq_receive(*resultQueue, (char *)&resultMessage, sizeof(resultMessage), &msg_prio) < 1)
    {
        ERR("mq_receive");
    }

    printf("Result from worker [%d]: [%f]\n", resultMessage.pid, resultMessage.result);
}

int getRandom(int min, int max) { return min + rand() % (max - min + 1); }

void childWork(mqd_t taskQueue, mqd_t resultQueue)
{
    (void)resultQueue;
    pid_t pid = getpid();
    printf("[%d] Worker ready!\n", pid);
    srand(pid);

    int tasksLeft = 5;
    while (tasksLeft)
    {
        task_t task;
        unsigned msg_prio;
        if (TEMP_FAILURE_RETRY(mq_receive(taskQueue, (char *)&task, sizeof(task_t), &msg_prio)) < 0)
        {
            ERR("mq_receive");
        }

        printf("[%d] Received task [%f,%f]\n", pid, task.a, task.b);

        int sleepTime = getRandom(CHILD_SLEEP_MIN, CHILD_SLEEP_MAX);
        struct timespec t1 = {(int)(sleepTime * MS_TO_NS / S_TO_NS), (sleepTime * MS_TO_NS) % S_TO_NS};
        struct timespec t2;
        while (nanosleep(&t1, &t2))
        {
            t1 = t2;
        }

        resultM_t resultM;
        resultM.result = task.a + task.b;
        resultM.pid = pid;

        if (TEMP_FAILURE_RETRY(mq_send(resultQueue, (const char *)&resultM, sizeof(resultM_t), 1)) < 0)
            ERR("mq_send");

        printf("[%d] Result sent [%f]\n", pid, resultM.result);
        tasksLeft--;
    }

    printf("[%d] Exits!\n", pid);
    return;
}

void parentWork(int N, int T1, int T2, mqd_t taskQueue, mqd_t *resultQueues)
{
    (void)resultQueues;
    int tasksToCreate = 5 * N;
    srand(getpid());

    while (tasksToCreate)
    {
        int sleepTime = getRandom(T1, T2);
        struct timespec t1 = {(int)(sleepTime * MS_TO_NS / S_TO_NS), (sleepTime * MS_TO_NS) % S_TO_NS};
        struct timespec t2;
        while (nanosleep(&t1, &t2))
        {
            t1 = t2;
        }

        task_t task;
        task.a = getRandom(0, 100);
        task.b = getRandom(0, 100);

        if (TEMP_FAILURE_RETRY(mq_send(taskQueue, (const char *)&task, sizeof(task_t), 1)) < 0)
        {
            if (errno == EAGAIN)
            {
                printf("Queue is full!\n");
                break;
            }
            else
                ERR("mq_send");
        }

        printf("New task queued: [%f,%f]\n", task.a, task.b);
        tasksToCreate--;
    }
}

void createChildren(int N, mqd_t taskQueue, mqd_t *resultQueues)
{
    for (int i = 0; i < N; i++)
    {
        switch (fork())
        {
            case 0:
                childWork(taskQueue, resultQueues[i]);
                exit(EXIT_SUCCESS);
            case -1:
                ERR("fork");
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 4)
        usage(argv[0]);

    int N = atoi(argv[1]);
    if (N < MIN_WORKERS || N > MAX_WORKERS)
        usage(argv[0]);

    int T1 = atoi(argv[2]);
    int T2 = atoi(argv[3]);
    if (T1 < MIN_TIME || T1 >= T2 || T2 > MAX_TIME)
        usage(argv[0]);

    sethandler(sigchld_handler, SIGCHLD);
    sethandler(mq_handler, SIGRTMIN);

    mqd_t taskQueue;
    char taskQueueName[MAX_QUEUE_NAME];
    snprintf(taskQueueName, MAX_QUEUE_NAME, TASK_QUEUE_NAME, getpid());

    struct mq_attr attr;
    attr.mq_maxmsg = MAX_MSGS;
    attr.mq_msgsize = sizeof(task_t);

    if ((taskQueue = TEMP_FAILURE_RETRY(mq_open(taskQueueName, O_RDWR | O_CREAT, 0600, &attr))) == (mqd_t)-1)
        ERR("mq_open");

    mqd_t *resultQueues = malloc(sizeof(mqd_t) * N);
    if (!resultQueues)
        ERR("malloc");

    for (int i = 0; i < N; i++)
    {
        char resultQueueName[MAX_QUEUE_NAME];
        snprintf(resultQueueName, MAX_QUEUE_NAME, RESULT_QUEUE_NAME, getpid(), i);

        if ((resultQueues[i] =
                 TEMP_FAILURE_RETRY(mq_open(resultQueueName, O_RDWR | O_CREAT | O_NONBLOCK, 0600, &attr))) == (mqd_t)-1)
            ERR("mq_open");

        static struct sigevent noti;
        noti.sigev_notify = SIGEV_SIGNAL;
        noti.sigev_signo = SIGRTMIN;
        noti.sigev_value.sival_ptr = &resultQueues[i];
        if (mq_notify(resultQueues[i], &noti) < 0)
            ERR("mq_notify");
    }

    printf("Server is starting...\n");

    childrenAlive = N;
    createChildren(N, taskQueue, resultQueues);

    struct mq_attr parentAttr;
    attr.mq_flags = O_NONBLOCK;
    attr.mq_maxmsg = MAX_MSGS;
    attr.mq_msgsize = sizeof(task_t);

    mq_setattr(taskQueue, &parentAttr, NULL);
    parentWork(N, T1, T2, taskQueue, resultQueues);

    while (childrenAlive)
    {
        sleep(1);
    }

    printf("All child processes have finished\n");

    free(resultQueues);

    return EXIT_SUCCESS;
}
