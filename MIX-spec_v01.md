# MIX Language Specification

> *"Fast to run, fast to write, fast to read."*

Version 0.1 — Draft Spec

---

## Philosophy

MIX is a systems programming language with the footprint of C and the clarity of a modern language.

- **Small** — ~30 keywords, the whole language fits in your head
- **Obvious** — if a beginner can't guess it, it's out
- **Fast** — compiles to native, no GC, no runtime overhead
- **Safe** — memory safety through zones, not a borrow checker
- **No semicolons** — newlines are statement terminators
- **No header files** — file = module, always

---

## 1. Variables

```mix
x = 42          // immutable
x! = 42         // mutable

x! += 1         // mutation — only works on mutable values
```

Immutable by default. Mutability is explicit with `!`. No `let`, `var`, `const`, `val`.

### Type Annotation (optional)

```mix
x = 42              // inferred: int
x: float = 42       // explicit — only when needed
```

---

## 2. Primitive Types


| Type    | Description                     |
| ------- | ------------------------------- |
| `int`   | 64-bit signed integer (default) |
| `float` | 64-bit float (default)          |
| `bool`  | `true` or `false`               |
| `byte`  | single byte                     |
| `str`   | UTF-8 immutable string          |


### Sized variants (low-level use only)

```
int8  int16  int32  int64
uint8 uint16 uint32 uint64
float32 float64
```

Only use sized types when working with hardware, protocols, or binary formats. Everywhere else, `int` and `float` are fine.

### Type Aliases

```mix
type Id     = int
type Grid   = [[float]]
type Handler = (str, int) -> bool

player_id: Id = 42
```

---

## 3. Functions

Four kinds. The suffix tells you everything before you read the body:


| Signature    | Meaning                                  |
| ------------ | ---------------------------------------- |
| `f(a, b)`    | pure — no side effects, no mutation      |
| `f(a, b) ~`  | side effects (I/O, print, network, etc.) |
| `f(a, b) !`  | mutates its arguments                    |
| `f(a, b) !~` | mutates arguments AND has side effects   |


```mix
// Pure
add(a, b)
    a + b

// Side effects
greet(name) ~
    print("Hello {name}!")

// Mutates
update(n) !
    n! += 1

// Both
save(data, path) !~
    f = open!(path)
    f.write!(data)
```

- Last expression is always the return value — no keyword needed
- `done` exits early, optionally with a value

```mix
find(list: [int], target: int) -> int?
    for i, v in list
        if v == target
            done i          // early exit with value
    none

validate(x: int) ~
    if x < 0
        done                // early exit, no value
    print("valid: {x}")
```

### Return Types

```mix
add(a: int, b: int) -> int
    a + b
```

Return type annotation is optional but recommended for public functions.

### First-class Functions

```mix
apply(f: int -> int, x: int) -> int
    f(x)

double = x => x * 2
apply(double, 5)        // 10

// inline
apply(x => x * 2, 5)
```

---

## 4. Control Flow

### If

```mix
if x > 0
    print("positive")
else if x == 0
    print("zero")
else
    print("negative")
```

`if` is an expression:

```mix
label = if score > 90 then "A" else "B"
```

### While

```mix
while running
    tick!()
```

### For

```mix
// over a collection
for item in list
    process(item)

// with index
for i, item in list
    print("{i}: {item}")

// range (exclusive end)
for i in 0..10
    print(i)

// range (inclusive end)
for i in 0..=10
    print(i)
```

### Repeat

```mix
repeat 5
    ping!()
```

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

### Match

```mix
match status
    200 => print("ok")
    404 => print("not found")
    500 => print("server error")
    _   => print("unknown")
```

Match is exhaustive — the compiler warns if a case is unhandled. `_` is the catch-all.

Match on shapes (tagged unions):

```mix
match shape
    Circle(r)    => 3.14 * r * r
    Rect(w, h)   => w * h
    Triangle(b, h) => 0.5 * b * h
```

---

## 5. Shapes (Structs)

```mix
shape Vec2
    x, y: float
```

Methods live inside the shape:

