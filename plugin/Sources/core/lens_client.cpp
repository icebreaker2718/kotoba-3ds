#include "core/https.hpp"
#include "core/lens.hpp"

namespace kt {

bool lensOcr(const std::string &image, int width, int height, uint64_t random, int timeoutMs,
             std::vector<std::string> &paragraphs, std::string &error) {
    paragraphs.clear();
    std::string request = lensRequest(image, width, height, random);
    std::vector<std::string> headers = {
        "Content-Type: application/x-protobuf",
        std::string("X-Goog-Api-Key: ") + kLensApiKey,
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/131.0.0.0 Safari/537.36",
        "Accept: */*",
        "Sec-Fetch-Mode: no-cors",
        "Sec-Fetch-Dest: empty",
    };
    std::string reply;
    int status = 0;
    if (!httpsPost(kLensHost, kLensPath, headers, request, timeoutMs, reply, status, error)) {
        if (status == 400 || status == 403 || status == 404)
            error += " - Google may have changed Lens; the plugin needs an update";
        return false;
    }
    std::vector<LensLine> lines;
    if (!lensParse(reply, lines, error)) return false;
    paragraphs = lensParagraphs(lines);
    if (paragraphs.empty()) {
        error = "Google Lens found no text in the picture";
        return false;
    }
    return true;
}

}  // namespace kt
