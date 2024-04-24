override CFLAGS=-Wall -Wextra -Wshadow -g -O0 -fsanitize=address,undefined -lm

ifdef CI
override CFLAGS=-Wall -Wextra -Wshadow -Werror -lm
endif

NAME=sop-integral

.PHONY: clean all

all: ${NAME}

${NAME}: ${NAME}.c macros.h
	gcc $(CFLAGS) -o ${NAME} ${NAME}.c

clean:
	rm -f ${NAME}

clean-resources:
	rm -f /dev/shm/sem.sop-shmem-sem /dev/shm/sop-shmem
