# Devlog 003: Variables, lists and a bug hunt

> mysh devlog series: [000 The plan](000-the-plan.md) · [001 Planning the shell](001-planning-the-shell.md) · [002 Building a shell](002-building-the-shell.md) ·**003Variables, lists and a bug hunt**

**Goal:** extend the shell from devlog 002 with variables (`$VAR`, `$?`,
`VAR=value`), command lists (`;`, `&&`, `||`), `|&`, and fd redirection
(`2>&1`, `&>`, `2>>`).

**What happened:** I merged in these features using code from outside help
(I did not write all of it myself). The merged file looked complete, but when
I had it reviewed it turned out to be broken in several layers, like peeling
an onion: fixing one bug exposed the next. This post records each layer, what
the symptom would be, and the lesson.

**Result:** all features work, 64 regression tests pass, also under
AddressSanitizer + UBSan.

## Hurdle 1: it didn't compile

The last line of the main loop was `free_tokens(cl_pipes_tok... );`, a
half-finished placeholder. The correct call was `free_tokens(tok, n);`.

**Lesson:** merged code has to be built before it is trusted. A clean compile
with `-Wall -Wextra -Wpedantic` is the minimum bar.

## Hurdle 2: once it compiled, nothing ran

Each pipeline's command count `n` started at 0 and was never incremented for
the first command. A plain `ls` had `n == 0`, so the executor forked nothing,
and builtins were skipped because they were gated on `n == 1`. A pipe made it
worse: `a | b` wrote `b` into the slot where `a` was stored, leaving a
half-overwritten command.

**Fix:** pipelines start with `n = 1`, and `|` does `c = &cur->cmds[cur->n++]`.
**Lesson:** off-by-one bugs in a parser are silent. A single end-to-end test
(`echo hi`) would have caught it at once.

## Hurdle 3: `2>&1` meant "a file called 1"

Only the shorthand `>&` set the "duplicate an fd" flag. The numbered form
`2>&` fell through to the file branch, so `2>&1` redirected stderr into a file
named `1`. Related bugs in the same function:

- `2>>file` truncated instead of appending (only the exact string `>>`
  enabled append mode)
- `3>file` opened the file read-only (open flags were chosen from the fd
  number instead of from the operator's direction)
- `cat < 123` treated a file named `123` as a file descriptor

**Fix:** rewrote `add_redir` to read the operator once: optional leading
digit, then `<`, `>`, `>>`, `>&` or `&>`, and store the direction explicitly
(`is_input`). Also guarded `dup2` for when `open()` already returns the
target fd, since `close(fd)` afterwards would close it.
**Lesson:** decide behaviour from the operator, not from side effects like
the fd number.

## Hurdle 4: variables were expanded too early

The tokenizer expanded `$VAR` while splitting the line, before anything ran.
So `A=5; echo $A` printed nothing (the assignment hadn't happened yet), and
`false || echo $?` showed the *previous* line's status.

**Fix:** the tokenizer now only splits and strips quotes. Expansion is a
separate step that runs just before each pipeline executes. Two marker bytes
carry what the expander needs to know: one for a `$` that sat inside single
quotes (stays literal) and one for words that contained quotes (so an empty
`"$UNSET"` is kept but an unquoted empty `$UNSET` disappears, like in bash).
**Lesson:** the order of shell stages matters. Tokenize, parse, expand,
execute is the standard pipeline for a reason.

## Hurdle 5: `&&` / `||` stopped the whole list

On a skip, the loop did `break`. So `false && echo a || echo b` never
printed `b`. In a real shell, a skipped command is skipped *alone*, and the
status carries on to the next operator.

**Fix:** use `continue` and keep the previous status:
`false` (1) → skip `a` (status stays 1) → `||` sees nonzero → run `b`.
Related: `$?` was never updated after `echo` or assignments, and `exit`
did not stop the rest of the list.

## Hurdle 6: environment variables vs. the shell's variable table

The merged code kept its own variable table and appended it to the child's
environment. Two problems: `HOME=/tmp` changed `$HOME` inside the shell, but
child programs still saw the old value (the environment then held `HOME`
twice, and `getenv` returns the first match), and prefix assignments like
`FOO=bar printenv FOO` were skipped without ever being applied.

**Fix:** dropped the custom table. Assignments call `setenv()`, and children
use plain `execvp`. Prefix assignments call `setenv()` inside the child, after
`fork()`, so they never leak into the shell.
**Trade-off:** every variable is exported. Real shells need `export`; I note
this in the README.

## Hurdle 7: smaller bugs found along the way

- `|&` applied to the whole pipeline, so `a | b |& c` also sent `a`'s stderr
  into the pipe. It is now a flag on the single command before the `|&`.
- Redirection target strings were `strdup`'d and never freed. Found in code
  review; the fix borrows pointers from the token array, which is freed once
  per line. The sanitizer run is clean.
- Trailing operators (`echo hi |`, `echo hi &&`) were silently accepted; they
  are now syntax errors.
- Background job completion changed `$?` behind the scenes. Each job now
  stores its own status and only foreground work updates `$?`.

## Testing approach

I extended `tests.sh` from 20 to 64 cases, one per rule above (for example
`false && echo a || echo b`, `A=5; echo $A`, `2>>`, `3>`, `|&` per link).
Everything also runs under AddressSanitizer + UBSan via `make test-asan`.
Two habits that paid off: every bug gets a test, and tests run with a
timeout, because a broken pipe leaves the shell hanging, not failing.

## What I learned

1. A shell is a pipeline of stages, and where you do each job (expansion,
   short-circuiting, redirection) decides what is possible.
2. File descriptors and `dup2` ordering are the heart of redirection.
3. Merging code I didn't write requires reading it line by line and testing
   every feature, not trusting that it works.
4. Sanitizers and a regression suite turn "it seems to work" into evidence.

## Next steps

- Globbing (`*.c`), `$(...)` command substitution, `export`/`unset`
- Command history and line editing
- Then: start the kernel, where this fork/exec/signal knowledge gets reused
