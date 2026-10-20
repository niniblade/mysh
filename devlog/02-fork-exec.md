Parse and execute

A tokenizer splits the line into words and operators. I made | < > >> & their own tokens even without spaces, so echo x>file works. It also handles 'single' and "double" quotes. Execution is the classic trio:

fork()  ->  child: execvp()     parent: waitpid()

If execvp returns, it failed, so the child prints the error and calls _exit(127). It uses _exit, not exit, so the child doesn't flush stdio buffers it inherited from the parent.
