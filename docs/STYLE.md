# RocketNet code style

RocketNet's C and C++ follow the owner's style as written in the Network Next
SDK. The owner asked for the code to "uniformly match my coding style in the
network next SDK", "including my formatting and whitespacing additions between
different parts of code in a function to make things easier to read, and my
comment then extra newline code style etc."

`.clang-format` (clang-format 18) handles indentation, braces, spaces and
pointer placement. It does not add or remove the blank lines that split a
function into parts, it does not move comments, and it does not change names.
Those rules are applied by hand, and they are the subject of this guide.

**Source.** Every rule and count below is taken from `networknext/next` at
`main`, commit `b4efefcad40c05d72f43ffa9f5b91b4472033f32`, files
`sdk/include/*.h` and `sdk/source/*.cpp` (65 files, 28394 lines, 801 function
bodies). Examples are quoted as `file:line` relative to `sdk/`. Counts come
from scripts that walk every function body; "N of M" means N cases follow
the rule out of M cases measured. Where the SDK is split, the guide says so
with the counts and does not pick a side the SDK does not pick.

**Names.** Where the SDK writes `next_`, `next_*_t` and `NEXT_`, RocketNet
writes `rocketnet_`, `rocketnet_*_t` and `ROCKETNET_`.

**Enforced by.** Each rule ends with *clang-format* (and the key that does it)
or *by hand*. In the `.clang-format` this repository uses,
`MaxEmptyLinesToKeep: 4`, `KeepEmptyLinesAtTheStartOfBlocks: true`,
`SeparateDefinitionBlocks: Leave`, `InsertBraces: false` and
`ReflowComments: false`, so clang-format keeps whatever blank lines, braces
and comment lines a person writes (up to four blank lines in a row).

---

## 1. A function body is a sequence of parts separated by one blank line

A part is a run of lines that does one step: check the arguments, create one
thing and check it, fill a struct, send, clean up, return. One blank line
separates each part from the next. Several calls of the same kind (a group of
resets, a list of field assignments) form one part.

- Part size, over 4259 runs of non-blank lines in function bodies: median 2
  lines, 75% are 5 lines or fewer, 90% are 8 or fewer. 1734 parts (41%) are a
  single line. Longer runs are almost all one braced block with no blank line
  inside it, or a list of like statements.
- A step is a single line when nothing else belongs to it: `next_client_internal_update`
  puts each of its seven update calls in its own part.

```
source/next_client.cpp:2478  next_client_t * client = (next_client_t*) next_malloc( context, sizeof(next_client_t) );
source/next_client.cpp:2479  if ( !client )
source/next_client.cpp:2480      return NULL;
source/next_client.cpp:2481
source/next_client.cpp:2482  memset( client, 0, sizeof( next_client_t) );
source/next_client.cpp:2483
source/next_client.cpp:2484  next_client_initialize_sentinels( client );
```

```
include/next_replay_protection.h:41  next_replay_protection_initialize_sentinels( replay_protection );
include/next_replay_protection.h:42
include/next_replay_protection.h:43  replay_protection->most_recent_sequence = 0;
include/next_replay_protection.h:44
include/next_replay_protection.h:45  memset( replay_protection->received_packet, 0xFF, sizeof( replay_protection->received_packet ) );
include/next_replay_protection.h:46
include/next_replay_protection.h:47  next_replay_protection_verify_sentinels( replay_protection );
```

```
source/next_client.cpp:2356  next_client_internal_update_direct_pings( client );
source/next_client.cpp:2357
source/next_client.cpp:2358  next_client_internal_update_next_pings( client );
```

Never two blank lines in a row inside a function: 0 in 801 bodies.

*By hand* (clang-format keeps up to four blank lines, `MaxEmptyLinesToKeep: 4`,
and never inserts one).

## 2. How a function opens and closes

- **No blank line after the opening brace**, in any block: 0 of 2497 `{`
  lines are followed by a blank line. The first line of a function is code
  (797 of 797 non-empty bodies).
- **No blank line before the closing brace**: 2 of 2494 `}` lines follow a
  blank line (both in console platform stubs).
