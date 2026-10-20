Devlog 001: Building a shell from scratch

Goal: a small Unix shell in C (mysh.c, ~450 lines) to understand processes, file descriptors and signals before writing a kernel.

Result: REPL, quoting, builtins, < > >>, pipelines of any length, background jobs, and real job control (Ctrl-C, Ctrl-Z, jobs, fg, bg). The test suite passes under AddressSanitizer + UBSan with no leaks.


1. REPL

getline() in a loop, print mysh> , handle EOF (Ctrl-D). getline allocates the buffer for us. We reuse it every iteration and free it once at exit.
e.g:
  ❯ ./mysh
  mysh> test
  you said: [test]
  mysh> hello world
  you said: [hello world]
  mysh>

  ~/myp/mysh master* 8s
  ❯ printf 'hello\nworld\n' | ./mysh
  mysh> you said: [hello]
  mysh> you said: [world]
  mysh>
