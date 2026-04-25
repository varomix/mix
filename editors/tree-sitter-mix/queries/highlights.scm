; tree-sitter-mix highlight queries
; Compatible with neovim nvim-treesitter and Helix.

; --- Comments ---
(line_comment) @comment

; --- Literals ---
(integer) @number
(float) @float
(string) @string
(string_interp) @string
(bool) @boolean
(none_lit) @constant.builtin

; --- Keywords ---
[
  "if" "else" "while" "for" "in" "match"
  "break" "continue" "done" "shape" "union"
  "extern" "use" "pub" "type" "zone" "defer"
  "unsafe" "go" "wait" "shared" "set"
  "stack" "heap"
] @keyword

[
  "and" "or" "not"
] @keyword.operator

["fail"] @keyword.exception
["return" "done"] @keyword.return

; --- Type keywords ---
(_type_keyword) @type.builtin

; --- Operators ---
[
  "+" "-" "*" "/" "%"
  "==" "!=" "<" ">" "<=" ">="
  "=" "+=" "-=" "*=" "/="
  "->" "=>" "?" "~" "|" "&"
  ".." "..="
] @operator

; --- Punctuation ---
[ "(" ")" "[" "]" "{" "}" ] @punctuation.bracket
[ "," "." ":" ";" ] @punctuation.delimiter

; --- Declarations ---
(fn_decl name: (identifier) @function)
(shape_decl name: (identifier) @type)
(union_decl name: (identifier) @type)
(type_alias name: (identifier) @type)
(const_decl name: (identifier) @constant)

(param name: (identifier) @variable.parameter)
(param name: (identifier_mut) @variable.parameter)
(param type: (_) @type)

(call_expr callee: (identifier) @function.call)
(method_call method: (identifier) @function.method)
(method_call method: (identifier_mut) @function.method)

(field_access field: (identifier) @property)
(shape_lit shape: (identifier) @type)
(shape_lit field: (identifier) @property)

; --- `use` paths ---
(use_decl path: (dotted_path (identifier) @namespace))
(use_decl alias: (identifier) @namespace)

; --- Identifiers (catch-all, lower priority) ---
(identifier) @variable
(identifier_mut) @variable
