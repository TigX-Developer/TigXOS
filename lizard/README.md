# Green Lizard

**TigxOS’s native userland language** — sources use `.gl`, compiler is `glc`.

A small, easy language that mixes **C-style** structure (braces, functions) with
**Python-ish** ergonomics (`print`, `let`, no required semicolons).  
(`.tig` is still accepted with a deprecation warning.)

```text
examples/hello.gl  →  glc  →  hello.gl.c  →  gcc  →  bin/hello.elf  →  spawn
```

## Quick syntax

```gl
// comment
fn main() {
    print("hello")
    let x = 1 + 2
    if x > 2 {
        print("yes")
    }
    exit(0)
}
```

## Command-line args (NEW in 1.0.62 glc)

Inside **`fn main()`** only:

| Name | Type | Meaning |
|------|------|---------|
| `argc` | int | Number of arguments (including the program name) |
| `argv[i]` | str | Argument string at index `i` (`0` .. `argc-1`) |

- **`argv` alone** is not allowed — always use **`argv[i]`**.
- Index must be an integer expression.
- Out-of-range access is **not** checked yet (don’t run past `argc`).

Example:

```gl
fn main() {
    print("argc =", argc)
    let i = 0
    while i < argc {
        print(i, argv[i])
        i = i + 1
    }
    exit(0)
}
```

Typical **echo** pattern (skip `argv[0]`, print the rest):

```gl
fn main() {
    let i = 1
    while i < argc {
        if i > 1 {
            write(1, " ")
        }
        write(1, argv[i])
        i = i + 1
    }
    write(1, "\n")
    exit(0)
}
```

## Builtins (map to syscalls / libc)

| green lizard        | Meaning                              |
|---------------------|--------------------------------------|
| `print(...)`        | print args with spaces + newline     |
| `write(fd, s)`      | write string to fd (no extra newline)|
| `exit(code)`        | SYS_EXIT                             |
| `getpid()`          | SYS_GETPID                           |
| `yield()`           | SYS_YIELD                            |
| `sleep(ms)`         | SYS_SLEEP_MS                         |
| `open(path, flags)` | SYS_OPEN                             |
| `close(fd)`         | SYS_CLOSE                            |
| `spawn(path)`       | SYS_SPAWN (argv not exposed in gl yet)|
| `wait(pid)`         | SYS_WAIT                             |
| `len(s)`            | string length                        |
| `readch(fd)`        | read one byte (-1 = EOF)             |
| `writech(fd, c)`    | write one byte                       |
| `listdir()`         | print root listing (name + size)     |
| `unlink(path)`      | delete file                          |
| `stat(path)`        | file size, or -1                     |
| `O_RDONLY` etc.     | open flags (also O_CREAT, O_TRUNC)   |
| `break` / `continue`| loop control                         |

## Types (Green Lizard 2)

| Type | C mapping | Notes |
|------|-----------|--------|
| `int` | `long` | Default for unannotated values |
| `str` | `const char *` | Literals, `argv[i]`, concat |
| `bool` | `long` (0/1) | `true` / `false`; comparisons yield `bool` |
| `void` | `void` | Return type only |

```gl
let n: int = 10
let ok: bool = true
let name: str = "Tig"

fn add(a: int, b: int) -> int {
    return a + b
}

let s: str = add(1, 2) as str   // cast int → str
```

Annotations are optional for compatibility; untyped `let x = 1` defaults to `int`.
Mismatches are compile errors (`let s: str = 5`).

## Control flow

- `if cond { ... } else { ... }`
- `while cond { ... }`
- `for init; cond; step { ... }`  (C-style; any part optional; `let` allowed in init)
- `break` / `continue`
- `return` / `return expr`

## Strings & chars (NEW in 1.0.65)

| Feature | Example | Notes |
|---------|---------|-------|
| Concat  | `"a" + "b"` or `s + t` | `str_concat` (4×256 rotating). One-shot OK; **do not** loop `s = s + x` |
| Index   | `s[i]` | Returns char code as `int` |
| Compare | `s == "hello"`, `s != t` | Via `strcmp` |
| Char lit| `'h'`, `'\n'` | Integer character codes |

## Compile

```bash
python3 lizard/glc.py examples/echo.gl -o user/echo.gl.c
make ARCH=x86_64    # rebuilds seeded ELFs
```

## What is not supported yet

- General arrays (only `argv[i]` and string index)
- Structs / floats
- Bare `argv` without an index
- Nested string concat beyond buffer depth (use carefully)