- **The opening checks are their own part.** A `verify_sentinels` call is
  followed by a blank line in 94 of 102 functions that open with one. A run
  of `next_assert` lines at the top is followed by a blank line in 107 of 149
  bodies longer than six lines; in bodies of six lines or fewer it runs
  straight into the code in 83 of 86 (the sentinel helpers).

```
source/next_server.cpp:4511  next_server_verify_sentinels( server );
source/next_server.cpp:4512
source/next_server.cpp:4513  next_assert( server->internal );
source/next_server.cpp:4514
source/next_server.cpp:4515  next_proxy_session_entry_t * pending_entry = next_proxy_session_manager_find( server->pending_session_manager, address );
```

```
source/next_client.cpp:2519  {
source/next_client.cpp:2520      next_client_verify_sentinels( client );
source/next_client.cpp:2521
source/next_client.cpp:2522      return client->bound_port;
source/next_client.cpp:2523  }
```

- **The final return.** Of 364 functions that end in a `return` at body
  level, 125 have only that line, 120 put a blank line before it, and 119
  do not. In bodies of four lines or fewer the return is tight 44 times. In
  longer bodies the SDK is split, 120 blank to 75 tight; the tight cases are
  a return that completes the part above it: it hands back the value the
  line above just computed (`source/next_packets.cpp:35-38`), ends a list of
  `serialize_*` calls (13), or follows a closing `}` of a loop that searched
  for the result (17). When the return hands back a result built by earlier
  parts, it is its own part:

```
source/next_client.cpp:2513  next_client_verify_sentinels( client );
source/next_client.cpp:2514
source/next_client.cpp:2515  return client;
```

```
source/next_packets.cpp:35  int packet_length = p - packet_data;
source/next_packets.cpp:36  next_generate_pittle( a, from_address, to_address, packet_length );
source/next_packets.cpp:37  next_generate_chonkle( b, magic, from_address, to_address, packet_length );
source/next_packets.cpp:38  return packet_length;
```

- **An early return sits on the line before it**: 612 of 685 returns inside
  a nested block follow the line before them directly (usually the
  `next_printf` that reports the failure); 38 are the first line of their
  block; 35 have a blank line before.

*By hand* (`KeepEmptyLinesAtTheStartOfBlocks: true` keeps a blank line after
`{` rather than removing it, so remove it yourself).

## 3. A comment introduces its part, then a blank line

The owner's "comment then extra newline": a `//` comment that starts a new
part stands on its own line, preceded by a blank line (or the opening brace)
and followed by one blank line, then the code it introduces.

- Plain comments in SDK code (not tests) that start a part: 156 of 187 are
  followed by a blank line. Preceded by a blank line, 131 of 144 are
  followed by a blank line.
- The comment names the step, short and lower case: "create socket", "bind
  to port", "send upgrade session command to internal server".

```
source/next_address.cpp:26  memset( address, 0, sizeof( next_address_t ) );
source/next_address.cpp:27
source/next_address.cpp:28  // first try to parse the string as an IPv6 address:
source/next_address.cpp:29  // 1. if the first character is '[' then it's probably an ipv6 in form "[addr6]:portnum"
source/next_address.cpp:30  // 2. otherwise try to parse as an IPv6 address using inet_pton
source/next_address.cpp:31
source/next_address.cpp:32  char buffer[NEXT_MAX_ADDRESS_STRING_LENGTH + NEXT_ADDRESS_BUFFER_SAFETY*2];
```

```
source/next_platform_linux.cpp:188
source/next_platform_linux.cpp:189  // create socket
source/next_platform_linux.cpp:190
source/next_platform_linux.cpp:191  socket->type = socket_type;
```

```
source/next_server.cpp:4463  // send upgrade session command to internal server
source/next_server.cpp:4464
source/next_server.cpp:4465  next_server_command_upgrade_session_t * command = (next_server_command_upgrade_session_t*) next_malloc( ... );
```

### When the comment sits directly on its code

Three cases, all counted:

- **`IMPORTANT:` comments** explain why the next statement is the way it
  is, and sit on that statement: 30 of 33. (`source/next_client.cpp:776` is
  one of the two with a blank line after, a three-line explanation.)
