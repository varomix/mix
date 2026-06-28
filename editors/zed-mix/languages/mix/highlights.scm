; tree-sitter-mix highlight queries for Zed.

(line_comment) @comment

(integer) @number
(float) @number
(string) @string
(string_interp) @string
(bool) @boolean
(none_lit) @constant.builtin

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
] @operator

["fail"] @keyword
["done"] @keyword

[
  "int" "float" "bool" "byte" "str"
  "int8" "int16" "int32" "int64"
  "uint8" "uint16" "uint32" "uint64"
  "float32" "float64"
] @type.builtin

[
  "+" "-" "*" "/" "%"
  "==" "!=" "<" ">" "<=" ">="
  "=" "+=" "-=" "*=" "/="
  "->" "=>" "?" "~" "|" "&"
  ".." "..="
] @operator

[ "(" ")" "[" "]" "{" "}" ] @punctuation.bracket
[ "," "." ":" ] @punctuation.delimiter

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

(use_decl path: (dotted_path (identifier) @namespace))
(use_decl alias: (identifier) @namespace)

(identifier) @variable
(identifier_mut) @variable
