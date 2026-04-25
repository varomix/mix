" Vim syntax file for the MIX programming language
" Language:   MIX
" Maintainer: varomix
" Version:    0.1.0

if exists("b:current_syntax")
    finish
endif

let s:cpo_save = &cpo
set cpo&vim

" ── Keywords ──
syn keyword mixKeyword  if else while for in match done shape union extern use pub
syn keyword mixKeyword  type zone defer unsafe go wait shared as then
syn keyword mixKeyword  break continue
syn keyword mixKeyword  run stream yield repeat set fail

" ── Compile-time directives ──
syn match mixDirective  '@os\>'
syn match mixDirective  '@arch\>'
syn match mixDirective  '@debug\>'
syn match mixDirective  '@release\>'
syn match mixDirective  '@const\>'
syn match mixDirective  '@T\>'

" ── Boolean literals ──
syn keyword mixBoolean  true false

" ── None literal ──
syn keyword mixNone     none

" ── Built-in types ──
syn keyword mixType     int float bool byte str
syn keyword mixType     int8 int16 int32 int64
syn keyword mixType     uint8 uint16 uint32 uint64
syn keyword mixType     float32 float64
syn keyword mixType     list map

" ── Numeric literals ──
syn match mixInteger    '\<\d\+\>'
syn match mixInteger    '\<0x[0-9a-fA-F]\+\>'
syn match mixInteger    '\<0b[01]\+\>'
syn match mixFloat      '\<\d\+\.\d*\>'
syn match mixFloat      '\<\d*\.\d\+\>'

" ── Identifiers inside interpolation (contained items) ──
syn match mixInterpIdent    '\<\a\w*\>' contained
syn match mixInterpField    '\.\a\w*' contained
syn match mixInterpMethod   '\.\a\w*!\?\ze\s*(' contained

" ── String literals ──
" Interpolation uses a specific contains list (NOT contains=TOP) to avoid
" string regions nesting inside {}, which breaks highlighting.
syn region mixString    start='"' skip='\\"' end='"'
            \ contains=mixEscape,mixInterp
syn match  mixEscape    '\\[nrt\\0"]' contained
syn region mixInterp    start='{' end='}' contained
            \ contains=mixInterpIdent,mixInterpField,mixInterpMethod,
            \mixInteger,mixFloat,mixBoolean,mixOperator,mixBuiltin

" ── Operators ──
syn match mixOperator   '[+\-*/%]'
syn match mixOperator   '[<>]=\?'
syn match mixOperator   '[!=]='
syn match mixOperator   '->'
syn match mixOperator   '=>'
syn match mixOperator   '\.\.'
syn match mixOperator   '\.\.='
syn match mixOperator   '[&|?~]'

" ── Delimiters (exclude {} from global match to avoid interfering with interp) ──
syn match mixDelimiter  '[()\[\]:,]'

" ── Side effects marker (~) and mutation marker (!) ──
syn match mixMutMark    '!\ze\s*[=)]'
syn match mixMutMark    '\~\ze\s*('

" ── Function definitions: name followed by ( ──
syn match mixFuncDef    '\<\a\w*\ze\s*('
syn match mixFuncDef    '\<\a\w*[~!]\ze\s*('

" ── Method calls: .name( ──
syn match mixMethod     '\.\a\w*!\?\ze\s*('

" ── Shape name (capitalized identifiers) ──
syn match mixShapeName  '\<\u\w*\>'

" ── Field access: .name (not followed by paren) ──
syn match mixField      '\.\a\w*\>'

" ── Mutable variable declarations: name! ──
syn match mixMutVar     '\<\a\w*!\s*='

" ── Built-in functions ──
syn keyword mixBuiltin  print sqrt abs sin cos tan log floor ceil round pow
syn keyword mixBuiltin  min max to_string to_int to_float to_set
syn keyword mixBuiltin  file_open file_read file_write file_close
syn keyword mixBuiltin  file_read_all file_write_all file_exists list_dir
syn keyword mixBuiltin  shell shell_output env exit getcwd mkdir args
syn keyword mixBuiltin  str_reverse str_count ord chr
syn keyword mixBuiltin  alloc bytes peek_u32 free_mem
syn keyword mixBuiltin  len panic assert sizeof type_of

" ── Comments (must be defined AFTER keywords so the match takes priority) ──
syn match mixComment    '//.*$' contains=mixTodo
syn keyword mixTodo     TODO FIXME XXX HACK NOTE contained

" ── Linking ──
hi def link mixComment       Comment
hi def link mixTodo          Todo
hi def link mixKeyword       Keyword
hi def link mixDirective     PreProc
hi def link mixBoolean       Boolean
hi def link mixNone          Constant
hi def link mixType          Type
hi def link mixInteger       Number
hi def link mixFloat         Float
hi def link mixString        String
hi def link mixEscape        SpecialChar
hi def link mixInterp        Special
hi def link mixInterpIdent   Special
hi def link mixInterpField   Special
hi def link mixInterpMethod  Special
hi def link mixOperator      Operator
hi def link mixDelimiter     Delimiter
hi def link mixFuncDef       Function
hi def link mixMethod        Function
hi def link mixShapeName     Structure
hi def link mixField         Identifier
hi def link mixMutVar        Identifier
hi def link mixMutMark       Special
hi def link mixBuiltin       Function

let b:current_syntax = "mix"

let &cpo = s:cpo_save
unlet s:cpo_save
