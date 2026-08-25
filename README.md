# pico2

The Raspberry Pi Pico 2 W for sysl — the board's own entry points, so that a program can be written
in sysl and hosted by the C SDK.

```sysl
import sh.sysl.pico2.*
import sysl.time.DurationUnits

@export("main")
run() -> int =
    if !init()
        print("the wireless chip did not start")
        return 1

    wait_for_terminal()
    print("hello from sysl, on an RP2350")

    loop
        led(true)
        sleep(120.ms)
        led(false)
        sleep(880.ms)
```

That is a whole program. There is no C in it, and no C in the project that builds it: sysl exports
`main` and the SDK's `crt0` calls it.

```hocon
dependencies {
  pico2 { git = "github.com/sysl-lang/pico2", version = "0.0.6" }
}
```

## The radio

The Wi-Fi surface is the larger half of this package. A program can join a network, scan for the ones
within earshot, read the address DHCP handed over, resolve a name, and fetch a document over http or
https:

```sysl
join("network", "password", Wpa2, 20.s) match
    Ok(_) ->
        ip() match
            Some(a) -> print(s"the board is $a")
            None -> ()

        fetch("https://api.ipify.org", 20.s) match
            Ok(r) -> print(s"the world sees ${r.body}")
            Err(e) -> print(s"could not fetch — $e")

    Err(e) -> print(s"could not join — $e")
```

**The https half needs the CMake project to have linked mbedtls**, which is the one place this
package's requirements reach past the SDK's Wi-Fi support. Without it `fetch` answers `Unsupported`
and a program that does not want TLS pays nothing for it. The certificate authorities are compiled
into this package, and there are deliberately only two — a host whose chain ends elsewhere is refused.

## What this package is, and what it is not

**It is nearly all declaration.** Most names in it are symbols the pico-sdk already has, so there is
little for a linker to be pointed at, which makes it different in kind from the org's other bindings.

**Four files are C, and each is a case the SDK cannot be reached from sysl directly.** `scan.c`,
because the driver reports results to a callback in its own background context; `net.c`, because lwIP
reads an address out of a `netif` with macros rather than functions; `dns.c` and `http.c`, for the
same callback reason as the scan. Each says at length why, and they are worth reading before adding a
fifth.

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
| `sleep_ms(u32)` **and** `sleep_us(u64)` | `sleep(d: Duration)` |
| `time_us_64() -> uint64_t` | `monotonic() -> Duration` |

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

**Waiting.** `sleep(d: Duration)`. One function where C has two, now that `sysl.time` can name a
length shorter than a second, and `sysl.time` writes one number-first: `sleep(120.ms)`.

**Reading the clock.** `monotonic() -> Duration` — how long the board has been running, straight off
the RP2350's timer. It is a `Duration` and not an `Instant`, which is the same distinction
`sysl.posix.time` draws under the same name: a counter from an origin nobody specifies is not a point
on the timeline, so one reading answers nothing and only the difference of two means anything.

    val t0 = monotonic()
    work()
    print(s"took ${whole_millis(monotonic() - t0)}ms")

The timer counts microseconds and a `Duration` is made of microseconds, so nothing is scaled on the
way through. The header promises it never wraps — 64 bits of microseconds is 584,000 years.

**Bytes and characters.** `read_byte()`/`write_byte()` and `read_char()`/`write_char()`. Both pairs
exist because they are different things: a `char` is a Unicode scalar and may take four bytes on the
wire, while echoing a half-typed line wants bytes, since a byte pulled out of one is not a character
yet. `read_char` decodes UTF-8 and answers **U+FFFD** for malformed input, so one bad byte cannot end
a session; `None` means the input ended.

**A line of text.** `sysl.term.edit` does this now, and `read_line` is gone. `console_in()` and
`console_out()` are the port as a `Reader` and a `Writer`, which is all an editor asks for.

## The editor moved into the standard library

