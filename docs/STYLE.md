# RocketNet code style

RocketNet's C and C++ follow one house style, set out in this guide. It covers
the layout of code inside a function as well as its formatting: the blank
lines that split a function into parts so that it is easy to read, and a
comment followed by a blank line before the code it introduces.

`.clang-format` (clang-format 18) handles indentation, braces, spaces and
pointer placement. It does not add or remove the blank lines that split a
function into parts, it does not move comments, and it does not change names.
Those rules are applied by hand, and they are the subject of this guide.

**Examples.** Examples are taken from RocketNet's own sources and quoted as
`sdk/<file>:<line>` followed by the exact text of that line.

**Names.** Functions, variables and macros used like calls use the
`rocketnet_` prefix, struct types use `rocketnet_*_t`, and constants use
`ROCKETNET_`.

**Enforced by.** Each rule ends with *clang-format* (and the key that does it)
or *by hand*. In the configuration in effect (`clang-format-18 --dump-config` with this repository's `.clang-format`), `MaxEmptyLinesToKeep: 4`,
`ReflowComments: false` and `InsertBraces: true` are set in `.clang-format`, and
`KeepEmptyLinesAtTheStartOfBlocks: true`, `SeparateDefinitionBlocks: Leave`
and `RemoveBracesLLVM: false` hold by default (inherited
from `BasedOnStyle: LLVM`, not written in the file). So clang-format keeps
whatever blank lines and comment lines a person writes (up to four
blank lines in a row), and braces every control statement body it reaches.

---

## 1. A function body is a sequence of parts separated by one blank line

A part is a run of lines that does one step: check the arguments, create one
thing and check it, fill a struct, send, clean up, return. One blank line
separates each part from the next. Several calls of the same kind (a group of
resets, a list of field assignments) form one part.

- Parts are short: most are a few lines, and many are a single line. A longer
  run is one braced block with no blank line inside it, or a list of like
  statements.
- A step is a single line when nothing else belongs to it.

```
sdk/rocketnet_address.cpp:275      const uint8_t a = uint8_t( address->data.ipv4[0] );
sdk/rocketnet_address.cpp:276      const uint8_t b = uint8_t( address->data.ipv4[1] );
sdk/rocketnet_address.cpp:277      const uint8_t c = uint8_t( address->data.ipv4[2] );
sdk/rocketnet_address.cpp:278      const uint8_t d = uint8_t( address->data.ipv4[3] );
sdk/rocketnet_address.cpp:279  
sdk/rocketnet_address.cpp:280      address->type = ROCKETNET_ADDRESS_IPV6;
sdk/rocketnet_address.cpp:281      address->data.ipv6[0] = 0x0000;
```

```
sdk/rocketnet_platform.cpp:4509      s->context = context;
sdk/rocketnet_platform.cpp:4510      s->send_failures = 0;
sdk/rocketnet_platform.cpp:4511      s->send_replay_failures = 0;
sdk/rocketnet_platform.cpp:4512      s->connected = false;
sdk/rocketnet_platform.cpp:4513  
sdk/rocketnet_platform.cpp:4514      rocketnet_assert( address );
sdk/rocketnet_platform.cpp:4515      rocketnet_assert( address->type != ROCKETNET_ADDRESS_NONE );
```

Never two blank lines in a row inside a function.

*By hand* (clang-format keeps up to four blank lines, `MaxEmptyLinesToKeep: 4`,
and never inserts one).

## 2. How a function opens and closes

- **No blank line after the opening brace**, in any block. The first line of
  a function is code.
- **No blank line before the closing brace.**
- **The opening checks are their own part.** A sentinel check, or a run of
  `rocketnet_assert` lines at the top, is followed by a blank line. In a body
  of six lines or fewer the checks may run straight into the code.

```
sdk/rocketnet_address.cpp:51      rocketnet_assert( address );
sdk/rocketnet_address.cpp:52      rocketnet_assert( address_string_in );
sdk/rocketnet_address.cpp:53  
sdk/rocketnet_address.cpp:54      if ( !address )
```

```
sdk/rocketnet_address.cpp:310  {
sdk/rocketnet_address.cpp:311      rocketnet_assert( address );
sdk/rocketnet_address.cpp:312      uint32_t ipv4;
sdk/rocketnet_address.cpp:313      memcpy( (char *) &ipv4, address->data.ipv4, 4 );
sdk/rocketnet_address.cpp:314      return ipv4;
sdk/rocketnet_address.cpp:315  }
```

- **The final return.** In a short body the return may sit on the line above
  it. In a longer body the return sits on the line above only when it
  completes the part above it: it hands back the value the line above just
  computed (`sdk/rocketnet_address.cpp:313-314`), ends a list of like calls,
  or follows the closing `}` of a loop that searched for the result. When the
  return hands back a result built by earlier parts, it is its own part:

```
sdk/rocketnet_address.cpp:44      port = (uint16_t) value;
sdk/rocketnet_address.cpp:45  
sdk/rocketnet_address.cpp:46      return true;
```

- **An early return sits on the line before it**, usually the
  `rocketnet_error` that reports the failure and the clean-up after it.

```
sdk/rocketnet_platform.cpp:4535          rocketnet_error( "failed to create socket: %s", rocketnet_platform_socket_error_text( rocketnet_platform_socket_error() ).text );
sdk/rocketnet_platform.cpp:4536          rocketnet_free( context, s );
sdk/rocketnet_platform.cpp:4537          return NULL;
```

*By hand* (`KeepEmptyLinesAtTheStartOfBlocks: true` keeps a blank line after
`{` rather than removing it, so remove it yourself).

## 3. A comment introduces its part, then a blank line

A comment then an extra newline: a `//` comment that starts a new part stands
on its own line, preceded by a blank line (or the opening brace) and followed
by one blank line, then the code it introduces.

- The comment names the step, short and lower case: "create socket", "bind
  to port", "set ipv6 sockets as dual stack".

```
sdk/rocketnet_platform.cpp:4516  
sdk/rocketnet_platform.cpp:4517      // create socket
sdk/rocketnet_platform.cpp:4518  
sdk/rocketnet_platform.cpp:4519      s->ipv6 = address->type == ROCKETNET_ADDRESS_IPV6;
```

```
sdk/rocketnet_platform.cpp:4588  
sdk/rocketnet_platform.cpp:4589      // bind to port
sdk/rocketnet_platform.cpp:4590  
sdk/rocketnet_platform.cpp:4591      if ( address->type == ROCKETNET_ADDRESS_IPV6 )
```

```
sdk/rocketnet_address.cpp:136      address->port = port;
sdk/rocketnet_address.cpp:137  
sdk/rocketnet_address.cpp:138      // inet_pton returns network order, matching the byte-oriented IPv4 storage
sdk/rocketnet_address.cpp:139  
sdk/rocketnet_address.cpp:140      memcpy( address->data.ipv4, &addr4, 4 );
```

### When the comment sits directly on its code

Three cases:

- **`IMPORTANT:` comments** explain why the next statement is the way it
  is, and sit on that statement.
- **A comment that labels a scoped `{ }` block** sits on the `{`.
- **A short label for a small branch body**, as the first line inside the
  branch. Both forms are accepted: the label followed by a blank line, or
  the label sitting on the code. Keep what the surrounding function does.

```
sdk/rocketnet_platform.cpp:4542  
sdk/rocketnet_platform.cpp:4543      // IMPORTANT: tell windows we don't want to receive any connection or network
sdk/rocketnet_platform.cpp:4544      // reset reports for this socket, otherwise a receive errors out after a remote endpoint
sdk/rocketnet_platform.cpp:4545      // disconnects hard and its ICMP port unreachable comes back
sdk/rocketnet_platform.cpp:4546      rocketnet_platform_socket_disable_udp_resets( (uintptr_t) s->handle );
```

```
sdk/rocketnet_server_transport.cpp:4044  
sdk/rocketnet_server_transport.cpp:4045      // setup next stats frame
sdk/rocketnet_server_transport.cpp:4046      {
```

```
sdk/rocketnet_endpoint.cpp:814      if ( type == ROCKETNET_RELIABILITY_UNRELIABLE )
sdk/rocketnet_endpoint.cpp:815      {
sdk/rocketnet_endpoint.cpp:816          // the SINGLE form: the message at the payload root, the form byte after it
sdk/rocketnet_endpoint.cpp:817  
sdk/rocketnet_endpoint.cpp:818          length = rocketnet_message_single_finish( payload, capacity, bytes );
```

```
sdk/rocketnet_channel.cpp:787      if ( !descriptor )
sdk/rocketnet_channel.cpp:788      {
sdk/rocketnet_channel.cpp:789          // the slot is live for another number (check 10, SLOT_COLLISION)
sdk/rocketnet_channel.cpp:790          rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION );
```

A comment does not follow code with no blank line between.

*By hand* (clang-format never inserts or removes blank lines around comments).

## 4. Comment style

- **`//` only, inside code.** No `/* */` comment appears inside a function
  body. `/* */` is used for the licence header at the top of each file, whose
  `*/` is followed by a blank line, and `/** */` for doc blocks in headers,
  which sit directly on the declaration they document.
- **Lower case, no full stop.** Comments start lower case; the exception is
  `IMPORTANT:`. Comments do not end with a full stop. Comments are phrases
  ("bind to port", "create socket"), not sentences; `IMPORTANT:` comments
  may run to full sentences.
- **One line, however long.** A comment inside a function is one line; only
  lists and multi-sentence `IMPORTANT:` notes run over more than one line.
  Comments are not wrapped at a column.
- **Comments on the end of a code line are rare**: short notes on one value
  or one condition (`sdk/rocketnet_base64.cpp:26`).
- **No comment directly above a function definition.** A top-level
  definition has a blank line on the line above.
- **A group comment names a group of definitions** and sits at column 0, then
  one blank line, then the first definition of the group (`// init`,
  `// threads`, `// time`, `// sockets`). A top-level separator has no
  comment above it.

```
sdk/rocketnet_platform.cpp:3974  // init
sdk/rocketnet_platform.cpp:3975  
sdk/rocketnet_platform.cpp:3976  bool rocketnet_platform_init()
```

```
sdk/rocketnet_platform.cpp:4381  // sockets
sdk/rocketnet_platform.cpp:4382  
sdk/rocketnet_platform.cpp:4383  bool rocketnet_platform_client_dual_stack()
```

- **`#endif` names its condition, repeating the directive that opened it**:
  `#endif // #if NAME` and `#endif // #ifdef NAME`. Copy the opening
  directive as written; do not turn `#ifndef` or `#ifdef` into `#if`. A
  header guard is the one exception: it closes with the guard's name alone,
  `#endif // NAME_H` (`#endif // ROCKETNET_SDK_ROCKETNET_ACK_H`).

Examples of the rule:

```
sdk/rocketnet_address.cpp:1  /*
sdk/rocketnet_address.cpp:2      RocketNet. Copyright 2026 Más Bandwidth LLC.  
sdk/rocketnet_address.cpp:3      Licensed under the GNU Affero General Public License v3.0
sdk/rocketnet_address.cpp:4  */
sdk/rocketnet_address.cpp:5  
```

```
sdk/rocketnet_layers.h:70  /** @brief Capacity in bytes of transport address strings, including the terminating NUL. */
sdk/rocketnet_layers.h:71  #define ROCKETNET_MAX_ADDRESS_STRING_LENGTH                256
```

```
sdk/rocketnet_base64.cpp:26          return -1; // integer overflow
```

```
sdk/rocketnet.cpp:38  #else
sdk/rocketnet.cpp:39  #error "asserts not supported on this platform!"
sdk/rocketnet.cpp:40  #endif // #if defined( _MSC_VER )
```

```
sdk/rocketnet_history.h:16  #ifndef ROCKETNET_HISTORY_PACKETS
sdk/rocketnet_history.h:17  #define ROCKETNET_HISTORY_PACKETS 4096
sdk/rocketnet_history.h:18  #endif // #ifndef ROCKETNET_HISTORY_PACKETS
```

```
sdk/rocketnet_platform.h:90  #ifdef __cplusplus
sdk/rocketnet_platform.h:91  #define ROCKETNET_DEFAULT_NULL = NULL
sdk/rocketnet_platform.h:92  #else // #ifdef __cplusplus
sdk/rocketnet_platform.h:93  #define ROCKETNET_DEFAULT_NULL
sdk/rocketnet_platform.h:94  #endif // #ifdef __cplusplus
```

*By hand* (`ReflowComments: false` keeps comment lines exactly as written;
`AlignTrailingComments: Leave` keeps trailing comments where they are).

## 5. Blank lines around declarations and around if, for and while

- **A declaration starts a part** and stays on the code that first uses it.
  A run of declarations (locals and struct fields) is preceded by a blank
  line or `{`. After the run comes either a blank line or the code that uses
  the declared names directly: a declaration, its first use and the check
  of that use are one part.

```
sdk/rocketnet_address.cpp:128  
sdk/rocketnet_address.cpp:129      uint32_t addr4;
sdk/rocketnet_address.cpp:130      if ( !rocketnet_platform_inet_pton4( host, &addr4 ) )
```

- **A braced `if`, `for`, `while` or `switch` block is its own part**, with
  a blank line (or the enclosing `{`) before it and a blank line (or the
  enclosing `}`) after it. The exception: when the block checks or consumes
  what the line above produced (it tests a name assigned on the line above,
  or follows the declaration of the variable the condition fills, as above),
  it sits on that line.

```
sdk/rocketnet_address.cpp:59      memset( address, 0, sizeof( rocketnet_address_t ) );
sdk/rocketnet_address.cpp:60  
sdk/rocketnet_address.cpp:61      if ( !address_string_in )
sdk/rocketnet_address.cpp:62      {
sdk/rocketnet_address.cpp:63          return false;
sdk/rocketnet_address.cpp:64      }
sdk/rocketnet_address.cpp:65  
sdk/rocketnet_address.cpp:66      char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
```

```
sdk/rocketnet_address.cpp:86          char * close = strchr( host, ']' );
sdk/rocketnet_address.cpp:87          if ( !close )
```

- **Braces on single-statement bodies.** Every `if`, `else`, `for`, `while`
  and `do` body is braced, including a body that is one `return`, `break` or
  `continue`.

```
sdk/rocketnet_address.cpp:23      if ( !*text )
sdk/rocketnet_address.cpp:24      {
sdk/rocketnet_address.cpp:25          return false;
sdk/rocketnet_address.cpp:26      }
```

*clang-format* for the braces (`InsertBraces: true`; control statements
inside a `#define` body and the other cases the header of `.clang-format`
lists are braced by hand), and Allman brace placement
(`BreakBeforeBraces: Allman`). *By hand* for the blank lines.

## 6. Between functions, and section separators

- **Exactly one blank line between functions**: a function end is followed
  by one blank line and the next definition.
- **Section separators** split a file into groups (one struct and its
  functions, one area of the code). At file level (column 0) a separator is
  a line of `// ` and dashes alone, with one blank line before and after it,
  and carries no title (the line after it is always blank). Its width is not
  fixed.
- **Titled banners inside a function.** A very long function is split into
  sections by a three-line banner: dashes, an UPPER CASE title, dashes as
  wide as the title, with one blank line before and after.

```
sdk/rocketnet_address.cpp:18      address->port = rocketnet_platform_ntohs( big_endian_port );
sdk/rocketnet_address.cpp:19  }
sdk/rocketnet_address.cpp:20  
sdk/rocketnet_address.cpp:21  static bool rocketnet_address_parse_port( const char * text, uint16_t & port )
```

```
sdk/rocketnet.cpp:23  #include <condition_variable>
sdk/rocketnet.cpp:24  
sdk/rocketnet.cpp:25  // -------------------------------------------------
sdk/rocketnet.cpp:26  
sdk/rocketnet.cpp:27  static void default_assert_function( const char * condition, const char * function, const char * file, int line )
```

```
sdk/rocketnet.cpp:55  }
sdk/rocketnet.cpp:56  
sdk/rocketnet.cpp:57  // -------------------------------------------------------------
sdk/rocketnet.cpp:58  
sdk/rocketnet.cpp:59  static std::atomic<bool> log_quiet{ false };
```

*By hand* (`SeparateDefinitionBlocks` is `Leave` by default and not set in
`.clang-format`; `Always` would insert the blank line between definitions).

## 7. Names

| What | Form |
|---|---|
| functions | `rocketnet_` + snake_case, `rocketnet_address_parse`; static helpers are snake_case too |
| struct types | `rocketnet_` + snake_case ending `_t`, `rocketnet_address_t` |
| packet structs | `RocketNet` + PascalCase + `Packet`, if such a type is declared |
| classes | PascalCase, methods PascalCase, members `m_` + camelCase, if such a type is declared |
| constants | `#define ROCKETNET_` + UPPER_SNAKE, `ROCKETNET_MAX_ADDRESS_STRING_LENGTH` |
| macros used like calls | `rocketnet_` + lower snake_case, `rocketnet_assert`, `rocketnet_check` |
| locals and fields | snake_case, `packet_bytes`, `address_string` |
| local `const` | snake_case, `const bool bracketed` |

Names spell out their unit: `packet_bytes`, `packet_data`, `num_packets`,
`current_time`; a count is `num_*`, a size in bytes is `*_bytes`.

Examples of the rule:

```
sdk/rocketnet_address.cpp:49  bool rocketnet_address_parse( rocketnet_address_t * address, const char * address_string_in )
```

```
sdk/rocketnet_layers.h:710  struct rocketnet_address_t
```

```
sdk/rocketnet_layers.h:71  #define ROCKETNET_MAX_ADDRESS_STRING_LENGTH                256
```

```
sdk/rocketnet_address.cpp:81      const bool bracketed = buffer[0] == '[';
```

*By hand* (clang-format does not touch names).

## 8. What clang-format already enforces

Not repeated above, and not to be fixed by hand: Allman braces
(`BreakBeforeBraces: Allman`), 4-space indent (`IndentWidth: 4`), spaces inside
call and control parentheses (`SpacesInParens: Custom`, `Other: true`,
`InConditionalStatements: true`), `char * p` (`PointerAlignment: Middle`),
`(int) value` casts (`SpaceAfterCStyleCast: true`), no line length limit
(`ColumnLimit: 0`), includes left in the order written (`SortIncludes: Never`),
indented case labels (`IndentCaseLabels: true`), braces on every control
statement body (`InsertBraces: true`). The header of `.clang-format` lists
what it cannot express.

---

## Checklist for one function

Read the function top to bottom and check each line:

1. No blank line after any `{` and none before any `}`.
2. The opening checks (a sentinel check, the run of asserts) are one part,
   followed by a blank line (bodies of six lines or fewer may run straight on).
3. Every step is its own part, one blank line between parts, never two.
   A part longer than about eight lines is either one braced block or a list
   of like statements; otherwise split it.
4. A declaration starts a part and sits directly on its first use.
5. Each braced `if`/`for`/`while`/`switch` has a blank line before it, unless
   it tests or consumes the line directly above, and a blank line after it,
   unless the enclosing block closes.
6. Every `if`, `else`, `for`, `while` and `do` body is braced, a single
   `return`, `break` or `continue` too.
7. An early return sits directly under the line that reports the failure.
8. The final return has a blank line before it, unless it returns the value
   the line directly above computed.
9. A comment that starts a part: blank line (or `{`) above, the comment, one
   blank line, then the code. Exceptions that sit on the code: `IMPORTANT:`
   comments, a label on a scoped `{ }` block.
10. Comments are `//`, lower case, no full stop, one line however long; no
    `/* */` inside code; no comment touching the function signature.
11. Every `#endif` names its condition with the directive that opened it:
    `#endif // #if NAME` or `#endif // #ifdef NAME`; a header guard closes
    with its name alone, `#endif // NAME_H`.
12. One blank line after the closing `}` before the next function; a file-level
    separator is an untitled `// ----` line with a blank line on each side; a
    section inside a long function opens with a titled three-line banner
    (`// ----`, `// UPPER CASE TITLE`, `// ----`) with a blank line on each side;
    a group comment (`// sockets`) sits at column 0, then a blank line, then the
    group's first definition.
13. Names: `rocketnet_` snake_case functions, `rocketnet_*_t` types,
    `ROCKETNET_` constants, snake_case locals that name their unit.
14. Run clang-format 18 last; it must not change the blank lines you placed.
