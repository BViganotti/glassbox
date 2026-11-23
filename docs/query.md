# Query language

The query bar filters trace records with a small, total predicate language. It
is intentionally not SQL and has no side effects.

## Grammar

```
expr       := or
or         := and (("OR" | "||") and)*
and        := unary (("AND" | "&&") unary)*
unary      := ("NOT" | "!") unary | primary
primary    := "(" expr ")" | comparison | text
comparison := field (":" | "=" | "==" | "!=" | "<" | "<=" | ">" | ">="
                    | "~" | "!~") value
value      := number | duration | string | boolean | bare-word
text       := quoted-string | bare-word | number
```

Keywords are case-insensitive. `:` is shorthand for equality, which makes
`type:alloc` read naturally. `&&`/`||` are aliases for `AND`/`OR`. `~` is a
case-insensitive substring match; `!~` negates it.

Durations are accepted wherever a number is: `ns`, `us`, `ms`, `s`, `m` are
converted to nanoseconds. This is convenient for timestamps:
`ts > 250ms` means "after 250 milliseconds into the recording".

Strings can be quoted with `"` or `'` and support `\n`, `\t`, `\r`, `\\`, `\"`,
`\'`. Unquoted values may contain letters, digits, `.`, `/`, `\`, `_`, `$`,
`@`, `-`, `+`; a colon inside an unquoted value is allowed via a small
extension so `site = render.cpp:42` works.

## Fields

| Field | Meaning |
|---|---|
| `type` | record type name (`log`, `alloc`, `span_begin`, ...) |
| `ts`, `t` | timestamp in nanoseconds (use durations) |
| `thread`, `tid` | dense thread slot |
| `span` | innermost active span id on the record |
| `level` | log severity (0..5, only on log records) |
| `size`, `ptr`, `owner` | integer attributes of allocation records |
| `name`, `msg`, `site`, `class`, `category`, `frames`, ... | any field key stored on the record |

Field lookup is by exact key. Comparing a missing field is false, including
`!=` (a record without `size` does not satisfy `size != 0`).

`type` compares against the type name. String comparisons are case-insensitive
when testing equality or containment; ordering comparisons on strings are
lexicographic.

Bare text (not followed by an operator) searches the record type name and every
string field, case-insensitively. Numbers used as bare text search for their
literal spelling.

## Examples

```
type:alloc AND size > 1024
type:alloc AND site ~ "asset_loader"
type:log AND level >= 3
msg ~ "timeout" OR msg ~ "failed"
class = Texture AND ts > 1ms
thread = 2 AND NOT type:counter
(type:alloc OR type:free) AND size >= 4096
name ~ Frame AND span != 0
```

`run_query` and the `/api/query` endpoint evaluate the predicate over a time
range, returning at most `limit` record indices (clamped to 10,000). The UI
caps its request at 500 and reports truncation.

## Precedence

`NOT` binds tightest, then `AND`, then `OR`. Parentheses override. The parser
builds an immutable AST; `describe()` renders a normalized form with explicit
parentheses so the UI can show what was understood.

## Limits and errors

- Source length: 4,096 bytes.
- Tokens: 256.
- Nesting depth: 32.
- String literal: 512 bytes.
- No recursion at evaluation time beyond the AST depth; evaluation is
  iterative for AND/OR chains of arbitrary length.

Parsing never throws. Errors carry a byte offset and a message, surfaced by the
UI next to the query bar. Invalid queries match nothing. Unterminated strings,
dangling operators, unknown duration units, unmatched parentheses, excessive
nesting and oversized input are all rejected.

## Determinism

Results are ordered by record timestamp (stable emission order for equal
timestamps). `newest=1` reverses the scan but still returns newest first. The
same query on the same trace always returns the same sequence.
