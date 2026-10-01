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
zero, the packet is given up: it is reported as failed, and the
packet behind it takes its sequence number.

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

A link may give the top bits of the field to a tag, which belongs
to the application and travels in every packet at no cost in
payload: a link with 32 channels on an 8-bit field has 3 bits to
spare. The bits below the tag then select the channel. The width
of the tag is a property of the link, and both ends must agree
on it. The reserved channel carries no tag, and a packet that
selects it with one is dropped.

**`User Payload`:**

Data for a given channel. This field is **optional** for the
handshake packets sent during **States 1 and 2**, allowing them to
carry device capability information.

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

The handshake packets a node sends in **State 1** and **State 2**
(i.e., the packets with `id=1` and `id=2`) may optionally contain a
payload, the same in both. A node always leaves State 1 on one of
these two packets from its peer, whereas a data packet or a PING from
a peer already connected may end State 2 (see *Handshake Recovery*
below). Either packet may be the one: a peer that receives `id=0` and
`id=1` back to back may pass through State 1 without sending `id=1`,
its transmitter getting no turn in between. The node therefore takes
the payload from whichever packet moves it out of State 1: once per
handshake, and before it reaches **State 3** (*Connected*). The
payload must not change for the life of the link. The `id=0` packet
stays empty, being what tells a node still in a session that its peer
has restarted.

This payload **cannot be fragmented** and its size must fit within the
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
    Acknowledgment Field that a PING would. The ping time must exceed
    the round trip plus the time a full window takes to send (see
    *Giving a Packet Up* below).
2.  **Immediate Acknowledgment:** If a packet is received, or a copy of one
    received already, and the outgoing data queue is empty, a PING is
    generated immediately. This ensures acknowledgment information is sent
    to the peer without waiting for new data or for the ping timer to
    expire.

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
to ensure correct sequence number calculation, and **at most half the
sequence numbers** (64 with 8-bit IDs, whose top bit is a flag). A receiver
tells a packet sent again from one it has yet to see by where its sequence
number falls: in the window behind the last one received, or in the window
ahead of it. The two must not overlap.
A sender may transmit up to this "window size" number of packets before
receiving an acknowledgment. If the window is full, further transmissions
are blocked.

**Acknowledgments (ACKs)**
When a packet is received, the receiver's next outgoing packet (data or
ping) carries the sequence number of the just-received packet in its
Acknowledgment Number field.

Upon receiving this ACK, the sender confirms all packets in its queue
up to and including that sequence number, freeing space in the sliding
window. The ACK may cover packets the sender has gone back to send again
after a NAK, the receiver holding them already, and the sender then
carries on from the first packet the receiver does not hold.

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

  * **Lost Packets**: If a receiver gets a packet ahead of the next one
    in sequence, the packets before it were lost, and it signals a NAK
    by setting the NAK Flag in its next outgoing packet. The
    Acknowledgment Number will be set to the *last good packet* it
    received. A PING or an unnumbered packet shows a loss the same way,
    its Transmit ID being the number the sender uses next.
  * **One NAK per Loss**: Everything the sender transmitted before it
    saw the NAK shows the same loss, and a NAK for each would send it
    back once per frame. The receiver therefore sends one, and no other
    until it sees the sender answer it:
      * the missing packet arrives;
      * a PING or an unnumbered packet carries the sequence number
        expected, which shows that nothing is missing any more;
      * the sender goes back, which shows as a sequence number no higher
        than the last one received from it, the sender otherwise
        transmitting in rising order;
      * or the sender acknowledges the first numbered packet the
        receiver sent after the NAK, by which time it has had the NAK
        or lost it.

    Meanwhile a PING carries the NAK again. While a NAK stands the
    receiver asks for no frame, so that PING is its keep-alive, a ping
    time after its last frame. Without it, a NAK lost each way would
    leave each node waiting for an ACK the other cannot send.
  * **Copies**: A packet at or behind the last one received is a copy,
    sent again because the sender missed the ACK or went back after a
    NAK. The receiver drops it and acknowledges it again.
  * **NAK Reception**: When a sender receives a NAK:
    1.  It resets its internal send counter back to its last
        confirmed packet.
    2.  It decrements the Time-To-Live (TTL) counter of the packet the
        NAK identifies, which is the one after the Acknowledgment
        Number, as that is the packet that failed to arrive. It does
        so once for each copy sent: a NAK that arrives before the copy
        sent in answer to the previous one has gone out cannot be about
        it. No other TTL is decremented. The remaining packets are
        re-transmitted because the receiver discards everything after a
        gap, not because they were lost.
    3.  If that packet's TTL reaches 0, it is given up and reported as
        a failure to the user. The packets behind it move up one place,
        each taking the sequence number of the one before (see *Giving
        a Packet Up* below).
    4.  The sender begins re-transmitting all surviving unconfirmed
        packets in its window.

