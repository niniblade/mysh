CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -Wpedantic -g -O0
 
mysh: mysh.c
	$(CC) $(CFLAGS) -o $@ $<
 
asan: mysh.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o mysh-asan $<
 
test: mysh
	./tests.sh
 
test-asan: asan
	SH=./mysh-asan ./tests.sh
 
clean:
	rm -f mysh mysh-asan
 
.PHONY: asan test test-asan clean
