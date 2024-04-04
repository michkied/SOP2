override CFLAGS=-Wall -Wextra -Wshadow -g -O0 

ifdef CI
override CFLAGS=-Wall -Wextra -Wshadow -Werror
endif

NAME=sop-rlt

.PHONY: clean all

all: ${NAME}

${NAME}: ${NAME}.c
	gcc $(CFLAGS) -o ${NAME} ${NAME}.c

clean:
	rm -f ${NAME}
