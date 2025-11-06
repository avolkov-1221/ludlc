# LuDLC Protocol
Copyright © 2025 Andrey VOLKOV and LuDLC Contributors

---

## Overview

The **Lightweight Micro Devices Link Control (LuDLC)** is a reliable,
point-to-point communication protocol designed specifically for
resource-constrained devices.

## Key Features

- **Connection-Oriented**: Establishes a symmetrical, peer-to-peer connection via a 3-way handshake.
- **Reliable Delivery**: Guarantees in-order packet delivery using sequence numbers and acknowledgments (ACKs).
- **Error Recovery**: Uses Negative Acknowledgments (NAK) and re-transmissions with a Time-To-Live (TTL).
- **Flow Control**: Sliding window protocol prevents buffer overruns and improves throughput via cumulative acknowledgments.
- **Keep-Alives**: Ping/pong mechanism acts as a connection watchdog.
- **Multiplexing**: Supports multiple logical channels over a single link.
- **Flexible Transport**: Transport-agnostic design; compatible with UART, SPI, CAN, and other physical layers.

Please refer to [ludlc.md](doc/ludlc.md) to get all information related to LuDLC protocol

## Getting Started

### Prerequisites

- C/C++ compiler
- CMake 3.20 or later
- Git

### Building the Project

1. Clone the repository:

```bash
   git clone https://github.com/avolkov-1221/ludlc.git
   cd ludlc
```
2. Create a build directory and run CMake:
```bash
   mkdir build
   cd build
   cmake ..
```
3. Compile the project:
```bash
   cmake --build .
```

Running Examples:

Example applications demonstrating the protocol can be found in the `src/samples`
directory. After building, you can run them directly:
```bash
   ./src/samples/ludlc_demo
```

## Contact

See [OWNERS](OWNERS) or submit issues/pull requests via GitHub.
