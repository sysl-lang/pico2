# pico2

The Raspberry Pi Pico 2 W for sysl — the board's own entry points, so that a program can be written
in sysl and hosted by the C SDK.

```sysl
import sh.sysl.pico2.*

@export("main")
run() -> int =
    if !init()
        print("the wireless chip did not start")
        return 1

    wait_for_terminal()
    print("hello from sysl, on an RP2350")

    loop
        led(true)
        sleep_ms(120)
        led(false)
        sleep_ms(880)
```

That is a whole program. There is no C in it, and no C in the project that builds it: sysl exports
`main` and the SDK's `crt0` calls it.

```hocon
dependencies {
  pico2 { git = "github.com/sysl-lang/pico2", version = "0.0.3" }
}
```

## What this package is, and what it is not

**It declares and does not implement.** Every name in it is a symbol the pico-sdk already has, so no
C is carried here and there is nothing for a linker to be pointed at. That makes it different in kind
from the org's other bindings, which vendor a library and compile it.

The consequence to understand before using it: **a program importing `pico2` must be built with
`sysl build-c` and linked by a CMake project that has the SDK.** A plain `sysl build` will compile it
happily and then fail to link, naming `sleep_ms`, and the fault will be the build rather than the
code.

**Why not bind the SDK properly?** Because the SDK is not a library. It is a build system that
generates a second-stage bootloader, runs `pioasm`, selects a board header and drives the final link
with its own linker script and image signing. None of that fits inside something `sysl build`
fetches. Turning the arrangement the other way up — CMake owns the link, sysl produces an archive —
costs one `add_custom_command` and gets the whole SDK, including USB and Wi-Fi.

## Two modules

| module | job |
|---|---|
| `sh.sysl.pico2.externs` | the SDK's surface, declared verbatim — C names, C conventions |
| `sh.sysl.pico2` | the same board as sysl, written in terms of the above |

The inner module has to be **faithful**: a signature that disagrees with the C header links perfectly
and corrupts the call at run time, so each declaration is grouped under the header it was read from.
The outer one has to be **pleasant**, which is a different question and would otherwise be answered
in the same breath. It also means both may use a name — `sleep_ms` is C's in one and sysl's in the
other, and neither has to be renamed to avoid the other.

What the outer module actually removes:

| C | sysl |
|---|---|
| `stdio_getchar() -> int`, `-1` for failure | `read_byte() -> Option[u8]` |
| `cyw43_arch_init() -> int`, non-zero for failure | folded into `init() -> bool` |
| `putchar(c: int) -> int` | `write_byte(b: u8)`, and `write_char(c: char)` |
| `cyw43_arch_gpio_put(wl_gpio, value)` | `led(on: bool)` |

A caller writing `if c < 0` is writing C; a caller matching on `None` is writing sysl.

The name is plural because **`extern` is a reserved word** and cannot be a module path segment.

## The surface

**Starting up.** `init()` brings up stdio and the wireless chip, answering whether the chip started —
a program that only wants the LED still has to check, because the LED is on that chip.
`terminal_connected()` and `wait_for_terminal()` are about the *host*: the SDK discards stdio output
while nothing has the USB serial port open, so a banner printed at startup is lost and the program
looks dead rather than early.

**The board.** `led(on)`, `led_is_on()`, `on_usb_power()`, and the three CYW43 pin numbers as
constants — `led_pin`, `smps_pin`, `vbus_pin`.

**Waiting.** `sleep_ms(ms)`, `sleep_us(us)`. C's two spellings, because `sysl.time` has no `millis`
or `micros` constructor for a `Duration` yet.

**Bytes and characters.** `read_byte()`/`write_byte()` and `read_char()`/`write_char()`. Both pairs
exist because they are different things: a `char` is a Unicode scalar and may take four bytes on the
wire, while echoing a half-typed line wants bytes, since a byte pulled out of one is not a character
yet. `read_char` decodes UTF-8 and answers **U+FFFD** for malformed input, so one bad byte cannot end
a session; `None` means the input ended.

**A line of text.** `read_line() -> Result[string, Utf8Error]`, echoed as it is typed.

## `read_line` is a line editor, and here is why it has to be

A serial terminal is neither a file nor a shell, and both gaps make a REPL look broken rather than
wrong.

**Line endings.** `sysl.io`'s line cursor splits on `\n`, which is right for a pipe. `screen` sends a
bare `\r` when Enter is pressed, so a program reading lines that way waits forever and prints
nothing. CR, LF and CRLF all end a line here.

