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
// The socket service needs one page-aligned block from the plugin's heap; after a long session
// the heap is in pieces, so smaller blocks are tried when the full size isn't there.
// netReserve() (at game start, while the heap is in one piece): takes the block now and keeps it for the
// whole session (128 KB), so networking doesn't need a big block from a heap the reader has filled.
bool netReserve();
bool netReserved();
// Lends netStart() memory owned by someone else (the recognizer's workers' block, idle outside reads): the
// largest page-aligned 512 / 256 / 128 KB inside [p, p + bytes) is used for the socket service from then on,
// instead of a reserved or newly taken block. While the service runs (netStart .. netStop) that memory belongs to
// it: the lender must not touch it. nullptr withdraws the loan (only while the service is stopped).
void netLend(void *p, uint32_t bytes);
bool netUp();
bool netInLentMemory();  // the running service uses netLend's memory
bool netStart(std::string &error);
void netStop();
// Size of the block the socket service runs on (the reserved one, or the running service's; 0 otherwise), for the log.
uint32_t netBufferSize();

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

// POSTs `body` (read from `src`, streamed) to http://host:port/path and returns the body (at
// most maxReply bytes).
bool netPost(const std::string &host, int port, const std::string &path,
             const std::vector<std::string> &headers, Reader &src,
             int connectTimeoutMs, int replyTimeoutMs, std::string &responseBody, std::string &error,
             size_t maxReply = 256 * 1024);

// The same, but the reply's body goes to `sink` as it arrives (nothing big held in memory): for a long answer
// such as the known-words list. write() returning false stops it (error left to the sink's owner to say).
// A reply other than 200 is read into `error` as netPost does.
struct ReplySink {
    virtual ~ReplySink() {}
    virtual bool write(const char *p, size_t n) = 0;
};
bool netPostToSink(const std::string &host, int port, const std::string &path,
                   const std::vector<std::string> &headers, Reader &src, int connectTimeoutMs, int replyTimeoutMs,
                   ReplySink &sink, std::string &error, uint64_t maxReply);

// Parses the server reply: "KOTOBA1\n<sentence>\n<chunk>\t<chunk>...\n".
bool parseOcrReply(const std::string &body, std::string &sentence, std::vector<std::string> &chunks,
                   std::string &error);

// "192.168.1.20:7331" -> host/port. Port defaults to 7331.
bool parseHostPort(const std::string &s, std::string &host, int &port);

}  // namespace kt
