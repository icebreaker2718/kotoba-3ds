#include "core/https.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <errno.h>
#include <sys/socket.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"  // error codes only
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "core/google_roots.hpp"
#include "core/net.hpp"

#ifdef __3DS__
#include <3ds.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kt {

// ---- randomness for the TLS handshake ----
static int Entropy(void *, unsigned char *out, size_t len) {
#ifdef __3DS__
    static bool psTried = false, psOk = false;
    if (!psTried) { psTried = true; psOk = R_SUCCEEDED(psInit()); }
    if (psOk && R_SUCCEEDED(PS_GenerateRandomBytes(out, len))) return 0;
    // no ps:ps in this game: mix the CPU tick counter (jitter between calls) instead
    u64 x = svcGetSystemTick() ^ (osGetTime() << 20);
    for (size_t i = 0; i < len; i++) {
        x ^= svcGetSystemTick() + (x << 7) + (x >> 3);
        x *= 0x9E3779B97F4A7C15ull;
        out[i] = (unsigned char)(x >> 56);
        for (volatile int spin = 0; spin < (int)(x & 63); spin++) {}
    }
    return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, len);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
#endif
}

// ---- socket I/O for mbedTLS, with a timeout ----
struct Conn {
    int sock = -1;
    int timeoutMs = 20000;
};

static int SendCb(void *ctx, const unsigned char *buf, size_t len) {
    Conn *c = (Conn *)ctx;
    if (!netWait(c->sock, false, c->timeoutMs)) return MBEDTLS_ERR_SSL_TIMEOUT;
    ssize_t n = send(c->sock, buf, len > 8192 ? 8192 : len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)n;
}

