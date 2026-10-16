#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_ARGS 64
int main(void) {
    char *line = NULL;
    size_t cap = 0;

    while (1) {
        printf("mysh> ");                     /* step 1 — unchanged */
        fflush(stdout);                       /* step 1 — unchanged */

        ssize_t n = getline(&line, &cap, stdin);   /* step 1 — unchanged */
        if (n < 0) { if (feof(stdin)) printf("\n"); break; }
        line[strcspn(line, "\n")] = '\0';     /* step 1 — unchanged */

        char *argv[MAX_ARGS];
        int argc = 0;
        char *tok = strtok(line, " \t");
        while (tok && argc < MAX_ARGS - 1) {
            argv[argc++] = tok;
            tok = strtok(NULL, " \t");
        }
        
        argv[argc] = NULL;
        if (argc == 0) continue;

        pid_t pid = fork();
        if (pid == 0) {
            execvp(argv[0], argv);
            fprintf(stderr, "mysh: %s: %s\n", argv[0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }
        int status;
        waitpid(pid, &status, 0);
    }

    free(line);
    return 0;
}