A packet that is repeatedly lost is decremented by each NAK that reports
it, so it still expires in the end, whilst a packet delayed by another
packet's gap keeps its full budget. A link that has stopped carrying
anything at all is handled by the watchdog rather than by TTL.

**Giving a Packet Up**

A packet given up hands its sequence number to the one behind it. The
receiver holds no copy of it, and none can still be on its way: a NAK is
sent when a packet arrives ahead of the one expected, and on a link that
keeps its order, every copy sent before that packet has then been lost. As
the sender charges a packet once for each copy, its TTL counts copies known
to be lost.

The PING that carries a standing NAK again is the exception, as no arrival
triggers it. It goes out a ping time after the receiver's last frame, so the
ping time must exceed the time the answer to a NAK takes to come back: the
round trip, plus the time a full window queued ahead of the answer takes to
send. Where it does not, that PING can charge a copy still on its way. If
this gives the packet up, the copy arrives under the number now held by the
next packet: the receiver passes it on and drops the next packet as a copy,
so the packet reported failed is delivered, and the one behind it is lost
whilst reported delivered.

Once the packets behind it are given up as well, a standing NAK is about
packets the sender no longer has. The sender's PING then carries the
sequence number the receiver expects, which answers the NAK. In case that
PING is lost and the NAK is repeated all the same, the next packet the sender
transmits is not charged for its first copy, which may still be on its way.

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

The last handshake step is a numbered control packet as well, empty or
carrying the sender's capabilities, and arrives a second time whenever the
reply to it is lost. A step carrying that ID is therefore read as the
handshake rather than as a restart. The two are told apart exactly, a
deliberate disconnect being unnumbered.

The control channel is reserved for these packets. Callers cannot send on
it, with a tag or without, as a handshake step and a disconnect are told
apart by the shape of the packet, which a caller could otherwise imitate.

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
requirement. It states the length of a frame before sending it (see Appendix
B), so it needs a framing whose wire form can be measured before it is
produced. `raw` is that framing, and is what such a link runs by default;
`cobs` counts its own stuffing and can be carried as well, for a peer that
would rather not speak two framings. `hdlc` cannot be carried at all, as
escaping cannot be measured until it has been done.

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

## Appendix B: Transports That Announce a Frame

A transport that carries each frame in a transfer of its own, rather than
handing over an octet stream, has to know how long that frame is before it
starts: both ends clock the same octets, so both must be told the same length
first. SPI is the case implemented, and its wire format is specified by
`ludlc_spi_proto.h`, the header both ends compile, precisely because an
announcement written by one and read by the other is where two
implementations drift apart in silence. In outline:

  * **One exchange is two transfers**, each with its own chip select. Nothing
    on an SPI bus moves until the controller clocks it, and every octet
    clocked out brings one back, so the two directions are announced and then
    carried together rather than independently.
  * **The first transfer is the announcement** and never varies in length.
    Each end sends a magic octet, a check octet over what follows, and the
    length of the frame it wants to send, least significant octet first as
    everywhere else in this protocol. Either length may be zero, which is how
    an end says it has nothing. Each end reads the other's in the same clocks.
  * **The second transfer carries the frames**, and is as long as the longer
    of the two lengths announced. Both ends know both by then, so neither has
    to be told. An end whose frame is shorter pads the rest, an end with
    nothing to send pads all of it, and if both announced nothing the transfer
    does not happen. Padding is never read: the announcement says where a
    frame ends and the receiver stops there.
  * **The gap between the two** is where a peripheral prepares a buffer of the
    announced length. How long it needs is a property of its firmware, which
    says so, and the controller waits that long.
  * **The framing must state a length before producing a frame**, which is
    what rules `hdlc` out (see Appendix A). `raw` leans on the announcement
    entirely and is what such a link runs by default; `cobs` counts its own
    stuffing and announces the stuffed length.

The checksum is the link's rather than the transport's here as anywhere else:
both ends agree on it the same way as on a link over any other wire.

**Being heard**

A peripheral cannot begin an exchange, so the format is built around the
controller asking and around the peripheral being able to ask it to. An
exchange happens when the controller has a frame of its own to send, when the
peripheral raises a data-ready line, or, on a link with no such line, on a
poll interval. A controller polling announces nothing and learns in the same
exchange what the peripheral has.

**Losing step**

The two transfers are told apart by their order and by nothing in them, so an
end that misses one reads the next as the other half. Nothing prevents this
and nothing needs to: an announcement read out of a frame fails its check
octet, a frame read out of an announcement fails the checksum, and the
protocol resends what was lost. A link finds its footing again within an
exchange or two.
