# mysh

A Unix shell written from scratch in C (~700 lines, no libraries beyond libc).
It supports pipes, redirection, variables, command lists and full job control,
and it is covered by a 64-case regression suite that also runs under
AddressSanitizer.

Built as a learning project: before writing a kernel, I wanted to understand
`fork`, `exec`, file descriptors, process groups and signals from the user
side first. The design notes and the bugs I hit along the way are in the
[devlog](devlog/).

## Demo

```
mysh> echo hello | tr a-z A-Z
HELLO
mysh> ls /nonexistent 2>&1 | wc -l
1
mysh> A=5; echo $A
5
mysh> false && echo yes || echo no
no
mysh> FOO=bar printenv FOO
bar
mysh> echo "[$FOO]"
[]
mysh> sleep 60 &
[1] 4242
mysh> jobs
[1] Running   sleep 60 &
mysh> fg
sleep 60
^C
mysh> cd /tmp && echo $PWD
/tmp
```

## Features

| Area | Supported |
|---|---|
| Basics | `mysh>` REPL, `'single'` and `"double"` quotes, exit status |
| Builtins | `cd` (including `cd -`), `exit [n]`, `echo [-n]`, `jobs`, `fg [%n]`, `bg [%n]` |
| Variables | `$VAR`, `${VAR}`, `$?`, `$$`, `VAR=value`, `FOO=1 cmd` prefixes |
| Redirection | `<`  `>`  `>>`  `N<`  `N>`  `N>>`  `N>&M`  `&>`  `>&file` |
| Pipes | `a \| b \| c`, and `\|&` to pipe stderr too (per link) |
| Lists | `;`  `&&`  `\|\|`  and `&` anywhere on the line |
| Job control | process groups, Ctrl-C, Ctrl-Z, `jobs`, `fg`, `bg` |

## Build and run

Requires `gcc` (or any C compiler) and `make` on Linux or macOS.

```sh
git clone https://github.com/YOUR-USERNAME/mysh.git
cd mysh
make
./mysh
```

## Testing

```sh
make test        # 64 regression cases against ./mysh
make test-asan   # same suite against a build with AddressSanitizer + UBSan
```

The suite (`tests.sh`) feeds command lines to the shell and compares the
output. Every bug I fixed has a test that pins it down. Tests run with a
timeout, because a pipe bug makes a shell hang rather than fail.
GitHub Actions runs both suites on every push.

## How it works

Each line goes through four stages:

1. **Tokenize**: split into words and operators (`| < > >> & && || ; ...`),
   strip quotes. Variables are *not* expanded here.
2. **Parse**: build a command list of pipelines, each a list of commands with
   their arguments and redirections. Syntax errors are caught here.
3. **Expand**: replace `$VAR`, `$?` and friends, right before each pipeline
   runs, so `A=1; echo $A` sees the new value.
4. **Execute**: `fork` one child per pipeline stage, wire up pipes and
   redirections with `dup2`, put the pipeline in its own process group, then
   `exec` (or wait, for foreground jobs).

Details worth knowing:

- Expanding late is what makes `false || echo $?` and `A=1; echo $A` correct.
- `&&` / `||` skip only the *next* command and carry the status forward, so
  `false && a || b` runs `b`.
- Redirections are applied in order, so `> f 2>&1` and `2>&1 > f` differ,
  like in a real shell.
- Every process group gets the terminal when it runs in the foreground, which
  is why Ctrl-C stops the job and not the shell.

## Project layout

```
mysh.c                      the whole shell
tests.sh                    regression suite
Makefile                    build, test, test-asan targets
devlog/                     design notes and bugs
.github/workflows/ci.yml    CI: build + both test suites
```

## Devlog

- [002: Building a shell](devlog/002-building-a-shell.md): the core: REPL,
  fork/exec, pipes, redirection, job control, and the classic pipe and leak bugs
- [003: Variables, lists and a bug hunt](devlog/003-variables-lists-and-a-bug-hunt.md):
  adding variables, `&&`/`||`, fd redirection, and fixing what broke

## Limitations

- No globbing (`*.c`), `$(...)`, `export`/`unset`, history, tab completion or
  here-docs. `$1`-`$9`, `$@`, `$*` expand to empty; `$#` is always 0.
- All assigned variables are exported to child processes (real shells need
  `export`).
- `a && b &` backgrounds only `b`, not the whole list.
- `N<&M` is not supported, and builtins other than `echo` don't run inside
  pipelines.

## What's next

This shell is a stepping stone: its fork/exec/pipe/signal logic maps directly
onto the process model I plan to build in my own hobby kernel, and this shell
is what I intend to port to it.

## License

MIT, see [LICENSE](LICENSE).
