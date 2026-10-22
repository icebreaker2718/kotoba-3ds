#include "core/net.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#endif

namespace kt {

#ifdef __3DS__
static u32 *g_socBuf = nullptr;  // allocated by netStart, freed by netStop (never at exit)
static bool g_socUp = false;
static const u32 kSocBufSize = 0x80000;
static int closeSock(int s) { return closesocket(s); }
#else
static int closeSock(int s) { return close(s); }
#endif

bool netStart(std::string &error) {
#ifdef __3DS__
    if (g_socUp) return true;
    if (!g_socBuf) g_socBuf = (u32 *)memalign(0x1000, kSocBufSize);
    if (!g_socBuf) { error = "not enough memory for networking"; return false; }
    Result r = socInit(g_socBuf, kSocBufSize);
    if (R_FAILED(r)) {
        char buf[200];
        if ((u32)r == 0xE0A01BF5)  // kernel: invalid address -> plugin memory isn't PRIVATE
            snprintf(buf, sizeof(buf),
                     "networking needs Luma3DS v13.2 or newer (socInit %08lX).\n"
                     "Update Luma, then try again.", (unsigned long)r);
        else
            snprintf(buf, sizeof(buf), "network unavailable in this game (socInit %08lX)", (unsigned long)r);
        error = buf;
        free(g_socBuf);
        g_socBuf = nullptr;
        return false;
    }
    g_socUp = true;
#else
    (void)error;
#endif
    return true;
}

void netStop() {
#ifdef __3DS__
    if (g_socUp) {
        socExit();
        g_socUp = false;
        // give the 512 KB back: CTRPF's screenshots need room in the plugin's memory
        free(g_socBuf);
        g_socBuf = nullptr;
    }
#endif
}

bool parseHostPort(const std::string &s, std::string &host, int &port) {
    std::string t;
    for (char c : s) if (c != ' ') t += c;
    size_t colon = t.find(':');
    host = t.substr(0, colon);
    port = 7331;
    if (colon != std::string::npos) port = atoi(t.c_str() + colon + 1);
    in_addr a;
    return !host.empty() && port > 0 && port < 65536 && inet_pton(AF_INET, host.c_str(), &a) == 1;
}

static bool waitFor(int s, short events, int timeoutMs) {
    pollfd p;
    p.fd = s;
    p.events = events;
    p.revents = 0;
    int r = poll(&p, 1, timeoutMs);
    return r > 0 && (p.revents & (events | POLLHUP | POLLERR));
}

static std::string errStr(const char *what, int code) {
    char buf[96];
    snprintf(buf, sizeof(buf), " (%s, error %d)", what, code);
    return buf;
}

static int connectTo(const std::string &host, int port, int timeoutMs, std::string &error) {
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    bool named = inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1;
    const std::string who = named ? host : "the PC at " + host;
    if (named) {
        hostent *he = gethostbyname(host.c_str());  // a name (internet services), not an IP
        if (!he || he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0]) {
            error = "could not look up " + host + " (no internet connection?)";
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { error = "could not open a socket" + errStr("socket", errno); return -1; }
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) flags = 0;
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int r = connect(s, (sockaddr *)&addr, sizeof(addr));
    if (r < 0 && errno != EISCONN) {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY) {
            error = "could not reach " + who + errStr("connect", errno);
            closeSock(s);
            return -1;
        }
        // Wait until the socket is writable, then confirm by connecting again: EISCONN means
        // connected. (SO_ERROR is not reliable on the 3DS, so we don't use it.)
        bool connected = false;
        int lastErr = errno;
        for (int waited = 0; waited < timeoutMs && !connected;) {
            int slice = timeoutMs - waited < 250 ? timeoutMs - waited : 250;
            waitFor(s, POLLOUT, slice);
            waited += slice;
            int r2 = connect(s, (sockaddr *)&addr, sizeof(addr));
            if (r2 == 0 || errno == EISCONN) {
                connected = true;
            } else if (errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK) {
                lastErr = errno;  // still connecting
            } else {
                error = "could not reach " + who + (named ? "" : " (is ocr_server.py running?)") +
                        errStr("connect", errno);
                closeSock(s);
                return -1;
            }
        }
        if (!connected) {
            error = who + " did not answer" + errStr("timeout", lastErr);
            closeSock(s);
            return -1;
        }
    }
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);  // blocking from here on; reads still use poll()
    return s;
}

