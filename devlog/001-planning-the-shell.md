# Devlog 001: Planning the shell and setting up the project

> mysh devlog series: [000 The plan](000-the-plan.md) · **001 Planning the shell** · [002 Building a shell](002-building-a-shell.md) · [003 Variables, lists and a bug hunt](003-variables-lists-and-a-bug-hunt.md)

First warm-up project from the [plan](000-the-plan.md): a Unix shell, `mysh`.
Before writing code I broke it into six steps, each one a small program that
works on its own. This post is that plan plus how the project is set up.

## Why a shell first?

A shell is a tiny tour of the operating system's process model:

- processes: `fork`, `exec`, `wait`
- file descriptors: redirection and pipes
- signals and process groups: Ctrl-C, Ctrl-Z, background jobs

My kernel will have to implement the other side of these ideas, so I want to
use them from user space first.

## The six steps

| # | Step | Key calls | Idea |
|---|---|---|---|
| 1 | REPL | `getline()` | Loop: print `mysh> `, read a line, print it back |
| 2 | Parse and execute | `fork()`, `execvp()`, `waitpid()` | Split the line by spaces; the child runs the program, the parent waits |
| 3 | Builtins | none (runs in the shell itself) | `cd`, `exit`, `echo` must run in the parent, because `cd` changes the current process's directory and an `exec`'d child can't do that for the parent |
| 4 | Redirection `<` `>` | `open()`, `dup2()`, `close()` | Before `exec`, point fd 0 or fd 1 at a file |
| 5 | Pipes `\|` | `pipe()`, `fork()` twice, `dup2()` | First child writes to the pipe, second reads from it |
| 6 | Job control `&`, `fg`, `bg` | `setpgid()`, `tcsetpgrp()`, signals | Don't wait for `&` commands; keep a table of background jobs to bring back later |

Each step ends with something I can run and demonstrate, which also gives me
one devlog-worthy milestone per step.

## Design decisions made up front

- **Language: C.** The calls above are C APIs, and C is what the kernel will
  most likely use too (the kernel language itself is still undecided).
- **Process groups from the start.** Step 6 only works if each job is its own
  process group, so I'll design for it early instead of bolting it on.
- **A job table.** A small array of jobs (pids, state, command text) so `jobs`,
  `fg` and `bg` can find things later.
- **Tests alongside the code.** A shell is easy to break silently, so I want
  a script that feeds commands in and compares the output.

## Project setup

```
mysh/
├── mysh.c        the shell
├── Makefile      build and test targets
├── tests.sh      regression suite
├── devlog/       one post per milestone
└── README.md
```

- **Git:** repo initialised from the start, with a `.gitignore` for the
  compiled binaries so only source is tracked.
- **Build:** `make`, compiling with
  `-std=gnu11 -Wall -Wextra -Wpedantic -g`. Warnings on, debug symbols on.
- **Sanitizers:** a second build target (`make asan`) using AddressSanitizer
  and UBSan to catch memory leaks and undefined behaviour while testing.
- **Devlog:** one markdown post per milestone in `devlog/`, numbered from 000.

## Things I expect to be tricky

These are the spots I think will cause bugs, so I'll test them on purpose:

- **Pipes:** every process must close the pipe ends it doesn't use, or
  programs wait forever for an end-of-file that never comes.
- **Memory:** every string from the parser is allocated, so ownership has to
  be clear, and the sanitizer should report no leaks.
- **Signals:** the shell must survive Ctrl-C while its children do not.
- **Background jobs:** finished children must be reaped so they don't linger
  as zombies.

## Definition of done

- All six steps work, each with at least one test
- The suite passes under AddressSanitizer with no leaks
- A README with a demo, plus a devlog post covering the bugs

## Next

[Devlog 002](002-building-a-shell.md): building it.
