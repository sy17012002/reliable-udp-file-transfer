// Go-Back-N receiver: accepts packets only in order and sends cumulative ACKs.
//
// Usage: ./receiver <port> <outputFile> [ackLossRate] [ackCorruptRate]
//   e.g. ./receiver 9000 received.jpg 0.1 0.05
//
// The receiver is even simpler than in stop-and-wait:
//   - If the packet is the next one expected, write it and move on.
//   - Anything else (a gap after a lost packet, or a duplicate) is dropped.
//   - After every valid packet, reply "ACK expected", meaning
//     "I have every packet before this number". The sender resends the rest.

#include "common.hpp"

#include <chrono>
#include <fstream>
#include <iostream>

const int LINGER_MS = 500;  // after the last packet, keep answering resends for a while

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <port> <outputFile> [ackLossRate] [ackCorruptRate]\n";
        return 1;
    }
    int port = std::atoi(argv[1]);
    std::ofstream out(argv[2], std::ios::binary);
    double ackLossRate = (argc > 3) ? std::atof(argv[3]) : 0;
    double ackCorruptRate = (argc > 4) ? std::atof(argv[4]) : 0;
    srand(7);

    // 1. Create a UDP socket and bind it to the port so the sender can reach us.
    sock_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(sock, (sockaddr*)&addr, sizeof addr) < 0) {
        std::cerr << "Cannot bind port " << port << "\n";
        return 1;
    }
    std::cout << "Waiting on UDP port " << port << "...\n";

    uint32_t expected = 0;  // next packet number we want
    int delivered = 0, outOfOrder = 0, corrupted = 0;
    bool finished = false;

    while (true) {
        // 2. Wait for a packet. Once finished, wait at most LINGER_MS: if the
        //    sender lost our last ACK it will resend, and we must ACK again.
        Packet pkt;
        sockaddr_in sender{};
        socklen_t senderLen = sizeof sender;
        int n = recvfrom(sock, (char*)&pkt, sizeof pkt, 0, (sockaddr*)&sender, &senderLen);
        if (n < 0 && finished) break;  // nothing more for LINGER_MS: sender is done
        if (n < HEADER_SIZE) continue;
        toHostOrder(pkt);

        // 3. Corrupted? Drop it and send nothing. The sender will time out and resend.
        if (pkt.len > DATA_SIZE || n != HEADER_SIZE + pkt.len || computeChecksum(pkt) != pkt.checksum) {
            corrupted++;
            continue;
        }

        // 4. In order: write it. Out of order or duplicate: drop it.
        if (pkt.seq == expected) {
            out.write(pkt.data, pkt.len);
            expected++;
            delivered++;
            if (pkt.len == 0 && !finished) {  // end-of-file packet
                finished = true;
                out.close();
                setRecvTimeout(sock, LINGER_MS);
            }
        } else {
            outOfOrder++;
        }

        // 5. Cumulative ACK: "I have every packet before `expected`".
        //    Sent through the bad network: it may be lost or corrupted.
        Packet ack;
        int ackSize = makeAck(ack, expected);
        unreliableSend(sock, ack, ackSize, sender, ackLossRate, ackCorruptRate);
    }

    std::cout << "Done. New packets: " << delivered << ", out-of-order/duplicates dropped: "
              << outOfOrder << ", corrupted: " << corrupted << "\n";
    closeSocket(sock);
    return 0;
}
