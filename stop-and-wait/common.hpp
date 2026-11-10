#pragma once
// Shared by sender.cpp and receiver.cpp: the packet layout and small helpers.

#include <cstdint>
#include <cstdlib>       // rand

// ---- Platform differences (Windows Winsock vs Linux/Mac POSIX sockets) ----
#ifdef _WIN32
    #define _WINSOCK_DEPRECATED_NO_WARNINGS   // Visual Studio otherwise rejects inet_addr()
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")   // Visual Studio links Winsock automatically
    #endif
    typedef SOCKET sock_t;

    // Windows needs Winsock started before any socket call. This object is
    // created before main() runs and cleaned up after it returns.
    struct WinsockInit {
        WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
        ~WinsockInit() { WSACleanup(); }
    };
    static WinsockInit winsockInit;

    inline void closeSocket(sock_t s) { closesocket(s); }

    // On Windows the receive timeout is given in milliseconds.
    inline void setRecvTimeout(sock_t s, int ms) {
        DWORD t = ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
    }
#else
    #include <arpa/inet.h>   // htonl, ntohl, htons, ntohs, inet_addr
    #include <sys/socket.h>  // socket, sendto, recvfrom
    #include <sys/time.h>    // timeval
    #include <unistd.h>      // close
    typedef int sock_t;

    inline void closeSocket(sock_t s) { close(s); }

    // On Linux/Mac the receive timeout is a timeval (seconds + microseconds).
    inline void setRecvTimeout(sock_t s, int ms) {
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    }
#endif
// ---------------------------------------------------------------------------

const int DATA_SIZE = 1024;   // max file bytes per packet
const int HEADER_SIZE = 8;    // seq (4) + len (2) + checksum (2)

// One data packet. Only HEADER_SIZE + len bytes are actually sent.
struct Packet {
    uint32_t seq;        // packet number: 0, 1, 2, ...
    uint16_t len;        // bytes of file data; 0 means "end of file"
    uint16_t checksum;   // detects corrupted packets
    char data[DATA_SIZE];
};

// The ACK is just the sequence number being acknowledged (4 bytes).

// Simple 16-bit checksum: add up the header fields and every data byte.
// Any single flipped bit changes the sum, so the receiver can detect it.
// (Real protocols use CRC, which also catches many multi-bit errors.)
inline uint16_t computeChecksum(const Packet& p) {
    uint32_t sum = p.seq + p.len;
    for (int i = 0; i < p.len; i++) {
        sum += (uint8_t)p.data[i];
    }
    return (uint16_t)(sum + (sum >> 16));
}

// Network byte order is big-endian; x86 is little-endian, so convert
// multi-byte fields before sending and after receiving.
inline void toNetworkOrder(Packet& p) {
    p.seq = htonl(p.seq);
    p.len = htons(p.len);
    p.checksum = htons(p.checksum);
}

inline void toHostOrder(Packet& p) {
    p.seq = ntohl(p.seq);
    p.len = ntohs(p.len);
    p.checksum = ntohs(p.checksum);
}

// Returns true with probability p (0.0 to 1.0). Used to simulate a bad network.
inline bool chance(double p) {
    return (double)rand() / RAND_MAX < p;
}
