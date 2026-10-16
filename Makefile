CC      = cc
CFLAGS  = -Wall -Wextra -g -O0 -fsanitize=address
LDFLAGS = -fsanitize=address

mysh: mysh.o
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f mysh *.o