static int g_lastSendErr = 0;

static bool sendAll(int s, const char *data, size_t len, int timeoutMs) {
    (void)timeoutMs;  // the socket is blocking here; a dead peer makes send() fail
    while (len > 0) {
        size_t piece = len > 8192 ? 8192 : len;
        ssize_t n = send(s, data, piece, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            waitFor(s, POLLOUT, 200);
            continue;
        }
        if (n <= 0) { g_lastSendErr = n < 0 ? errno : -1; return false; }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

static bool readReply(int s, int timeoutMs, std::string &body, std::string &error) {
    std::string raw;
    char buf[2048];
    size_t headerEnd = std::string::npos;
    long contentLength = -1;
    while (true) {
        if (!waitFor(s, POLLIN, timeoutMs)) { error = "the PC took too long to answer"; return false; }
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n < 0) { error = "connection error while reading the answer" + errStr("recv", errno); return false; }
        if (n == 0) break;
        raw.append(buf, (size_t)n);
        if (raw.size() > 256 * 1024) { error = "answer too large"; return false; }
        if (headerEnd == std::string::npos) {
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                size_t p = raw.find("Content-Length:");
                if (p == std::string::npos) p = raw.find("content-length:");
                if (p != std::string::npos && p < headerEnd) contentLength = atol(raw.c_str() + p + 15);
            }
        }
        if (headerEnd != std::string::npos && contentLength >= 0 &&
            raw.size() >= headerEnd + 4 + (size_t)contentLength)
            break;
    }
    if (headerEnd == std::string::npos) { error = "the PC sent an unexpected answer"; return false; }
    int status = 0;
    if (raw.compare(0, 5, "HTTP/") == 0) {
        size_t sp = raw.find(' ');
        if (sp != std::string::npos) status = atoi(raw.c_str() + sp + 1);
    }
    body = raw.substr(headerEnd + 4, contentLength >= 0 ? (size_t)contentLength : std::string::npos);
    if (status != 200) {
        // server sends "ERR\n<message>"
        size_t nl = body.find('\n');
        error = "OCR server error: " + (nl != std::string::npos ? body.substr(nl + 1) : body);
        while (!error.empty() && (error.back() == '\n' || error.back() == '\r')) error.pop_back();
        return false;
    }
    return true;
}

int netConnect(const std::string &host, int port, int timeoutMs, std::string &error) {
    return connectTo(host, port, timeoutMs, error);
}
void netClose(int s) { if (s >= 0) closeSock(s); }
bool netWait(int s, bool forRead, int timeoutMs) { return waitFor(s, forRead ? POLLIN : POLLOUT, timeoutMs); }

bool netPing(const std::string &host, int port, int timeoutMs, std::string &error) {
    int s = connectTo(host, port, timeoutMs, error);
    if (s < 0) return false;
    std::string req = "GET /ping HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    std::string body;
    bool ok = sendAll(s, req.data(), req.size(), timeoutMs);
    if (!ok) error = "could not talk to the PC at " + host + errStr("send", g_lastSendErr);
    if (ok) ok = readReply(s, timeoutMs, body, error);
    closeSock(s);
    if (ok && body.compare(0, 9, "KOTOBA3DS") != 0) { error = "that address is not the Kotoba3DS OCR server"; ok = false; }
    return ok;
}

