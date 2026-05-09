# Build Your Own Shell — Enhanced C Implementation

[![progress-banner](https://backend.codecrafters.io/progress/shell/82a54cee-ca3d-400c-b4e1-0abff979727d)](https://app.codecrafters.io/users/codecrafters-bot?r=2qF)

This repository contains a C implementation for the **Build Your Own Shell** challenge on CodeCrafters.  
The implementation provides an interactive POSIX‑like shell with a line editor, history, programmable tab completion, variable expansion, pipelines, redirections, job control, and a set of useful built‑ins.

---

## Overview

This shell is intended as a practical, extendable starting point for the CodeCrafters challenge. It demonstrates how to:

- Build a simple raw‑mode line editor using `termios`.
- Parse shell input with support for quotes, escapes, and pipelines.
- Expand shell and environment variables.
- Run external programs (searching `$PATH`) and built‑ins.
- Support redirections and background jobs.

---

## Features

- **Interactive REPL**
  - Raw terminal mode using `termios`.
  - Arrow‑key history navigation (↑ / ↓).
  - Tab key triggers programmable completion.

- **Parsing**
  - Quote‑aware argument parsing (`'...'`, `"..."`) and backslash escaping.
  - Pipeline splitting (`cmd1 | cmd2 | ...`).
  - Redirection parsing: `>`, `>>`, `1>`, `1>>`, `2>`, `2>>`.
  - Variable expansion: `$VAR` and `${VAR}` (uses shell local variables and environment).

- **Built‑ins**
  - `echo`, `cd`, `pwd`, `type`, `history`, `jobs`, `complete`, `declare`, `exit`.

- **History**
  - In‑memory history with optional persistence via file operations:
    - `history`
    - `history -r <file>` (read)
    - `history -w <file>` (write)
    - `history -a <file>` (append)
  - Arrow keys navigate history interactively.

- **Job Control**
  - Background execution with `&`.
  - Jobs are tracked with job id, PID, and running/done state.
  - Automatic reaping of finished jobs.

- **Pipelines and Redirections**
  - Full support for pipelines where built‑ins run in child processes when used inside a pipeline.
  - External commands are located via `$PATH`, forked, and `execv`‑ed.

---

## Built‑in Commands

| **Command** | **Behavior** |
|---|---|
| **echo** | Prints arguments; supports redirection. |
| **cd** | Change current working directory; `cd` with no args goes to `$HOME`. |
| **pwd** | Print current working directory. |
| **type** | Report whether a name is a shell builtin or an external command. |
| **history** | Show history or read/write/append history files. |
| **jobs** | List background jobs and their status. |
| **complete** | Register or print programmable completion scripts. |
| **declare** | Set shell variables (and `declare -p VAR` to print). |
| **exit** | Exit the shell. |

---

## Variable Expansion

- Supports `$VAR` and `${VAR}`.
- Shell‑local variables (set via `declare`) are consulted first, then environment variables.
- Example:
  ```sh
  declare NAME=world
  echo "Hello $NAME"

# Command History
- Stored in memory.
- Optional persistence via file operations (history -w, history -r, history -a).
- Interactive navigation with the up/down arrow keys.

# Job Control
- Background execution using &:
    sh
    sleep 5 &
    jobs
- Jobs are printed with job id, PID, and status (Running / Done).
- Done jobs are removed from the job list automatically.

# External Commands
- The shell searches $PATH for executables.
- External commands are executed via fork() + execv().
- Redirections and pipelines work with external commands.

# Programmable Completion
- Press Tab to trigger completion.
- Completion scripts can be registered with:
  sh
  complete -C /path/to/script commandname
- The shell sets COMP_LINE and COMP_POINT before running a completion script so scripts can inspect the current line and cursor position.

# Usage
Build and run (development)
- Ensure you have a C toolchain installed (e.g., gcc, make, cmake if used).
- Build the program implemented in src/main.c according to your project layout.
- Run the compiled binary to start the interactive shell.
- Use arrow keys for history, Tab for completion, and standard shell syntax for pipelines and redirections.

# Example commands
- sh
- Run an external command and redirect output
ls -la | grep src > out.txt
- Background job
sleep 10 & jobs
- Save history to a file
history -w my_history.txt
- Register a completion script
complete -C /usr/local/bin/my-completer mycmd

# Implementation notes and excerpts

- The implementation includes a number of focused helpers and behaviors. Two short excerpts from the source illustrate key behaviors:
c

/* Expand $VAR and ${VAR} using shell_vars and environment */

c

/* Built‑ins inside pipelines must run in the child */

- Source: uploaded file #include _stdio.h_.txt.
- Extending the Shell

# Ideas for improvements and extensions:
- Add export, unset, fg, and bg built‑ins for richer job and environment control.
- Improve the line editor with left/right movement, in‑line editing, and multi‑line support.
- Enhance completion to support context‑sensitive completions and richer file path handling.
- Harden parsing for edge cases and add a test suite for parsing, completion, and job control.

# Contributing
Contributions are welcome. Suggested workflow:
- Fork the repository.
- Create a feature branch.
- Implement and test your changes.
- Open a pull request with a clear description of the change.
