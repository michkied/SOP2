#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UNUSED(x) ((void)(x))
#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), kill(0, SIGKILL), exit(EXIT_FAILURE))

#define MAX_NUM 36
#define PULL_OUT_PERCENTAGE 10

typedef struct betMsg
{
    pid_t pid;
    int number;
    int ammount;
} betMsg_t;

void usage(char* name)
{
    fprintf(stderr, "USAGE: %s N M\n", name);
    fprintf(stderr, "N: N >= 1 - number of players\n");
    fprintf(stderr, "M: M >= 100 - initial amount of money\n");
    exit(EXIT_FAILURE);
}

int sethandler(void (*f)(int), int sigNo)
{
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = f;
    if (-1 == sigaction(sigNo, &act, NULL))
        return -1;
    return 0;
}

void sigchld_handler(int sig)
{
    UNUSED(sig);
    pid_t pid;
    for (;;)
    {
        pid = waitpid(0, NULL, WNOHANG);
        if (0 == pid)
            return;
        if (0 >= pid)
        {
            if (ECHILD == errno)
                return;
            ERR("waitpid:");
        }
    }
}

void parentWork(int N, int* betPipes, int* resultPipes)
{
    srand(getpid());

    betMsg_t* bets;
    bets = malloc(sizeof(betMsg_t) * N);
    if (!bets)
        ERR("malloc");

    ssize_t count;
    int players;
    do
    {
        players = 0;

        for (int i = 0; i < N; i++)
        {
            count = TEMP_FAILURE_RETRY(read(betPipes[i], &(bets[i]), sizeof(betMsg_t)));
            if (count < 0)
                ERR("read");
            if (count == 0)
                continue;

            printf("Croupier: %d placed %d on a %d\n", bets[i].pid, bets[i].ammount, bets[i].number);
            players++;
        }

        if (players == 0)
            break;

        int result = rand() % MAX_NUM + 1;
        printf("Croupier: %d is the lucky number!\n", result);

        for (int i = 0; i < N; i++)
        {
            if (TEMP_FAILURE_RETRY(write(resultPipes[i], &result, sizeof(int)) < 0))
            {
                if (errno == EPIPE)
                    continue;
                ERR("write");
            }
        }

    } while (players > 0);

    printf("Casinno always wins!\n");
    free(bets);
};

void childWork(int M, int betPipeWrite, int resultPipeRead)
{
    srand(getpid());
    int balance = M;

    while (balance > 0)
    {
        if (rand() % 101 < PULL_OUT_PERCENTAGE)
        {
            printf("[%d]: I saved $%d\n", getpid(), balance);
            break;
        }

        betMsg_t msg;
        msg.pid = getpid();
        msg.number = rand() % MAX_NUM + 1;
        msg.ammount = 1 + rand() % (balance);

        balance -= msg.ammount;

        if (TEMP_FAILURE_RETRY(write(betPipeWrite, &msg, sizeof(betMsg_t)) < 0))
            ERR("write");

        int result;
        if (TEMP_FAILURE_RETRY(read(resultPipeRead, &result, sizeof(int)) < 0))
            ERR("read");

        if (result == msg.number)
        {
            balance += msg.ammount * 36;
            printf("[%d]: Woah! I won [%d] and my current balance is %d\n", getpid(), msg.ammount * 36, balance);
        }
    }
    if (balance <= 0)
        printf("[%d]: I'm broke!\n", getpid());
    if (close(betPipeWrite) || close(resultPipeRead))
        ERR("close");
};

void spawnChildren(int N, int M, int* betPipes, int* resultPipes)
{
    for (int i = 0; i < N; i++)
    {
        int betPipe[2], resultPipe[2];
        if (pipe(betPipe) || pipe(resultPipe))
            ERR("pipe");

        pid_t pid = fork();
        switch (pid)
        {
            case 0:
            {
                if (close(betPipe[0]) || close(resultPipe[1]))
                    ERR("close");

                printf("[%d]: I have $%d and I'm going to play roulette\n", getpid(), M);
                childWork(M, betPipe[1], resultPipe[0]);
                exit(EXIT_SUCCESS);
            }
            default:
            {
                if (close(betPipe[1]) || close(resultPipe[0]))
                    ERR("close");
                betPipes[i] = betPipe[0];
                resultPipes[i] = resultPipe[1];
            }
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
        usage(argv[0]);

    int N = atoi(argv[1]);
    int M = atoi(argv[2]);
    if (N <= 0)
        usage(argv[0]);
    if (M < 100)
        usage(argv[0]);

    int* betPipes;
    betPipes = malloc(sizeof(int) * N);
    int* resultPipes;
    resultPipes = malloc(sizeof(int) * N);
    if (!betPipes || !resultPipes)
        ERR("malloc");

    sethandler(sigchld_handler, SIGCHLD);
    sethandler(SIG_IGN, SIGPIPE);

    spawnChildren(N, M, betPipes, resultPipes);
    parentWork(N, betPipes, resultPipes);

    for (int i = 0; i < N; i++)
    {
        if (close(betPipes[i]) || close(resultPipes[i]))
            ERR("close");
    }

    free(betPipes);
    free(resultPipes);
}