```mix
shape Circle
    radius: float

    area()
        3.14 * radius * radius

    grow(amount) !
        radius! += amount

    scale(factor) -> Circle
        Circle(radius: radius * factor)
```

Construction uses named fields:

```mix
c = Circle(radius: 2.0)
v = Vec2(x: 1.0, y: 3.0)
```

### Computed Fields

Look like fields, behave like functions — no `()` needed at call site:

```mix
shape Rect
    width, height: float

    area() -> float
        width * height

r = Rect(width: 4.0, height: 3.0)
print(r.area)       // no parens — reads like a field
```

### Operator Overloading

Name the operator directly:

```mix
shape Vec2
    x, y: float

    +(other: Vec2) -> Vec2
        Vec2(x: x + other.x, y: y + other.y)

    *(scalar: float) -> Vec2
        Vec2(x: x * scalar, y: y * scalar)

    ==(other: Vec2) -> bool
        x == other.x and y == other.y
```

Supported operators: `+` `-` `*` `/` `%` `==` `!=` `<` `>` `<=` `>=`

### Tagged Unions (Enum Shapes)

```mix
shape Shape
    Circle(radius: float)
    Rect(w: float, h: float)
    Triangle(base: float, height: float)
```

Used with `match` — compiler enforces exhaustiveness.

---

## 6. Collections

### List

```mix
nums = [1, 2, 3, 4, 5]
nums[0]             // 0-indexed
nums.len            // length
nums.push!(6)       // append
nums.sort!()            // sorts: int, float, and string lists
```

### Map

```mix
ages! = {"alice": 30, "bob": 25}

ages["alice"]           // read — 30
ages!["carol"] = 28     // add or update
ages.remove!("bob")     // delete
ages.has("carol")       // true
ages.len                // count
ages.keys               // ["alice", "carol"]
ages.values             // [30, 28]

// iterate
for name, age in ages
    print("{name} is {age}")
```

### Slice (zero-copy view)

```mix
nums = [1, 2, 3, 4, 5]

nums[1..3]      // by range      → [2, 3]
nums[..2]       // first 2       → [1, 2]
nums[2..]       // from index 2  → [3, 4, 5]
nums[-3..]      // last 3        → [3, 4, 5]
nums[..-1]      // all but last  → [1, 2, 3, 4]
nums[0..=2]     // inclusive end → [1, 2, 3]
```

Negative indices count from the end. Omitting either side means "from start" or "to end". No allocation — slices are zero-copy views into the original list.

### Comprehensions

```mix
squares = [x*x for x in nums]
evens   = [x for x in nums if x % 2 == 0]
pairs   = [(x, y) for x in xs for y in ys]
```

---

## 7. Strings

```mix
name = "Alice"
greeting = "Hello {name}!"      // interpolation
path = "line one\nline two"     // escape sequences

// multiline
msg = "
    Hello {name},
    Welcome to MIX.
"
```

Common operations:

```mix
name.len
name.upper()
name.lower()
name.trim()
name.split(" ")
name.contains("li")
name.starts_with("Al")
name.replace("Alice", "Bob")
name.sort()                     // "Acel" — sort characters
```

### String Comparison

Strings compare lexicographically (alphabetical order):

```mix
"apple" < "banana"      // true
"hello" == "hello"      // true
"xyz" > "abc"           // true
```

All comparison operators work: `==`, `!=`, `<`, `>`, `<=`, `>=`.

### Character Operations

```mix
ord("A")                // 65 (Unicode code point)
chr(65)                 // "A" (code point to string)
"A".code()              // 65 (same as ord)

// Round-trip
chr(ord("X"))           // "X"
```

`ord()` and `chr()` support full UTF-8 (1-4 byte sequences).

---

## 8. Optionals

`?` after any type means "this value or nothing":

```mix
find(list: [int], val: int) -> int?
    for x in list
        if x == val
            x
    none
```

Handling an optional:

```mix
// pattern match
result = find(nums, 7)
    some(v) => print("found {v}")
    none    => print("not there")

// default value
val = find(nums, 7) else 0

// propagate (float up if none)
val = find(nums, 7)?
```