bool netPost(const std::string &host, int port, const std::string &path,
             const std::vector<std::string> &headers, Reader &src, int connectTimeoutMs, int replyTimeoutMs,
             std::string &responseBody, std::string &error) {
    int s = connectTo(host, port, connectTimeoutMs, error);
    if (s < 0) return false;
    uint32_t size = src.size();
    char lenbuf[32];
    snprintf(lenbuf, sizeof(lenbuf), "%lu", (unsigned long)size);
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n" +
                      "Content-Type: application/octet-stream\r\nContent-Length: " + lenbuf + "\r\n";
    for (const std::string &h : headers) req += h + "\r\n";
    req += "\r\n";
    bool ok = sendAll(s, req.data(), req.size(), connectTimeoutMs);
    if (!ok) error = "could not talk to the PC at " + host + errStr("send", g_lastSendErr);
    std::vector<char> chunk(16 * 1024);
    for (uint32_t off = 0; ok && off < size;) {
        uint32_t n = size - off < chunk.size() ? size - off : (uint32_t)chunk.size();
        if (!src.read(off, chunk.data(), n)) { error = "could not read the screenshot"; ok = false; break; }
        if (!sendAll(s, chunk.data(), n, connectTimeoutMs * 2)) {
            error = "upload to the PC failed" + errStr("send", g_lastSendErr);
            ok = false;
            break;
        }
        off += n;
    }
    if (ok) ok = readReply(s, replyTimeoutMs, responseBody, error);
    closeSock(s);
    return ok;
}

bool netDiscover(int port, int timeoutMs, std::string &hostOut, int &portOut, std::string &error) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { error = "could not open a socket"; return false; }
#ifndef __3DS__
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
#endif
    sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    bind(s, (sockaddr *)&local, sizeof(local));

    std::vector<uint32_t> targets;
    targets.push_back(htonl(INADDR_BROADCAST));
#ifdef __3DS__
    uint32_t me = (uint32_t)gethostid();  // our IP, network order; also try the /24 broadcast
    if (me) targets.push_back((me & htonl(0xFFFFFF00)) | htonl(0xFF));
#else
    targets.push_back(htonl(INADDR_LOOPBACK));  // lets the native tests find a local server
#endif
    const char *ask = "KOTOBA3DS?";
    for (uint32_t t : targets) {
        sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = t;
        sendto(s, ask, strlen(ask), 0, (sockaddr *)&to, sizeof(to));
    }
    bool found = false;
    char buf[64];
    while (!found && waitFor(s, POLLIN, timeoutMs)) {
        sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
        if (n <= 0) break;
        buf[n] = 0;
        if (strncmp(buf, "KOTOBA3DS!", 10) == 0) {
            char ip[32];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            hostOut = ip;
            portOut = n > 11 ? atoi(buf + 11) : port;
            if (portOut <= 0) portOut = port;
            found = true;
        }
    }
    closeSock(s);
    if (!found) error = "no PC running ocr_server.py was found on this network";
    return found;
}

bool parseOcrReply(const std::string &body, std::string &sentence, std::vector<std::string> &chunks,
                   std::string &error) {
    sentence.clear();
    chunks.clear();
    if (body.compare(0, 8, "KOTOBA1\n") != 0) { error = "unexpected answer from the OCR server"; return false; }
    size_t a = 8, b = body.find('\n', a);
    sentence = body.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (b != std::string::npos) {
        size_t c = body.find('\n', b + 1);
        std::string line = body.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1);
        size_t start = 0;
        while (start <= line.size() && !line.empty()) {
            size_t t = line.find('\t', start);
            std::string piece = line.substr(start, t == std::string::npos ? std::string::npos : t - start);
            if (!piece.empty()) chunks.push_back(piece);
            if (t == std::string::npos) break;
            start = t + 1;
        }
    }
    std::string joined;
    for (const std::string &c : chunks) joined += c;
    if (joined != sentence) {  // fall back to one chunk per character
        chunks.clear();
        for (size_t i = 0; i < sentence.size();) {
            size_t j = i + 1;
            while (j < sentence.size() && ((unsigned char)sentence[j] & 0xC0) == 0x80) j++;
            chunks.push_back(sentence.substr(i, j - i));
            i = j;
        }
    }
    if (sentence.empty()) { error = "no text found on the screen (try cropping to the text box)"; return false; }
    return true;
}

}  // namespace kt
