// Go-Back-N sender: keeps up to WINDOW packets in flight at once.
//
// Usage: ./sender <host> <port> <file> [window] [lossRate] [corruptRate]
//   e.g. ./sender 127.0.0.1 9000 photo.jpg 8 0.1 0.05
//
// How it works (all in one thread, no locks needed):
//   - base    = oldest packet that is not yet ACKed
//   - nextSeq = next new packet to send
//   - Packets base .. nextSeq-1 are "in flight" and kept in a buffer.
//   - The receiver sends cumulative ACKs: "ACK k" means "I have every packet
//     before k". So one ACK can confirm many packets at once.
//   - There is ONE timer, for the oldest unACKed packet (base). If it expires,
//     the whole window is resent ("go back N").

#include "common.hpp"

#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>

using Clock = std::chrono::steady_clock;

const int TIMEOUT_MS = 50;       // resend the window if base isn't ACKed in time
const int MAX_TIMEOUTS = 20;     // give up after this many timeouts in a row

double lossRate = 0, corruptRate = 0;
int packetsSent = 0, retransmissions = 0, timeouts = 0;

// Sends through a deliberately bad "network": may drop the packet,
// or flip one random bit in it. (Same as the stop-and-wait version.)
void unreliableSend(sock_t sock, const Packet& pkt, int size, const sockaddr_in& dest) {
    if (chance(lossRate)) return;  // packet lost

    Packet copy = pkt;
    if (chance(corruptRate)) {
        char* bytes = (char*)&copy;
        bytes[rand() % size] ^= (1 << (rand() % 8));  // flip one bit
    }
    sendto(sock, (const char*)&copy, size, 0, (const sockaddr*)&dest, sizeof dest);
}

// A packet waiting in the window: ready to send (already in network order).
struct Slot {
    Packet pkt;
    int size;
};

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <host> <port> <file> [window] [lossRate] [corruptRate]\n";
        return 1;
    }
    const char* host = argv[1];
    int port = std::atoi(argv[2]);
    const char* path = argv[3];
    int window = (argc > 4) ? std::atoi(argv[4]) : 8;
    if (argc > 5) lossRate = std::atof(argv[5]);
    if (argc > 6) corruptRate = std::atof(argv[6]);
    if (window < 1) window = 1;
    srand(42);  // fixed seed so runs are repeatable

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Cannot open " << path << "\n";
        return 1;
    }

    // 1. Create a UDP socket.
    sock_t sock = socket(AF_INET, SOCK_DGRAM, 0);

    // 2. Receiver's address.
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = inet_addr(host);
    if (dest.sin_addr.s_addr == INADDR_NONE) {
        std::cerr << "Bad IP address: " << host << "\n";
        return 1;
    }

    uint32_t base = 0;          // oldest unACKed packet
    uint32_t nextSeq = 0;       // next new packet to send
    bool fileDone = false;      // true once the end-of-file packet (len 0) is queued
    uint32_t lastSeq = 0;       // sequence number of the end-of-file packet
    std::deque<Slot> inFlight;  // packets base .. nextSeq-1, oldest first
    Clock::time_point timerStart = Clock::now();
    int timeoutsInARow = 0;
    long totalBytes = 0;

    auto start = Clock::now();
    while (!(fileDone && base > lastSeq)) {
        // 3. Fill the window: send new packets while there is room.
        while (!fileDone && nextSeq < base + (uint32_t)window) {
            Slot s{};
            file.read(s.pkt.data, DATA_SIZE);
            s.pkt.len = (uint16_t)file.gcount();
            s.pkt.seq = nextSeq;
            s.size = HEADER_SIZE + s.pkt.len;
            totalBytes += s.pkt.len;
            if (s.pkt.len == 0) {  // end-of-file packet
                fileDone = true;
                lastSeq = nextSeq;
            }
            s.pkt.checksum = computeChecksum(s.pkt);
            toNetworkOrder(s.pkt);

            if (base == nextSeq) timerStart = Clock::now();  // window was empty: start timer
            unreliableSend(sock, s.pkt, s.size, dest);
            packetsSent++;
            inFlight.push_back(s);
            nextSeq++;
        }

        // 4. Wait for an ACK, but only until the timer for base runs out.
        int elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                          Clock::now() - timerStart).count();
        int remaining = TIMEOUT_MS - elapsed;
        if (remaining > 0) {
            setRecvTimeout(sock, remaining);
            uint32_t ack;
            if (recv(sock, (char*)&ack, sizeof ack, 0) == sizeof ack) {
                uint32_t k = ntohl(ack);  // "I have every packet before k"
                if (k > base && k <= nextSeq) {
                    // 5. Slide the window: drop every packet the ACK confirms.
                    while (base < k) {
                        inFlight.pop_front();
                        base++;
                    }
                    timeoutsInARow = 0;
                    timerStart = Clock::now();  // restart timer for the new base
                }
                // An old or duplicate ACK (k <= base): ignore it.
                continue;
            }
        }

        // 6. Timer expired: resend every packet in the window.
        timeouts++;
        if (++timeoutsInARow > MAX_TIMEOUTS) {
            if (fileDone && base == lastSeq) {
                // All data was ACKed; only the end-of-file ACK went missing.
                std::cout << "Note: no ACK for end-of-file packet (receiver probably finished)\n";
                break;
            }
            std::cerr << "Gave up: no progress at packet " << base << " after "
                      << MAX_TIMEOUTS << " timeouts\n";
            closeSocket(sock);
            return 1;
        }
        for (const Slot& s : inFlight) {
            unreliableSend(sock, s.pkt, s.size, dest);
            packetsSent++;
            retransmissions++;
        }
        timerStart = Clock::now();
    }

    double secs = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Sent " << totalBytes << " bytes in " << lastSeq + 1 << " packets, "
              << secs << " s (window " << window << ")\n";
    std::cout << "Transmissions: " << packetsSent << " (" << retransmissions
              << " retransmissions, " << timeouts << " timeouts)\n";
    closeSocket(sock);
    return 0;
}