No null. No null pointer exceptions. Ever.

---

## 9. Error Handling

Errors float up automatically — no try/catch, no explicit propagation needed:

```mix
read(path: str) -> str ~
    f = open!(path)         // if this fails, error floats to caller
    f.read!()               // never reached if open failed
```

### Raising Errors

Use `fail` to raise a specific error — it immediately starts floating up:

```mix
read(path: str) -> str ~
    f = open!(path) else
        fail AppError.NotFound(path: path)

    if no_permission(f)
        fail AppError.PermissionDenied(path: path)

    f.read!()
```

### Catching at the Call Site

```mix
// provide a fallback
data = read("file.txt") else "default"

// catch specific variants — same pattern as match
data = read("file.txt")
    ok(v)                            => v
    err(AppError.NotFound(p))        => "default content"
    err(AppError.PermissionDenied(p)) => panic("no access: {p}")
    err(e)                           => panic("unknown: {e}")

// propagate manually with ?
data = read("file.txt")?
```

If you handle some variants but not all, unhandled ones keep floating up. The compiler warns if you handle *some* variants without a catch-all `err(e)`.

### Defining Errors

```mix
shape AppError
    NotFound(path: str)
    PermissionDenied(path: str)
    ParseError(msg: str, line: int)
```

Errors are just shapes. `fail` raises them. `err(...)` catches them. No special syntax beyond that.


| Keyword    | Meaning                                       |
| ---------- | --------------------------------------------- |
| `fail`     | raise an error, starts floating               |
| `err(...)` | catch at call site, pattern match the variant |


---

## 10. Memory — Zones

A zone is a block of memory with a clear lifetime. Everything born in a zone dies when the zone ends. No GC. No borrow checker.

```mix
zone
    buf = bytes(1024)
    data = load!(buf)
    process(data)
// buf and data freed here automatically
```

### Nested Zones

```mix
zone
    a = bytes(1024)

    zone
        b = bytes(512)
        process(a, b)       // inner can see outer
    // b freed here

    finish(a)               // b is gone, a still alive
// a freed here
```

### Escaping a Zone

One value can escape — the last expression:

```mix
result = zone
    raw = fetch!("https://example.com")
    parsed = parse(raw)
    parsed          // escapes; raw is freed
```

### Named Zones

```mix
zone:game
    world = World(...)

    while world.running
        zone:frame
            events = poll_events!()
            world.update!(events)
            render!(world)
        // events freed every frame

// world freed here
```

### Passing Values — Lend vs Move

```mix
zone
    data = load("file.txt")

    process(data)       // lend — data still alive after
    sort!(data)         // lend mutable — data still alive after
    archive -> data     // move — data belongs to archive now
                        // data cannot be used here anymore
```

`->` is the move operator. Visually clear — the value goes with the arrow.

### Explicit Heap / Stack (rare)

```mix
x = stack int(42)   // force stack allocation
x = heap int(42)    // force heap allocation
```

The compiler picks the right location 99% of the time. These are escape hatches for low-level work.

---

## 11. Generics

`@T` before a function or shape means "T is whatever you pass in":

```mix
@T
first(list: [T]) -> T?
    if list.len == 0
        none
    else
        list[0]

@T, K
map(list: [T], f: T -> K) -> [K]
    [f(x) for x in list]
```

### Constraints

```mix
@T has area
total_area(shapes: [T]) -> float
    shapes.sum(by: s => s.area())

@T has +, ==
contains(list: [T], val: T) -> bool
    for x in list
        if x == val
            done true
    false
```

`has` checks that a type implements a method or operator. Multiple constraints separated by `,`:

```mix
@T has +, <, ==
```

---

## 12. Concurrency

### Spawn a Task

```mix
go fetch_data("url")            // fire and forget

t = go compute(data)            // spawn, get handle
result = wait t                 // block until done
```

### Run Block — structured concurrency

All branches run concurrently. Block exits when ALL are done:

```mix
run
    a = fetch_user(id)
    b = fetch_posts(id)
    c = fetch_notifications(id)

build_page(a, b, c)
```

### Streams — pipelines

