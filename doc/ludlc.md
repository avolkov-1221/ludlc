# Lightweight Micro Devices Link Control (LuDLC)

**Version:** 1.0 \
**Author:** Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

The Lightweight Micro Devices Link Control (LuDLC) is a reliable
point-to-point communication protocol specifically developed for
resource-constrained devices. It is designed for implementation
in embedded systems and provides key functionalities such as
in-order data delivery, flow control, and channel multiplexing to ensure
efficient communication.

## Terminology

**ACK (Acknowledgment)**

A confirmation that a packet was successfully received. In LuDLC,
this is achieved by including the received packet's sequence number
in the ``Acknowledgment Field`` of a return packet.

**NAK (Negative Acknowledgment)**

An explicit notification that a packet was missed or received
out of order. This is signaled by setting the ``NAK Flag`` in the
``Acknowledgment Field``.

**PING**

A keep-alive packet, indicated by the ``PING Flag``. It contains
no payload and is used to verify the connection is alive and to
transmit timely ACK/NAK information when no data is being sent.

**Sliding Window**

The flow control mechanism that allows a sender to transmit a
configurable number of packets (the "window") before it must
wait for an ACK. This prevents buffer overruns on the receiver.

**TTL (Time-To-Live)**

A counter on an unconfirmed packet. Each time a NAK is received
(indicating a re-transmission is needed), the TTL is decremented.
If it reaches zero, the packet is dropped.

**SOF (Start of Frame)**

A special byte marker used in the optional serial implementation to
signal the beginning of a packet.

**EOF (End of Frame)**

A marker for the end of a packet in the optional serial
implementation. The SOF of a *new* packet implicitly serves
as the EOF for the *previous* one. A standalone EOF (identical
to the SOF byte) is only sent if the line is about to go idle.

**ESC (Escape Byte)**

A special byte marker used in the optional serial implementation
to facilitate byte stuffing. It signals that the following byte
has been modified (escaped) to avoid collision with the SOF or
ESC bytes.

### Introduction

LuDLC is a data-link layer protocol derived from HDLC but simplified for
resource-constrained, point-to-point links that do not require
multi-drop bus support. Key modifications include the removal of HDLC’s
U-frames and the replacement of the 2-way command/response handshake
(e.g., SABM/UA) with a symmetrical 3-way state exchange
on the control channel.

The LuDLC protocol provides reliable, in-order packet delivery, connection
management, and channel multiplexing. While optimized for point-to-point links,
it still can be extended to multi-drop bus topologies by encapsulating packets
within an outer protocol that adds an addressing layer.

### Key Features

* **Connection-Oriented**: A 3-way handshake is used to establish a
  symmetrical, peer-to-peer connection.
* **Reliable Delivery**: Guarantees in-order packet delivery using
  sequence numbers and acknowledgments (ACKs).
* **Error Recovery**: Uses Negative Acknowledgments (NAK) and
  re-transmissions with a Time-To-Live (TTL).
* **Flow Control**: Implements a sliding window protocol to prevent buffer
  overruns and improve data throughput by cumulatively acknowledging
  groups of packets.
* **Keep-Alives**: A ping/pong mechanism acts as a keep-alive and
  connection watchdog.
* **Multiplexing**: Supports multiple logical channels over a single link.
* **Flexible Transport**: The protocol is transport-agnostic and can run
  over various physical layers, such as UART, SPI, or CAN.

## LuDLC Protocol (Logic Layer)

The LuDLC Protocol defines the logical packet structure, connection
management, and reliability mechanisms. It is designed to be
transport-agnostic and can be adapted to different physical layers.

For simple transports (like UART), it can be encapsulated by a framing
layer (see Appendix A) that adds packet delimitation and error
detection. For transports with built-in integrity and framing, such as CAN bus,
this optional layer can be minimal, allowing the LuDLC packet to be transmitted
nearly directly, with only minor rearrangement of the header’s fields.

