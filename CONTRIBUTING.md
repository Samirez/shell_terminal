# Contributing to shell_terminal

Thanks for your interest in improving this project.

## Ways to contribute

- Fix bugs and edge cases
- Improve shell behavior and builtins
- Improve documentation
- Add tests when test infrastructure is introduced
- Propose focused feature enhancements

## Development setup

1. Fork the repository and create a branch from `main`.
2. Build locally:

   ```sh
   cmake -S . -B build
   cmake --build build
   ```

3. Run the shell:

   ```sh
   ./build/shell
   ```

## Coding guidelines

- Keep changes focused and minimal.
- Follow the existing C style in `src/main.c`.
- Preserve current behavior unless your change intentionally updates it.
- Avoid introducing new dependencies unless necessary.

## Pull request guidelines

- Use clear commit messages.
- Explain what changed and why.
- Include manual validation steps (commands run and observed behavior).
- Keep pull requests small and easy to review.

## Suggested validation before opening a PR

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

> Note: If no tests are defined yet, `ctest` may report “No tests were found”.

## Reporting issues and ideas

- For bugs: include reproduction steps, expected behavior, and actual behavior.
- For feature requests: describe the problem first, then the proposed solution.