```mix
stream count_up(max: int)
    i! = 0
    while i < max
        yield i
        i! += 1

// pipe with ->
count_up(100)
    -> filter(x => x % 2 == 0)
    -> map(x => x * x)
    -> take(10)
    -> each(x => print(x))
```

Each stage runs concurrently, passing values through like Unix pipes.

### Shared State

```mix
counter! = shared int(0)        // explicit shared mutable value

go worker_a(counter)
go worker_b(counter)

// safe access — lock is automatic
counter.update!(v => v + 1)
x = counter.read()
```

No manual mutexes. The `shared` type carries the lock. You can't accidentally share without saying so.

### Channels

```mix
ch = channel(int)

// sender
go
    for i in 0..10
        ch.send!(i)
    ch.close!()

// receiver
for val in ch
    print(val)
```

Channels are typed and first-class. `for val in ch` blocks until next value, stops on close.

### Concurrency Summary


| Need             | How                      |
| ---------------- | ------------------------ |
| Fire and forget  | `go fn()`                |
| Get result later | `t = go fn()` / `wait t` |
| Parallel + sync  | `run { }`                |
| Pipeline         | `stream` + `->`          |
| Shared state     | `shared` type            |
| Message passing  | `channel(T)`             |


---

## 13. Modules

File = module. Always. No exceptions.

```mix
// math.mix
pub add(a, b)
    a + b

pub PI = 3.14159

internal_helper()       // no pub = private
    PI * 2
```

### Importing

```mix
// use last segment as name
use engine.physics          // → physics.simulate!()
use engine.renderer         // → renderer.draw!()

// or give it an alias
use phy  engine.physics
use rnd  engine.renderer

phy.simulate!(world)
rnd.draw!(world)
```

### Selective Import

```mix
use math: add, PI       // named imports, no prefix
use graphics: *         // import everything (use sparingly)

x = add(1, 2)
```

### Folder = Namespace

```
engine/
  physics.mix
  renderer.mix
  audio.mix
main.mix
```

```mix
use phy  engine.physics
use rnd  engine.renderer
use aud  engine.audio
```

No `mod.rs`. No `__init__.py`. No `index.js`. Folders are namespaces automatically.

### Name Collisions — aliases required

```mix
use gl  graphics.opengl
use vk  graphics.vulkan     // same last segment — must alias
```

### External Packages

```mix
use http
use json
```

If a package isn't local, it's fetched from the registry automatically. No separate install step.

### C Interop

```mix
extern "libc"
    malloc(size: int) -> *byte
    free(ptr: *byte)

extern "SDL2"
    SDL_Init(flags: uint32) -> int
    SDL_Quit() ~
```

`extern` + library name. C types map directly. No binding generator needed.

---

## 14. Compile-time

`@` means "evaluate at compile time":

```mix
@const MAX_SIZE = 1024
@const VERSION  = "0.1.0"

// conditional compilation
@os == "linux"
    page_size() -> int
        4096

@os == "windows"
    page_size() -> int
        65536

// architecture
@arch == "arm64"
    use platform.arm

// debug vs release
@debug
    log(msg: str) ~
        print("[DEBUG] {msg}")

@release
    log(msg: str) ~
        // no-op in release
```

---

## 15. Pointers (Low-level)

Available for systems work. Opt-in, explicit:

```mix
x! = 10
p: *int = &x        // pointer to x
*p = 20             // deref and assign
y = *p              // deref and read
```

Pointer arithmetic (in a `unsafe` block):

```mix
unsafe
    p2 = p + 1
    *p2 = 99
```

`unsafe` makes intent visible. Everything outside `unsafe` is safe by construction.

---

## 16. Defer

Run something when the current scope exits, no matter how:

```mix
f = open!("file.txt")
defer f.close!()        // runs when function returns

// works with zones too
zone
    buf = alloc!(1024)
    defer free!(buf)
    process(buf)
// free! called here automatically
```

---

## 17. Built-in Functions