#### LuDLC Packet Structure

The protocol defines a header that precedes any user data.

```text
    +-------------------+---------------+-------------------+
    | Sequence/Ack IDs  | Channel ID    |   User Payload    |
    |   (Variable)      | (Variable)    |    (Variable)     |
    +-------------------+---------------+-------------------+
    | <------ LuDLC Packet Header ----> | <-- Data ... -->  |
````

The header fields are defined as:

**`Transmit ID Field`:**

This field contains both the packet's sequence number and a flag
to indicate a PING. The underlying type must be an **unsigned
integer**. To avoid slow multi-word computations, its size
should be chosen to be no larger than the device's native CPU
word size (e.g., `uint32_t` for a 32-bit CPU).

  * **Sequence Number**: The packet's unique, wrapping sequence number.
  * **PING Flag**: A flag (typically the most significant bit) that,
    if set, indicates this packet is a PING (keep-alive) and contains
    no channel ID or user payload.

**`Acknowledgment Field`:**

This field contains the last sequence number received from the peer
and a flag to indicate a NAK. It must be the same unsigned
integer type as the Transmit ID Field.

  * **Acknowledgment Number**: The sequence number of the last valid,
    in-order packet received from the peer.
  * **NAK Flag**: A flag (typically the most significant bit) that,
    if set, signals a Negative Acknowledgment.

**`Channel Field`:**

A field to identify the logical channel for the payload. This type
must also be an **unsigned integer**, and for performance, its
size should ideally be no larger than the CPU's native word size.
One channel ID (typically `0`) is reserved for connection
management (handshake, disconnect). This field is omitted in
PING packets.

**`User Payload`:**

Data for a given channel. This field is **optional** for the
handshake packet sent during **State 1**, allowing it to carry
device capability information.

> **Note:**
>
> **Header Endianness**
>
> If any multi-byte fields are used (`Transmit ID Field`,
> `Acknowledgment Field`, or `Channel Field`), they **must**
> be transmitted in **little-endian** byte order.
>
> Nodes on big-endian architectures are responsible for
> byte-swapping these fields upon reception and before transmission
> to ensure interoperability.

#### Connection Management

LuDLC uses a symmetrical 3-way handshake on the reserved control channel.
There is no client/server relationship; either node can initiate the
connection. All nodes follow the same state logic.

The Transmit ID of a control packet indicates the sender's current state.
When a node receives a control packet, it advances its own state.

1.  **Initial State**: Both **Node A** and **Node B** are in **State 0**
    (Disconnected). They begin sending control packets with a
    Transmit ID of 0.
2.  **First Exchange**:
      * Node A (State 0) receives Node B's `id=0` packet. It moves to
        **State 1** and begins sending `id=1`.
      * Node B (State 0) receives Node A's `id=0` packet. It moves to
        **State 1** and begins sending `id=1`.
3.  **Second Exchange**:
      * Node A (State 1) receives Node B's `id=1` packet. It moves to
        **State 2** and begins sending `id=2`.
      * Node B (State 1) receives Node A's `id=1` packet. It moves to
        **State 2** and begins sending `id=2`.
4.  **Connection Established**:
      * Node A (State 2) receives Node B's `id=2` packet. It moves to
        **State 3** (*Connected*).
      * Node B (State 2) receives Node B's `id=2` packet. It moves to
        **State 3** (*Connected*).

The connection is now established.

```text
      Node A                                Node B
 (State 0, DISCONNECTED)                   (State 0, DISCONNECTED)
 (Sending id=0)                            (Sending id=0)
       |                                        |
       | --- [Control, id=0] ----------->       |
       |      <----------- [Control, id=0] ---- |
       |                                        |
(Receives id=0, moves to State 1)     (Receives id=0, moves to State 1)
 (State 1, WAIT_CONN_1)                    (State 1, WAIT_CONN_1)
 (Sending id=1)                            (Sending id=1)
       |                                        |
       | --- [Control, id=1] ----------->       |
       |      <----------- [Control, id=1] ---- |
       |                                        |
