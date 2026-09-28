<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors -->
# LuDLC Linux platform

This directory holds the Linux home of the LuDLC core and the transports for
it. It builds three modules.

## Modules

- `ludlc_core` — the protocol and everything Linux-specific it needs
  - `../../ludlc.o`: the transport-agnostic protocol core
  - `ludlc_bus.o`: the LuDLC software bus, the platform API `ludlc.c` calls,
    being locks, timers, TX requests and the helpers a transport uses for a
    system transition, and the module entry point
  - `ludlc_csum.o`: the checksums a link can run
  - `ludlc_cfg.o`: configfs front-end for describing bus devices, built only
    where the kernel provides configfs
  - `ludlc_cfg_port.o`: the configfs port directory every transport shares,
    being what a link is made of rather than what carries it
- `ludlc_serdev` — serial transport built on `serdev`
  - `ludlc_serdev_drv.o`: the driver, matching `ludlc,serdev` from a DT node
    or from ACPI through the generic `PRP0001` device
  - `ludlc_serdev_cfg.o`: the UART's own configfs attributes under
    `/sys/kernel/config/ludlc-serdev`, built only where the kernel provides
    configfs
- `ludlc_spi_master` — transport for a peer on an SPI bus this system drives
  - `ludlc_spi_master_drv.o`: the driver, matching `ludlc,spi`
  - `ludlc_spi_cfg.o`: the exchange's own configfs attributes under
    `/sys/kernel/config/ludlc-spi`, built the same way

## Device model

The bus is named `ludlc` and carries a link's logical channels as devices
rather than one device per link. `CONFIG_LUDLC_CONTROL_CHANNEL` belongs to the
protocol and is never a device. Drivers match on compatible strings, wherever
they come from: a DT node, an ACPI one, or a configfs software node.

A channel belongs to one device. That device usually has just the one, named
by its `reg` property, and several where one driver has to speak for a group of
them, none of which it could own alone; firmware then lists them all:

```
	coproc@2 {
		compatible = "vendor,coproc";
		reg = <2 4 7 0x19>;
	};
```

The device is named after the first, and `receive()` and `confirm()` say which
channel a packet belongs to, so a driver owning one can ignore it. A configfs
node takes the same list, comma-separated: `echo 2,4,7,0x19 > reg`.

A driver receives its traffic from `receive()` and `confirm()` once `probe()`
returns. A `probe()` that needs a reply from the peer before it can finish
calls `ludlc_device_listen()` to start dispatch early, and
`ludlc_device_unlisten()` if it then has to fail. The bus does the same either
side of `remove()`, so a driver is never called into after it has gone.

## Data path

- RX: the decoder feeds whole packets to `ludlc_receive()`.
- TX: `ludlc_get_packet_to_send()` hands over one frame at a time, driven by
  `write_wakeup` and a workqueue rather than a TX kthread.
- A negative return means the core has nothing to send and the session is
  over, which is how a link reports that its goodbye has just left. The
  transport sends nothing after it.

## Leaving the link

On suspend the link says goodbye rather than leaving the peer to time out.
`ludlc_conn_send_disconnect()` sends `LUDLC_CTRL_DISCONNECT` as an unnumbered
packet, so it goes out ahead of anything queued and needs no room in the
window, and the transport then runs the TX round and gives the port a bounded
time to take it. A link carrying a wakeup-capable device is left alone, that
device and the peer having their own arrangement.

The core takes the link down itself once the frame has drained, so the
transport does not inspect the frames it sends to work out when a session has
ended.

An unbind leaves the peer to its watchdog, which is all it would have had
before the link existed.

## System transitions

`prepare()` and `complete()` bracket the whole transition and stop the bus
from following its link, so that no device is added or removed while the
driver core walks them. `suspend()` and `resume()` bracket the suspend itself,
stopping the link timers on the way in so that nothing fires into a suspended
port, and restarting the link on the way out.

## Kernel-style choices

- Uses the in-kernel CRC helpers (`crc_ccitt_byte`, `crc16_byte`, `crc32_le`)
  instead of local lookup tables.
- Reuses kernel timer, workqueue, and device/bus APIs instead of custom logic.
- Keeps policy minimal in transport; protocol state machine remains in
  `ludlc.c`.

## Integration notes

- Integrate this directory's `Kconfig` and `Makefile` into your kernel tree.
- Ensure `ludlc.c` and the public/private LuDLC headers are built with
  `CONFIG_LUDLC_CORE` (same module or built-in object group).
- Enable `CONFIG_LUDLC_SERDEV` for a link over a UART, `CONFIG_LUDLC_SPI_MASTER`
  for one over an SPI bus this system drives.
- Pick the checksum a link runs when neither the `crc=` module parameter,
  firmware nor configfs names one: `CONFIG_LUDLC_CRC_DFLT_*` in tree, or
  `CONFIG_LUDLC_CRC_DFLT` in the `Makefile` out of tree. Both ends of a link
  must agree on it. It belongs to the core rather than to a transport, every
  transport choosing one the same way.

## What both ends have to agree on