```mix
print(...)          // print to stdout with newline
panic(msg)          // abort with message
assert(cond, msg)   // abort if condition false
len(x)              // length of string/list/map
bytes(n)            // allocate n bytes
alloc!(T)           // allocate a T on the heap
free!(ptr)          // free heap memory
sizeof(T)           // size of type in bytes
type_of(x)          // type name as str (debug use)
ord(s)              // Unicode code point of first character
chr(n)              // code point to character string
```

Math:

```mix
sqrt(x)   abs(x)   min(a, b)   max(a, b)
floor(x)  ceil(x)  round(x)
sin(x)    cos(x)   tan(x)
pow(x, n) log(x)   log2(x)
```

---

## 18. Full Language Reference (One Page)


| Concept          | Syntax                           |
| ---------------- | -------------------------------- |
| Immutable value  | `x = 1`                          |
| Mutable value    | `x! = 1`                         |
| Type annotation  | `x: int = 1`                     |
| Pure function    | `f(a, b)`                        |
| Side-effect fn   | `f(a, b) ~`                      |
| Mutating fn      | `f(a, b) !`                      |
| Both             | `f(a, b) !~`                     |
| Raise error      | `fail ErrorShape.Variant(...)`   |
| Early exit       | `done` or `done value`           |
| Struct           | `shape Name`                     |
| Tagged union     | `shape Name` with variant fields |
| Optional type    | `T?`                             |
| None value       | `none`                           |
| Optional default | `x else default`                 |
| Error float      | automatic                        |
| Error catch      | `f()                             |
| Generic          | `@T`                             |
| Constraint       | `@T has method`                  |
| Zone             | `zone { }`                       |
| Named zone       | `zone:name { }`                  |
| Move value       | `f -> val`                       |
| Shared value     | `shared T(val)`                  |
| Spawn task       | `go fn()`                        |
| Await task       | `wait t`                         |
| Parallel block   | `run { }`                        |
| Stream           | `stream fn()` + `yield`          |
| Channel          | `channel(T)`                     |
| Import           | `use path.to.module`             |
| Import alias     | `use alias path.to.module`       |
| Selective import | `use module: a, b`               |
| Public export    | `pub`                            |
| C interop        | `extern "lib"`                   |
| Comptime const   | `@const NAME = val`              |
| Comptime if      | `@os == "linux"`                 |
| Unsafe block     | `unsafe { }`                     |
| Defer            | `defer expr`                     |
| Match            | `match x                         |
| Wildcard         | `_`                              |


**~30 keywords. No semicolons. No headers. No build files.**

---

## 19. Complete Example — HTTP API Server

```mix
use http
use json
use phy  engine.physics

shape User
    id:    int
    name:  str
    email: str

shape AppError
    NotFound(id: int)
    InvalidInput(msg: str)

// in-memory store (shared — accessed from multiple handlers)
store! = shared {"alice": User(id: 1, name: "Alice", email: "alice@mix.dev")}

find_user(name: str) -> User? ~
    store.read()[name]

main() !~
    server = http.Server(port: 8080)

    server.get!("/users/{name}") => (req) !~
        user = find_user(req.params["name"]) else
            done req.reply(404, "not found")
        req.reply(200, json.encode(user))

    server.post!("/users") => (req) !~
        u = json.decode(req.body, User) else
            done req.reply(400, "invalid body")
        store.update!(s => s[u.name] = u)
        req.reply(201, json.encode(u))

    server.delete!("/users/{name}") => (req) !~
        name = req.params["name"]
        store.update!(s => s.remove!(name))
        req.reply(204, "")

    print("MIX server running on :8080")
    server.start!()
```

---

## 20. Complete Example — SDL3 Window + Event Loop

SDL3 via C interop — creating a window, handling events, and a game loop.

