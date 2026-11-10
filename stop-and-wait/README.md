# Reliable File Transfer over UDP (Stop-and-Wait), C++

UDP can lose, corrupt or duplicate packets. This project makes file transfer
over UDP reliable using **stop-and-wait ARQ**: send one packet, wait for its
acknowledgement (ACK), resend if the ACK doesn't arrive in time. A built-in
"bad network" mode randomly drops and corrupts packets to prove it works.

About 250 lines of C++, using only standard POSIX sockets.

## Files

| File | What it does |
|---|---|
| `common.hpp` | Packet layout, checksum, byte-order helpers |
| `sender.cpp` | Reads the file in 1 KB pieces and sends each one reliably |
| `receiver.cpp` | Checks each packet, writes new data to the output file, sends ACKs |

## Packet format

```
Data packet (8-byte header + up to 1024 bytes of data)
 +-------------+-----------+----------------+------------------+
 | seq (4 B)   | len (2 B) | checksum (2 B) | data (len bytes) |
 +-------------+-----------+----------------+------------------+
 len = 0 means "end of file"

ACK packet:  | seq (4 B) |   = number of the packet being acknowledged
```

## How it works

```
Sender                                   Receiver
  send packet 0  ───────────────────►   checksum OK, seq 0 is new → write it
                 ◄───────────────────   ACK 0
  send packet 1  ─────── ✗ lost
  ... 50 ms, no ACK ... timeout
  resend packet 1 ──────────────────►   new → write it
                 ◄─────── ✗ ACK lost
  ... timeout
  resend packet 1 ──────────────────►   seq 1 already written → duplicate,
                 ◄───────────────────   don't write again, but ACK 1 again
  send packet 2  ...
```

The three problems and how each is handled:

| Problem | Detected by | Handled by |
|---|---|---|
| Packet lost | No ACK within 50 ms | Sender resends |
| Packet corrupted | Checksum mismatch at receiver | Receiver drops it silently → sender times out and resends |
| ACK lost | Sender times out | Sender resends; receiver sees a duplicate `seq`, skips writing it, ACKs again |

## Build and run

The same code builds on Linux, Mac and Windows (Winsock); the differences are
in one `#ifdef _WIN32` block at the top of `common.hpp`.

**Windows (MinGW g++, e.g. from Code::Blocks or MSYS2)**: in Command Prompt, in the project folder:

```bat
g++ -std=c++17 -O2 -o receiver.exe receiver.cpp -lws2_32
g++ -std=c++17 -O2 -o sender.exe sender.cpp -lws2_32

:: Window 1
receiver.exe 9000 received.jpg 0.1
:: Window 2
sender.exe 127.0.0.1 9000 photo.jpg 0.1 0.05

fc /b photo.jpg received.jpg
```

In Code::Blocks, make two console projects (one per .cpp) and add `ws2_32`
under Project → Build options → Linker settings. In Visual Studio it links
automatically. Start the receiver before the sender. Allow the program if
Windows Firewall asks.

**Linux / Mac / WSL:**

```bash
make

# Terminal 1: receiver. Optional 3rd argument = chance each ACK is lost
./receiver 9000 received.jpg 0.1

# Terminal 2: sender. Optional = packet loss rate, corruption rate
./sender 127.0.0.1 9000 photo.jpg 0.1 0.05

# Check the copy is identical
cmp photo.jpg received.jpg && echo identical
```

## Results

1 MB file (978 packets) over loopback. Loss is applied to both data packets
and ACKs. **The file arrived byte-for-byte identical in every test**, including
an empty file and a file that is an exact multiple of 1 KB.

| Loss (each direction) | Corruption | Time | Retransmissions | Corrupted packets caught |
|---:|---:|---:|---:|---:|
| 0% | 0% | 0.03 s | 0 | 0 |
| 10% | 0% | 14.1 s | 251 | 0 |
| 20% | 0% | 31.6 s | 564 | 0 |
| 10% | 5% | 17.5 s | 313 | 58 |
| 20% | 5% | 36.8 s | 657 | 64 |

**What the numbers show:** with no loss the transfer takes 0.03 s, but at 10%
loss it takes 14 s. Almost all of that time is spent waiting out 50 ms
timeouts, while the actual round trip on loopback is well under 1 ms. This
points to the two standard improvements:

1. **Sliding window** (Go-Back-N / Selective Repeat): keep several packets in
   flight instead of one, so the link isn't idle while waiting.
2. **Adaptive timeout**: measure the round-trip time and set the timeout from
   it, as TCP does, instead of a fixed 50 ms.

## Interview prep

**60-second explanation**

> "UDP doesn't guarantee delivery, so I built a reliable file transfer on top
> of it using stop-and-wait. The file is split into 1 KB packets, each with a
> sequence number and a checksum. The sender sends one packet and waits for an
> ACK with the same number. If no ACK comes within 50 ms, it resends. The
> receiver drops corrupted packets and uses the sequence number to spot
> duplicates, which happen when an ACK is lost. To test it I added options that
> randomly drop and corrupt packets. Even at 20% loss with 5% corruption, the
> file arrives identical. The limitation is speed: most of the time goes on
> waiting for timeouts, which is why real protocols use a sliding window."

**Likely questions**

- **Why not just use TCP?** TCP already does this, and in practice you would use
  it. The point is to understand how it works. Some real protocols (QUIC,
  which runs HTTP/3, and many games and VoIP apps) do build their own
  reliability on UDP for more control.
- **Why does the receiver need sequence numbers?** To tell a new packet from a
  resent copy. Without them, a lost ACK would make it write the same data twice.
- **Why ACK a duplicate?** The duplicate arrived because our previous ACK was
  lost. If we don't ACK again, the sender keeps resending forever.
- **Why does the receiver send nothing for a corrupted packet?** It can't trust
  any field, even the sequence number, so it stays quiet and lets the sender's
  timeout handle it.
- **How is the timeout implemented?** `setsockopt(SO_RCVTIMEO)` on the socket:
  `recv()` returns -1 if nothing arrives within 50 ms.
- **Why `htonl` / `ntohl`?** Network byte order is big-endian, x86 is
  little-endian. Without converting, a sequence number of 1 would be read as
  16,777,216 on the other machine.
- **How would you choose the timeout properly?** Measure the round-trip time
  and keep a running average and variation: `RTO = SRTT + 4 × RTTVAR`
  (TCP's formula).
- **What's the efficiency of stop-and-wait?** At most one packet per round trip.
  Utilisation = `T_transmit / (T_transmit + RTT)`, which is tiny on long or
  fast links.
- **How do you make it faster?** Sliding window: Go-Back-N (resend everything
  from the lost packet) or Selective Repeat (resend only the lost one).
- **Textbook stop-and-wait only needs 1-bit sequence numbers (0/1). Why 32 bits?**
  The alternating bit is enough when packets can't be reordered. A 32-bit
  counter is easier to debug and doesn't break if the network delivers a
  very old copy late.
- **Known limitation?** If the ACK for the final "end of file" packet is lost
  every time, the sender can't be sure the receiver finished (the classic Two
  Generals problem). The sender gives up after 20 tries; the data itself was
  already confirmed packet by packet.

## Resume line

Built a reliable file-transfer protocol over UDP in C++ (stop-and-wait ARQ with
sequence numbers, checksums, ACKs and timeout-based retransmission); verified
byte-exact delivery under simulated 20% packet loss and 5% corruption.
