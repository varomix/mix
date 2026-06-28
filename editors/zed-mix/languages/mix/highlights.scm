; tree-sitter-mix highlight queries for Zed.

; --- Identifiers (catch-all, lowest priority — must come first) ---
(identifier) @variable.builtin
(identifier_mut) @variable.builtin

; --- Comments ---
(line_comment) @comment

; --- Literals ---
(integer) @number
(float) @number
(string_content) @string
(escape_sequence) @string
(bool) @boolean
(none_lit) @constant.builtin

; --- Control-flow keywords ---
[
  "if" "else" "while" "for" "in" "match"
  "break" "continue" "done"
] @keyword

; --- Declaration / scoping keywords ---
[
  "pub" "use" "extern" "defer" "zone"
  "type" "shape" "union" "has" "fail"
  "set" "stack" "heap"
] @keyword

; --- Comptime directives ---
(const_decl "@const" @keyword)
(cond_decl) @keyword
(generic_params "@" @keyword)

["and" "or" "not"] @operator

; --- Type builtins ---
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

; --- Calls ---
(call_expr callee: (identifier) @function.call)
(method_call method: (identifier) @function.method)
(method_call method: (identifier_mut) @function.method)

; --- Fields / Shapes ---
(field_access field: (identifier) @property)
(shape_lit shape: (identifier) @type)
(shape_lit field: (identifier) @property)

; --- Namespaces ---
(use_decl path: (dotted_path (identifier) @namespace))
(use_decl alias: (identifier) @namespace)



