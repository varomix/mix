# MIX Language Documentation

> *"Fast to run, fast to write, fast to read."*

MIX is a systems programming language that compiles to native code via QBE. It combines the performance of C with a clean, Python-inspired syntax — targeting graphics applications, games, and systems programming.

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [Language Reference](#language-reference)
3. [Variables](#variables)
4. [Functions](#functions)
5. [Types](#types)
6. [Control Flow](#control-flow)
7. [Shapes (Structs)](#shapes)
8. [Methods](#methods)
9. [Operator Overloading](#operator-overloading)
10. [Generics](#generics)
11. [Lists](#lists)
12. [Maps](#maps)
13. [Strings](#strings)
14. [Optionals](#optionals)
15. [Tagged Unions](#tagged-unions)
16. [Error Handling](#error-handling)
17. [Lambdas](#lambdas)
18. [String Interpolation](#string-interpolation)
19. [Modules](#modules)
20. [C Interop](#c-interop)
21. [Unsafe Blocks](#unsafe-blocks)
22. [Zones](#zones)
23. [Concurrency](#concurrency)
24. [File I/O](#file-io)
25. [Math Functions](#math-functions)
26. [OS Builtins](#os-builtins)
27. [Build System](#build-system)
28. [Compile-time Constants](#compile-time-constants)
29. [Conditional Compilation](#conditional-compilation)
30. [Defer](#defer)
31. [Type Aliases](#type-aliases)
32. [C Header Bindings](#c-header-bindings)
33. [Debug Info](#debug-info)
34. [Compiler Reference](#compiler-reference)
35. [SDL3 Example](#sdl3-example)
36. [Implementation Report](#implementation-report)

---

## Getting Started

### Prerequisites

**macOS (Apple Silicon)**:

```bash
# Install QBE compiler backend (build from source)
git clone git://c9x.me/qbe.git
cd qbe
make
sudo cp qbe /usr/local/bin/

# Verify QBE is installed
qbe -h

# Xcode command line tools (for cc/clang linker)
xcode-select --install
```

### Building the Compiler

```bash
git clone <your-repo-url> mix_lang
cd mix_lang
make
```

This produces `build/mix` — the MIX compiler.

### Hello World

Create `hello.mix`:

```mix
main()
    print("Hello, MIX!")
```

Compile and run:

```bash
./build/mix hello.mix -o hello
./hello
```

Output:

```
Hello, MIX!
```

### How It Works

The compiler pipeline:

```
hello.mix → [mix] → hello.ssa → [qbe] → hello.s → [cc] → hello (native binary)
```

1. **mix** reads `.mix` source, lexes, parses, type-checks, and emits QBE IR (`.ssa`)
2. **QBE** compiles the IR to native assembly (`.s`) for arm64
3. **cc** (clang) assembles and links into a native Mach-O binary

### Running Tests

```bash
make test    # 45/45 tests pass
```

---

## Language Reference

### Quick Syntax Overview

```mix
// Variables — no let/var keyword
x = 42              // immutable
x! = 42             // mutable
x: float = 3.14     // explicit type

// Functions — no fn keyword, last expression is return value
add(a: int, b: int) -> int
    a + b

// Shapes (structs)
shape Vec2
    x, y: float

// Control flow — indentation based, no braces
if x > 0
    print("positive")
else
    print("negative")

// Comments
// this is a comment
```

**Key principles:**
- No semicolons — newlines are statement terminators
- No braces — 4-space indentation defines blocks
- No `fn`/`let`/`var`/`return` keywords
- Last expression in a function is the return value
- `done` for early exit (replaces `return`)
- `!` marks mutability: `x! = 0` and `x! += 1`

---

## Variables

### Immutable (default)

```mix
x = 42
name = "Alice"
pi = 3.14159
```

Immutable variables cannot be reassigned. The type is inferred from the value.

### Mutable

```mix
counter! = 0
counter! += 1
counter! = counter + 10
```

The `!` suffix marks a variable as mutable. Only mutable variables can be reassigned.

### Type Annotations

```mix
x = 42              // inferred: int
x: float = 42       // explicit: float
p: *byte = 0        // pointer type
```

Type annotations are optional — use them when the compiler needs help or for documentation.

---

## Functions

Functions are defined by name followed by parameters in parentheses, with the body indented below. No `fn` keyword is needed.

### Basic Function

```mix
add(a: int, b: int) -> int
    a + b
```

The last expression is the return value — no `return` keyword needed.

### Calling Functions

```mix
result = add(10, 20)    // 30
```

### Early Exit with `done`

```mix
fibonacci(n: int) -> int
    if n <= 1
        done n
    fibonacci(n - 1) + fibonacci(n - 2)
```

`done` exits the function immediately, optionally with a value.

### Effect Markers

Functions can declare their side effects:

| Marker | Meaning |
|--------|---------|
| *(none)* | Pure — no side effects |
| `~` | Has side effects (I/O, network, etc.) |
| `!` | Mutates its arguments |
| `!~` | Both mutation and side effects |

```mix
// Pure function
add(a: int, b: int) -> int
    a + b

// Side effects
greet(name: str) ~
    print("Hello {name}!")
```

### Main Function

`main()` is the entry point. It implicitly returns 0 and has implicit `~` (side effects).

```mix
main()
    print("Hello!")
    // implicitly returns 0
```

---

## Types

### Primitive Types

| Type | Description | QBE | Size |
|------|-------------|-----|------|
| `int` | Native word integer (64-bit on arm64) | `l` | 8 bytes |
| `float` | 64-bit floating point | `d` | 8 bytes |
| `bool` | `true` or `false` | `w` | 4 bytes |
| `byte` | Single byte | `w` | 1 byte |
| `str` | UTF-8 string (pointer) | `l` | 8 bytes |

### Sized Variants

For hardware, protocols, or binary formats:

```
int8   int16   int32   int64
uint8  uint16  uint32  uint64
float32  float64
```

### Pointer Types

```mix
p: *int = &x        // pointer to int
p: *byte = 0        // null pointer
```

### Type Inference

The compiler infers types from values:

```mix
x = 42          // int
y = 3.14        // float
s = "hello"     // str
b = true        // bool
```

---

## Control Flow

### If / Else

```mix
if x > 0
    print("positive")
else if x == 0
    print("zero")
else
    print("negative")
```

### While

```mix
i! = 0
while i < 10
    print(i)
    i! += 1
```

### For Range

```mix
// exclusive end (0, 1, 2, 3, 4)
for i in 0..5
    print(i)

// inclusive end (0, 1, 2, 3, 4, 5)
for i in 0..=5
    print(i)
```

### For-Each (Lists)

```mix
items = [10, 20, 30]
for item in items
    print(item)
```

### For-Each (Maps)

```mix
ages = {"alice": 30, "bob": 25}
for key, value in ages
    print("{key}: {value}")
```

### Match

```mix
match status
    200 => print("ok")
    404 => print("not found")
    500 => print("server error")
    _   => print("unknown")
```

`_` is the wildcard catch-all. Each arm uses `=>` to separate the pattern from the action.

### Break / Continue

```mix
while true
    x = next()
    if x == 0
        break
    if x < 0
        continue
    process(x)
```

---

## Shapes

Shapes are MIX's structs — named data structures with typed fields.

### Definition

```mix
shape Vec2
    x, y: float

shape Color
    r, g, b, a: uint8
```

Multiple fields of the same type can share an annotation with commas.

### Construction

Named fields:

```mix
v = Vec2(x: 1.0, y: 2.0)
c = Color(r: 255, g: 128, b: 0, a: 255)
```

Positional (no field labels):

```mix
v = Vec2(1.0, 2.0)
c = Color(255, 128, 0, 255)
```

### Field Access

```mix
print(v.x)      // 1.0
print(v.y)      // 2.0
```

### Passing to Functions

```mix
vec2_add(a: Vec2, b: Vec2) -> Vec2
    Vec2(x: a.x + b.x, y: a.y + b.y)

v3 = vec2_add(v1, v2)
```

Shapes are passed by reference using QBE's aggregate type system.

---

## Methods

Methods are functions defined inside a shape. They can access fields directly by name (no `self.` prefix needed).

```mix
shape Circle
    radius: float

    area() -> float
        3.14159 * radius * radius

    circumference() -> float
        2.0 * 3.14159 * radius
```

### Calling Methods

```mix
c = Circle(radius: 5.0)
print(c.area())              // 78.5397
print(c.circumference())    // 31.4159
```

### Methods with Parameters

```mix
shape Vec2
    x, y: float

    dot(other: Vec2) -> float
        x * other.x + y * other.y

    length_sq() -> float
        x * x + y * y
```

```mix
v1 = Vec2(x: 3.0, y: 4.0)
v2 = Vec2(x: 1.0, y: 0.0)
print(v1.dot(v2))       // 3.0
print(v1.length_sq())   // 25.0
```

### Computed Fields

Zero-parameter methods can be called without `()`:

```mix
shape Rect
    w, h: float

    area() -> float
        w * h

r = Rect(w: 3.0, h: 4.0)
print(r.area)     // 12.0 — no parens needed
print(r.area())   // 12.0 — parens also work
```

### How Methods Work Internally

Methods are compiled as regular functions with a hidden `self` parameter:

```
shape Circle { area() }  →  function Circle_area(:Circle %self)
c.area()                  →  call $Circle_area(:Circle %v.c)
```

---

## Operator Overloading

Shapes can define custom operators:

```mix
shape Vec2
    x, y: float

    +(other: Vec2) -> Vec2
        Vec2(x: x + other.x, y: y + other.y)

    -(other: Vec2) -> Vec2
        Vec2(x: x - other.x, y: y - other.y)

    *(scalar: float) -> Vec2
        Vec2(x: x * scalar, y: y * scalar)

    ==(other: Vec2) -> bool
        x == other.x and y == other.y
```

```mix
a = Vec2(x: 1.0, y: 2.0)
b = Vec2(x: 3.0, y: 4.0)
c = a + b           // Vec2(4.0, 6.0)
d = a * 2.0         // Vec2(2.0, 4.0)
print(a == b)       // false
```

Supported operators: `+ - * / % == != < > <= >=`

---

## Generics

Generic functions use `@T` type parameters:

```mix
@T
identity(x: T) -> T
    x

@T
max_of(a: T, b: T) -> T
    if a > b
        done a
    b
```

```mix
print(identity(42))        // 42
print(identity("hello"))   // hello
print(max_of(3, 7))        // 7
```

### Multiple Type Parameters

```mix
@T, K
make_pair(a: T, b: K) -> int
    print(a)
    print(b)
    0
```

### Constraints

Use `has` to declare required capabilities:

```mix
@T has ==
contains(items: [T], target: T) -> bool
    for item in items
        if item == target
            done true
    false
```

---

## Lists

### Literals

```mix
nums = [1, 2, 3, 4, 5]
empty = []
```

### Operations

```mix
nums = [10, 20, 30]
print(nums.len)          // 3
print(nums[0])           // 10
nums.push!(40)           // append
print(nums.len)          // 4
```

### Mutation Methods

```mix
nums = [5, 3, 1, 4, 2]
nums.sort!()             // [1, 2, 3, 4, 5]
nums.reverse!()          // [5, 4, 3, 2, 1]
last = nums.pop!()       // removes and returns last element
nums.insert!(0, 99)      // insert 99 at index 0
nums.remove!(0)          // remove element at index 0
```

### Query Methods

```mix
nums = [1, 2, 3, 4, 5]
print(nums.contains(3))     // true
print(nums.contains(9))     // false
print(nums.index_of(4))     // 3
print(nums.index_of(9))     // -1
```

### Join

```mix
words = ["hello", "world"]
print(words.join(", "))  // hello, world
```

### Iteration

```mix
for item in nums
    print(item)
```

### Slicing

```mix
items = [10, 20, 30, 40, 50]
a = items[1..3]      // [20, 30]
b = items[..2]       // [10, 20]
c = items[3..]       // [40, 50]
d = items[0..=2]     // [10, 20, 30] (inclusive)
```

### List Comprehensions

```mix
squares = [x * x for x in items]
evens = [x for x in items if x % 2 == 0]
```

### List Type in Parameters

```mix
sum_list(items: [int]) -> int
    total! = 0
    for item in items
        total! += item
    total
```

---

## Maps

### Literals

```mix
ages = {"alice": 30, "bob": 25, "charlie": 35}
empty = {}
```

### Operations

```mix
ages = {"alice": 30, "bob": 25}
print(ages["alice"])         // 30
print(ages.len)              // 2
print(ages.has("bob"))       // true

ages!["charlie"] = 35        // add entry
ages.remove!("bob")          // remove entry

keys = ages.keys             // list of keys
vals = ages.values           // list of values
```

### Iteration

```mix
for key, value in ages
    print("{key} is {value}")
```

---

## Strings

### Built-in Properties

```mix
s = "Hello, World!"
print(s.len)                 // 13
```

### Methods

```mix
s = "  Hello, World!  "
print(s.upper())             // "  HELLO, WORLD!  "
print(s.lower())             // "  hello, world!  "
print(s.trim())              // "Hello, World!"
print(s.contains("World"))   // true
print(s.starts_with("  H")) // true
print(s.ends_with("!  "))   // true
print(s.replace("World", "MIX"))  // "  Hello, MIX!  "
print(s.char_at(2))          // "H"

parts = "a,b,c".split(",")  // ["a", "b", "c"]
```

### String Concatenation

```mix
greeting = "Hello" + " " + "World"
print(greeting)              // Hello World
```

### Conversion to String

```mix
s = to_string(42)            // "42"
sf = to_string(3.14)         // "3.14"
msg = to_string(100) + " points"
```

---

## Optionals

### Optional Types

Functions can return optional values using `T?`:

```mix
find(items: [int], target: int) -> int?
    for item in items
        if item == target
            done item
    none
```

### Unwrapping with `else`

```mix
result = find(items, 42) else -1
print(result)    // 42 if found, -1 if not
```

---

## Tagged Unions

### Definition

```mix
shape Shape
    Circle(radius: float)
    Rect(w: float, h: float)
    Point()
```

### Construction

```mix
s1 = Circle(radius: 5.0)
s2 = Rect(w: 3.0, h: 4.0)
s3 = Point()
```

### Destructuring Match

```mix
match s
    Circle(r) => print(r)
    Rect(w, h) => print(w * h)
    Point() => print("point")
    _ => print("unknown")
```

---

## Error Handling

MIX uses a Result-based error system. Functions that can fail return a `Result` type automatically — no manual wrapping needed.

### Fail (Simple Panic)

In functions **without** a return value (void) or **without** the `~` marker, `fail` panics and exits:

```mix
validate(x: int) ~
    if x < 0
        fail "value must be non-negative"
    print("valid")
```

### Result-Based Errors

When a function has `~` (side effects), a **non-void return type**, and contains `fail` in its body, the compiler automatically wraps the return type in a `Result`. The caller can handle errors with `else`:

```mix
divide(a: float, b: float) -> float ~
    if b == 0.0
        fail "division by zero"
    done a / b

main()
    // Successful — unwraps the ok value
    result = divide(10.0, 3.0) else 0.0
    print(result)        // 3.33333

    // Failed — uses the fallback
    safe = divide(10.0, 0.0) else -1.0
    print(safe)          // -1
```

### Error Propagation with `?`

The `?` operator unwraps a result if ok, or propagates the error to the caller:

```mix
parse_positive(x: int) -> int ~
    if x < 0
        fail "negative"
    done x * 10

double_positive(x: int) -> int ~
    val = parse_positive(x)?    // propagates error if parse_positive fails
    done val * 2

main()
    r1 = double_positive(5) else -1
    print(r1)       // 100

    r2 = double_positive(-3) else -1
    print(r2)       // -1 (error propagated from parse_positive)
```

### How It Works

- `fail "msg"` in a result-returning function creates an error result (not a panic)
- `done value` in a result-returning function wraps the value in an ok result
- `expr else default` checks if result is ok, unwraps if yes, uses fallback if error
- `expr?` checks if result is ok, unwraps if yes, returns the error to the caller if not
- Functions without `~` or with void return keep the old behavior (`fail` = panic)

---

## Lambdas

Anonymous functions using `=>` syntax.

### Single Parameter

```mix
double = x => x * 2
print(double(21))    // 42
```

### Multiple Parameters

```mix
combine = (a, b) => a * 10 + b
print(combine(3, 7))    // 37
```

### Higher-Order Functions

Pass lambdas to other functions:

```mix
apply(f: int, x: int) -> int
    f(x)

main()
    print(apply(x => x * 2, 21))    // 42
    print(apply(x => x + 10, 5))    // 15
```

Lambdas compile to standalone functions; the variable holds a function pointer.

---

## String Interpolation

Embed expressions inside strings with `{expr}`:

```mix
name = "MIX"
x = 42
print("Hello {name}!")         // Hello MIX!
print("x = {x}")              // x = 42
print("{x} + {x} = {x + x}")  // 42 + 42 = 84
```

Expressions inside `{}` are evaluated and formatted based on their type (int, float, str, bool).

---

## Modules

File = module. Folder = namespace.

### Creating a Module

`math/vec.mix`:

```mix
pub shape Vec2
    x, y: float

    length_sq() -> float
        x * x + y * y

pub vec2_add(a: Vec2, b: Vec2) -> Vec2
    Vec2(x: a.x + b.x, y: a.y + b.y)
```

`pub` marks functions, shapes, and constants as public (visible to importers).

### Importing

`main.mix`:

```mix
use math.vec

main()
    a = Vec2(x: 1.0, y: 2.0)
    b = Vec2(x: 3.0, y: 4.0)
    c = vec2_add(a, b)
    print(c.x)    // 4.0
```

### Module Resolution

`use math.vec` resolves to `math/vec.mix` relative to the importing file's directory.

---

## C Interop

MIX can call any C library function through `extern` blocks.

### Declaring C Functions

```mix
extern "libc"
    puts(s: *byte) -> int32 ~
    printf(fmt: *byte) -> int32 ~

extern "SDL3"
    SDL_Init(flags: uint32) -> int32 ~
    SDL_Quit() ~
    SDL_CreateWindow(title: *byte, w: int32, h: int32, flags: uint64) -> *byte ~
```

### Type Mapping

| MIX Type | C Type |
|----------|--------|
| `int` | `int64_t` / `long` |
| `int32` | `int32_t` / `int` |
| `uint32` | `uint32_t` |
| `uint8` | `uint8_t` |
| `float` | `double` |
| `float32` | `float` |
| `*byte` | `char*` / `void*` |
| `bool` | `bool` |

### Linking

```bash
./build/mix game.mix -o game -L/opt/homebrew/lib -lSDL3
```

Pass `-l` and `-L` flags to link with C libraries.

---

## Unsafe Blocks

Pointer arithmetic and dereferencing require `unsafe`:

```mix
main()
    x! = 42
    unsafe
        p = &x
        *p = 100
    print(x)    // 100
```

---

## Zones

Zones provide scoped memory management — all allocations within a zone are freed when the zone exits:

```mix
main()
    zone
        data = alloc(1024)
        process(data)
        // data freed automatically here

    zone:named
        // nested zones supported
        zone
            // inner zone
```

---

## Concurrency

MIX provides minimal concurrency primitives built on pthreads.

### shared — Mutex-Wrapped Values

`shared` creates a thread-safe value protected by a mutex:

```mix
main()
    counter! = shared int(0)
    x = counter.read()
    print(x)                        // 0

    counter.update!(v => v + 1)
    counter.update!(v => v + 1)
    counter.update!(v => v + 1)

    y = counter.read()
    print(y)                        // 3
```

- `shared int(initial_value)` — creates a mutex-wrapped integer
- `.read()` — locks mutex, reads value, unlocks
- `.update!(fn)` — locks mutex, applies function to current value, stores result, unlocks

### go / wait — Task Spawning

`go` spawns a function call in a new thread. `wait` joins the thread and returns the result:

```mix
compute(x: int) -> int
    x * x + 1

main()
    t = go compute(5)
    result = wait t
    print(result)           // 26

    // Multiple concurrent tasks
    t2 = go compute(10)
    t3 = go compute(3)
    r2 = wait t2
    r3 = wait t3
    print(r2)               // 101
    print(r3)               // 10
```

- `go fn(args)` — spawns function in a new pthread, returns a task handle
- `wait handle` — joins the thread, returns the function's result
- Functions can take 0-8 arguments
- On macOS, pthreads are part of libSystem (no `-lpthread` needed)

---

## File I/O

### Convenience Functions

Read or write entire files in one call:

```mix
// Write a file
ok = file_write_all("/tmp/data.txt", "hello from MIX")
print(ok)        // true

// Read a file
content = file_read_all("/tmp/data.txt")
print(content)   // hello from MIX
```

### Manual File Operations

For more control, open/write/close explicitly:

```mix
f = file_open("/tmp/output.txt", "w")
file_write(f, "line one\n")
file_write(f, "line two\n")
file_close(f)

data = file_read_all("/tmp/output.txt")
print(data)
```

### Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `file_open` | `(str, str) -> int` | Open file with mode ("r", "w", "a"), returns handle (0 on failure) |
| `file_read` | `(int) -> str` | Read chunk from handle |
| `file_write` | `(int, str) -> void` | Write string to handle |
| `file_close` | `(int) -> void` | Close file handle |
| `file_read_all` | `(str) -> str` | Read entire file contents |
| `file_write_all` | `(str, str) -> bool` | Write string to file (true on success) |

---

## Math Functions

All math functions operate on `float` (64-bit double). Integer arguments are auto-converted.

### Single Argument

```mix
print(sqrt(4.0))     // 2
print(abs(-5.0))     // 5
print(floor(3.7))    // 3
print(ceil(3.2))     // 4
print(round(3.5))    // 4
print(sin(0.0))      // 0
print(cos(0.0))      // 1
print(log(1.0))      // 0
print(tan(0.0))      // 0
```

### Two Arguments

```mix
print(pow(2.0, 10.0))  // 1024
print(min(3.0, 7.0))   // 3
print(max(3.0, 7.0))   // 7
```

### Available Functions

| Function | Description |
|----------|-------------|
| `sqrt(x)` | Square root |
| `abs(x)` | Absolute value |
| `sin(x)`, `cos(x)`, `tan(x)` | Trigonometric functions |
| `log(x)` | Natural logarithm |
| `floor(x)` | Round down |
| `ceil(x)` | Round up |
| `round(x)` | Round to nearest |
| `pow(x, y)` | x raised to the power y |
| `min(x, y)` | Minimum of two values |
| `max(x, y)` | Maximum of two values |

---

## OS Builtins

MIX provides built-in functions for interacting with the operating system.

### Shell Commands

```mix
// Run a command, get exit code
ret = shell("make clean")
print(ret)    // 0

// Run a command, capture stdout
output = shell_output("uname -m")
print(output)  // arm64
```

### File System

```mix
// Check if a file exists
print(file_exists("main.mix"))    // true
print(file_exists("nope.xyz"))    // false

// List directory contents
files = list_dir(".")
for f in files
    print(f)

// Create directories (recursive, like mkdir -p)
mkdir("build/output/debug")

// Get current working directory
cwd = getcwd()
print(cwd)
```

### Environment and Process

```mix
// Read environment variable (returns "" if unset)
home = env("HOME")
print(home)

// Get command-line arguments
a = args()
print(a.len)    // at least 1 (the binary name)
for arg in a
    print(arg)

// Exit the process
exit(1)
```

### Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `shell` | `(str) -> int` | Run shell command, return exit code |
| `shell_output` | `(str) -> str` | Run command, capture stdout |
| `file_exists` | `(str) -> bool` | Check if file exists |
| `list_dir` | `(str) -> [str]` | List directory entries (excludes `.` and `..`) |
| `env` | `(str) -> str` | Read environment variable (`""` if unset) |
| `exit` | `(int) -> void` | Exit process with code |
| `getcwd` | `() -> str` | Get current working directory |
| `mkdir` | `(str) -> bool` | Recursive mkdir (`true` on success) |
| `args` | `() -> [str]` | Get command-line arguments |

---

## Build System

MIX has a built-in build system using `build.mix` — a MIX program that describes how to build your project. This is similar to Zig's `build.zig`.

### Quick Start

Create `build.mix` in your project root:

```mix
main() ~
    app = Project(
        name: "game",
        entry: "src/game.mix",
        output: "build/game"
    )
    app.build()
    print("Build complete!")
```

Run it:

```bash
mix build    # detects build.mix, compiles and runs it
```

### The Project Shape

`Project` is a built-in shape with these fields:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | `str` | *required* | Project name (used in error messages) |
| `entry` | `str` | *required* | Entry point source file |
| `output` | `str` | `""` (derived from entry) | Output binary path |
| `libs` | `[str]` | `[]` | Libraries to link (`-l` flags) |
| `lib_paths` | `[str]` | `[]` | Library search paths (`-L` flags) |
| `include_paths` | `[str]` | `[]` | Include paths |
| `flags` | `[str]` | `[]` | Extra compiler flags |
| `debug` | `bool` | `false` | Enable debug mode (`--debug`) |

Only `name` and `entry` are required. All other fields have defaults and can be omitted.

### Examples

**Minimal:**

```mix
main() ~
    app = Project(name: "hello", entry: "hello.mix")
    app.build()
```

**With libraries (SDL3):**

```mix
main() ~
    game = Project(
        name: "game",
        entry: "src/game.mix",
        output: "build/game",
        libs: ["SDL3", "m"],
        lib_paths: ["/opt/homebrew/lib"]
    )
    game.build()
```

**Debug build:**

```mix
main() ~
    app = Project(
        name: "app",
        entry: "src/main.mix",
        output: "build/app",
        debug: true
    )
    app.build()
```

**Multiple targets:**

```mix
main() ~
    server = Project(
        name: "server",
        entry: "src/server.mix",
        output: "build/server"
    )
    client = Project(
        name: "client",
        entry: "src/client.mix",
        output: "build/client"
    )
    server.build()
    client.build()
    print("All targets built!")
```

### How It Works

1. `mix build` (with no file argument) looks for `build.mix` in the current directory
2. If found, it compiles `build.mix` to a temporary binary and runs it
3. The `MIX_COMPILER` environment variable is set to the path of the `mix` binary, so `Project.build()` can invoke the compiler
4. `Project.build()` constructs and runs a `mix build` command with all the configured flags

---

## Compile-time Constants

`@const` defines values that are inlined at compile time:

```mix
@const MAX_SIZE = 1024
@const PI = 3.14159
@const SDL_INIT_VIDEO = 0x00000020

main()
    print(MAX_SIZE)    // 1024
    print(PI)          // 3.14159
```

Constants have zero runtime overhead — they're substituted directly into the code.

---

## Conditional Compilation

Compile-time conditions select which declarations to include:

```mix
@os == "macos"
    native_path() -> str
        "/usr/local/bin"

@arch == "aarch64"
    simd_available() -> bool
        true

@debug
    debug_log(msg: str) ~
        print(msg)
```

Supported conditions: `@os` (`"macos"`, `"linux"`), `@arch` (`"aarch64"`, `"x86_64"`), `@debug`, `@release`.

---

## Defer

`defer` schedules a statement to run when the current function exits, in reverse order:

```mix
main()
    print("start")
    defer print("cleanup 1")
    defer print("cleanup 2")
    print("work")
```

Output:

```
start
work
cleanup 2
cleanup 1
```

Deferred statements run before every `done` (early exit) and at the end of the function:

```mix
main()
    SDL_Init(SDL_INIT_VIDEO)
    defer SDL_Quit()

    win = SDL_CreateWindow("MIX", 800, 600, 0)
    defer SDL_DestroyWindow(win)
    // All cleanup happens automatically in reverse order
```

---

## Type Aliases

Create named synonyms for existing types:

```mix
type Score = int
type Position = float

s: Score = 100
p: Position = 3.14
```

---

## C Header Bindings

MIX can auto-generate binding files from C headers using `--bind`. This eliminates the need to manually write `extern` blocks.

### Single Header

```bash
./build/mix --bind /opt/homebrew/include/SDL3/SDL.h -o sdl3.mix --lib SDL3
```

### Entire Directory

```bash
./build/mix --bind /opt/homebrew/include/SDL3/ -o sdl3.mix --lib SDL3
```

### What It Does

1. Runs the C preprocessor (`cc -E`) to expand macros and includes
2. Parses function prototypes from the preprocessed output
3. Maps C types to MIX types (`int` -> `int32`, `double` -> `float`, pointers -> `*byte`, etc.)
4. Extracts `#define` constants (integer and float literals only)
5. Writes a valid `.mix` file with `extern` blocks and `@const` declarations

### Example Output

Given this C header:

```c
int add(int a, int b);
void quit(void);
double compute(double x, float y);
char *get_name(const char *prefix, int id);
#define MAX_SIZE 1024
```

Running `./build/mix --bind header.h -o bindings.mix --lib mylib` produces:

```mix
// Auto-generated MIX bindings for mylib
// Source: header.h
// Generated by: mix --bind

extern "mylib"
    add(a: int32, b: int32) -> int32 ~
    quit() ~
    compute(x: float, y: float32) -> float ~
    get_name(prefix: *byte, id: int32) -> *byte ~

@const MAX_SIZE = 1024
```

### Type Mapping

| C Type | MIX Type |
|--------|----------|
| `int` | `int32` |
| `long` / `long long` / `int64_t` | `int` |
| `float` | `float32` |
| `double` | `float` |
| `char` | `byte` |
| `uint8_t` / `unsigned char` | `uint8` |
| `uint32_t` / `unsigned int` | `uint32` |
| `size_t` | `int` |
| `bool` / `_Bool` | `bool` |
| Any pointer (`T *`) | `*byte` |
| `void` | (no return type) |

### Notes

- All generated extern functions are marked `~` (side effects) since purity cannot be inferred from C
- Functions returning structs/unions are skipped (not representable in MIX yet)
- Function pointer parameters are emitted as `*byte`
- System constants (INT_MAX, TARGET_OS_*, etc.) are filtered out
- If `--lib` is omitted, the library name is derived from the file/directory name

---

## Debug Info

Compile with `--debug` to emit DWARF line number information. This enables source-level debugging with `lldb` or `gdb`.

```bash
./build/mix program.mix -o program --debug
```

The compiler emits QBE `dbgfile` and `dbgloc` directives that produce `.file` and `.loc` assembler directives, which are encoded into DWARF line number tables in the binary.

### Viewing Debug IR

```bash
./build/mix program.mix --emit-ir -o - --debug
```

Output includes:

```
dbgfile "program.mix"
export function w $main() {
@start
    dbgloc 2
    %t0 =l copy 42
    dbgloc 3
    %t1 =l call $mix_print_int(l %t0)
    ret 0
}
```

---

## Compiler Reference

### Usage

```
Usage: mix [command] [options] [file.mix]

Commands:
    build [file.mix]    Compile to binary (default)
    run [file.mix]      Compile and execute

    If no file is given with 'build', checks for build.mix in CWD first,
    then auto-discovers a .mix file with main().

Options:
    -o <file>       Output file (default: derived from input)
    --emit-ir       Output QBE IR only
    --emit-tokens   Print token stream (debug)
    --emit-ast      Print AST (debug)
    --debug         Enable debug mode (DWARF info + @debug conditionals)
    --release       Enable release mode (@release conditionals active)
    --bind <path>   Generate .mix bindings from C header(s)
    --lib <name>    Library name for --bind (e.g., SDL3)
    -l<lib>         Link with library
    -L<path>        Library search path
    -f <file>       Explicit file (for run mode)
    -v              Verbose (show pipeline commands)
```

### Examples

```bash
# Compile and run
./build/mix hello.mix -o hello && ./hello

# Using subcommands
./build/mix build hello.mix -o hello
./build/mix run hello.mix

# Build using build.mix (auto-detected)
./build/mix build

# View generated QBE IR
./build/mix hello.mix --emit-ir -o hello.ssa

# View token stream
./build/mix hello.mix --emit-tokens

# View AST
./build/mix hello.mix --emit-ast

# Link with SDL3
./build/mix game.mix -o game -L/opt/homebrew/lib -lSDL3

# Verbose compilation
./build/mix hello.mix -o hello -v

# Generate C bindings
./build/mix --bind /opt/homebrew/include/SDL3/ -o sdl3.mix --lib SDL3

# Compile with debug info
./build/mix game.mix -o game --debug
```

---

## SDL3 Example

A complete SDL3 window with event handling and cornflower blue background:

```mix
extern "SDL3"
    SDL_Init(flags: uint32) -> int32 ~
    SDL_Quit() ~
    SDL_CreateWindow(title: *byte, w: int32, h: int32, flags: uint64) -> *byte ~
    SDL_DestroyWindow(win: *byte) ~
    SDL_CreateRenderer(win: *byte, name: *byte) -> *byte ~
    SDL_DestroyRenderer(rnd: *byte) ~
    SDL_SetRenderDrawColor(rnd: *byte, r: uint8, g: uint8, b: uint8, a: uint8) -> int32 ~
    SDL_RenderClear(rnd: *byte) -> int32 ~
    SDL_RenderPresent(rnd: *byte) -> int32 ~
    SDL_PollEvent(event: *byte) -> int32 ~
    SDL_Delay(ms: uint32) ~

extern "runtime"
    mix_bytes(n: int) -> *byte ~
    mix_peek_u32(ptr: *byte) -> uint32
    mix_free(ptr: *byte) ~

@const SDL_INIT_VIDEO = 0x00000020
@const SDL_EVENT_QUIT = 0x100

main()
    SDL_Init(SDL_INIT_VIDEO)

    win = SDL_CreateWindow("MIX + SDL3", 800, 600, 0)
    rnd = SDL_CreateRenderer(win, 0)
    event = mix_bytes(128)
    running! = 1

    while running > 0
        while SDL_PollEvent(event) > 0
            etype = mix_peek_u32(event)
            if etype == SDL_EVENT_QUIT
                running! = 0

        SDL_SetRenderDrawColor(rnd, 100, 149, 237, 255)
        SDL_RenderClear(rnd)
        SDL_RenderPresent(rnd)
        SDL_Delay(16)

    mix_free(event)
    SDL_DestroyRenderer(rnd)
    SDL_DestroyWindow(win)
    SDL_Quit()
```

Build and run:

```bash
./build/mix examples/sdl3_window.mix -o sdl3_window -L/opt/homebrew/lib -lSDL3
./sdl3_window
```

---

## Implementation Report

### Compiler Architecture

```
Source (.mix)
    |
    v
 [Lexer]        lexer.c    — Indentation-based tokenization, INDENT/DEDENT
    |
    v
 [Parser]       parser.c   — Recursive descent, Pratt precedence climbing
    |
    v
 [Sema]         sema.c     — Type inference, scope-based symbol table
    |
    v
 [QBE Emitter]  qbe_emit.c — SSA IR generation, aggregate types for shapes
    |
    v
 [QBE]          External   — Compiles IR to arm64 assembly
    v
 [cc/clang]     Links assembly + runtime into native binary
```

### Features Implemented

#### Phase 1 — Foundation
- [x] Variables: `x = 42` (immutable), `x! = 42` (mutable)
- [x] Type inference from literals (int, float, str, bool)
- [x] Functions with implicit return (last expression)
- [x] `done` / `done expr` for early exit
- [x] `if` / `else if` / `else`, `while` loops
- [x] Arithmetic, comparisons, compound assignment, boolean operators
- [x] `extern "lib"` C function interop
- [x] `print(...)` auto-formatting by type
- [x] Indentation-based blocks (4 spaces), `//` comments

#### Phase 2 — Data and Control
- [x] Shapes (structs) with named fields and field access
- [x] Shape passing to/from functions (QBE aggregate types)
- [x] `@const` compile-time constants
- [x] `defer` for scope-exit cleanup (reverse order)
- [x] `for i in 0..n` and `for i in 0..=n` range loops
- [x] `match` with `=>` arms and `_` wildcard
- [x] String interpolation: `"Hello {name}!"`

#### Phase 3 — Abstraction
- [x] Shape methods with implicit `self`, dot-call syntax
- [x] Modules: `use path.to.module`, `pub` exports
- [x] Multi-file compilation and linking
- [x] Type aliases: `type Score = int`
- [x] Lambdas and higher-order functions

#### Phase 4 — Collections & Memory
- [x] Lists: `[1, 2, 3]`, indexing, `.len`, `.push!()`, for-each
- [x] Operator overloading: `+ - * / % == != < > <= >=` on shapes
- [x] Unsafe blocks with pointer deref and pointer arithmetic
- [x] Zones: `zone { ... }` with automatic cleanup

#### Phase 5 — Error Handling & Optionals
- [x] Optional types: `T?`, `none`, `done value` wrapping, `expr else default`
- [x] Tagged unions with variant construction and destructuring match
- [x] `fail "message"` panic

#### Phase 6 — Generics & Advanced Types
- [x] `@T` generic type parameters with `has` constraints
- [x] Computed fields (zero-param methods without parens)
- [x] Positional shape construction

#### Phase 7 — Concurrency (Minimal)
- [x] `shared int(0)` — mutex-wrapped values with `.read()` and `.update!(fn)`
- [x] `go fn(args)` — spawn function in new pthread
- [x] `wait handle` — join thread, return result
- [x] Runtime: pthreads-based MixShared and MixTask

#### Phase 8 — Practical Features
- [x] String methods: `.upper()`, `.lower()`, `.trim()`, `.split()`, `.contains()`, `.starts_with()`, `.replace()`
- [x] Maps: `{"key": val}`, indexing, `.has()`, `.remove!()`, `.keys`, `.values`, iteration
- [x] Slices: `list[1..3]`, `list[..n]`, `list[n..]`, `list[0..=2]`
- [x] List comprehensions: `[x*x for x in list if cond]`
- [x] Conditional compilation: `@os`, `@arch`, `@debug`, `@release`

#### Phase 9 — Tooling
- [x] C header binding generator: `--bind <path> -o bindings.mix --lib name`
- [x] DWARF debug info: `--debug` emits `dbgfile`/`dbgloc` for lldb source-level debugging

#### Phase 10 — Ecosystem Improvements
- [x] File I/O: `file_read_all`, `file_write_all`, `file_open`/`file_read`/`file_write`/`file_close`
- [x] Math: `sqrt`, `abs`, `sin`, `cos`, `tan`, `log`, `pow`, `floor`, `ceil`, `round`, `min`, `max`
- [x] String ops: `str + str` concatenation, `.ends_with()`, `.char_at()`, `to_string()`
- [x] List ops: `.pop!()`, `.remove!()`, `.insert!()`, `.sort!()`, `.reverse!()`, `.contains()`, `.index_of()`, `.join()`
- [x] Result-based error handling: auto-wrapped return types, `fail` → error result, `expr else default`, `?` propagation
- [x] Examples: shapes_methods, tagged_unions, vec2_math, string_interp, concurrency
- [x] README.md

#### Phase 11 — Build System
- [x] OS builtins: `shell`, `shell_output`, `file_exists`, `list_dir`, `env`, `exit`, `getcwd`, `mkdir`, `args`
- [x] argc/argv capture: `main()` receives argc/argv, stored for `args()` builtin
- [x] Built-in `Project` shape: declarative build configuration with `name`, `entry`, `output`, `libs`, `lib_paths`, `include_paths`, `flags`, `debug`
- [x] `Project.build()` method: invokes the mix compiler as subprocess with configured flags
- [x] `build.mix` detection: `mix build` (no file) auto-discovers and runs `build.mix`
- [x] Default field values: missing Project fields filled with `""`, `[]`, or `false`

### Test Suite

45 end-to-end tests covering all implemented features::

| # | Test | What It Verifies |
|---|------|------------------|
| 000 | return_zero | Implicit main return 0 |
| 001 | arithmetic | `+ - * / %` operators |
| 002 | hello | String output |
| 003 | if_else | Branching and else-if |
| 004 | functions | User functions, implicit return |
| 005 | while | While loops, mutable variables |
| 006 | extern | C interop with libc puts |
| 007 | fibonacci | Recursion, done (early exit) |
| 008 | shapes | Shape construction, field access |
| 009 | shape_funcs | Shapes passed to/from functions |
| 010 | const | @const compile-time constants |
| 011 | defer | Defer in reverse order |
| 012 | for_range | For-range loops |
| 013 | match | Match with wildcards |
| 014 | string_interp | String interpolation |
| 015 | methods | Shape methods, dot-call |
| 016 | modules | Multi-file use/pub |
| 017 | module_shapes | Imported shapes and methods |
| 018 | lambdas | Lambda variables |
| 019 | higher_order | Lambdas as function arguments |
| 020 | lists | List literals, indexing, len, push, for-each |
| 021 | operator_overload | +, -, *, == on shapes |
| 022 | unsafe | Pointer deref, pointer arithmetic |
| 023 | zones | Scoped memory, nested zones |
| 024 | optionals | T?, none, expr else default |
| 025 | tagged_unions | Variant shapes, destructuring match |
| 026 | fail | Error handling with fail keyword |
| 027 | generics | @T generic functions |
| 028 | computed_fields | No-parens method access |
| 029 | positional_shapes | Shape construction without field labels |
| 030 | string_methods | .upper(), .lower(), .trim(), .split(), .contains(), .starts_with(), .replace() |
| 031 | maps | Map literals, indexing, .has(), index assign, .remove!(), .len, .keys, .values |
| 032 | slices_comp | List slicing and list comprehensions |
| 033 | cond_compile | @os == "macos", @arch == "aarch64" conditional declarations |
| 034 | has_constraints | @T has == generic constraints |
| 035 | shared | shared int(0), .read(), .update!(fn), mutex-wrapped values |
| 036 | go_wait | go compute(5), wait t, multiple concurrent tasks |
| 037 | file_io | file_write_all, file_read_all, file_open/write/close |
| 038 | math | sqrt, abs, floor, ceil, round, pow, min, max, sin, cos |
| 039 | string_ops | str + str concat, ends_with, char_at, to_string |
| 040 | list_ops | sort, contains, index_of, pop, reverse, insert, remove, join |
| 041 | error_handling | Result type: divide with fail, catch with else |
| 042 | error_propagation | ? operator: error propagation through call chain |
| 043 | build_builtins | OS builtins: shell, file_exists, env, getcwd, args |
| 044 | match_expr | Match as expression with implicit return (str, int) |

### What's Not Yet Implemented (from spec)

The full MIX spec describes additional features for future phases:

- `run` blocks (structured concurrency — all tasks in block run in parallel)
- Streams and channels (`stream`, `yield`, `channel`)
- Move semantics (`->` operator)
- Pattern matching on error variants (`ok(v) => ...`, `err(e) => ...`)
- Package manager
- Self-hosting

---

## Verification

```bash
make              # build compiler
make test         # 44/44 tests pass
./build/mix examples/sdl3_window.mix -o sdl3 -lSDL3      # cornflower blue window
./build/mix examples/raylib_example.mix -o raylib -lraylib # solid red window
./build/mix --bind /path/to/header.h -o bindings.mix --lib name  # C bindings
./build/mix program.mix -o prog --debug                   # DWARF debug info
./build/mix build                                          # run build.mix
```
