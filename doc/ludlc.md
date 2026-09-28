# Lightweight Micro Devices Link Control (LuDLC)

**Version:** 1.1 \
**Author:** Andrey VOLKOV <andrey@volkov.fr> & LuDLC Contributors

The Lightweight Micro Devices Link Control (LuDLC) is a reliable
point-to-point communication protocol specifically developed for
resource-constrained devices. It is designed for implementation
in embedded systems and provides key functionalities such as
in-order data delivery, flow control, and channel multiplexing to ensure
efficient communication.

## Acknowledgments

The author acknowledges [MUNIC SA](https://munic.io) for sponsoring the
development of LuDLC. Their support has enabled continued design,
implementation, and validation of the protocol.

Additional recognition goes to the open-source community for feedback
and testing throughout its evolution.

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

**Unnumbered Packet**

A data packet sent outside the sequence, also indicated by the
``PING Flag``. It takes no place in the window and is neither
acknowledged nor re-transmitted, so it can be sent immediately
regardless of the state of the numbered traffic.

**Sliding Window**

The flow control mechanism that allows a sender to transmit a
configurable number of packets (the "window") before it must
wait for an ACK. This prevents overruns of the receiver's link buffers.

**TTL (Time-To-Live)**

A counter on an unconfirmed packet. Each time a NAK reports this
particular packet missing, its TTL is decremented. If it reaches
zero, the packet is dropped.

**Frame**

What a transport carries: one LuDLC packet and a checksum over it.
Its maximum size is agreed by the network, and it is what bounds
a payload, the header and the checksum coming off it first.

**Framing**

The optional layer that puts a frame on the wire and says where it
ends. Three are defined: `cobs`, `hdlc` and `raw`. Which one a link
runs is agreed by both of its ends, as described in Appendix A.

**SOF (Start of Frame)**

A special byte marker used in the escaping framing (`hdlc`) to
signal the beginning of a packet.

**EOF (End of Frame)**

A marker for the end of a packet in the escaping framing. The SOF
of a *new* packet implicitly serves as the EOF for the *previous*
one. A standalone EOF (identical to the SOF byte) is only sent if
the line is about to go idle.

**ESC (Escape Byte)**

A special byte marker used in the escaping framing to facilitate
byte stuffing. It signals that the following byte has been modified
(escaped) to avoid collision with the SOF or ESC bytes.

### Introduction

LuDLC is a data-link layer protocol derived from HDLC but simplified for
resource-constrained, point-to-point links that do not require
multi-drop bus support. Key modifications include the reduction of HDLC’s
U-frames to a single unnumbered data packet and the replacement of the
2-way command/response handshake (e.g., SABM/UA) with a symmetrical 3-way
state exchange on the control channel.

The LuDLC protocol provides reliable, in-order packet delivery, connection
management, and channel multiplexing. While optimized for point-to-point links,
it still can be extended to multi-drop bus topologies by encapsulating packets
within an outer protocol that adds an addressing layer.

### Key Features

* **Connection-Oriented**: A 3-way handshake is used to establish a
  symmetrical, peer-to-peer connection.
* **Announced Disconnect**: A node on its way out says so, in a frame that
  needs no room in the window, so the peer need not wait out its watchdog.
* **Reliable Delivery**: Guarantees in-order packet delivery using
  sequence numbers and acknowledgments (ACKs).
* **Error Recovery**: Uses Negative Acknowledgments (NAK) and
  re-transmissions with a Time-To-Live (TTL).
* **Flow Control**: Implements a sliding window protocol to prevent
  overruns of the receiver's link buffers and improve data throughput by
  cumulatively acknowledging groups of packets.
* **Keep-Alives**: A ping/pong mechanism acts as a keep-alive and
  connection watchdog.
* **Multiplexing**: Supports multiple logical channels over a single link.
* **Flexible Transport**: The protocol is transport-agnostic and can run
  over various physical layers, such as UART, SPI, or CAN, each link
  choosing the framing and the integrity check that suit its wire.

## LuDLC Protocol (Logic Layer)

The LuDLC Protocol defines the logical packet structure, connection
management, and reliability mechanisms. It is designed to be
transport-agnostic and can be adapted to different physical layers.

For simple transports (like UART), it can be encapsulated by one of the
framing layers of Appendix A, which add packet delimitation and error
detection. Which one a link runs is a property of the network rather than of
either node, so both ends agree on it as they agree on the checksum. For
transports with built-in integrity and framing, such as CAN bus, this optional
layer can be minimal, allowing the LuDLC packet to be transmitted nearly
directly, with only minor rearrangement of the header’s fields.

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
  * **PING Flag**: A flag (typically the most significant bit) that, if
    set, means the packet takes no sequence number of its own, and that
    the field holds the number the sender will use next. Two kinds of
    packet carry it: a PING (keep-alive), which ends after the
    Acknowledgment Field, and an unnumbered packet, which carries a
    channel and a payload (see
    [Unnumbered Packets](#unnumbered-packets)).

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
PING packets, but present in unnumbered ones.

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
      * Node B (State 2) receives Node A's `id=2` packet. It moves to
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
net wide defined maximum packet size for the connection (see
Appendix A).

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
    Sending any packet postpones this by one ping time, including an
    unnumbered packet, as it carries the same Transmit ID and
    Acknowledgment Field that a PING would.
2.  **Immediate Acknowledgment:** If a packet is received and the outgoing
    data queue is empty, a PING is generated immediately. This ensures
    acknowledgment information is sent to the peer without waiting for
    new data or for the ping timer to expire.

In both cases, the PING packet carries valid and up-to-date
acknowledgment information in its Acknowledgment Field.

**Watchdog (Timeout)**
If no valid packet (including PINGs) is received for a watchdog period
(typically 3 times the "ping time"), the connection is considered lost,
and the network stack resets to Disconnected.

**Disconnect**
A session ends in one of three ways: it is announced, it is inferred from a
peer that has restarted, or it runs out on the watchdog above. The announced
form is an unnumbered packet and is described with those, after
[Unnumbered Packets](#unnumbered-packets).

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
ping) carries the sequence number of the just-received packet in its
Acknowledgment Number field.

Upon receiving this ACK, the sender confirms all packets in its queue
up to and including that sequence number, freeing space in the sliding
window.

An ACK indicates that the packet reached the peer's link layer and was
passed to its channel, and nothing more. It does not guarantee that the
receiving application has consumed the payload, and it is never delayed
while that application is busy. A receiver that cannot accept a payload
drops it instead of delaying the ACK, because a delayed ACK would stall
the whole link, blocking every channel on it, and would eventually trip
the peer's watchdog timer.

End-to-end flow control therefore belongs to the channel that needs it,
as only the channel knows how much its consumer can accept. The window
belongs to the link and bounds only the packets in flight on it.

**Negative Acknowledgments (NAKs) and Retries**

  * **Out-of-Order Packet**: If a receiver gets a packet with an
    unexpected sequence number (i.e., not the next one in sequence), it
    signals a NAK by setting the NAK Flag in its next outgoing packet.
    The Acknowledgment Number will be set to the *last good packet* it
    received.
  * **NAK Reception**: When a sender receives a NAK:
    1.  It resets its internal send counter back to its last
        confirmed packet.
    2.  It decrements the Time-To-Live (TTL) counter of the packet the
        NAK identifies, which is the one after the Acknowledgment
        Number, as that is the packet that failed to arrive. No other
        TTL is decremented. The remaining packets are re-transmitted
        because the receiver discards everything after a gap, not
        because they were lost.
    3.  If that packet's TTL reaches 0, it is dropped and reported as
        a failure to the user. As a NAK identifies a single packet, at
        most one packet is dropped per NAK, and the remaining packets
        move up to fill the gap.
    4.  The sender begins re-transmitting all surviving unconfirmed
        packets in its window.

A packet that is repeatedly lost is decremented by each NAK that reports
it, so it still expires in the end, whilst a packet delayed by another
packet's gap keeps its full budget. A link that has stopped carrying
anything at all is handled by the watchdog rather than by TTL.

#### Unnumbered Packets

Everything above describes numbered traffic, which is reliable but ordered.
A packet cannot be sent before the ones ahead of it, and whilst the sender
is re-transmitting a window it is sending old packets rather than new ones.
For events that cannot wait, such as a power failure, a sender may send an
**unnumbered packet**, the one survivor of HDLC's U-frames.

An unnumbered packet carries the PING Flag, a Channel ID and a payload. It
is distinguished from a PING by its length, as a PING ends after the two ID
fields, so anything longer carries a channel and data.

```text
    +-------------------+---------------+-------------------+
    | PING Flag + Next  | Channel ID    |   User Payload    |
    |   Seq / Ack IDs   |               |                   |
    +-------------------+---------------+-------------------+
```

Its Transmit ID holds the sequence number the sender will use *next*, just
as a PING does, so the packet consumes no number of its own and the
receiver can still detect a gap. Its Acknowledgment Field is ordinary and
is processed as usual.

A receiver passes the payload to its channel immediately and leaves its
received-sequence state unchanged. The packet is not acknowledged and takes
no place in either node's window, so:

  * it may be sent at any time, including during a re-transmission or with
    the window full;
  * it is never re-transmitted, and its loss is not reported, which is the
    cost of not waiting;
  * its loss affects nothing else, as it leaves no gap and therefore causes
    no NAK and no re-transmission;
  * it serves as a keep-alive, being a PING with data attached, so it
    postpones the sender's next PING and resets the receiver's watchdog.

A sender holds at most one unnumbered packet at a time, as a packet meant
not to wait should not queue behind another of its kind.

#### Connection Disconnect

A session ends in one of three ways: it is announced, it is inferred from a
peer that has restarted, or it runs out on the watchdog.

An **announced disconnect** is an unnumbered packet on the control channel
whose single payload octet is the Disconnect opcode (`0x01`):

```text
    +-------------------+---------------+-------------------+
    | PING Flag + Next  | Control Chan  |  Disconnect (0x01)|
    |   Seq / Ack IDs   |               |                   |
    +-------------------+---------------+-------------------+
```

Being unnumbered, it goes out ahead of anything queued and needs no room in
the window, so a node on its way out can say so whatever its queue is doing.
A caller's unnumbered packet still waiting is displaced, a bye being the
last frame of the session either way.

```text
      Node A                                   Node B
 (State 3, CONNECTED)                      (State 3, CONNECTED)
       |                                          |
       | --- [Unnumbered, Ctrl, 0x01] ------>     |
       |                                          |
(Session ends as the frame leaves)   (Receives it, ends at once)
 (State 0, DISCONNECTED)                   (State 0, DISCONNECTED)
```

The receiver ends the session at once rather than waiting out its watchdog,
and sends nothing back: a handshake step would invite a peer that has just
left, whilst one on its way back begins the handshake itself. The sender's
own session ends as the frame leaves, and the packets still queued are
failed.

A peer that has **restarted** cannot announce anything, having no memory of
a session to end. It begins a handshake instead, and its first step, an
empty control packet, is taken as notice that the session is over.

The last handshake step is an empty control packet as well, and arrives a
second time whenever the reply to it is lost. A step carrying that ID is
therefore read as the handshake rather than as a restart. The two are told
apart exactly, a deliberate disconnect no longer being empty.

The control channel is reserved for these packets. Callers cannot send on
it, as a handshake step and a disconnect are told apart by the shape of the
packet, which a caller could otherwise imitate.

## Appendix A: Optional Framing Layer

The following describes the framing layer implementations. This layer is
**optional** and is intended for transports that lack framing, integrity
checks, or both: an octet stream such as a UART has neither, whilst a
transport that carries each frame in a transfer of its own, such as SPI,
delimits for itself and wants only the check.

#### What a Frame Is

Whichever framing a link runs, what that framing carries is the same: one
LuDLC packet and a checksum over it.

```text
    +-------------------------------------+------------------+
    |        LuDLC Protocol Packet        |     Checksum     |
    |   (header and payload, see above)   |    (Variable)    |
    +-------------------------------------+------------------+
    |<----------------------- Frame ------------------------>|
```

The largest frame a link may carry is what bounds a payload: the header and
this link's checksum come off it, and what is left is the most one packet can
hold. A wider checksum therefore costs payload, and a link whose transport
checks frames itself, carrying no checksum, keeps those octets for data.

Both ends of a link must agree on the framing, on the checksum and on that
maximum frame size, for the same reason in all three cases: each describes the
network and not either node. Disagree on any one of them and nothing ever
arrives.

#### Checksum

A checksum of the packet *as it stands*, before the framing has touched it,
used to ensure data integrity. It is what the framing carries and what the
receiver checks once the framing has been undone, so neither stuffing nor
escaping is ever covered by it.

The specific algorithm (e.g., CRC-8, CRC-16 etc) is user-defined, and so is
its width: the algorithm's own width is what goes on the wire, whatever the
width of the type a given build computes it in. The user should choose an
algorithm appropriate for their device's architecture and requirements, and
both ends of a link must choose the same one.

The checksum belongs to the transport, not to the protocol engine. A link is
told which one it runs through `struct ludlc_csum`: the running function, its
seed, the residual it must leave behind, an optional final transform and its
width in octets. A link that needs none, its transport validating frames in
hardware for instance, is opened with a NULL checksum: none is then computed,
sent or checked.

The checksum is transmitted **least significant octet first**, whatever the
endianness of either peer. An algorithm that is defined the other way round
(CRC-16/XMODEM against CRC-16/KERMIT) is expressed by swapping the value
before it is handed over, which is also where a final XOR belongs (HDLC's
FCS-16).

#### Choosing a Framing

Three framings are defined. What tells them apart is how a frame ends and what
that costs:

  * **`cobs`**: A frame ends at a chosen delimiter octet (default
    `0x00`), that value never occurring inside it. Cost: two octets per
    frame, plus one for every 254 octets of it. Carried by any
    transport.
  * **`hdlc`**: A frame ends at a marker, which also begins the next
    frame. Cost: an octet to begin a frame, plus one for every reserved
    value in it, which on unlucky data is every octet. Carried by an
    octet stream.
  * **`raw`**: A frame ends wherever the transport says it does. Cost:
    nothing. Carried by a transport that delimits.

A transport that hands over an octet stream has nothing to say about where one
frame stops and the next begins, so it must run a framing that delimits
itself: `hdlc`, which such a link runs unless it is told otherwise, or `cobs`,
whose cost does not depend on the data.

A transport that carries a frame in a transfer of its own has the opposite
requirement. It states the length of a frame before sending it, so it needs a
framing whose wire form can be measured before it is produced. `raw` is that
framing, and is what such a link runs by default; `cobs` counts its own
stuffing and can be carried as well, for a peer that would rather not speak two
framings. `hdlc` cannot be carried at all, as escaping cannot be measured until
it has been done.

#### Stuffing and a Delimiter (`cobs`)

Consistent Overhead Byte Stuffing removes one octet value from a frame
and then uses it as the delimiter. That value is configurable; the
default is `0x00`. A frame is sent as a series of groups:

```text
    +------+-------------------+------+---------------+-----------+
    | Code |    Frame octets   | Code |  Frame octets | delimiter |
    +------+-------------------+------+---------------+-----------+
    |<-------- group --------->|<------ group ------->|(delimiter)|
```

  * A **group** is a code octet followed by up to 254 octets of the frame,
    none of them equal to the delimiter.
  * The **code** octet is one more than the number of octets that follow it,
    so it is never equal to the delimiter itself and never says more than
    255.
  * A **delimiter value** in the frame is not sent: it is what ends a
    group, and the receiver puts it back when it starts the next one.
  * A group that fills up at 254 octets is followed by another code octet even
    if the frame ends there, as a full group says that 254 octets follow it
    and not that the frame stops.
  * A single **delimiter octet** ends the frame. That value never appears
    within one, so a receiver that has lost its place recovers at the next
    delimiter.

The cost is therefore two octets per frame, the first code octet and the
delimiter, plus one more code octet for every full run of 254. It does not
depend on the data: an occurrence of the delimiter value costs nothing,
being what a group ends at, so the same frame always occupies the same
number of octets. That is what lets a transport announce a stuffed frame's
length before stuffing it.

A run of delimiters with nothing between them is not a frame and is ignored,
which is how a transport that has to pad a transfer does so.

#### Escaped Delimiters (`hdlc`)

This framing method is conceptually similar to PPP (Point-to-Point
Protocol) but, by default, uses different Flag Sequence and Escape values.

A single escaped frame has the following structure:

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

  * **Escaped Checksum**: The checksum described above, byte-stuffed in
    the same way as the payload it covers.

  * **[EOF] (End of Frame)**: An optional octet identical to the SOF
    octet. It is **only** sent to explicitly terminate a packet if no
    other packet is immediately ready. If another packet is ready,
    this octet is **omitted**, and the SOF of the *next* packet serves
    as the EOF for this one.

**Byte Stuffing**

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

Two octets of every 256 are therefore doubled on average, and a frame of
unlucky data doubles entirely. This is the price of a framing that need not
know how long a frame is going to be, and the reason a transport that has to
say so beforehand cannot carry it.

#### The Frame as It Stands (`raw`)

Nothing is added and nothing is escaped: the packet and its checksum go on the
wire exactly as they are, and where the frame ends is whatever the transport
says. There is no marker to look for and no octet value that a payload may not
contain, which also means a receiver has nothing to resynchronise on. A frame
lost or truncated is a frame the checksum rejects, and the next transfer
starts afresh.

This is the framing for a transport that delimits, and on such a link it is
the default.
