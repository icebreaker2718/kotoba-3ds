// lens.hpp - Google Lens OCR (the endpoint Chrome's Lens overlay uses: unofficial, may change).
// Builds the protobuf request and reads the text out of the reply. No network code here
// (see https.hpp), so it is unit-tested natively.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kt {

static const char *const kLensHost = "lensfrontend-pa.googleapis.com";
static const char *const kLensPath = "/v1/crupload";
// Chrome's public API key for the Lens overlay (the one owocr / chrome-lens-py use).
static const char *const kLensApiKey = "AIzaSyDr2UxVnv_U85AbhhY8XSHSIavUW0DC-sY";

// LensOverlayServerRequest for one JPEG/PNG image. `random` seeds the request id.
std::string lensRequest(const std::string &image, int width, int height, uint64_t random,
                        const std::string &language = "ja");

struct LensLine {
    std::string text;
    float height = 0;   // line box height (normalized 0..1)
    float centerY = 0;
    int paragraph = 0;
};

// Lines in reading order. Returns false (with error) if the reply isn't a Lens response.
bool lensParse(const std::string &reply, std::vector<LensLine> &lines, std::string &error);

// One string per paragraph, furigana dropped (kana-only lines much shorter than the text
// around them), lines of a paragraph joined. Empty paragraphs removed.
std::vector<std::string> lensParagraphs(const std::vector<LensLine> &lines);

}  // namespace kt

namespace kt {

// Sends the image to Google Lens and returns the text paragraphs (furigana dropped).
// Needs internet; call between netStart()/netStop(). Defined in lens_client.cpp.
bool lensOcr(const std::string &image, int width, int height, uint64_t random, int timeoutMs,
             std::vector<std::string> &paragraphs, std::string &error);

}  // namespace kt
