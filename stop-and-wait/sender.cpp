// Stop-and-wait sender: sends one packet, waits for its ACK, resends on timeout.
//
// Usage: ./sender <host> <port> <file> [lossRate] [corruptRate]
//   e.g. ./sender 127.0.0.1 9000 photo.jpg 0.1 0.05

#include "common.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>

const int TIMEOUT_MS = 50;   // how long to wait for an ACK before resending
const int MAX_TRIES = 20;    // give up on a packet after this many attempts

double lossRate = 0, corruptRate = 0;
int packetsSent = 0, retransmissions = 0, badAcks = 0;

// Sends one packet and waits for its ACK, resending on every timeout.
// Returns true once the matching ACK arrives.
bool sendReliably(sock_t sock, const sockaddr_in& dest, Packet pkt) {
    uint32_t seq = pkt.seq;
    int size = HEADER_SIZE + pkt.len;
    pkt.checksum = computeChecksum(pkt);
    toNetworkOrder(pkt);

    for (int attempt = 1; attempt <= MAX_TRIES; attempt++) {
        if (attempt > 1) retransmissions++;
        packetsSent++;
        unreliableSend(sock, pkt, size, dest, lossRate, corruptRate);

        // recv() returns -1 when TIMEOUT_MS passes with nothing received.
        Packet ack;
        int n;
        while ((n = recv(sock, (char*)&ack, sizeof ack, 0)) >= 0) {
            uint32_t k;
            if (!readAck(ack, n, k)) { badAcks++; continue; }  // corrupted ACK: ignore
            if (k == seq) return true;  // got it, move on
            // An ACK for an older packet (a delayed duplicate): ignore it.
        }
        // Timed out: loop around and send again.
    }
    return false;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <host> <port> <file> [lossRate] [corruptRate]\n";
        return 1;
    }
    const char* host = argv[1];
    int port = std::atoi(argv[2]);
    const char* path = argv[3];
    if (argc > 4) lossRate = std::atof(argv[4]);
    if (argc > 5) corruptRate = std::atof(argv[5]);
    srand(42);  // fixed seed so runs are repeatable

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Cannot open " << path << "\n";
        return 1;
    }

    // 1. Create a UDP socket and set the ACK timeout on it.
    sock_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    setRecvTimeout(sock, TIMEOUT_MS);

    // 2. Receiver's address.
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = inet_addr(host);  // "127.0.0.1" -> 32-bit address
    if (dest.sin_addr.s_addr == INADDR_NONE) {
        std::cerr << "Bad IP address: " << host << "\n";
        return 1;
    }

    // 3. Read the file 1 KB at a time and send each piece reliably.
    //    The last packet has len 0 and tells the receiver we're done.
    auto start = std::chrono::steady_clock::now();
    long totalBytes = 0;
    uint32_t seq = 0;
    while (true) {
        Packet pkt{};
        file.read(pkt.data, DATA_SIZE);
        pkt.len = (uint16_t)file.gcount();
        pkt.seq = seq;
        bool isLast = (pkt.len == 0);

        if (!sendReliably(sock, dest, pkt)) {
            if (isLast) {
                // All data was ACKed; only the "done" ACK went missing.
                std::cout << "Note: no ACK for end-of-file packet (receiver probably finished)\n";
                break;
            }
            std::cerr << "Gave up on packet " << seq << " after " << MAX_TRIES << " tries\n";
            closeSocket(sock);
            return 1;
        }
        if (isLast) break;
        totalBytes += pkt.len;
        seq++;
    }

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "Sent " << totalBytes << " bytes in " << seq + 1 << " packets, " << secs << " s\n";
    std::cout << "Transmissions: " << packetsSent << " (" << retransmissions << " retransmissions, " << badAcks << " corrupted ACKs ignored)\n";
    closeSocket(sock);
    return 0;
}