A link is a conversation, so a few of its properties describe the network
rather than either driver, and are set the same way: a firmware property, a
configfs attribute, or a module parameter for the whole system. Getting one
wrong gives a link on which nothing ever arrives.

| | Checksum | Framing | Payload size |
| --- | --- | --- | --- |
| firmware | `ludlc,csum = "crc16-pr0"` | `ludlc,framing = "cobs"` | `ludlc,max-payload = <32>` |
| configfs | `csum`, with `csum_available` | `framing`, with `framing_available` | `max_payload`, with `max_payload_available` |
| parameter | `ludlc_core.crc=` | `<transport>.framing=` | `<transport>.max_payload=` |
| default | `CONFIG_LUDLC_CRC_DFLT_*` | `hdlc`, `raw` over SPI | `CONFIG_LUDLC_MAX_PAYLOAD_SIZE` |

Where a checksum is chosen by name, `none` is also accepted, for a link that
runs no checksum: none is computed, sent or checked, so the peer must be set
the same way. `ludlc_core.crc=none`, or `CONFIG_LUDLC_CRC_DFLT_NONE` at build
time, makes that the default of every link.

There are three. `cobs` stuffs the frame so that no zero occurs within it and
ends it with one, which costs one octet per frame and one per 254 whatever the
frame holds. `hdlc` delimits a frame with a start octet and escapes that octet
and the escape wherever they occur in it; it suits an octet arriving at a time
and costs an extra octet per reserved value in the frame, which on unlucky data
is every one of them. `raw` sends the frame as it stands and costs nothing at
all, leaving the transport to say where it ends.

The serdev transport carries the two that delimit themselves: a port hands over
a stream of octets and has nothing to say about where a frame stops, so it
defaults to `hdlc` and leaves `cobs` for a peer that speaks it. The SPI
transport announces a frame's length before sending it and pads the rest of the
transfer, which is delimiting enough, so it defaults to `raw` and pays nothing
for framing; `cobs` is there for a peer that only speaks it. `hdlc` it cannot
carry at all, escaping being unmeasurable until it is done. Each driver takes
its framing the same three ways and refuses, in all of them, one its wire
cannot carry; `framing_available` in a port directory lists what is left.

Both ends must also agree on the frame size, since that is what an SPI
announcement is checked against.

## Describing a link without firmware

Where firmware describes the port but not the LuDLC link on it, or leaves the
port disabled for user space to claim, a link is described under configfs
instead. Both transports register the same tree, `ludlc-serdev` and
`ludlc-spi`, differing only in the settings of their own wire:

```
	/sys/kernel/config/ludlc-serdev/serial0-0/
		baudrate parity flow_control        the UART
		csum framing max_payload            the link
		live                                open it
		ludlc/<dev>/                        what the link carries

	/sys/kernel/config/ludlc-spi/spi0.0/
		turnaround_us poll_interval_ms      the exchange
		csum framing max_payload live ludlc/  as above
```

A directory must be named after the device it describes, as `dev_name()` shows
it, and its settings are picked up when that device probes. An attribute never
written reads back empty and changes nothing, so a port created only to reach
its `ludlc/` subdirectory leaves a firmware-described device alone.

Writing `live` is the only way to open a link on a port firmware left
unavailable, and a way to reopen one it does describe with different settings.
An SPI peripheral is not created here: a system that describes one well enough
for the SPI core to register it has already said where it sits on the bus and
how fast; what it has no way of saying is that LuDLC runs over it.

## How large a payload

A per-link setting, like the checksum, and fixed once the link is open. A link
takes it from firmware (`ludlc,max-payload`), from configfs (`max_payload`),
or from the `max_payload` module parameter, in that order of precedence;
unset, it carries `CONFIG_LUDLC_MAX_PAYLOAD_SIZE`, the most this build allows.

The frame is the transport's business: it adds the header and the checksum to
the payload. `LUDLC_FRAME_BUF_SIZE` sizes the receive buffers, and covers the
widest checksum this build computes, since a buffer is sized where the driver
is compiled and a checksum is named where a link is opened:

| | Per link | Sized by |
| --- | --- | --- |
| `ludlc_serdev` | one RX frame, one TX frame, one TX fifo | `LUDLC_COBS_FRAME_MAX` |
| `ludlc_spi_master` | one RX frame, one TX frame | the same |

So raising the ceiling, which `CONFIG_LUDLC_MAX_PAYLOAD_SIZE` does, costs
memory on every link whether it uses it or not, while lowering a single link's
`max_payload` only limits what that link puts on the wire. `max_payload` is
what `ludlc_conn_max_payload()` returns and what `ludlc_enqueue_data()`
enforces. The transport adds a header and this link's checksum to get the
frame, so a link on a narrow checksum, or on none at all, keeps the octets a
wider one would have spent.

A peripheral has the same buffers on far less memory, and it is the smaller end
that settles what a link can carry.

## The other end of an SPI link

`ludlc_spi_master` drives the bus; something has to answer on it. The wire
format is neither end's to define and lives in
[`ludlc_spi_proto.h`](../../include/ludlc_spi_proto.h), shared with the
peripheral so that an announcement built by one and read by the other cannot
drift.