- **A comment that labels a scoped `{ }` block** sits on the `{`: 7 in SDK
  code, 27 in `next_tests.cpp`, where the sub-cases of a test are labelled
  blocks (`source/next_tests.cpp:121-122`).
- **A short label for a small branch body**, as the first line inside the
  branch. Here the SDK is split: of 52 comments that are the first line of a
  block, 25 are followed by a blank line, 18 sit on the code, 9 are alone in
  an empty branch. Either is in style; keep what the surrounding function
  does, and in new code use the majority, a blank line after.

```
source/next_client.cpp:424
source/next_client.cpp:425  // IMPORTANT: for many platforms it's best practice to bind to ipv6 and go dual stack on the client
source/next_client.cpp:426  if ( next_platform_client_dual_stack() )
```

```
source/next_client.cpp:632
source/next_client.cpp:633  // run packet filters
source/next_client.cpp:634  {
```

```
source/next_platform_linux.cpp:302  {
source/next_platform_linux.cpp:303      // non-blocking
source/next_platform_linux.cpp:304      if ( fcntl( socket->handle, F_SETFL, O_NONBLOCK, 1 ) == -1 )
```

```
source/next_server.cpp:4706  {
source/next_server.cpp:4707      // passthrough packet
source/next_server.cpp:4708
source/next_server.cpp:4709      next_server_send_packet_direct( server, to_address, packet_data, packet_bytes );
```

A comment follows code with no blank line between only 5 times in SDK code.

*By hand* (clang-format never inserts or removes blank lines around comments).

## 4. Comment style

- **`//` only, inside code.** No `/* */` comment appears inside a function
  body. `/* */` is used for the licence header at the top of each file, and
  `/** */` for the doc blocks of the serialization classes
  (`include/next_bitpacker.h`, `next_stream.h`, `next_serialize.h`, 71
  blocks). Every one of the 136 `*/` closers is followed by a blank line.
- **Lower case, no full stop.** 263 of 298 comments start lower case; of the
  35 that start upper case, 33 are `IMPORTANT:`. 275 of 305 do not end with
  a full stop. Comments are phrases ("bind to port", "no port number"), not
  sentences; `IMPORTANT:` comments may run to full sentences
  (`source/next_client.cpp:776-778`).
- **One line, however long.** Comment lines have a median of 37 characters,
  90% are 102 or fewer, the longest is 159; only 24 of the 305 comments
  inside functions run over more than one line, and those are lists or
  multi-sentence `IMPORTANT:` notes. Comments are not wrapped at a column.
- **Comments on the end of a code line are rare**: 21 in function bodies,
  short notes on one value or one condition (`include/next_bitpacker.h:37`,
  `source/next_base64.cpp:26`).
- **No comment directly above a function definition.** All 691 top-level
  definitions have a blank line on the line above. A comment about a group
  of functions goes above a section separator (rule 6), with a blank line
  after it, the same comment-then-blank-line form as rule 3.
- **`#endif` names its condition, repeating the directive that opened it**:
  302 of 346 `#endif` lines. Of those, 249 read `#endif // #if NAME`, 44 read
  `#endif // #ifndef NAME_H` (header guards), and 9 read
  `#endif // #ifdef NAME`. 4 name the condition another way
  (`#endif // __cplusplus`, `include/next_platform.h:147`) and 40 are bare.
  Copy the opening directive as written; do not turn `#ifndef` or `#ifdef`
  into `#if`.

Examples of the rule:

```
source/next_platform_linux.cpp:231
source/next_platform_linux.cpp:232      // bind to port
source/next_platform_linux.cpp:233
```

```
source/next_base64.cpp:26          return -1; // integer overflow
```

```
source/next_client.cpp:374  #if !NEXT_DEVELOPMENT
source/next_client.cpp:375      next_printf( NEXT_LOG_LEVEL_INFO, "client sdk version is %s", NEXT_VERSION_FULL );
source/next_client.cpp:376  #endif // #if !NEXT_DEVELOPMENT
```

