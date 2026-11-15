# Reliable File Transfer over UDP (Go-Back-N), C++

The next step after the stop-and-wait version. Stop-and-wait keeps only **one**
packet in flight, so the link sits idle while the sender waits for each ACK.
**Go-Back-N** keeps up to **N packets in flight** at once (the "window"), and
only falls back to resending when something goes wrong.

Same packet format (data and ACKs both checksummed), and the same "bad network"
test mode, which drops and corrupts packets in both directions. About 300 lines
of C++, standard POSIX sockets / Winsock, **single-threaded**.

## Files

| File | What it does |
|---|---|
| `common.hpp` | Packet layout, checksum, byte-order helpers, fault injection, ACK helpers (same as stop-and-wait) |
| `sender.cpp` | Keeps a window of up to N packets in flight, slides it on ACKs, resends the window on timeout |
| `receiver.cpp` | Accepts packets only in order, sends cumulative ACKs |

## What changed from stop-and-wait

| | Stop-and-wait | Go-Back-N |
|---|---|---|
| Packets in flight | 1 | Up to N (`window`) |
| Sender state | current `seq` | `base` (oldest unACKed) and `nextSeq` (next new packet) |
| ACK meaning | "I got packet k" | **Cumulative**: "I have every packet before k" |
| Timer | one per packet | **one** timer, for `base` |
| On timeout | resend 1 packet | resend **every** packet from `base` to `nextSeq − 1` |
| Receiver | write if new, ACK it | write **only the next expected packet**, drop everything else, ACK `expected` |

## How it works

```
Window N = 4

Sender                                      Receiver (expected = 0)
  send 0, 1, 2, 3  ───────────────────►
                     0 ok, 1 ✗ lost, 2 & 3 arrive out of order
                 ◄───────────────────   ACK 1   (after 0: "have everything before 1")
                 ◄───────────────────   ACK 1   (2 dropped, still expecting 1)
                 ◄───────────────────   ACK 1   (3 dropped)
  base = 1, window slides: send 4
  ... timer for packet 1 expires ...
  go back: resend 1, 2, 3, 4 ─────────►   all in order now → written
                 ◄───────────────────   ACK 2, ACK 3, ACK 4, ACK 5
```

### Sender loop (one thread, no locks)

```
while not (file finished and everything ACKed):
    while window has room:  read 1 KB, send it, keep a copy, nextSeq++
    wait for an ACK, but only until base's timer runs out
    corrupted ACK             →  ignore it, keep waiting
    ACK k arrived (k > base)  →  drop packets base..k-1, base = k, restart timer
    timer expired             →  resend every packet in the window, restart timer
```

The receive timeout is set to "time left on the timer" before each `recv()`, so
one loop handles sending, ACKs and timeouts. No threads are needed.

### Why ACKs need a checksum here
A cumulative ACK moves the window. If a bit flip turned "ACK 5" into "ACK 7",
the sender would throw away packets 5 and 6 although the receiver never got
them, and the transfer would stall. So ACKs use the same checksummed 8-byte
header as data (with `len = 0`), and the sender ignores any ACK that fails the
check.

### Ending cleanly
The last packet has `len = 0`. After writing it, the receiver **lingers for
500 ms**, answering any resends. Without this, if the final ACK were lost, the
sender would keep resending to a receiver that had already exited.

## Build and run

**Linux / Mac / WSL:**

```bash
make

# Terminal 1: receiver. Optional: ACK loss rate, ACK corruption rate
./receiver 9000 received.jpg 0.1 0.05

# Terminal 2: sender. Arguments: window size, packet loss rate, corruption rate
./sender 127.0.0.1 9000 photo.jpg 8 0.1 0.05

cmp photo.jpg received.jpg && echo identical
```

**Windows (MinGW g++):**

```bat
g++ -std=c++17 -O2 -o receiver.exe receiver.cpp -lws2_32
g++ -std=c++17 -O2 -o sender.exe sender.cpp -lws2_32

:: Window 1
receiver.exe 9000 received.jpg 0.1 0.05
:: Window 2
sender.exe 127.0.0.1 9000 photo.jpg 8 0.1 0.05

fc /b photo.jpg received.jpg
```

## Results

1 MB file (978 packets) over loopback, 50 ms timeout. Loss and corruption are
applied to both data packets and ACKs. **The file arrived byte-for-byte
identical in every run.** Edge cases also pass under 20% loss and 5%
corruption: an empty file, a 1-byte file, and files of exactly 1 KB,
1 KB + 1 byte and 2 KB.

**Transfer time (seconds)**

| Loss (each direction) | Corruption (each direction) | Stop-and-wait | GBN N=4 | GBN N=8 | GBN N=16 |
|---:|---:|---:|---:|---:|---:|
| 10% | 0% | 13.7 | 6.9 | 6.6 | 6.2 |
| 20% | 0% | 31.6 | 14.8 | 15.8 | 14.0 |
| 10% | 5% | 21.2 | 9.6 | 8.1 | 8.8 |
| 20% | 5% | 41.4 | 17.7 | 18.0 | 16.7 |

**Retransmissions**

| Loss | Corruption | Stop-and-wait | GBN N=4 | GBN N=8 | GBN N=16 |
|---:|---:|---:|---:|---:|---:|
| 10% | 0% | 243 | 482 | 928 | 1741 |
| 20% | 0% | 561 | 1043 | 2221 | 3918 |
| 20% | 5% | 739 | 1248 | 2537 | 4667 |

**Timeouts** (10% loss, no corruption): stop-and-wait 243, GBN N=8 116.

With no loss, every version finishes 1 MB in about 0.01 s on loopback; the
difference there is too small to measure reliably.

### What the numbers show

1. **Go-Back-N is about 2–2.5× faster under loss.** Nearly all the time is
   spent waiting out 50 ms timeouts, so speed tracks the number of timeouts.
   Stop-and-wait times out whenever a packet *or* its ACK is lost (about 19% of
   sends at 10% loss). In Go-Back-N a lost ACK usually doesn't matter, because
   the next cumulative ACK covers it, so timeouts come mainly from lost data
   packets. That halves the timeouts (243 → 116), and the time with them.
2. **Bigger windows send many more retransmissions.** One lost packet makes the
   sender resend the *whole* window, including packets that arrived fine. At N=16
   that's ~7× the retransmissions of stop-and-wait. This is the known weakness of
   Go-Back-N, and exactly what **Selective Repeat** fixes (resend only the lost packet).
3. **Going from N=4 to N=16 barely helps here.** On loopback the round trip is
   well under 1 ms, so even a small window keeps the link busy; the time is
   dominated by the 50 ms timeouts after losses. On a real network with a long
   round-trip time, larger windows matter much more (throughput ≈ window ÷ RTT).
   A shorter, **adaptive timeout** based on measured RTT, or **fast retransmit**
   (resend on repeated duplicate ACKs, as TCP does), would cut the remaining time.
