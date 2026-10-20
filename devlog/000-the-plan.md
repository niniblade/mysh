# Devlog 000: The plan

> mysh devlog series: **000 The plan** · [001 Planning the shell](001-planning-the-shell.md) · [002 Building a shell](002-building-a-shell.md) · [003 Variables, lists and a bug hunt](003-variables-lists-and-a-bug-hunt.md)

I'm a software engineering student, and I want projects that teach me how
computers really work, are fun to build, and make a strong portfolio. The
idea that grabbed me most: **build my own operating system from scratch.**
This post is the plan: what that actually means, what I decided, and the
order I'll build things in.

## What "my own Linux OS" can mean

"From scratch" covers very different amounts of work, so I split it into
three levels:

| Level | What it is | Effort |
|---|---|---|
| Custom distro | Take the Linux kernel and assemble my own userland (init, packages, shell, installer) with Buildroot, Yocto or Linux From Scratch (LFS) | a weekend to a few weeks |
| Remix | Customize Arch, Debian or Ubuntu with my own packages and install scripts | days |
| Hobby kernel | Write my own kernel: bootloader, memory management, interrupts, scheduler, syscalls, filesystem | months |

Writing Linux itself from scratch isn't realistic (it is millions of lines),
so "from scratch" in practice means LFS or a hobby kernel.

## The decision

I'm going for the **hobby kernel**, because it teaches the most. That means
it won't be Linux anymore. It will be my own OS, which I think is cooler.

One thing I had to accept: "make the kernel, then make the OS" hides a lot
of work. A kernel alone gives booting, memory, interrupts and scheduling. A
usable OS also needs a filesystem, userspace, a libc, a shell and programs.
So I'll build it in layers, not as one giant goal.

## Warm-ups first

Before the kernel, I'll build a few smaller projects that teach pieces I'll
need there. The most important one is a **shell**: it forces me to
understand processes, file descriptors, pipes and signals from the user side.
That makes the kernel's process model much easier to design later. Optionally
I'll also follow Linux From Scratch to see how a whole system fits together.

## The roadmap

**Phase 0: Setup and warm-ups**
- [x] Create a Git repo with a README and a `devlog/` folder
- [x] Build a **shell from scratch** (parsing, pipes, redirection, job control)
- [ ] Choose a language (C or Rust) and target (x86_64 or RISC-V) for the kernel
- [ ] Install QEMU, gdb, a cross-compiler toolchain and Make
- [ ] Optional: Linux From Scratch or Buildroot

**Phase 1: Kernel**
- [ ] Boot with a ready bootloader (e.g. Limine) and print "Hello"
- [ ] GDT/IDT (x86) and exception handling
- [ ] Timer and keyboard interrupts
- [ ] Physical memory allocator
- [ ] Paging and virtual memory
- [ ] Kernel heap (`kmalloc` / `kfree`)
- [ ] Multitasking: context switching and a scheduler
- [ ] User mode and system calls
- [ ] RAM disk filesystem, then a disk driver and FAT or ext2

**Phase 2: Userspace**
- [ ] A tiny libc
- [ ] ELF loader to run programs
- [ ] Port my shell to my OS
- [ ] Basic utilities (`ls`, `cat`, `echo`)

**Phase 3: A full OS (stretch goals)**
- [ ] Networking (drivers plus a basic TCP/IP stack)
- [ ] A simple GUI
- [ ] Port existing software
- [ ] Installer or bootable ISO

**Phase 4: Other projects**
- [ ] Text editor (kilo-style) in C
- [ ] HTTP server on raw sockets
- [ ] Key-value store or small database
- [ ] Compiler or interpreter for a small language
- [ ] Mini container runtime (namespaces and cgroups)
- [ ] CHIP-8 or Game Boy emulator
- [ ] Git clone
- [ ] Ray tracer
- [ ] Multiplayer game server

## Habits to keep it a portfolio, not a pile of code

- Commit often with clear messages
- Add screenshots or demo GIFs to each README
- Write a devlog post per milestone (this series)
- Pin the best 3 to 4 projects on GitHub
- Prefer a few well-documented, finished projects over many half-done ones

## Expectations

- Use **QEMU + gdb** from day one. Debugging a kernel without them is miserable.
- Expect triple faults and mysterious reboots. That's normal.
- Honest caveat: portfolio value depends on presentation. A documented repo
  with a write-up and a demo beats a giant unfinished project.

## Resources I plan to use

The OSDev wiki, *Operating Systems: Three Easy Pieces* (free online), Philipp
Oppermann's "Writing an OS in Rust", and MIT's xv6 for reading a small, clean
Unix-like kernel. I haven't verified these links are current, so I'll check
them before relying on them.

## Next

[Devlog 001](001-planning-the-shell.md): planning the first warm-up, the shell.
