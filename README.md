# pico2

The Raspberry Pi Pico 2 W for sysl — the board's own entry points, declared, so that a program can
be written in sysl and hosted by the C SDK.

```sysl
import sh.sysl.pico2.*

@export("main")
run() -> int =
    if !init()
        print("the wireless chip did not start")
        return 1

    print("hello from sysl, on an RP2350")

    loop
        led(true)
        sleep_ms(120)
        led(false)
        sleep_ms(880)
```

That is a whole program. There is no C in it, and there is no C in the project that builds it.

## What this package is, and what it is not

**It declares and does not implement.** Every name in it is a symbol the pico-sdk already has, so no
C is carried here and there is nothing for a linker to be pointed at. That makes it different in kind
from the org's other bindings, which vendor a library and compile it.

The consequence is the thing to understand before using it: **a program importing `pico2` must be
built with `sysl build-c` and linked by a CMake project that has the SDK.** A plain `sysl build` will
compile it happily and then fail to link, naming `sleep_ms`, and the fault will be the build rather
than the code.

**Why not bind the SDK properly?** Because the SDK is not a library. It is a build system that
generates a second-stage bootloader, runs `pioasm`, selects a board header and drives the final link
with its own linker script and image signing. None of that fits inside something `sysl build` fetches
and compiles. Turning the arrangement the other way up — CMake owns the link, sysl produces an
archive — costs one `add_custom_command` and gets the whole SDK, including USB and Wi-Fi.

## Using it

The project's `CMakeLists.txt` runs the compiler and links what it writes:

```cmake
set(PICO_HARD_FLOAT_ABI 1)          # before the SDK is imported

add_custom_command(
    OUTPUT ${SYSL_ARCHIVE} ${SYSL_ARCHIVE}.h
    COMMAND sysl build-c ${CMAKE_CURRENT_SOURCE_DIR}/app --target thumb-freestanding --no-std-lib
            --lib /path/to/pico2 -o ${SYSL_ARCHIVE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/app.sysl
    VERBATIM)

add_executable(app ${SYSL_ARCHIVE})
set_target_properties(app PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(app pico_stdlib pico_cyw43_arch_none ${SYSL_ARCHIVE})
```

`sysl-lang/pico` is a worked example of exactly this.

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
wireless driver has to be started even by a program that never touches the radio. `init()` does that,
and `led()` writes to it. A plain Pico 2 puts the LED on an ordinary RP2350 pin instead, which is why
`led_pin` is documented here as a *board* fact rather than a chip one.

## What is not here

**No tests.** Every function ends in a call to a board, so there is nothing a host could run and
`sysl test .` would have nothing to report.

**No checking of the declarations against the SDK.** An `extern` whose signature disagrees with the C
one links perfectly and corrupts the call at run time. The signatures here were read out of
`pico/time.h`, `pico/stdio.h` and `pico/cyw43_arch.h` rather than remembered, and that is currently
the whole of the assurance. A generated translation unit that takes the address of each function at
the declared signature, compiled against the real headers, would turn a mismatch into a compile
error; it is not built.

**No registers, no Wi-Fi, no `static inline`.** Much of the SDK's hardware API is `static inline` —
45 functions in `hardware/gpio.h` alone — so it has no symbol to declare and would need a C shim.
Reaching the RP2350's registers directly is a different package and does not need the SDK at all.

## Licence

ISC.