(Receives id=1, moves to State 2)     (Receives id=1, moves to State 2)
 (State 2, WAIT_CONN_2)                    (State 2, WAIT_CONN_2)
 (Sending id=2)                            (Sending id=2)
       |                                        |
       | --- [Control, id=2] ----------->       |
       |      <----------- [Control, id=2] ---- |
       |                                        |
(Receives id=2, moves to State 3)     (Receives id=2, moves to State 3)
 (State 3, CONNECTED)                      (State 3, CONNECTED)
```

**Optional Handshake Payload**

The handshake packet sent by a node when it enters **State 1**
(i.e., the packet with `id=1`), may optionally contain a payload. This
payload **cannot be fragmented** and its size must fit within the
net wide defined maximum packet size for the connection.

While transports like CAN bus inherently limit this payload (e.g., to 8
bytes), other transports like UART or SPI may allow for a significantly
larger capability payload, limited only by the implementation's buffer
sizes.

This payload can be used to transmit metadata about the device, such as:

  * Device type or model
  * Hardware and software capabilities
  * Firmware information

The structure and parsing of this single-packet capability payload are
defined by the user's application.

**Handshake Recovery (Lost Packets)**

The handshake is robust against packet loss because it is state-driven.
A node will continuously re-transmit the control packet corresponding to
its *current* state (e.g., "I am in State 1," "I am in State 1," ...)
until it receives a packet that advances its state.

  * **If a node receives a packet for a state it has already passed**
    (e.g., Node A is in State 2 but receives an `id=1` from Node B),
    it ignores the packet (treating it as a late re-transmission)
    and simply re-sends its *own* current state packet (`id=2`) to
    help Node B catch up.

  * **If a node receives a packet for the *next* state** (e.g., Node
    A is in State 0 but receives an `id=1`), it immediately advances
    to that state (State 1).

  * **If a node is in State 2 (WAIT\_CONN\_2) and receives a data
    packet or PING** instead of the expected final handshake packet (`id=2`),
    it infers that the peer node has already transitioned to the *Connected*
    state after receiving the WAIT\_CONN\_2 packet from this node. In this case,
    the node will unilaterally advance to State 3 (*Connected*) and process the
    incoming packet as a standard data or PING packet.

**Keep-Alives (PINGs)**
A PING packet is a special packet with the PING Flag set and no payload.
It is generated by the logic layer in two scenarios:

1.  **Keep-Alive:** If no data has been sent for a configured "ping time,"
    a PING is generated to maintain the connection and signal liveness.
2.  **Immediate Acknowledgment:** If a packet is received and the outgoing
    data queue is empty, a PING is generated immediately. This ensures
    acknowledgment information is sent to the peer without waiting for
    new data or for the ping timer to expire.

In both cases, the PING packet carries valid and up-to-date
acknowledgment information in its Acknowledgment Field.

**Watchdog (Timeout)**
If no valid packet (including PINGs) is received for a watchdog period
(typically 3 times the "ping time"), the connection is considered lost,
and the state machine resets to Disconnected.

**Disconnect**
A (non-ping) control packet received while in the *Connected* state is
treated as a disconnect notification, causing the connection to reset.

#### Reliable Data Transfer

**Sliding Window**
LuDLC uses a sliding window for flow control. The window size is
configurable but **must be a power of two** (e.g., 2, 4, 8)
to ensure correct sequence number calculation and should be significantly
smaller than MAX\_ID to prevent false IDs when the counter wraps around to 0.
A sender may transmit up to this "window size" number of packets before
receiving an acknowledgment. If the window is full, further transmissions
are blocked.

**Acknowledgments (ACKs)**
When a packet is received, the receiver's next outgoing packet (data or
ping) will have its Acknowledgment Number set to the sequence number of
the just-received packet.

When the sender receives this acknowledgment, it "confirms" all packets
in its queue up to and including that sequence number, freeing space in
the sliding window.

**Negative Acknowledgments (NAKs) and Retries**

  * **Out-of-Order Packet**: If a receiver gets a packet with an
    unexpected sequence number (i.e., not the next one in sequence), it
    signals a NAK by setting the NAK Flag in its next outgoing packet.
    The Acknowledgment Number will be set to the *last good packet* it
    received.
  * **NAK Reception**: When a sender receives a NAK:
    1.  It resets its internal send counter back to its last
        confirmed packet.
    2.  It decrements a Time-To-Live (TTL) counter for all unconfirmed
        packets.
    3.  If a packet's TTL reaches 0, it is dropped and reported as
        a failure to the user.
    4.  The sender begins re-transmitting all surviving unconfirmed
        packets in its window.

## Appendix A: Optional Serial Framing Implementation

The following describes the serial framing layer implementation. This
layer is **optional** and is intended for octet-stream transports that
lack hardware framing and integrity checks (like UART).

This framing method is conceptually similar to PPP (Point-to-Point
Protocol) but, by default, uses different Flag Sequence and Escape values.

#### Frame Structure

A single serial frame has the following structure:

```text
    +----------+----------------------------+------------------+-----------+
    |   SOF    |   Escaped Packet Payload   | Escaped Checksum |   [EOF]   |
    | (1 octet)|          (Variable)        |    (Variable)    | (1 octet) |
    +----------+----------------------------+------------------+-----------+