```
include/next_platform_gdk.h:52  #endif // #ifdef _GAMING_XBOX
include/next_platform_gdk.h:53
include/next_platform_gdk.h:54  #endif // #ifndef NEXT_PLATFORM_GDK_H
```

*By hand* (`ReflowComments: false` keeps comment lines exactly as written;
`AlignTrailingComments: Leave` keeps trailing comments where they are).

## 5. Blank lines around declarations and around if, for and while

- **A declaration starts a part** and stays on the code that first uses it.
  A run of declarations (locals and struct fields) is preceded by a blank
  line or `{` in 1327 of 1559 runs. After the run, the next line is blank in 616, and the code that
  uses the declared names directly in 922: a declaration, its first use and
  the check of that use are one part.

```
source/next_address.cpp:96
source/next_address.cpp:97  uint32_t addr4;
source/next_address.cpp:98  if ( next_platform_inet_pton4( address_string, &addr4 ) == NEXT_OK )
```

- **A braced `if`, `for`, `while` or `switch` block is its own part.**
  Before it: a blank line 753 times, the enclosing `{` 162, a comment 11, a
  closing `}` 23, other code 538. The 538 are the exception the rule makes:
  the block checks or consumes what the line above produced (298 of them
  test a name assigned on the line above; most of the rest follow the
  declaration of the variable the condition fills, as above). After a
  braced block: a blank line 681 times, the enclosing `}` 232, code 173.

```
source/next_server.cpp:4515  next_proxy_session_entry_t * pending_entry = next_proxy_session_manager_find( server->pending_session_manager, address );
source/next_server.cpp:4516  if ( pending_entry != NULL )
source/next_server.cpp:4517      return true;
source/next_server.cpp:4518
source/next_server.cpp:4519  next_proxy_session_entry_t * entry = next_proxy_session_manager_find( server->session_manager, address );
```

```
source/next_server.cpp:4625      }
source/next_server.cpp:4626
source/next_server.cpp:4627      if ( multipath )
source/next_server.cpp:4628      {
source/next_server.cpp:4629          send_upgraded_direct = true;
source/next_server.cpp:4630      }
source/next_server.cpp:4631
```

- **Braces on single-statement bodies.** A body that is one `return`,
  `break` or `continue` goes without braces 211 of 248 times
  (`source/next_server.cpp:4516-4517`); a body that is one other statement
  is braced 191 of 216 times (`source/next_server.cpp:4627-4630`). The
  braced single returns (37) exist, e.g. `include/next_replay_protection.h:67-70`.

*By hand* (`InsertBraces: false`, `RemoveBracesLLVM: false`; Allman brace
placement itself is clang-format, `BreakBeforeBraces: Allman`).

## 6. Between functions, and section separators

- **Exactly one blank line between functions**: 559 of 559 function ends
  are followed by one blank line and the next definition.
- **Section separators** split a file into groups (commands, notifications,
  one struct and its functions): a line of `// ` and dashes alone, with one
  blank line before and after it, 116 of 116. A separator carries no title
  (the line after it is always blank). Its width is not fixed: lines of 40
  characters (28 lines), 105 (23), 54 (19), 66 (18), others.

```
source/next_client.cpp:2515      return client;
source/next_client.cpp:2516  }
source/next_client.cpp:2517
source/next_client.cpp:2518  uint16_t next_client_port( next_client_t * client )
```

```
source/next_client.cpp:25  #include <stdlib.h>
source/next_client.cpp:26
source/next_client.cpp:27  // ---------------------------------------------------------------
source/next_client.cpp:28
source/next_client.cpp:29  #define NEXT_CLIENT_COMMAND_OPEN_SESSION            0
```

```
source/next.cpp:95  }
source/next.cpp:96
source/next.cpp:97  // -------------------------------------------------------------
source/next.cpp:98
source/next.cpp:99  static int log_quiet = 0;
```

*By hand* (`SeparateDefinitionBlocks: Leave`; `Always` would insert the blank
line between definitions but is not set in this repository).

## 7. Names