static int RecvCb(void *ctx, unsigned char *buf, size_t len) {
    Conn *c = (Conn *)ctx;
    if (!netWait(c->sock, true, c->timeoutMs)) return MBEDTLS_ERR_SSL_TIMEOUT;
    ssize_t n = recv(c->sock, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return (int)n;  // 0 = closed
}

// The console clock may be wrong: accept certificates that look expired / not yet valid,
// everything else (chain, signature, host name) is still checked.
static int VerifyCb(void *, mbedtls_x509_crt *, int, uint32_t *flags) {
    *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}

static std::string TlsError(const char *what, int code) {
    char buf[160];
    char msg[100];
    mbedtls_strerror(code, msg, sizeof(msg));
    snprintf(buf, sizeof(buf), "%s (-0x%04X: %s)", what, (unsigned)-code, msg);
    return buf;
}

bool dechunk(const std::string &in, std::string &out) {
    out.clear();
    size_t p = 0;
    while (p < in.size()) {
        size_t eol = in.find("\r\n", p);
        if (eol == std::string::npos) return false;
        unsigned long n = strtoul(in.c_str() + p, nullptr, 16);
        p = eol + 2;
        if (n == 0) return true;
        if (p + n > in.size()) return false;
        out.append(in, p, n);
        p += n + 2;  // data + CRLF
    }
    return false;
}

static bool HeaderValue(const std::string &head, const char *name, std::string &value) {
    size_t n = strlen(name);
    for (size_t p = head.find("\r\n"); p != std::string::npos && p + 2 < head.size(); p = head.find("\r\n", p + 2)) {
        size_t s = p + 2;
        if (head.size() - s > n && strncasecmp(head.c_str() + s, name, n) == 0 && head[s + n] == ':') {
            size_t e = head.find("\r\n", s);
            value = head.substr(s + n + 1, e == std::string::npos ? std::string::npos : e - s - n - 1);
            while (!value.empty() && value[0] == ' ') value.erase(0, 1);
            return true;
        }
    }
    return false;
}

bool httpsPost(const std::string &host, const std::string &path, const std::vector<std::string> &headers,
               const std::string &body, int timeoutMs, std::string &response, int &status, std::string &error,
               int port, const char *rootsPem) {
    status = 0;
    response.clear();
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt roots;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&roots);
    mbedtls_ctr_drbg_init(&drbg);
    Conn conn;
    conn.timeoutMs = timeoutMs;
    bool ok = false;
    int r;
    std::string raw;
    do {
        const char pers[] = "kotoba3ds-lens";
        if ((r = mbedtls_ctr_drbg_seed(&drbg, Entropy, nullptr, (const unsigned char *)pers, sizeof(pers))) != 0) {
            error = TlsError("could not start encryption", r);
            break;
        }
        if ((r = mbedtls_x509_crt_parse(&roots, (const unsigned char *)(rootsPem ? rootsPem : kGoogleRootsPem),
                                          rootsPem ? strlen(rootsPem) + 1 : sizeof(kGoogleRootsPem))) < 0) {
            error = TlsError("bad built-in certificates", r);
            break;
        }
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &roots, nullptr);
        mbedtls_ssl_conf_verify(&conf, VerifyCb, nullptr);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        if ((r = mbedtls_ssl_setup(&ssl, &conf)) != 0) { error = TlsError("could not start encryption", r); break; }
        mbedtls_ssl_set_hostname(&ssl, host.c_str());

        conn.sock = netConnect(host, port, timeoutMs < 8000 ? timeoutMs : 8000, error);
        if (conn.sock < 0) break;
        mbedtls_ssl_set_bio(&ssl, &conn, SendCb, RecvCb, nullptr);
        while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            uint32_t v = mbedtls_ssl_get_verify_result(&ssl);
            error = TlsError("secure connection to Google failed", r);
            if (v) {
                char vb[64];
                snprintf(vb, sizeof(vb), " (certificate check %08lX)", (unsigned long)v);
                error += vb;
            }
            break;
        }
        if (r != 0) break;

        char len[24];
        snprintf(len, sizeof(len), "%lu", (unsigned long)body.size());
        std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n" +
                          "Content-Length: " + len + "\r\n";
        for (const std::string &h : headers) req += h + "\r\n";
        req += "\r\n";
        req += body;
        size_t sent = 0;
        while (sent < req.size()) {
            r = mbedtls_ssl_write(&ssl, (const unsigned char *)req.data() + sent, req.size() - sent);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r <= 0) break;
            sent += (size_t)r;
        }
        if (sent < req.size()) { error = TlsError("upload to Google failed", r); break; }

        unsigned char buf[4096];
        while (true) {
            r = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) break;
            if (r < 0) {
                if (raw.empty()) error = TlsError(r == MBEDTLS_ERR_SSL_TIMEOUT ? "Google took too long to answer"
                                                                              : "reading Google's answer failed", r);
                break;
            }
            raw.append((const char *)buf, (size_t)r);
            if (raw.size() > 2 * 1024 * 1024) { error = "answer too large"; break; }
            // stop as soon as a Content-Length body is complete
            size_t he = raw.find("\r\n\r\n");
            std::string cl;
            if (he != std::string::npos && HeaderValue(raw.substr(0, he + 2), "Content-Length", cl) &&
                raw.size() >= he + 4 + strtoul(cl.c_str(), nullptr, 10))
                break;
            if (he != std::string::npos && raw.size() >= 5 && raw.compare(raw.size() - 5, 5, "0\r\n\r\n") == 0) {
                std::string te;
                if (HeaderValue(raw.substr(0, he + 2), "Transfer-Encoding", te)) break;
            }
        }
        if (raw.empty()) { if (error.empty()) error = "Google closed the connection without answering"; break; }
        size_t he = raw.find("\r\n\r\n");
        if (he == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) { error = "unexpected answer from Google"; break; }
        size_t sp = raw.find(' ');
        status = sp != std::string::npos ? atoi(raw.c_str() + sp + 1) : 0;
        std::string head = raw.substr(0, he + 2), te, cl;
        std::string payload = raw.substr(he + 4);
        if (HeaderValue(head, "Transfer-Encoding", te) && te.find("chunked") != std::string::npos) {
            if (!dechunk(payload, response)) { error = "Google's answer was cut off"; break; }
        } else if (HeaderValue(head, "Content-Length", cl)) {
            size_t n = strtoul(cl.c_str(), nullptr, 10);
            if (payload.size() < n) { error = "Google's answer was cut off"; break; }
            response = payload.substr(0, n);
        } else {
            response = payload;
        }
        if (status != 200) {
            char sb[48];
            snprintf(sb, sizeof(sb), "Google answered HTTP %d", status);
            error = sb;
            break;
        }
        ok = true;
    } while (false);
    if (conn.sock >= 0) {
        mbedtls_ssl_close_notify(&ssl);
        netClose(conn.sock);
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&roots);
    mbedtls_ctr_drbg_free(&drbg);
    return ok;
}

}  // namespace kt
