# Reliable File Transfer over UDP (Go-Back-N), C++

The next step after the stop-and-wait version. Stop-and-wait keeps only **one**
packet in flight, so the link sits idle while the sender waits for each ACK.
**Go-Back-N** keeps up to **N packets in flight** at once (the "window"), and
only falls back to resending when something goes wrong.

Same packet format, checksum and "bad network" test mode as stop-and-wait.
About 300 lines of C++, standard POSIX sockets / Winsock, **single-threaded**.

## Files

| File | What it does |
|---|---|
| `common.hpp` | Packet layout, checksum, byte-order helpers (unchanged from stop-and-wait) |
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
    ACK k arrived (k > base)  →  drop packets base..k-1, base = k, restart timer
    timer expired             →  resend every packet in the window, restart timer
```

The receive timeout is set to "time left on the timer" before each `recv()`, so
one loop handles sending, ACKs and timeouts. No threads are needed.

### Ending cleanly
The last packet has `len = 0`. After writing it, the receiver **lingers for
500 ms**, answering any resends. Without this, if the final ACK were lost, the
sender would keep resending to a receiver that had already exited.

## Build and run

**Linux / Mac / WSL:**

```bash
make

# Terminal 1: receiver. Optional 3rd argument = chance each ACK is lost
./receiver 9000 received.jpg 0.1

# Terminal 2: sender. Arguments: window size, packet loss rate, corruption rate
./sender 127.0.0.1 9000 photo.jpg 8 0.1 0.05

cmp photo.jpg received.jpg && echo identical
```

**Windows (MinGW g++):**

```bat
g++ -std=c++17 -O2 -o receiver.exe receiver.cpp -lws2_32
g++ -std=c++17 -O2 -o sender.exe sender.cpp -lws2_32

:: Window 1
receiver.exe 9000 received.jpg 0.1
:: Window 2
sender.exe 127.0.0.1 9000 photo.jpg 8 0.1 0.05

fc /b photo.jpg received.jpg
```

## Results

1 MB file (978 packets) over loopback, 50 ms timeout. Loss is applied to both
data packets and ACKs. **The file arrived byte-for-byte identical in every
run.** Edge cases also pass under 20% loss: an empty file, a 1-byte file, and
files that are an exact multiple of 1 KB.

**Transfer time (seconds)**

| Loss (each direction) | Corruption | Stop-and-wait | GBN N=4 | GBN N=8 | GBN N=16 |
|---:|---:|---:|---:|---:|---:|
| 0% | 0% | 0.034 | 0.012 | 0.005 | 0.003 |
| 10% | 0% | 14.1 | 6.8 | 6.5 | 6.1 |
| 20% | 0% | 31.6 | 14.6 | 15.6 | 13.9 |
| 10% | 5% | 17.5 | 9.5 | 8.1 | 8.7 |
| 20% | 5% | 36.8 | 17.5 | 17.8 | 16.5 |

**Retransmissions**

| Loss | Corruption | Stop-and-wait | GBN N=4 | GBN N=8 | GBN N=16 |
|---:|---:|---:|---:|---:|---:|
| 10% | 0% | 251 | 482 | 928 | 1741 |
| 20% | 0% | 564 | 1042 | 2221 | 3918 |
| 20% | 5% | 657 | 1252 | 2536 | 4668 |

### What the numbers show

1. **Go-Back-N is about 2–2.3× faster under loss**, and 7–10× faster with no loss,
   because the sender no longer waits for each ACK before sending the next packet.
2. **Bigger windows send many more retransmissions.** One lost packet makes the
   sender resend the *whole* window, including packets that arrived fine. At N=16
   that's ~7× the retransmissions of stop-and-wait. This is the known weakness of
   Go-Back-N, and exactly what **Selective Repeat** fixes (resend only the lost packet).
3. **Going from N=4 to N=16 barely helps here.** On loopback the round trip is
   well under 1 ms, so even a small window keeps the link busy; the time is
   dominated by the 50 ms timeouts after losses. On a real network with a long
   round-trip time, larger windows matter much more (throughput ≈ window ÷ RTT).
   A shorter, **adaptive timeout** based on measured RTT would cut the remaining time.

## Interview prep

**60-second explanation**

> "I first built reliable file transfer over UDP with stop-and-wait, then
> upgraded it to Go-Back-N. Instead of one packet at a time, the sender keeps up
> to N packets in flight. The receiver only accepts packets in order and sends
> cumulative ACKs, meaning 'I have everything before k'. There's a single timer
> for the oldest unACKed packet; if it expires, the sender resends the whole
> window. Under 10% loss it was about 2× faster than stop-and-wait, but
> retransmissions grew with the window size, because one loss forces the whole
> window to be resent. That's the trade-off Selective Repeat addresses."

**Likely questions**

- **Why cumulative ACKs?** One ACK can confirm many packets, and a lost ACK
  doesn't matter if a later one arrives, because the later ACK covers it.
- **Why does the receiver drop out-of-order packets?** It keeps the receiver
  simple: no buffer, it only tracks `expected`. The cost is that the sender
  must resend them later.
- **Why only one timer?** Packets are always resent as a block starting at
  `base`, so only the oldest one's timer matters.
- **Did you use threads?** No. One loop sets the receive timeout to the time
  left on the timer, so sending, ACK handling and timeouts all happen in one
  thread with no locks. A two-thread version (sender + ACK listener) would need
  a mutex around `base` and the buffer.
- **Max window size?** With k-bit sequence numbers, N ≤ 2^k − 1. Otherwise the
  receiver can't tell a new packet from an old one with the same number. Here
  sequence numbers are 32-bit, so it's never a limit in practice.
- **How do you choose N?** About bandwidth × RTT ÷ packet size (the
  bandwidth-delay product), so the pipe stays full.
- **Go-Back-N vs Selective Repeat?** Selective Repeat ACKs each packet
  individually, buffers out-of-order packets at the receiver, keeps a timer per
  packet and resends only what's lost. Fewer retransmissions, more bookkeeping.
  Its window must be ≤ half the sequence space.
- **What does TCP use?** A mix: cumulative ACKs like Go-Back-N, but receivers
  buffer out-of-order data, and SACK lets the sender resend only missing
  segments, closer to Selective Repeat.
