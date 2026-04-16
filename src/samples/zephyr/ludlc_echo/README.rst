.. SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later

LuDLC Echo Sample
#################

Overview
********

This sample demonstrates a LuDLC serial echo counterpart on Zephyr.
It follows the same ECHO channel logic as ``src/samples/ludlc_demo.c``:

- receive packets on ``ECHO_CHANNEL``
- decrement ``payload[0]`` as a hop/TTL byte
- send the payload back while ``payload[0]`` is non-zero
- optionally (default enabled) send its own ECHO packet once per second

The sample is configured for ``native_sim`` and is currently marked
``build_only`` in ``sample.yaml``.

Requirements
************

* A board with UART and flash support
* Host PC / another board with UART support

Building and Running
********************

From the repository root:

.. code-block:: bash

   west build -b native_sim src/samples/zephyr/ludlc_echo -- -DTARGET_PLATFORM=zephyr


Expected Behavior
*****************

When connected to a LuDLC peer (for example ``ludlc_demo`` on the host),
the sample logs connection events, received packet metadata, and TX confirms.

For ECHO channel packets, it mirrors payloads back using the same
counterpart/TTL behavior used in ``ludlc_demo``.

Periodic TX can be toggled with:

- ``CONFIG_LUDLC_ECHO_PERIODIC_TX=y`` (default in sample ``prj.conf``)
- ``CONFIG_LUDLC_ECHO_PERIODIC_TX=n``