| What | Next SDK form | Count | RocketNet |
|---|---|---|---|
| functions | `next_` + snake_case, `next_client_internal_send_packet_to_server` | 580; plus 93 `test_*` and 17 static helpers, all snake_case | `rocketnet_*` |
| struct types | snake_case ending `_t`, `next_address_t` | 87, plus 16 typedefs | `rocketnet_*_t` |
| packet structs | `Next` + PascalCase + `Packet`, `NextUpgradeRequestPacket` | 20 | the same form with `RocketNet`, if such a type is declared |
| classes (serialization only) | PascalCase, `BitWriter`, methods `WriteBits`, members `m_numBits` | 8 classes, 31 methods, 15 members | the same, inside the serialization classes only |
| constants | `#define NEXT_` + UPPER_SNAKE, `NEXT_MAX_PACKET_BYTES` | 351 macros | `ROCKETNET_*` |
| macros used like calls | lower snake_case, `next_assert`, `next_check`, `serialize_int` | 23 (incl. 3 libc aliases) | `rocketnet_*` |
| locals and fields | snake_case, `packet_bytes`, `from_address_data` | 2545 of 2580 locals; 21 camelCase inside the serialization classes | snake_case |
| local `const` | snake_case, `const int packet_id` | 199 of 219; 4 UPPER, 16 camelCase in classes | snake_case |

Names spell out their unit: `packet_bytes`, `packet_data`, `num_entries`,
`packet_receive_time`, `delta_time`; a count is `num_*`, a size in bytes is
`*_bytes` (observed throughout, not counted).

Examples of the rule:

```
source/next_address.cpp:15  int next_address_parse( next_address_t * address, const char * address_string_in )
```

```
include/next_address.h:15  struct next_address_t
```

```
include/next.h:17  #define NEXT_ADVANCED_PACKET_FILTER                               0
```

```
include/next_packets.h:61  struct NextUpgradeRequestPacket
```

```
source/next_client.cpp:624      const int packet_id = packet_data[0];
```

*By hand* (clang-format does not touch names).

## 8. What clang-format already enforces

Not repeated above, and not to be fixed by hand: Allman braces
(`BreakBeforeBraces: Allman`), 4-space indent (`IndentWidth: 4`), spaces inside
call and control parentheses (`SpacesInParens: Custom`, `Other: true`,
`InConditionalStatements: true`), `char * p` (`PointerAlignment: Middle`),
`(int) value` casts (`SpaceAfterCStyleCast: true`), no line length limit
(`ColumnLimit: 0`), includes left in the order written (`SortIncludes: Never`),
indented case labels (`IndentCaseLabels: true`). The header of `.clang-format`
lists what it cannot express.

---

## Checklist for one function

Read the function top to bottom and check each line:

1. No blank line after any `{` and none before any `}`.
2. The opening `verify_sentinels` call and the run of asserts are one part,
   followed by a blank line (bodies of six lines or fewer may run straight on).
3. Every step is its own part, one blank line between parts, never two.
   A part longer than about eight lines is either one braced block or a list
   of like statements; otherwise split it.
4. A declaration starts a part and sits directly on its first use.
5. Each braced `if`/`for`/`while`/`switch` has a blank line before it, unless
   it tests or consumes the line directly above, and a blank line after it,
   unless the enclosing block closes.
6. A single `return`, `break` or `continue` body has no braces; a single other
   statement is braced.
7. An early return sits directly under the line that reports the failure.
8. The final return has a blank line before it, unless it returns the value
   the line directly above computed.
9. A comment that starts a part: blank line (or `{`) above, the comment, one
   blank line, then the code. Exceptions that sit on the code: `IMPORTANT:`
   comments, a label on a scoped `{ }` block.
10. Comments are `//`, lower case, no full stop, one line however long; no
    `/* */` inside code; no comment touching the function signature.
11. Every `#endif` names its condition with the directive that opened it:
    `#endif // #if NAME`, `#endif // #ifdef NAME` or `#endif // #ifndef NAME_H`.
12. One blank line after the closing `}` before the next function; a section
    separator is an untitled `// ----` line with a blank line on each side.
13. Names: `rocketnet_` snake_case functions, `rocketnet_*_t` types,
    `ROCKETNET_` constants, snake_case locals that name their unit.
14. Run clang-format 18 last; it must not change the blank lines you placed.