```mix
// Declare the SDL3 C bindings we need
extern "SDL3"
    // init / quit
    SDL_Init(flags: uint32) -> bool ~
    SDL_Quit() ~

    // window
    SDL_CreateWindow(title: *byte, w: int32, h: int32, flags: uint64) -> *byte ~
    SDL_DestroyWindow(win: *byte) ~

    // renderer
    SDL_CreateRenderer(win: *byte, name: *byte) -> *byte ~
    SDL_DestroyRenderer(rnd: *byte) ~
    SDL_SetRenderDrawColor(rnd: *byte, r: uint8, g: uint8, b: uint8, a: uint8) -> bool ~
    SDL_RenderClear(rnd: *byte) -> bool ~
    SDL_RenderPresent(rnd: *byte) -> bool ~

    // events
    SDL_PollEvent(event: *byte) -> bool ~
    SDL_GetEventType(event: *byte) -> uint32

@const SDL_INIT_VIDEO  = 0x00000020
@const SDL_EVENT_QUIT  = 0x100
@const WINDOW_W        = 800
@const WINDOW_H        = 600

shape SDLError
    InitFailed
    WindowFailed
    RendererFailed

main() !~
    // init
    ok = SDL_Init(SDL_INIT_VIDEO)
    if not ok
        fail SDLError.InitFailed

    defer SDL_Quit()

    // window
    win = SDL_CreateWindow("MIX + SDL3", WINDOW_W, WINDOW_H, 0)
    if win == none
        fail SDLError.WindowFailed

    defer SDL_DestroyWindow(win)

    // renderer
    rnd = SDL_CreateRenderer(win, none)
    if rnd == none
        fail SDLError.RendererFailed

    defer SDL_DestroyRenderer(rnd)

    // event buffer — raw bytes, passed to SDL
    event! = bytes(128)
    running! = true

    // game loop
    while running
        // drain event queue
        while SDL_PollEvent(event)
            type = SDL_GetEventType(event)
            if type == SDL_EVENT_QUIT
                running! = false

        // draw — cornflower blue background
        SDL_SetRenderDrawColor(rnd, 100, 149, 237, 255)
        SDL_RenderClear(rnd)
        SDL_RenderPresent(rnd)
```

Key things to notice:

- `defer` pairs every create with its destroy — nothing leaks even if we `done` early
- `extern "SDL3"` declares only what we use — no massive header dump
- The event buffer is raw `bytes(128)` — MIX doesn't pretend to know SDL's internal struct layout, we just pass the pointer through
- `fail` on init errors propagates up to whoever called `main`

---

## 21. Complete Example — OpenGL Triangle

OpenGL + SDL3 together — classic hello world of graphics.

