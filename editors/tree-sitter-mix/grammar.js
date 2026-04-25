/**
 * tree-sitter-mix v0.1
 *
 * MIX is indent-significant. A faithful tree-sitter grammar for MIX needs
 * an external scanner that emits INDENT / DEDENT tokens (see Python's
 * tree-sitter for the canonical pattern). This v0 punts on that and
 * instead recognizes lexical structure plus high-level declarations
 * loosely — enough to drive accurate syntax highlighting and basic
 * navigation in editors that use tree-sitter (Neovim, Helix, Zed). A
 * later v1 should add scanner.c and proper block grouping for
 * structural folding and text objects.
 */

module.exports = grammar({
  name: 'mix',

  // Conflicts and word are minimal — we keep the grammar permissive on
  // purpose so token-level coloring works even on partial input.
  word: $ => $.identifier,

  extras: $ => [
    /[ \t]+/,
    $.line_comment,
  ],

  rules: {
    source_file: $ => repeat(choice($._top_level, '\n')),

    // --- Top-level forms ------------------------------------------------

    _top_level: $ => choice(
      $.use_decl,
      $.use_c_decl,
      $.shape_decl,
      $.union_decl,
      $.const_decl,
      $.type_alias,
      $.extern_block,
      $.fn_decl,
      $.cond_decl,
    ),

    use_decl: $ => seq(
      'use',
      optional(field('alias', $.identifier)),
      field('path', $.dotted_path),
      optional(seq(':', commaSep1($.identifier))),
    ),

    use_c_decl: $ => seq(
      'use', 'c',
      field('header', $.string),
      optional(seq('link', field('lib', $.string))),
      optional(seq('source', field('source', $.string))),
    ),

    dotted_path: $ => seq($.identifier, repeat(seq('.', $.identifier))),

    shape_decl: $ => seq(
      optional('pub'),
      'shape',
      field('name', $.identifier),
      optional(seq('[', commaSep1($.identifier), ']')),
    ),

    union_decl: $ => seq(
      optional('pub'),
      'union',
      field('name', $.identifier),
    ),

    const_decl: $ => seq(
      optional('pub'),
      '@const',
      field('name', $.identifier),
      '=',
      field('value', $._expression),
    ),

    type_alias: $ => seq(
      optional('pub'),
      'type',
      field('name', $.identifier),
      '=',
      field('aliased', $._type),
    ),

    extern_block: $ => seq(
      'extern',
      field('lib', $.string),
    ),

    fn_decl: $ => prec.right(seq(
      optional('pub'),
      optional($.generic_params),
      field('name', $.identifier),
      $.param_list,
      optional(seq('->', field('return_type', $._type))),
      repeat(choice('~', '!')),
    )),

    generic_params: $ => seq(
      '@', $.identifier, repeat(seq(',', $.identifier)),
      optional(seq('has', commaSep1(choice($.identifier, $._operator_token)))),
    ),

    param_list: $ => seq(
      '(',
      optional(commaSep1($.param)),
      ')',
    ),

    param: $ => seq(
      field('name', choice($.identifier, $.identifier_mut)),
      optional(seq(':', field('type', $._type))),
    ),

    cond_decl: $ => seq(
      '@', choice('os', 'arch', 'debug', 'release'),
      optional(seq(choice('==', '!='), $.string)),
    ),

    // --- Statements -----------------------------------------------------

    _statement: $ => choice(
      $.var_decl,
      $.assign,
      $.if_stmt,
      $.while_stmt,
      $.for_stmt,
      $.match_stmt,
      $.done_stmt,
      $.fail_stmt,
      $.defer_stmt,
      $.zone_stmt,
      $.unsafe_block,
      'break',
      'continue',
      $._expression,
    ),

    var_decl: $ => seq(
      field('name', choice($.identifier, $.identifier_mut)),
      optional(seq(':', field('type', $._type))),
      '=',
      field('value', $._expression),
    ),

    assign: $ => seq(
      field('target', $.identifier),
      choice('=', '+=', '-=', '*=', '/='),
      field('value', $._expression),
    ),

    if_stmt: $ => seq('if', $._expression, optional(seq('else'))),
    while_stmt: $ => seq('while', $._expression),
    for_stmt: $ => seq(
      'for', $.identifier,
      optional(seq(',', $.identifier)),
      'in', $._expression,
    ),
    match_stmt: $ => seq('match', $._expression),
    done_stmt: $ => prec.right(seq('done', optional($._expression))),
    fail_stmt: $ => seq('fail', $._expression),
    defer_stmt: $ => seq('defer', $._statement),
    zone_stmt: $ => seq('zone', optional(seq(':', $.identifier))),
    unsafe_block: $ => 'unsafe',

    // --- Expressions ----------------------------------------------------

    _expression: $ => choice(
      $.integer,
      $.float,
      $.string,
      $.string_interp,
      $.bool,
      $.none_lit,
      $.identifier,
      $.identifier_mut,
      $.binary_expr,
      $.unary_expr,
      $.call_expr,
      $.method_call,
      $.field_access,
      $.index_expr,
      $.list_lit,
      $.map_lit,
      $.set_lit,
      $.shape_lit,
      $.try_expr,
      $.lambda,
      $.cast_expr,
      $.move_expr,
      $.alloc_hint,
      seq('(', $._expression, ')'),
    ),

    binary_expr: $ => choice(
      ...['+','-','*','/','%','==','!=','<','>','<=','>=','and','or','|','..','..='].map(op =>
        prec.left(seq(field('left', $._expression), op, field('right', $._expression)))
      ),
    ),

    unary_expr: $ => prec(10, seq(choice('-','*','&','not','!'), $._expression)),

    call_expr: $ => prec(11, seq(
      field('callee', $.identifier),
      optional($._type_args),
      $.argument_list,
    )),

    method_call: $ => prec(11, seq(
      field('object', $._expression),
      '.', field('method', choice($.identifier, $.identifier_mut)),
      $.argument_list,
    )),

    field_access: $ => prec(11, seq(
      field('object', $._expression),
      '.', field('field', $.identifier),
    )),

    index_expr: $ => prec(11, seq(
      field('object', $._expression),
      '[', $._expression, ']',
    )),

    argument_list: $ => seq('(', optional(commaSep1($._expression)), ')'),

    list_lit: $ => seq('[', optional(commaSep1($._expression)), ']'),
    set_lit: $ => seq('set', '{', optional(commaSep1($._expression)), '}'),
    map_lit: $ => seq('{', optional(commaSep1(seq($._expression, ':', $._expression))), '}'),

    shape_lit: $ => prec(12, seq(
      field('shape', $.identifier),
      optional($._type_args),
      '(',
      commaSep1(seq(field('field', $.identifier), ':', $._expression)),
      ')',
    )),

    try_expr: $ => prec(13, seq($._expression, '?')),
    lambda: $ => prec.right(seq($.identifier, '=>', $._expression)),
    cast_expr: $ => prec(11, seq(field('type', $._type_keyword), '(', $._expression, ')')),
    move_expr: $ => prec.left(seq($._expression, '->', $._expression)),
    alloc_hint: $ => seq(choice('stack', 'heap'), $._type, '(', optional($._expression), ')'),

    _type_args: $ => seq('[', commaSep1($._type), ']'),

    // --- Types ----------------------------------------------------------

    _type: $ => choice(
      $._type_keyword,
      $.identifier,
      $.pointer_type,
      $.list_type,
      $.set_type,
      $.optional_type,
      $.generic_type,
    ),

    _type_keyword: $ => choice(
      'int','float','bool','byte','str',
      'int8','int16','int32','int64',
      'uint8','uint16','uint32','uint64',
      'float32','float64',
    ),

    pointer_type: $ => seq('*', $._type),
    list_type: $ => seq('[', $._type, ']'),
    set_type: $ => seq('set', '[', $._type, ']'),
    optional_type: $ => prec(15, seq($._type, '?')),
    generic_type: $ => prec(14, seq($.identifier, '[', commaSep1($._type), ']')),

    // --- Lexical -------------------------------------------------------

    identifier: $ => /[a-zA-Z_][a-zA-Z0-9_]*/,
    identifier_mut: $ => /[a-zA-Z_][a-zA-Z0-9_]*!/,

    integer: $ => choice(
      /-?\d+/,
      /-?0x[0-9a-fA-F]+/,
      /-?0b[01]+/,
    ),
    float: $ => /-?\d+\.\d+([eE][-+]?\d+)?/,
    string: $ => /"([^"\\]|\\.)*"/,
    string_interp: $ => /"([^"\\]|\\.)*\{[^}]*\}([^"\\]|\\.)*"/,
    bool: $ => choice('true', 'false'),
    none_lit: $ => 'none',

    line_comment: $ => /\/\/[^\n]*/,

    _operator_token: $ => choice('+','-','*','/','%','==','!=','<','>','<=','>='),
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
