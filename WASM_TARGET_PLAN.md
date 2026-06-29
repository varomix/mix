# WASM Target Plan: `--target wasm32`

## Goal

Make `mix build hello.mix --target wasm32` produce a `.wasm` file that runs in
`wasmtime` / `wasmer` / a browser WASM runtime with WASI support.

## Strategy

Reuse the existing LLVM backend (LIR → textual LLVM IR → pipe to clang).
Instead of compiling for the host triple (`arm64-apple-darwin`), pass
`--target=wasm32-wasip1` to clang and link against the WASI sysroot.

No new emitter needed — just retarget the existing `clang -x ir` pipeline.

---

## Prerequisites (already installed)

| Tool | Path | Role |
|------|------|------|
| Emscripten LLVM clang | `$(brew --prefix emscripten)/libexec/llvm/bin/clang` | WASM-capable clang |
| wasi-libc | `$(brew --prefix)/share/wasi-sysroot` | WASI headers + crt1 + libc |
| wasi-runtimes | `$(brew --prefix)/share/wasi-runtimes` | Compiler-rt builtins for WASI |
| wasmtime | `$(brew --prefix)/bin/wasmtime` | Local runner |
| wasm-ld | included with Emscripten LLVM | WASM linker |

---

## Changes

### 1. `lib/runtime.c` — WASI portability guards

Only 2 functions fail under WASI: `popen`/`pclose` in `mix_shell_output` and
`system()` in `mix_shell`. Guard them:

```c
#ifndef __wasi__
int64_t mix_shell(const char *cmd) {
    return (int64_t)system(cmd);
}

char *mix_shell_output(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) { /* return empty string */ }
    // ... existing code ...
    pclose(fp);
    return buf;
}
#else
int64_t mix_shell(const char *cmd) { (void)cmd; return -1; }
char *mix_shell_output(const char *cmd) { /* return empty string */ }
#endif
```

Everything else in `lib/runtime.c` (pthreads, dirent, math, stdio, etc.)
compiles cleanly under WASI as-is.

### 2. `Makefile` — WASI runtime object

Add a build rule to pre-compile `runtime-wasi.o`.

```makefile
WASI_CLANG  = /opt/homebrew/Cellar/emscripten/6.0.1/libexec/llvm/bin/clang
WASI_FLAGS  = --target=wasm32-wasip1 --sysroot=/opt/homebrew/share/wasi-sysroot \
              -resource-dir=/opt/homebrew/share/wasi-runtimes
RUNTIME_WASI_O = $(BUILD_DIR)/runtime-wasi.o

$(RUNTIME_WASI_O): lib/runtime.c | $(BUILD_DIR)
	$(WASI_CLANG) $(WASI_FLAGS) -O2 -c $< -o $@

all: ... $(RUNTIME_WASI_O)
```

### 3. `src/main.c` — `--target wasm32` flag and link step changes

#### 3a. Flag parsing (near `-O` / `--backend` / `-v`)

```c
const char *target_arch = "native";
// ... in the option-parsing loop:
else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
    target_arch = argv[i + 1];
    i++;
}
```

#### 3b. WASI clang path detection (replace hardcoded with env var or brew path)

```c
static const char *detect_wasi_clang(void) {
    const char *env = getenv("WASI_CLANG");
    if (env) return env;
    // Fall back to brew-installed Emscripten clang
    const char *candidates[] = {
        "/opt/homebrew/Cellar/emscripten/6.0.1/libexec/llvm/bin/clang",
        "/opt/homebrew/bin/wasm32-wasi-clang",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0) return candidates[i];
    }
    return NULL;
}
```

#### 3c. Modify the link-argv assembly (lines ~1498-1578)

When `target_arch == "wasm32"`:

| Change | Reason |
|--------|--------|
| Use WASI clang as argv[0] | Host clang can't target wasm32 |
| Add `--target=wasm32-wasip1` | Sets triple |
| Add `--sysroot=...` | WASI headers + libc |
| Add `-resource-dir=...` | WASI compiler-rt builtins |
| Skip `-lm` | WASI libc includes math |
| Skip macOS SDK `-L` injection | Not applicable to WASM |
| Change output to `.wasm` | Correct extension |
| Use `build/runtime-wasi.o` instead of `build/runtime.o` | WASI-compiled runtime |

#### 3d. Help text

```
  --target <arch>     target architecture: native (default) or wasm32
```

#### 3e. `mix run` for wasm target

When `--target wasm32 && mode == MODE_RUN`, run
`wasmtime <output.wasm>` instead of executing the binary directly.

---

## End-to-end flow

```
$ mix build hello.mix --target wasm32 --timings
mix: --- compile timings ---
  lex             0.01 ms
  parse           0.02 ms
  sema            0.03 ms
  emit            0.12 ms
  cc             48.00 ms     (clang --target=wasm32-wasip1 ...)
  cleanup         0.01 ms

$ file hello
hello.wasm: WebAssembly (wasm) binary module version 0x1 (MVP)

$ wasmtime hello.wasm
Hello, MIX!

$ mix run hello.mix --target wasm32
Hello, MIX!           # runs via wasmtime under the hood
```

---

## Open questions / future work

- **Hardcoded clang path** — resolved with `$WASI_CLANG` env var
- **`mix run` default arch** — should `mix run hello.mix` default to `native`? Yes
- **`--target wasm32` without `--backend llvm`** — should error or auto-select LLVM
- **Browser JS glue** — WASI-only `.wasm` needs a WASI polyfill in the browser
  (`@wasmer/wasi` or `browser-wasi`). A `--target wasm-browser` sub-mode could
  add emscripten-style JS glue later
- **C backend** — could also emit `.c` and compile via `wasm32-wasi-clang`, but
  the LLVM path is faster and already works; leave this for later if needed
- **Optimization** — `runtime-wasi.o` is ~70KB of object; `wasm-ld` strips
  unused, but `-Os` could shrink further