```mix
extern "SDL3"
    SDL_Init(flags: uint32) -> bool ~
    SDL_Quit() ~
    SDL_CreateWindow(title: *byte, w: int32, h: int32, flags: uint64) -> *byte ~
    SDL_DestroyWindow(win: *byte) ~
    SDL_GL_CreateContext(win: *byte) -> *byte ~
    SDL_GL_DestroyContext(ctx: *byte) ~
    SDL_GL_SwapWindow(win: *byte) ~
    SDL_PollEvent(event: *byte) -> bool ~
    SDL_GetEventType(event: *byte) -> uint32

extern "GL"
    glViewport(x: int32, y: int32, w: int32, h: int32) ~
    glClearColor(r: float32, g: float32, b: float32, a: float32) ~
    glClear(mask: uint32) ~
    glGenVertexArrays(n: int32, arrays: *uint32) ~
    glBindVertexArray(vao: uint32) ~
    glGenBuffers(n: int32, buffers: *uint32) ~
    glBindBuffer(target: uint32, buffer: uint32) ~
    glBufferData(target: uint32, size: int, data: *byte, usage: uint32) ~
    glVertexAttribPointer(idx: uint32, size: int32, type: uint32, norm: bool, stride: int32, ptr: *byte) ~
    glEnableVertexAttribArray(idx: uint32) ~
    glCreateShader(type: uint32) -> uint32 ~
    glShaderSource(shader: uint32, count: int32, src: **byte, len: *int32) ~
    glCompileShader(shader: uint32) ~
    glCreateProgram() -> uint32 ~
    glAttachShader(prog: uint32, shader: uint32) ~
    glLinkProgram(prog: uint32) ~
    glUseProgram(prog: uint32) ~
    glDrawArrays(mode: uint32, first: int32, count: int32) ~

@const SDL_INIT_VIDEO      = 0x00000020
@const SDL_WINDOW_OPENGL   = 0x0000000000000002
@const SDL_EVENT_QUIT      = 0x100
@const GL_COLOR_BUFFER_BIT = 0x00004000
@const GL_TRIANGLES        = 0x0004
@const GL_FLOAT            = 0x1406
@const GL_ARRAY_BUFFER     = 0x8892
@const GL_STATIC_DRAW      = 0x88B4
@const GL_VERTEX_SHADER    = 0x8B31
@const GL_FRAGMENT_SHADER  = 0x8B30

// Shader sources as compile-time constants
@const VERT_SRC = "
    #version 330 core
    layout (location = 0) in vec3 aPos;
    void main() {
        gl_Position = vec4(aPos, 1.0);
    }
"

@const FRAG_SRC = "
    #version 330 core
    out vec4 FragColor;
    void main() {
        FragColor = vec4(1.0, 0.5, 0.2, 1.0);
    }
"

shape GfxError
    InitFailed
    WindowFailed
    ContextFailed

compile_shader(src: str, type: uint32) -> uint32 ~
    shader = glCreateShader(type)
    glShaderSource(shader, 1, &src, none)
    glCompileShader(shader)
    shader

build_program() -> uint32 ~
    vert = compile_shader(VERT_SRC, GL_VERTEX_SHADER)
    frag = compile_shader(FRAG_SRC, GL_FRAGMENT_SHADER)
    prog = glCreateProgram()
    glAttachShader(prog, vert)
    glAttachShader(prog, frag)
    glLinkProgram(prog)
    prog

main() !~
    if not SDL_Init(SDL_INIT_VIDEO)
        fail GfxError.InitFailed
    defer SDL_Quit()

    win = SDL_CreateWindow("MIX Triangle", 800, 600, SDL_WINDOW_OPENGL)
    if win == none
        fail GfxError.WindowFailed
    defer SDL_DestroyWindow(win)

    ctx = SDL_GL_CreateContext(win)
    if ctx == none
        fail GfxError.ContextFailed
    defer SDL_GL_DestroyContext(ctx)

    glViewport(0, 0, 800, 600)

    // triangle vertices — x, y, z
    verts: [float32] = [
         0.0,  0.5, 0.0,    // top
        -0.5, -0.5, 0.0,    // bottom left
         0.5, -0.5, 0.0     // bottom right
    ]

    // upload geometry to GPU
    vao!: uint32 = 0
    vbo!: uint32 = 0
    unsafe
        glGenVertexArrays(1, &vao)
        glGenBuffers(1, &vbo)

    glBindVertexArray(vao)
    glBindBuffer(GL_ARRAY_BUFFER, vbo)
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW)
    glVertexAttribPointer(0, 3, GL_FLOAT, false, 0, none)
    glEnableVertexAttribArray(0)

    prog = build_program()
    event! = bytes(128)
    running! = true

    while running
        while SDL_PollEvent(event)
            if SDL_GetEventType(event) == SDL_EVENT_QUIT
                running! = false

        glClearColor(0.1, 0.1, 0.1, 1.0)
        glClear(GL_COLOR_BUFFER_BIT)

        glUseProgram(prog)
        glBindVertexArray(vao)
        glDrawArrays(GL_TRIANGLES, 0, 3)

        SDL_GL_SwapWindow(win)
```

Key things to notice:

- `extern` blocks are the only FFI ceremony — declare once, use anywhere
- `unsafe` is scoped tightly to just the pointer address operations — everything else stays safe
- `defer` handles all cleanup in declaration order, reversed on exit — no resource leaks possible
- Shader sources are `@const` — evaluated at compile time, zero runtime overhead
- `zone` isn't needed here because `defer` + stack allocation handles lifetime naturally for this pattern

---

## 22. Design Non-Goals

MIX deliberately does **not** have:

- Inheritance
- Exceptions
- Null
- Header files
- Build configuration files
- Semicolons
- Implicit type coercion
- Hidden control flow
- A garbage collector
- A borrow checker
- Macros (comptime `@` replaces them)
- `return` keyword (replaced by `done`)

---

*MIX — small enough to fit in your head, fast enough to replace C.*