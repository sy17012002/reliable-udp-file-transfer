# Reliable File Transfer over UDP (Stop-and-Wait), C++

UDP can lose, corrupt or duplicate packets. This project makes file transfer
over UDP reliable using **stop-and-wait ARQ**: send one packet, wait for its
acknowledgement (ACK), resend if the ACK doesn't arrive in time. A built-in
"bad network" mode randomly drops and corrupts packets in both directions to
prove it works.

About 250 lines of C++, using only standard POSIX sockets / Winsock.

## Files

| File | What it does |
|---|---|
| `common.hpp` | Packet layout, checksum, byte-order helpers, fault injection, ACK helpers |
| `sender.cpp` | Reads the file in 1 KB pieces and sends each one reliably |
| `receiver.cpp` | Checks each packet, writes new data to the output file, sends ACKs |

## Packet format

```
Data packet (8-byte header + up to 1024 bytes of data)
 +-------------+-----------+----------------+------------------+
 | seq (4 B)   | len (2 B) | checksum (2 B) | data (len bytes) |
 +-------------+-----------+----------------+------------------+
 len = 0 means "end of file"

ACK packet: the same 8-byte header with len = 0
 +-------------+-----------+----------------+
 | seq (4 B)   | 0         | checksum (2 B) |   seq = packet being acknowledged
 +-------------+-----------+----------------+
```

ACKs carry a checksum too, so a corrupted ACK is detected and ignored instead
of being mistaken for an ACK of a packet that never arrived.

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

The problems and how each is handled:

| Problem | Detected by | Handled by |
|---|---|---|
| Packet lost | No ACK within 50 ms | Sender resends |
| Packet corrupted | Checksum mismatch at receiver | Receiver drops it silently → sender times out and resends |
| ACK lost | Sender times out | Sender resends; receiver sees a duplicate `seq`, skips writing it, ACKs again |
| ACK corrupted | Checksum mismatch at sender | Sender ignores it → times out and resends |
| Last ACK lost | Sender times out on the end-of-file packet | Receiver lingers 500 ms after finishing and ACKs any resend |

## Build and run

The same code builds on Linux, Mac and Windows (Winsock); the differences are
in one `#ifdef _WIN32` block at the top of `common.hpp`.

**Windows (MinGW g++, e.g. from Code::Blocks or MSYS2)**: in Command Prompt, in the project folder:

```bat
g++ -std=c++17 -O2 -o receiver.exe receiver.cpp -lws2_32
g++ -std=c++17 -O2 -o sender.exe sender.cpp -lws2_32

:: Window 1
receiver.exe 9000 received.jpg 0.1 0.05
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

# Terminal 1: receiver. Optional: ACK loss rate, ACK corruption rate
./receiver 9000 received.jpg 0.1 0.05

# Terminal 2: sender. Optional: packet loss rate, packet corruption rate
./sender 127.0.0.1 9000 photo.jpg 0.1 0.05

# Check the copy is identical
cmp photo.jpg received.jpg && echo identical
```

## Results

1 MB file (978 packets) over loopback, 50 ms timeout. Loss and corruption are
applied to both data packets and ACKs at the rates shown. **The file arrived
byte-for-byte identical in every test**, including an empty file, a 1-byte
file, and files of exactly 1 KB, 1 KB + 1 byte and 2 KB (all at 20% loss, 5%
corruption).

| Loss (each direction) | Corruption (each direction) | Time | Retransmissions | Corrupted packets caught | Corrupted ACKs caught |
|---:|---:|---:|---:|---:|---:|
| 0% | 0% | 0.01 s | 0 | 0 | 0 |
| 10% | 0% | 13.7 s | 243 | 0 | 0 |
| 20% | 0% | 31.6 s | 561 | 0 | 0 |
| 10% | 5% | 21.2 s | 375 | 59 | 59 |
| 20% | 5% | 41.4 s | 739 | 65 | 49 |

**What the numbers show:** with no loss the transfer takes 0.01 s, but at 10%
loss it takes 14 s. Almost all of that time is spent waiting out 50 ms
timeouts, while the actual round trip on loopback is well under 1 ms.

The retransmission counts match a simple model. A packet gets through only if
the packet *and* its ACK both survive: at 10% loss each way that is
0.9 × 0.9 = 0.81, so each packet needs 1 / 0.81 ≈ 1.23 sends on average, about
0.23 × 978 ≈ 230 retransmissions (measured: 243). At 20% loss and 5% corruption
each way, success is (0.8 × 0.95)² ≈ 0.58, predicting ≈ 715 (measured: 739).

This points to the two standard improvements:

1. **Sliding window** (Go-Back-N / Selective Repeat): keep several packets in
   flight instead of one, so the link isn't idle while waiting.
2. **Adaptive timeout**: measure the round-trip time and set the timeout from
   it, as TCP does, instead of a fixed 50 ms.
