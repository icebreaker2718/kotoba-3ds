// net.hpp - tiny network client for the PC OCR server (tools/ocr_server.py).
// Plain BSD sockets: on the 3DS through libctru's soc:U service, natively for the tests.
// Everything has a timeout, and every failure returns an error string so the caller can
// fall back to manual lookup.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/dict.hpp"  // kt::Reader

namespace kt {

// Request body from memory.
class MemReader : public Reader {
public:
    explicit MemReader(const std::string &data) : data_(data) {}
    bool read(uint32_t off, void *dst, uint32_t len) override {
        if ((uint64_t)off + len > data_.size()) return false;
        for (uint32_t i = 0; i < len; i++) ((char *)dst)[i] = data_[off + i];
        return true;
    }
    uint32_t size() override { return (uint32_t)data_.size(); }
private:
    std::string data_;
};

// Starts/stops the socket service (no-ops off the 3DS). Call netStop() when done, so nothing
// network-related is left open while the game runs or exits.
bool netStart(std::string &error);
void netStop();

// TCP connection to an IP address or host name (DNS), with a timeout. Returns the socket
// (blocking) or -1 with `error` set.
int netConnect(const std::string &host, int port, int timeoutMs, std::string &error);
void netClose(int s);
// Waits until the socket can be read (or written). false = timeout.
bool netWait(int s, bool forRead, int timeoutMs);

// Broadcasts "KOTOBA3DS?" on UDP `port` and waits for a server's reply.
bool netDiscover(int port, int timeoutMs, std::string &hostOut, int &portOut, std::string &error);

// Quick reachability check: GET /ping.
bool netPing(const std::string &host, int port, int timeoutMs, std::string &error);

// POSTs `body` (read from `src`, streamed) to http://host:port/path and returns the body.
bool netPost(const std::string &host, int port, const std::string &path,
             const std::vector<std::string> &headers, Reader &src,
             int connectTimeoutMs, int replyTimeoutMs, std::string &responseBody, std::string &error);

// Parses the server reply: "KOTOBA1\n<sentence>\n<chunk>\t<chunk>...\n".
bool parseOcrReply(const std::string &body, std::string &sentence, std::vector<std::string> &chunks,
                   std::string &error);

// "192.168.1.20:7331" -> host/port. Port defaults to 7331.
bool parseHostPort(const std::string &s, std::string &host, int &port);

}  // namespace kt
