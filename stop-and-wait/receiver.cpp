// Stop-and-wait receiver: checks each packet, writes new ones to the file,
// and ACKs everything that arrives intact.
//
// Usage: ./receiver <port> <outputFile> [ackLossRate] [ackCorruptRate]
//   e.g. ./receiver 9000 received.jpg 0.1 0.05

#include "common.hpp"

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
    int delivered = 0, duplicates = 0, corrupted = 0;
    bool finished = false;

    while (true) {
        // 2. Wait for a packet, remembering who sent it so we can reply.
        //    Once finished, wait at most LINGER_MS: if our last ACK was lost,
        //    the sender will resend the end-of-file packet and we must ACK again.
        Packet pkt;
        sockaddr_in sender{};
        socklen_t senderLen = sizeof sender;
        int n = recvfrom(sock, (char*)&pkt, sizeof pkt, 0, (sockaddr*)&sender, &senderLen);
        if (n < 0 && finished) break;  // quiet for LINGER_MS: sender is done
        if (n < HEADER_SIZE) continue;
        toHostOrder(pkt);

        // 3. Corrupted? Drop it and send nothing. The sender will time out and resend.
        if (pkt.len > DATA_SIZE || n != HEADER_SIZE + pkt.len || computeChecksum(pkt) != pkt.checksum) {
            corrupted++;
            continue;
        }

        // 4. New packet: save it. Old packet: our ACK was lost, so it was resent;
        //    don't write it twice, but do ACK it again.
        if (pkt.seq == expected) {
            out.write(pkt.data, pkt.len);
            expected++;
            delivered++;
            if (pkt.len == 0) {  // end-of-file packet
                finished = true;
                out.close();
                setRecvTimeout(sock, LINGER_MS);
            }
        } else {
            duplicates++;
        }

        // 5. ACK it (through the bad network: the ACK may be lost or corrupted).
        Packet ack;
        int ackSize = makeAck(ack, pkt.seq);
        unreliableSend(sock, ack, ackSize, sender, ackLossRate, ackCorruptRate);
    }

    std::cout << "Done. New packets: " << delivered << ", duplicates: " << duplicates
              << ", corrupted: " << corrupted << "\n";
    closeSocket(sock);
    return 0;
}
