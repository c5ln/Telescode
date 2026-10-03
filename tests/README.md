# Parser Tests

## Fixtures

| File | What it tests |
|---|---|
| `fixtures/simple_class.py` | Classes with base classes, regular/async methods, decorators |
| `fixtures/module_functions.py` | Module-level functions, imports, function calls, nested functions |
| `fixtures/empty_file.py` | Graceful handling of empty files |
| `fixtures/imports_only.py` | Files with only import statements and no classes/functions |

## Running Tests

```bash
bash tests/run_parser_test.sh
```

The script builds `TelescodeParser`, runs it against `tests/fixtures/`, writes CSVs to
`tests/output/`, and validates that expected entities (e.g. `Dog` class, `Animal` base
class, `greet` function, `IMPORTS` links) appear in the output.

Exit code 0 means all checks passed.
