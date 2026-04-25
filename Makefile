CC = cc
CFLAGS = -Wall -Wextra -std=c11 -g -O0
QBE = qbe

# Stamped into the binary so `mix --help` / `mix --version` shows the build date.
BUILD_DATE := $(shell date +%Y-%m-%d)

SRC_DIR = src
BUILD_DIR = build

# Shared frontend objects (used by both mix and mix-lsp)
FRONTEND_SRCS = $(SRC_DIR)/arena.c $(SRC_DIR)/ast.c $(SRC_DIR)/errors.c \
                $(SRC_DIR)/lexer.c $(SRC_DIR)/parser.c $(SRC_DIR)/sema.c \
                $(SRC_DIR)/symtab.c $(SRC_DIR)/types.c
FRONTEND_OBJS = $(FRONTEND_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Compiler-only objects
COMPILER_SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/qbe_emit.c $(SRC_DIR)/c_emit.c $(SRC_DIR)/cbind.c $(SRC_DIR)/fmt.c
COMPILER_OBJS = $(COMPILER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# LSP objects
LSP_SRCS = $(wildcard $(SRC_DIR)/lsp/*.c)
LSP_OBJS = $(LSP_SRCS:$(SRC_DIR)/lsp/%.c=$(BUILD_DIR)/lsp_%.o)

BIN = $(BUILD_DIR)/mix
LSP_BIN = $(BUILD_DIR)/mix-lsp

all: $(BIN) $(LSP_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lsp_%.o: $(SRC_DIR)/lsp/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# main.o is always rebuilt so MIX_VERSION_DATE tracks today's build.
.PHONY: $(BUILD_DIR)/main.o
$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DMIX_VERSION_DATE='"$(BUILD_DATE)"' -c $< -o $@

$(BIN): $(FRONTEND_OBJS) $(COMPILER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(LSP_BIN): $(FRONTEND_OBJS) $(LSP_OBJS) $(BUILD_DIR)/cbind.o $(BUILD_DIR)/fmt.o
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD_DIR)

# Compile a .mix file to a binary
# Usage: make run SRC=examples/hello.mix
run: $(BIN)
	$(BIN) $(SRC) -o $(BUILD_DIR)/output.ssa
	$(QBE) $(BUILD_DIR)/output.ssa -o $(BUILD_DIR)/output.s
	$(CC) $(BUILD_DIR)/output.s lib/runtime.c -o $(BUILD_DIR)/output
	$(BUILD_DIR)/output

test: $(BIN)
	bash tests/run_tests.sh

test-errors: $(BIN)
	bash tests/run_error_tests.sh

test-error-messages: $(BIN)
	bash tests/run_error_message_tests.sh

test-fmt: $(BIN)
	bash tests/run_fmt_tests.sh

test-all: test test-errors test-error-messages test-fmt

PREFIX ?= /usr/local
install: $(BIN) $(LSP_BIN)
	mkdir -p $(PREFIX)/bin
	mkdir -p $(PREFIX)/lib/mix/std
	cp $(BIN) $(PREFIX)/bin/
	cp $(LSP_BIN) $(PREFIX)/bin/
	cp lib/runtime.c $(PREFIX)/lib/mix/
	@if [ -d lib/std ] && [ "$$(ls -A lib/std/*.mix 2>/dev/null)" ]; then \
		cp lib/std/*.mix $(PREFIX)/lib/mix/std/; \
	fi

.PHONY: all clean run test test-errors test-error-messages test-fmt test-all install
