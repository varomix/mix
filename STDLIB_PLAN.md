# Plan: MIX Standard Library — Set Type + Stdlib Infrastructure

## Context

MIX has built-in list, map, and str types with methods hardcoded in sema/emit/runtime, but no `Set` type and no standard library. The user wants:
1. A built-in `Set` type (TYPE_SET) following the list/map pattern
2. A stdlib module system with auto-discovery relative to the compiler binary
3. Standard library modules: `std.math`, `std.string`, `std.collections`

This is a multi-phase plan. **Phase 1 (Set type) and Phase 2 (stdlib path) are independent and should be implemented first.** Phases 3-5 depend on Phase 2.

---

## Phase 1: Built-in Set Type (TYPE_SET)

**Design:** Set is backed by the existing MixMap runtime (keys = elements, values ignored). Since maps only support string keys, **Sets are initially string-element only** — same constraint as maps. This is consistent and avoidable later when the map generalizes.

**Syntax:**
- Type: `set[str]` (follows `[str]` list pattern)
- Literal: `set{"a", "b", "c"}`, empty: `set{}`
- Keyword: `set` becomes a reserved word

**Methods/Properties:**
| Method/Property | Signature | Description |
|---|---|---|
| `.add!(val)` | `(str) -> void` | Add element |
| `.remove!(val)` | `(str) -> void` | Remove element |
| `.has(val)` | `(str) -> bool` | Check membership |
| `.len` | `-> int` | Element count (property) |
| `.values` | `-> [str]` | All elements as list (property) |
| `.union(other)` | `(set[str]) -> set[str]` | Union of two sets |
| `.intersect(other)` | `(set[str]) -> set[str]` | Intersection |
| `.diff(other)` | `(set[str]) -> set[str]` | Difference (a - b) |

**Files to modify:**

### `src/token.h` — Add TOK_SET
Add `TOK_SET` to the TokenKind enum.

### `src/lexer.c` — Register keyword
Add `{"set", TOK_SET}` to keywords table. Add case to `token_kind_name()`.

### `src/types.h` — Add TYPE_SET and union member
```c
TYPE_SET,   // in TypeKind enum

struct { struct MixType *elem_type; } set;  // in MixType union
```

### `src/types.c` — Add TYPE_SET cases
- `type_kind_name()`: return `"set"`
- `type_to_qbe()`: return `"l"` (heap pointer)
- `type_size()`: return `8`

### `src/ast.h` — Add NODE_SET_LIT
```c
NODE_SET_LIT,

struct {
    AstNode **elements;
    int element_count;
} set_lit;
```

### `src/parser.c` — Parse set type and set literal
- In `parse_type()`: when `TOK_SET` is seen, expect `[`, parse inner type, expect `]`
- In `parse_primary()`: when `TOK_SET` is seen followed by `{`, parse comma-separated elements until `}`

### `src/sema.c` — Type checking
- Add `make_set_type(arena, elem)` helper
- In `resolve_type_node()`: handle TOK_SET → make_set_type
- In `resolve_expr()` NODE_SET_LIT: resolve elements, infer elem type, return set type
- In `resolve_expr()` NODE_METHOD_CALL TYPE_SET: validate `add`, `remove`, `has`, `union`, `intersect`, `diff`
- In `resolve_expr()` NODE_FIELD_EXPR TYPE_SET: validate `.len` (int), `.values` ([elem_type])
- In `analyze_stmt()` NODE_FOR_STMT: handle TYPE_SET iteration (loop var = elem_type)

### `src/qbe_emit.c` — Code generation
- NODE_SET_LIT: emit `$mix_set_new()` then `$mix_set_add()` per element
- NODE_METHOD_CALL TYPE_SET: dispatch to `$mix_set_*` runtime calls
- NODE_FIELD_EXPR TYPE_SET: `.len` → `$mix_set_len`, `.values` → `$mix_set_values`
- NODE_FOR_STMT TYPE_SET: convert via `$mix_set_values()` then iterate like list
- Print dispatch: `$mix_print_set`

### `lib/runtime.c` — Set runtime functions (~80 LOC)
All thin wrappers around existing MixMap:
```c
void *mix_set_new(void);                    // mix_map_new()
int64_t mix_set_len(const void *s);         // mix_map_len()
void mix_set_add(void *s, const char *e);   // mix_map_set(s, e, 1)
void mix_set_remove(void *s, const char *e); // mix_map_remove()
int32_t mix_set_has(const void *s, const char *e); // mix_map_has()
void *mix_set_values(const void *s);        // mix_map_keys()
void *mix_set_union(const void *a, const void *b);
void *mix_set_intersect(const void *a, const void *b);
void *mix_set_diff(const void *a, const void *b);
void mix_print_set(const void *s);          // set{"a", "b"}
```

### `tests/programs/045_sets.mix` + `tests/programs/expected/045.txt`
Test all operations: literal, add, remove, has, len, values, union, intersect, diff, for-in, print.