**Echo and editing.** A USB CDC port has no line discipline, so nothing appears as it is typed and a
mistake cannot be corrected. Nothing else was going to do it:

| | |
|---|---|
| `←` `→` | move within the line |
| `Home` `End` | and `Ctrl-A` / `Ctrl-E` |
| `Backspace` `Delete` | at the cursor, not only at the end |
| `Ctrl-U` `Ctrl-K` | kill the line, or from the cursor on |
| `Ctrl-B` `Ctrl-F` | left and right, for readline hands |

The cursor moves by **characters** while the line is stored as **bytes** — `sysl.text.is_char_boundary`
makes that cheap, and it is the combination that keeps `from_utf8` at the end without needing a
char-to-bytes encoder, which the standard library does not have. So one backspace erases a whole
`é` rather than orphaning its lead byte.

**Still assumed: one character, one column.** A wide character — CJK, most emoji — takes two, so
erasing one would leave half behind. Fixing that means asking `sysl.text.columns` for a width and
counting columns.

## Using it

`CMakeLists.txt` runs the compiler and links what it writes:

```cmake
set(PICO_HARD_FLOAT_ABI 1)          # before the SDK is imported

add_custom_command(
    OUTPUT ${SYSL_ARCHIVE} ${SYSL_ARCHIVE}.h
    COMMAND sysl build-c ${CMAKE_CURRENT_SOURCE_DIR}/app --target thumb-freestanding --no-std-lib
            -o ${SYSL_ARCHIVE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/app.sysl
    VERBATIM)

add_executable(app ${SYSL_ARCHIVE})
set_target_properties(app PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(app pico_stdlib pico_cyw43_arch_none ${SYSL_ARCHIVE})
```

`add_executable` takes the **archive** in its source list, which is what satisfies CMake's "a target
must have sources" rule with no C translation unit anywhere.

`sysl-lang/pico-scratch` is a worked example of exactly this — a blink program and a REPL.

### Three things there are load bearing

**`PICO_HARD_FLOAT_ABI`, set before the SDK is imported.** sysl's only Cortex-M33 target is
`thumbv8m.main-none-eabihf`, which passes floating-point arguments in VFP registers; the SDK defaults
to `softfp`. GNU ld refuses to merge objects that disagree *whether or not any float crosses the
boundary*, and the error names VFP register arguments rather than anything you wrote.

**`--no-std-lib`.** It reads as "no standard library" and means "compile the standard module from its
source rather than linking a prebuilt artifact". Without it the archive refers to library code it
does not contain, and nothing a C linker can reach provides it.

**`@export`, and not merely a function named `main`.** A `build-c` has no entry point of its own, so
an export is the only reachability root — with nothing exported the module prunes to nothing, and the
compiler warns. It also publishes the undecorated symbol that `crt0` branches to.

## The LED is not on a GPIO

On a Pico 2 W the LED hangs off the CYW43439 wireless chip, so `gpio_put` cannot reach it and the
wireless driver has to be started even by a program that never touches the radio. That is what
`init()` does. A plain Pico 2 puts the LED on an ordinary RP2350 pin instead, which is why `led_pin`
is documented here as a *board* fact rather than a chip one.

## What is not here

**No tests.** Every function ends in a call to a board, so there is nothing a host could run and
`sysl test .` would have nothing to report. This is the package's real weakness rather than an
oversight: the UTF-8 boundary walking and the redraw arithmetic are exactly the code that should be
tested, and the only thing stopping it is that the editor is wired directly to `stdio_getchar`.
Parameterising it over a byte source would fix that, and is a design change rather than a chore.

**No checking of the declarations against the SDK.** An `extern` whose signature disagrees with the C
one links perfectly and corrupts the call at run time. The signatures here were read out of
`pico/time.h`, `pico/stdio.h`, `pico/stdio_usb.h` and `pico/cyw43_arch.h` rather than remembered, and
that is currently the whole of the assurance. A generated translation unit taking the address of each
function at the declared signature, compiled against the real headers, would turn a mismatch into a
compile error; it is not built.

**No registers, no Wi-Fi, no `static inline`.** Much of the SDK's hardware API is `static inline` —
45 functions in `hardware/gpio.h` alone — so it has no symbol to declare and would need a C shim.
Reaching the RP2350's registers directly is a different package and does not need the SDK at all.

## Licence

ISC.
