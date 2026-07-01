; tree-sitter-mix highlight queries
; Compatible with neovim nvim-treesitter and Helix.

; --- Identifiers (catch-all, first so specific overrides win) ---
(identifier) @variable.builtin
(identifier_mut) @variable.builtin

; --- Comments ---
(line_comment) @comment

; --- Literals ---
(integer) @number
(float) @float
(string_content) @string
(escape_sequence) @string
(bool) @boolean
(none_lit) @constant.builtin

; --- Keywords ---
[
  "if" "else" "while" "for" "in" "match"
  "break" "continue" "done" "shape" "union"
  "extern" "use" "pub" "type" "zone" "defer"
  "set"
  "stack" "heap"
] @keyword

(unsafe_block) @keyword

[
  "and" "or" "not"
] @keyword.operator

["fail"] @keyword.exception
["done"] @keyword.return

; --- Type keywords ---
[
  "int" "float" "bool" "byte" "str"
  "int8" "int16" "int32" "int64"
  "uint8" "uint16" "uint32" "uint64"
  "float32" "float64"
] @type.builtin

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
[ "," "." ":" ] @punctuation.delimiter

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
