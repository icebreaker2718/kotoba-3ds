// https.hpp - one HTTPS POST over mbedTLS, for Google Lens. Verifies the server against the
// embedded Google root certificates. Call between netStart()/netStop().
#pragma once
#include <string>
#include <vector>

namespace kt {

// Returns the response body on HTTP 200. On failure: false, with `error` and the HTTP status
// (0 if the request never got an answer).
bool httpsPost(const std::string &host, const std::string &path, const std::vector<std::string> &headers,
               const std::string &body, int timeoutMs, std::string &response, int &status, std::string &error,
               int port = 443, const char *rootsPem = nullptr);  // the last two: for the tests

// Decodes a "Transfer-Encoding: chunked" body. Exposed for the tests.
bool dechunk(const std::string &in, std::string &out);

}  // namespace kt
