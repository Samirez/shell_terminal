# shell_terminal

A lightweight shell command-line interface written in C.

This project is focused on learning and evolving core shell behavior, including builtins, external command execution, tab completion, and background job tracking.

## Features

- Built-in commands:
  - `echo`
  - `type`
  - `pwd`
  - `cd`
  - `jobs`
  - `exit`
- Executes external commands available in `PATH`
- Basic quoting and escaping support for command parsing
- Tab-completion support for builtins and executable names
- Background job execution with `&` and job listing via `jobs`

## Requirements

- CMake 3.13+
- A C11-compatible compiler (for example GCC or Clang)
- A Unix-like environment (Linux/macOS) for terminal behavior and process APIs

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Run

```sh
./build/shell
```

## Quick usage

Examples after starting the shell:

- `echo hello`
- `type cd`
- `pwd`
- `cd ~/some-directory`
- `sleep 5 &`
- `jobs`
- `exit`

## Project structure

- `src/main.c` — shell implementation
- `CMakeLists.txt` — build definition
- `CONTRIBUTING.md` — contribution workflow
- `SECURITY.md` — vulnerability reporting process

## Contributing

See [CONTRIBUTING.md](./CONTRIBUTING.md) for development setup, workflow, and standards.

## Security

See [SECURITY.md](./SECURITY.md) to report vulnerabilities responsibly.