`read_line` was a full line editor living in this package — some two hundred lines of insertion,
backspace, delete, arrow keys, Home and End, `Ctrl-A/E/B/F/U/K`, and the redraw arithmetic under all
of it. It was here for one reason: a USB CDC port has no line discipline, so nothing appears as it is
typed and a mistake cannot be corrected, and nothing in the standard library would supply one.

`sysl.term.edit` supplies one now, over a `*Reader` and a `*Writer`, so a program on this board and a
program at a desktop terminal are the same program:

```sysl
import sh.sysl.pico2.{init, wait_for_terminal, console_in, console_out}
import sysl.term.edit.editor

var input = console_in()
var output = console_out()
var ed = editor(&input, &output)

for line in ed
    print("you typed", line)
```

**Three things came back that could not be had here**, and each was a real defect rather than a
missing nicety:

- **Columns, not characters.** This package assumed one character was one column, so erasing a CJK
  character or an emoji left half of it on the screen. The library asks `sysl.text.char_columns`.
- **Both spellings of an arrow key.** This read only `ESC [ …`; a terminal in application cursor key
  mode sends `ESC O …`, and those arrived as stray letters in the line.
- **Tests.** Wired straight to `stdio_getchar`, the editor could be exercised only by a person typing
  at a cable — which is why the section below used to call the absence of tests this package's real
  weakness. Over a `Reader` it is ordinary code, and the redraw arithmetic is checked by the
  compiler's own suite against a byte script.

What stays here is what only a board can do: `read_byte`, `write_byte`, `read_char`, `write_char`,
and the two stream types over them. **Neither needs flushing**, which is the one place a board is
simpler than a host — the SDK's `putchar` puts bytes on the wire rather than into a buffer, so what
the editor echoes appears as it is typed. `sysl.posix.tty.tty_writer` exists because a hosted C
library does not behave that way.

**Needs sysl 0.0.38 or newer**, which is where `sysl.term.edit` ships.

## Using it

`CMakeLists.txt` runs the compiler and links what it writes:

```cmake
# The directories the SDK's headers are actually behind, read off the target CMake has already
# resolved -- `cyw43.h` includes `lwip/netif.h`, which is two link libraries away from where this
# package's C sits, so naming targets one at a time gets each one's headers and none of what they
# pull in.
set(SYSL_C_INCLUDES $<TARGET_PROPERTY:app,INCLUDE_DIRECTORIES>)

add_custom_command(
    OUTPUT ${SYSL_ARCHIVE} ${SYSL_ARCHIVE}.h
    COMMAND sysl build-c ${CMAKE_CURRENT_SOURCE_DIR}/app --target thumb-freestanding-softfp
            --include-path pico_sdk=${PICO_SDK_PATH}
            "$<$<BOOL:${SYSL_C_INCLUDES}>:--include-path;$<JOIN:${SYSL_C_INCLUDES},;--include-path;>>"
            -o ${SYSL_ARCHIVE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/app.sysl
    COMMAND_EXPAND_LISTS
    VERBATIM)

add_executable(app ${SYSL_ARCHIVE})
set_target_properties(app PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(app pico_stdlib pico_cyw43_arch_none ${SYSL_ARCHIVE})
```

`add_executable` takes the **archive** in its source list, which is what satisfies CMake's "a target
must have sources" rule with no C translation unit anywhere.

`sysl-lang/pico-scratch` is a worked example of exactly this — a blink program and a REPL.

### The two kinds of include path do different jobs, and neither substitutes for the other

**`--include-path pico_sdk=${PICO_SDK_PATH}` answers this package's declaration.** The four C files
here include ten headers none of them carries — `pico/cyw43_arch.h`, `pico/time.h`, `cyw43.h`, four
out of `lwip/` and two out of mbedTLS and PSA — and `package.hocon` says so:

```hocon
requires { headers { pico_sdk = "the pico-sdk's headers, and the lwIP and mbedTLS ones …" } }
```

