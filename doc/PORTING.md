<!--
SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-or-later)

Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors
-->

# Porting LuDLC

A port is a directory. The ones in this tree get no special treatment from
the build system, and a port kept outside this tree is built the same way.
Nothing below depends on where the directory is.

## What a port provides

Three headers, included by those names from the core:

| Header | Provides |
| --- | --- |
| `ludlc_platform.h` | Types, locks, atomics, timers; the inline hooks |
| `ludlc_platform_args.h` | The per-transport argument union |
| `ludlc_logger.h` | `LUDLC_LOG_*` |

`src/lib/include/ludlc_platform_api.h` declares the functions the core calls
and documents which of them a port may supply as an inline instead, by
defining the matching `LUDLC_PLATFORM_*_INLINED` macro. That header defines the
API; this file covers only the build.

A port also provides its own sources: the platform implementation and its
transports.

## A CMake port

Add a `CMakeLists.txt` that sets `PLATFORM_SRC` to the sources and adds the
port's own directory to the include path:

```cmake
target_include_directories(ludlc_includes INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}"
)

set(PLATFORM_SRC
	${CMAKE_CURRENT_SOURCE_DIR}/my_platform.c
	${CMAKE_CURRENT_SOURCE_DIR}/my_serial.c
	CACHE INTERNAL "my port sources"
)
```

`PLATFORM_DEFS`, if set, is added to the compile definitions. Everything else,
such as `find_package`, generated headers and link libraries, is up to the
port.

Build against it by naming the directory:

```sh
cmake -B build -DLUDLC_PLATFORM_DIR=/path/to/my_port
```

`TARGET_PLATFORM` then defaults to the directory's name, and
`TARGET_PLATFORM_<NAME>` is set in upper case so that a port can test for
itself. Pass `-DTARGET_PLATFORM=` as well to use a different name.

## A make port

Name the framings and the port's sources, then include `ludlc-port.mk`:

```make
LUDLC_DIR := ../../ludlc
LUDLC_PORT_DIR := $(CURDIR)/ludlc_port

CONFIG_LUDLC_FRAMING_HDLC := 1
CONFIG_LUDLC_FRAMING_COBS := 0
CONFIG_LUDLC_FRAMING_RAW  := 0

LUDLC_PORT_SRCS := \
	$(LUDLC_PORT_DIR)/my_platform.c \
	$(LUDLC_PORT_DIR)/my_serial.c

include $(LUDLC_DIR)/src/lib/ludlc-port.mk

SRCS += $(LUDLC_SRCS)
CFLAGS += $(LUDLC_INCLUDES)
```

`LUDLC_SRCS` is the core plus the port, and `LUDLC_INCLUDES` the three include
paths, the port's own first.

A port kept in another project compiles the core where the checkout is. Build
the port's sources as the project builds its own, and give the core sources an
object rule of their own, so that a generic one does not write objects into
the checkout:

```make
SRCS += $(LUDLC_PORT_SRCS)
LUDLC_OBJS := $(patsubst $(LUDLC_DIR)/%.c,$(O)/ludlc/%.o,$(LUDLC_CORE_SRCS))

$(O)/ludlc/%.o: $(LUDLC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $<
```

Set the framings before the include. A codec has no `#ifdef` on its own
config, so a framing is left out of the image by the include dropping its file
from `LUDLC_SRCS`. Pass the same values to the compiler as
`-DCONFIG_LUDLC_FRAMING_*`, which is what the headers read. At least one
framing must be on.

Take the core sources from `ludlc-port.mk` rather than list them. That keeps
the port building when the core gains or loses a file.

## Configuration

A port configures the build with `-DCONFIG_LUDLC_*`. The defaults, and the
type widths they select, are in `ludlc_proto.h` and `ludlc_framing.h`. Both
ends of a link must agree on the payload ceiling, the window and the framing,
so those cannot be chosen freely.

A port whose build runs one checksum can have the escaping framing compute it
inline. It sets `-DCONFIG_LUDLC_SINGLE_CSUM=1`, and its `ludlc_platform.h`
provides `LUDLC_PLATFORM_CSUM_OCTETS` and three hooks for each direction,
`ludlc_platform_csum_tx_init()`, `_tx()` and `_tx_final()` for transmit and
`ludlc_platform_csum_rx_init()`, `_rx()` and `_rx_ok()` for receive. Each
direction may use its own backend, a CRC unit on one side and software on the
other. `ludlc_private.h` describes the contract.
