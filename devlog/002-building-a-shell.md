# Devlog 002: Building a shell from scratch

> mysh devlog series: [000 The plan](000-the-plan.md) · [001 Planning the shell](001-planning-the-shell.md) · **002 Building a shell** · [003 Variables, lists and a bug hunt](003-variables-lists-and-a-bug-hunt.md)

**Goal:** a small Unix shell in C (`mysh.c`) to understand processes, file descriptors and signals before writing a kernel.

This post covers the first version, following the six steps from [001](001-planning-the-shell.md). Variables, `&&`/`||` and fd redirection came later, in [003](003-variables-lists-and-a-bug-hunt.md).

**Result:** REPL, quoting, builtins, `<` `>` `>>`, pipelines of any length, background jobs, and real job control (Ctrl-C, Ctrl-Z, `jobs`, `fg`, `bg`). The test suite passes under AddressSanitizer + UBSan with no leaks.

## Build steps

### 1. REPL
`getline()` in a loop, print `mysh> `, handle EOF (Ctrl-D). `getline` allocates the buffer for us. We reuse it every iteration and `free` it once at exit.

### 2. Parse and execute
A tokenizer splits the line into words and operators. I made `| < > >> &` their own tokens even without spaces, so `echo x>file` works. It also handles `'single'` and `"double"` quotes. Execution is the classic trio:

```
fork()  ->  child: execvp()     parent: waitpid()
```

If `execvp` returns, it failed, so the child prints the error and calls `_exit(127)`. It uses `_exit`, not `exit`, so the child doesn't flush stdio buffers it inherited from the parent.

### 3. Builtins
`cd`, `exit`, `jobs`, `fg` and `bg` must run in the shell process itself, because they change the shell's own state (working directory, job table). `echo` is also a builtin. When it's part of a pipeline or has a redirect, it runs inside the forked child instead, so `echo hi | cat` works.

### 4. Redirection
In the child, before `exec`: `open()` the file, `dup2(fd, 0 or 1)`, then `close(fd)`. File descriptors survive `exec`, so the new program never knows it was redirected. `>>` just swaps `O_TRUNC` for `O_APPEND`.

### 5. Pipes
For `a | b | c`: create a pipe before forking each stage except the last. Each child `dup2`s its ends onto stdin/stdout, then closes every pipe fd it holds. The parent closes its copies right after each fork.

### 6. Job control
- Every pipeline gets its own **process group**, set with `setpgid()` in both parent and child to avoid a race.
- The shell ignores `SIGINT`/`SIGTSTP`/`SIGTTOU`. Children reset them to default, so Ctrl-C kills the foreground job and not the shell.
- Foreground jobs get the terminal via `tcsetpgrp()`. The shell takes it back afterwards.
- `waitpid(-pgid, ..., WUNTRACED)` waits for the foreground job to finish or stop. Background jobs are collected with `WNOHANG` before every prompt.
- A small job table (pids, state, command text) lets `fg`/`bg` find jobs again. `kill(-pgid, SIGCONT)` resumes them.

## Bugs and pitfalls

I reproduced the three classic bugs on purpose by deleting the relevant line, so I could see exactly how each one fails.

### Bug 1: the parent forgets to close its pipe ends, so everything hangs
**Symptom:** `echo hi | cat` prints `hi` and then never returns. `ps` shows `cat` still alive.
**Cause:** `cat` only sees EOF when **every** write end of the pipe is closed. The parent still held the write end, so the pipe never closed and `cat` waited forever.
**Fix:** after each `fork()`, the parent closes the previous read end and the current write end. Each child closes every pipe fd it doesn't use.
**Lesson:** a pipe is shared across processes, and EOF means all copies are closed, not just yours.

### Bug 2: an unused read end in the child, so `yes | head -1` never stops
**Symptom:** `head` exits, but `yes` keeps running forever.
**Cause:** `yes` should die from `SIGPIPE` when it writes to a pipe with no readers. A leaked read-end fd means a reader technically still exists, so no `SIGPIPE`.
**Fix:** close the unused read end in the writer child.
**Lesson:** fd leaks change program behaviour, not just resource usage.

### Bug 3: memory leaks from tokens
**Symptom:** AddressSanitizer reports on exit:
```
ERROR: LeakSanitizer: detected memory leaks
Direct leak of 23 byte(s) in 4 object(s) allocated from:
    #1 tokenize
    #2 main
```
**Cause:** each token is a `strdup`/`malloc` string, allocated every loop iteration and never freed.
**Fix:** `free_tokens()` runs once per command line. The parsed `Cmd` structs only store pointers into the tokens, so no extra copies need freeing. The tokenizer also frees its partial results on error paths such as an unterminated quote.
**Lesson:** decide up front who owns each allocation. Here the token array owns the strings and everything else borrows them. Run with `make asan` regularly.

### Pitfalls I designed around
- **Race on `setpgid`/`tcsetpgrp`:** the call is made in both parent and child, because either could run first.
- **Stale stop notifications:** when a pipeline stops, every member reports a stop. If only the first is collected, the others show up later and stop the job again after `fg`. After a stop I collect the rest immediately.
- **`exit` vs `_exit` in children** (see step 2).
- **Redirects override pipes:** in `a | b > f`, `b`'s stdout goes to the file. I apply redirection after wiring pipes, so the file wins.

## Testing
- `tests.sh`: 20 regression cases (quotes, redirects, 3-stage pipes, a 200,000-line pipe to catch buffer deadlocks, syntax errors, exit codes).
- Run everything under `make asan` (AddressSanitizer + UBSan).
- Job control was tested by driving the shell through a pseudo-terminal: Ctrl-C kills only the child, Ctrl-Z stops it, and `jobs`, `bg` and `fg` behave.

## Known limitations / next steps
- No `$VAR` expansion, globbing (`*.c`), `&&`/`||`/`;`, or command history.
- `cd -` and tab completion are missing.
- Background-job notices only appear when the next prompt is drawn (no `SIGCHLD` handler).
- Next: use what I learned about fds, `fork` and signals in the kernel (process table, syscalls, an ELF loader). This shell is what I'll port to my OS.