One name rather than three, because all a consumer knows is a single `PICO_SDK_PATH`; how the SDK
arranges lwIP and mbedTLS underneath it is not something to make them restate. Get it wrong and the
build stops before clang runs, naming the package, the headers and the flag. Before the declaration
existed it stopped *inside* clang instead, which named a file and knew nothing about this package:

```
fatal error: 'pico/cyw43_arch.h' file not found
```

**The bare list is what actually finds them**, and there are eighty-odd entries in it, since the SDK
spreads its headers one directory per library. A bare `--include-path` deliberately does not answer a
declaration — the check asks what a build says it has rather than what it might happen to find — so
both are needed: the named one is the consumer saying *where the SDK is*, the bare list is CMake
saying *how it is laid out*.

### Two things there are load bearing

**`-softfp` on the target, and a stock SDK.** GNU ld refuses to merge objects whose float ABIs
disagree *whether or not any float crosses the boundary*, and the error names VFP register arguments
rather than anything you wrote. So somebody has to move, and it is sysl: `thumb-freestanding-softfp`
matches what the SDK does by default, and no `PICO_HARD_FLOAT_ABI` is set here at all.

This README said the opposite until 0.0.9, and told a consumer to set that switch before importing
the SDK — true while `thumbv8m.main-none-eabihf` was sysl's only Cortex-M33 target, and wrong since
sysl 0.0.35 added the softfp one. A language whose `@export` claim is that it fits into somebody
else's build is the side that follows.

**`@export`, and not merely a function named `main`.** A `build-c` has no entry point of its own, so
an export is the only reachability root — with nothing exported the module prunes to nothing, and the
compiler warns. It also publishes the undecorated symbol that `crt0` branches to.

## The LED is not on a GPIO

On a Pico 2 W the LED hangs off the CYW43439 wireless chip, so `gpio_put` cannot reach it and the
wireless driver has to be started even by a program that never touches the radio. That is what
`init()` does. A plain Pico 2 puts the LED on an ordinary RP2350 pin instead, which is why `led_pin`
is documented here as a *board* fact rather than a chip one.

## What is not here

**No tests, and it matters much less than it did.** Every function here ends in a call to a board, so
there is nothing a host could run and `sysl test .` would have nothing to report.

This used to be the package's real weakness, and the reason was named precisely: the UTF-8 boundary
walking and the redraw arithmetic were exactly the code that should be tested, and the only thing
stopping it was that the editor was wired directly to `stdio_getchar`. Parameterising it over a byte
source was called a design change rather than a chore — which is what happened, one directory over.
That editor is `sysl.term.edit` now, and the compiler's own suite runs it against a byte script.

What is left untested is what was always untestable off a board — a `sleep_ms`, a `gpio_put`, a scan
of the radio — and no amount of parameterising reaches it.

**No checking of the declarations against the SDK.** An `extern` whose signature disagrees with the C
one links perfectly and corrupts the call at run time. The signatures here were read out of
`pico/time.h`, `hardware/timer.h`, `pico/stdio.h`, `pico/stdio_usb.h` and `pico/cyw43_arch.h` rather
than remembered, and that is currently the whole of the assurance. A generated translation unit
taking the address of each function at the declared signature, compiled against the real headers,
would turn a mismatch into a compile error; it is not built.

**No registers, and no `static inline`.** Much of the SDK's hardware API is `static inline` — 45
functions in `hardware/gpio.h` alone — so it has no symbol to declare and would need a C shim.
Reaching the RP2350's registers directly is a different package and does not need the SDK at all.

**Nothing that listens.** The radio can be an access point, but nothing here hands out addresses or
accepts a connection: a phone will associate and then wait for a DHCP lease that is not coming. The
fetch is a client and there is no server side.

**No wall clock, so no certificate expiry check.** `monotonic()` counts from power-up and is not a
date: a TLS chain is verified in full — a forged certificate is refused — but a genuine expired one
is not caught, because the board still does not know what day it is. That wants an `Instant` from
somewhere, and the two candidates are SNTP over the radio and an RTC on the board. Neither is built.

## Licence

ISC.