---

## Phase 2: Stdlib Search Path

**Design:** When `use std.math` is encountered, resolve `std.*` modules from the compiler binary's sibling `lib/` directory. No env var needed.

**Directory layout:**
```
<prefix>/bin/mix              (compiler binary)
<prefix>/lib/mix/std/         (stdlib .mix files)
<prefix>/lib/mix/runtime.c    (runtime — already searched here)

# During development:
build/mix                     → looks at lib/std/  (via ../lib/std/)
```

### `src/main.c` — Modify `resolve_module_path()`

Add `exe_dir` parameter (already computed for finding runtime.c). When module path starts with `"std."`:
1. Try `<exe_dir>/../lib/mix/std/<rest>.mix` (installed layout)
2. Try `<exe_dir>/../lib/std/<rest>.mix` (dev layout: build/ → lib/)
3. Fall back to existing relative-path logic

Move exe_dir computation earlier (before module compilation loop).

### `Makefile` — Add install target
```makefile
PREFIX ?= /usr/local
install:
    mkdir -p $(PREFIX)/bin $(PREFIX)/lib/mix/std
    cp build/mix $(PREFIX)/bin/
    cp lib/runtime.c $(PREFIX)/lib/mix/
    cp lib/std/*.mix $(PREFIX)/lib/mix/std/
```

### Create directory
```
lib/std/          (empty initially, populated in Phases 3-5)
```

---

## Phase 3: `std.math` Module

**File:** `lib/std/math.mix` — pure MIX, calls existing math builtins.

```mix
@const PI = 3.14159265358979323846
@const E = 2.71828182845904523536
@const TAU = 6.28318530717958647692

pub deg_to_rad(deg: float) -> float
    deg * PI / 180.0

pub rad_to_deg(rad: float) -> float
    rad * 180.0 / PI

pub hypot(x: float, y: float) -> float
    sqrt(x * x + y * y)

pub sign(x: float) -> float
    if x > 0.0
        done 1.0
    if x < 0.0
        done -1.0
    0.0

pub fmod(x: float, y: float) -> float
    x - floor(x / y) * y
```

Note: `@const` with float literals works (test 010 confirms). INF/NAN deferred — would need special handling.

### Test: `tests/programs/046_std_math.mix`

---

## Phase 4: `std.string` Module

**File:** `lib/std/string.mix` — pure MIX using existing str methods.

```mix
pub is_empty(s: str) -> bool
    s.len == 0

pub capitalize(s: str) -> str
    if s.len == 0
        done ""
    s.char_at(0).upper() + s.slice(1, s.len).lower()

pub pad_left(s: str, width: int, ch: str) -> str
    if s.len >= width
        done s
    ch.repeat(width - s.len) + s

pub pad_right(s: str, width: int, ch: str) -> str
    if s.len >= width
        done s
    s + ch.repeat(width - s.len)
```

Plus C-backed additions to `lib/runtime.c` + `src/sema.c` + `src/qbe_emit.c`:
- `str_reverse(s: str) -> str` — byte-level reversal needs C
- `str_count(s: str, sub: str) -> int` — counting occurrences

### Test: `tests/programs/047_std_string.mix`

---

## Phase 5: `std.collections` Module

**File:** `lib/std/collections.mix` — MIX shapes wrapping lists.

```mix
pub shape Stack
    items: [int]

    push!(val: int)~
        items.push!(val)

    pop!() -> int~
        items.pop!()

    peek() -> int
        items[items.len - 1]

    is_empty() -> bool
        items.len == 0

    size() -> int
        items.len

pub shape Queue
    items: [int]

    enqueue!(val: int)~
        items.push!(val)

    dequeue!() -> int~
        items.remove!(0)

    peek() -> int
        items[0]

    is_empty() -> bool
        items.len == 0

    size() -> int
        items.len
```

**Limitation:** No generics on shapes yet, so Stack/Queue are `int`-typed. This is acceptable for V1.

### Test: `tests/programs/048_std_collections.mix`

---

## Implementation Order

```
Phase 1 (Set type)        — standalone, no dependencies
Phase 2 (Stdlib path)     — standalone, no dependencies
    |
Phase 3 (std.math)        — depends on Phase 2
Phase 4 (std.string)      — depends on Phase 2
Phase 5 (std.collections) — depends on Phase 2
```

Recommend implementing **Phase 1 first** (most impactful), then **Phase 2**, then Phases 3-5 together.

---

## Verification

```bash
# After each phase:
make clean && make
make test   # all existing tests + new ones pass

# Phase 1:
./build/mix run tests/programs/045_sets.mix

# Phase 2+3:
./build/mix run tests/programs/046_std_math.mix

# Phase 2+4:
./build/mix run tests/programs/047_std_string.mix

# Phase 2+5:
./build/mix run tests/programs/048_std_collections.mix
```
