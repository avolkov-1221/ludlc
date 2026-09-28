.. SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
.. Copyright (C) 2025-2026 Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

LuDLC Echo Sample
#################

Overview
********

This sample demonstrates a LuDLC echo counterpart on Zephyr.
It follows the same ECHO channel logic as ``src/samples/ludlc_demo.c``:

- receive packets on ``ECHO_CHANNEL``
- decrement ``payload[0]`` as a hop/TTL byte
- send the payload back while ``payload[0]`` is non-zero
- optionally (default enabled) send its own ECHO packet once per second

The link runs over a UART by default. On ``nucleo_g0b1re`` it runs over SPI
instead, with this board as the peripheral and a Linux host running
``ludlc_spi_master`` as the controller; see `Over SPI`_ below. Which transport
is used is the only difference between the two - the sample logic is the same.

Both configurations are marked ``build_only`` in ``sample.yaml``.

Requirements
************

* A board with UART and flash support
* Host PC / another board with UART support

For the SPI configuration, a board whose SPI driver supports peripheral mode
and that has a hardware chip select line, plus a spare GPIO for the data-ready
line.

Building and Running
********************

From the repository root:

.. code-block:: bash

   west build -b native_sim src/samples/zephyr/ludlc_echo -- \
     -DTARGET_PLATFORM=zephyr \
     -DZEPHYR_EXTRA_MODULES=/path/to/ludlc

Over SPI
********

.. code-block:: bash

   west build -b nucleo_g0b1re src/samples/zephyr/ludlc_echo -- \
     -DTARGET_PLATFORM=zephyr \
     -DZEPHYR_EXTRA_MODULES=/path/to/ludlc

``boards/nucleo_g0b1re.conf`` moves the link off the UART and onto ``spi1``,
which frees ``usart2`` - the ST-LINK's virtual COM port - to carry the log.
``boards/nucleo_g0b1re.overlay`` names the GPIO the data-ready line uses and
documents the wiring; both files are the place to start when porting this to
another board.

The wire format is in ``src/lib/include/ludlc_spi_proto.h``, shared with the
controller so the two cannot drift. Both ends must be built for the same
maximum payload and must run the same checksum.

On the Linux side that means ``CONFIG_LUDLC_SPI_MASTER`` and a devicetree node
under the host's SPI controller with ``compatible = "ludlc,spi"``. Point its
``interrupts-extended`` at whichever host pin the board's data-ready line is
wired to; leave it out and the controller polls every
``ludlc,poll-interval-ms`` (10 ms by default) instead, which still works, but a
frame the peripheral has ready then waits for the next poll rather than being
fetched. ``ludlc,turnaround-us`` is how long the controller gives the
peripheral to arm between the two transfers of an exchange, 50 µs by default,
which is ample for a G0B1RE at 64 MHz.

Expected Behavior
*****************

When connected to a LuDLC peer (for example, ``ludlc_demo`` on the host),
the sample logs connection events, received packet metadata, and TX confirmations.

For ECHO channel packets, it mirrors payloads back using the same
counterpart/TTL behavior as ``ludlc_demo``.

The test can also act as an echo server, periodically sending ECHO packets:

- When enabled, the sample sends one ECHO packet per second in addition
  to reply traffic.
- When disabled, the sample behaves as a reply-only counterpart, sending
  packets only in response to received ECHO payloads.

Periodic TX can be toggled with:

CONFIG_LUDLC_ECHO_PERIODIC_TX=y (default in the sample prj.conf)
CONFIG_LUDLC_ECHO_PERIODIC_TX=n
