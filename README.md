# Reliable File Transfer over UDP (C++)

UDP can lose, corrupt and duplicate packets. This project builds reliable file
transfer on top of plain UDP sockets, in two versions:

| Folder | Protocol | Packets in flight |
|---|---|---|
| [`stop-and-wait/`](stop-and-wait) | Stop-and-wait ARQ | 1 |
| [`go-back-n/`](go-back-n) | Go-Back-N sliding window | up to N |

Both use the same 8-byte packet header (sequence number, length, 16-bit
checksum), handle lost packets, corrupted packets and lost ACKs, and include a
fault-injection mode that randomly drops and corrupts packets to prove the
transfer still works. Standard POSIX sockets / Winsock; builds on Linux, macOS
and Windows.

## Results

1 MB file, 50 ms timeout, loss applied to both data packets and ACKs. The file
arrived byte-for-byte identical in every run.

| Loss | Corruption | Stop-and-wait | Go-Back-N (N=8) |
|---:|---:|---:|---:|
| 0% | 0% | 0.034 s | 0.005 s |
| 10% | 0% | 14.1 s | 6.5 s |
| 20% | 5% | 36.8 s | 17.8 s |

Go-Back-N is about 2× faster under loss, at the cost of more retransmissions
(one loss resends the whole window). See each folder's README for the full
results, how it works, and how to build and run it.

## Quick start (Linux / macOS / WSL)

```bash
cd go-back-n
make
./receiver 9000 received.jpg 0.1                    # terminal 1
./sender 127.0.0.1 9000 photo.jpg 8 0.1 0.05        # terminal 2
cmp photo.jpg received.jpg && echo identical
```

Windows (MinGW) build commands are in each folder's README.