```

  * **SOF (Start of Frame)**: A single-octet marker, equivalent to
    the Flag Sequence in PPP, with a default value of `0x55`. This value
    is configurable and should be chosen to be one of the least likely octets
    to appear in a typical data stream. It serves a dual purpose:
    it **always** marks the beginning of a new frame and **simultaneously**
    can mark the end of the *previous* frame.

  * **Escaped Payload**: This is the complete "LuDLC Protocol Packet"
    (see above), which has been byte-stuffed.

  * **Escaped Checksum**: A checksum of the *original, un-escaped*
    payload, used to ensure data integrity. The specific algorithm
    (e.g., CRC-8, CRC-16 etc) and its size are user-defined. The user
    should choose an algorithm appropriate for their device's
    architecture and requirements. The chosen algorithm (e.g.,
    CRC-16/KERMIT vs. CRC-16/XMODEM) dictates the octet order
    (endianness) used for transmission.

  * **[EOF] (End of Frame)**: An optional octet identical to the SOF
    octet. It is **only** sent to explicitly terminate a packet if no
    other packet is immediately ready. If another packet is ready,
    this octet is **omitted**, and the SOF of the *next* packet serves
    as the EOF for this one.

#### Byte Stuffing (Escaping)

To ensure the SOF/EOF octet never appears in the payload or
checksum, byte stuffing is applied using a special Escape octet.

  * **Escape Octet**: A single octet marker, defaulting to `0xAA`.
    This value is configurable and should also be chosen to be one
    of low probability in the typical data stream.

If the data to be sent (payload or CRC) contains either the SOF octet
or the Escape octet, it is replaced as follows:

1.  The original octet is replaced with the **Escape octet**.
2.  The original octet, XORed with an escape mask (`0x20`), is
    sent next.

The following example assumes the default values (SOF=`0x55`,
ESC=`0xAA`, Mask=`0x20`):

```text
    Original Octet |      Sent Sequence
    ------------------------------------------
    0x55 (SOF)     | 0xAA, 0x75  (0x55 ^ 0x20)
    0xAA (ESC)     | 0xAA, 0x8A  (0xAA ^ 0x20)
```

On reception, if the Escape octet is received, the next octet is read
and XORed with the escape mask (`0x20`) to reconstruct the
original octet.
