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

## Configuration

A port configures the build with `-DCONFIG_LUDLC_*`. The defaults, and the
type widths they select, are in `ludlc_proto.h`. Both ends of a link must
agree on the payload ceiling and the window, so those cannot be chosen freely.
