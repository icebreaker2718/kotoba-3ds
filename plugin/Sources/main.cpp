// Kotoba3DS - in-game Japanese dictionary + Anki mining log, as a Luma3DS (CTRPF) plugin.
//
// In a game, press the hotkey (default ZL+ZR) on a line with a word you don't know: the game
// pauses, the 3DS reads the top screen itself (on-device OCR, plugin/Sources/vocr), you pick
// where the word starts and see its full entry on the top screen. Saved words go to
// SD:/3ds/kotoba3ds/mined.tsv with a screenshot; tools/mine.py on the PC (or ocr_server.py,
// right away) turns them into Anki cards through AnkiConnect.
#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <malloc.h>

#include "core/bmp.hpp"
#include "core/dict.hpp"
#include "core/local_ocr.hpp"
#include "core/net.hpp"
#include "core/search.hpp"
#include "core/textbox.hpp"
#include "core/textlines.hpp"
#include "core/wrap.hpp"

namespace CTRPluginFramework {

// ---------------------------------------------------------------------------
// Paths & settings
// ---------------------------------------------------------------------------
static const char *kRootDir = "/3ds/kotoba3ds";
static const char *kDictPath = "/3ds/kotoba3ds/dict.bin";
static const char *kLogPath = "/3ds/kotoba3ds/mined.tsv";
static const char *kShotDir = "/3ds/kotoba3ds/shots";
static const char *kSettingsPath = "/3ds/kotoba3ds/settings.txt";
static const char *kModelDir = "/3ds/kotoba3ds/ocr/";

struct HotkeyChoice { const char *name; u32 keys; };
static const HotkeyChoice kHotkeys[] = {
    {"ZL + ZR", Key::ZL | Key::ZR},
    {"L + R + Down", Key::L | Key::R | Key::DPadDown},
    {"R + Down", Key::R | Key::DPadDown},
    {"Start + Select", Key::Start | Key::Select},
    {"Off", 0},
};
static const int kHotkeyCount = sizeof(kHotkeys) / sizeof(kHotkeys[0]);
static int g_hotkey = 0;

// ---------------------------------------------------------------------------
// Dictionary on the SD card
// ---------------------------------------------------------------------------
class SdReader : public kt::Reader {
public:
    bool open(const std::string &path) {
        if (File::Open(file_, path, File::READ) != File::SUCCESS) return false;
        size_ = (u32)file_.GetSize();
        return true;
    }
    bool read(uint32_t off, void *dst, uint32_t len) override {
        if (file_.Seek(off, File::SET) != File::SUCCESS) return false;
        return file_.Read(dst, len) == File::SUCCESS;
    }
    uint32_t size() override { return size_; }
private:
    File file_;
    u32 size_ = 0;
};

// Long-lived state is created on first use and deliberately never destroyed. When the game
// closes, the plugin's memory can already be in the middle of being torn down, and freeing
// anything at that point crashes in free() (seen in StreetPass Mii Plaza). The console
// reclaims all plugin memory when the game ends, so nothing is lost by not freeing it.
template <typename T>
static T &Leaky(T *&slot) {
    if (!slot) slot = new T();
    return *slot;
}
static SdReader *g_readerPtr = nullptr;
static kt::Dictionary *g_dictPtr = nullptr;
static std::string *g_dictErrorPtr = nullptr;
static kt::SearchOutput *g_lastPtr = nullptr;
#define g_reader Leaky(g_readerPtr)
#define g_dict Leaky(g_dictPtr)
#define g_dictError Leaky(g_dictErrorPtr)
#define g_last Leaky(g_lastPtr)

static bool EnsureDict() {
    if (g_dict.isOpen()) return true;
    if (!g_reader.open(kDictPath)) {
        g_dictError = std::string("Dictionary not found:\n") + kDictPath +
                      "\n\nBuild it on your PC with tools/build_dict.py and copy dict.bin there.";
    } else if (!g_dict.open(&g_reader, g_dictError)) {
        // g_dictError set by open()
    }
    if (!g_dict.isOpen()) {
        MessageBox("Kotoba3DS", g_dictError)();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static std::string Timestamp() {
    // osGetTime: ms since 1900-01-01, console local time
    time_t t = (time_t)(osGetTime() / 1000ULL - 2208988800ULL);
    struct tm *tm = gmtime(&t);
    if (!tm) return "0000-00-00 00:00:00";
    return Utils::Format("%04d-%02d-%02d %02d:%02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1,
                         tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static std::string TitleIdHex() {
    std::string tid;
    Process::GetTitleID(tid);
    return tid;
}

static bool ReadWholeFile(const std::string &path, std::string &out, u32 maxBytes = 1024 * 1024) {
    File f;
    if (File::Open(f, path, File::READ) != File::SUCCESS) return false;
    u64 sz = f.GetSize();
    if (sz > maxBytes) sz = maxBytes;
    out.assign((size_t)sz, '\0');
    bool ok = sz == 0 || f.Read(&out[0], (u32)sz) == File::SUCCESS;
    f.Close();
    return ok;
}

static bool WriteWholeFile(const std::string &path, const std::string &data) {
    File f;
    if (File::Open(f, path, File::WRITE | File::CREATE | File::TRUNCATE | File::SYNC) != File::SUCCESS)
        return false;
    bool ok = data.empty() || f.Write(data.data(), data.size()) == File::SUCCESS;
    f.Close();
    return ok;
}

// Settings. Saved words can also go straight to the PC (tools/ocr_server.py) as Anki cards.
static bool g_cardsEnabled = true;
static u64 g_pcDownUntil = 0;       // after a failed attempt, don't retry for a while
static std::string *g_serverPtr = nullptr;  // "192.168.1.20:7331", empty = find automatically
#define g_server Leaky(g_serverPtr)
static const int kPcPort = 7331;

static void SaveSettings() {
    WriteWholeFile(kSettingsPath, Utils::Format("hotkey=%d\ncards=%d\nserver=", g_hotkey, g_cardsEnabled ? 1 : 0) +
                                      g_server + "\n");
}

static std::string SettingValue(const std::string &s, const char *key) {
    std::string k = std::string(key) + "=";
    size_t p = s.find(k);
    if (p == std::string::npos) return "";
    size_t e = s.find('\n', p);
    std::string v = s.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

static void LoadSettings() {
    std::string s;
    if (!ReadWholeFile(kSettingsPath, s, 4096)) return;
    std::string v = SettingValue(s, "hotkey");
    if (!v.empty()) {
        int h = atoi(v.c_str());
        if (h >= 0 && h < kHotkeyCount) g_hotkey = h;
    }
    v = SettingValue(s, "cards");
    if (!v.empty()) g_cardsEnabled = v != "0";
    g_server = SettingValue(s, "server");
}

// New 3DS: 804 MHz + L2 cache while a lookup is open (the game is paused), then back to what the
// game had. Old titles run at 268 MHz without L2; this is what Rosalina's "New 3DS menu" sets.
static s64 g_cpuBefore = -1;  // svcKernelSetState(10) bits: 1 = 804 MHz, 2 = L2 cache

static void BoostCpu() {
    if (!System::IsNew3DS() || g_cpuBefore >= 0) return;
    s64 clk = 0, l2 = 0;
    if (R_FAILED(svcGetSystemInfo(&clk, 0x10001, 0)) || R_FAILED(svcGetSystemInfo(&l2, 0x10001, 2))) return;
    g_cpuBefore = (l2 ? 2 : 0) | (clk > 268 ? 1 : 0);
    if (g_cpuBefore != 3) svcKernelSetState(10, 3u);
}

static void RestoreCpu() {
    if (g_cpuBefore < 0) return;
    if (g_cpuBefore != 3) svcKernelSetState(10, (u32)g_cpuBefore);
    g_cpuBefore = -1;
}

static int CpuMHz() {
    s64 clk = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&clk, 0x10001, 0)) ? (int)clk : 0;
}

// ---------------------------------------------------------------------------
// Bottom-screen-only UI: the top screen keeps showing the (paused) game.
// ---------------------------------------------------------------------------
static void ShowBusy(const std::string &line1, const std::string &line2 = "") {
    for (int i = 0; i < 2; i++) {  // draw into both buffers so it stays up
        const Screen &bottom = OSD::GetBottomScreen();
        bottom.DrawRect(16, 88, 288, 60, Color::Black, true);
        bottom.DrawRect(16, 88, 288, 60, Color::SkyBlue, false);
        bottom.DrawSysfont(line1, 28, 98, Color::White);
        if (!line2.empty()) bottom.DrawSysfont(line2, 28, 120, Color::Gray);
        OSD::SwapBuffers();
    }
}

// A list on the bottom screen only. Returns the chosen index, or < 0 for B / close.
static int BottomList(const std::vector<std::string> &items, int selected = 0) {
    Keyboard kb(items);
    kb.DisplayTopScreen = false;
    if (selected > 0 && selected < (int)items.size()) kb.ChangeSelectedEntry(selected);
    return kb.Open();
}

// Splits text into lines that fit (measured with the system font). See core/wrap.cpp.
static float SysfontWidth(const std::string &t) { return OSD::GetTextWidth(true, t); }
static std::vector<std::string> Wrap(const std::string &text, float maxWidth = 284.f) {
    return kt::wrapText(text, maxWidth, SysfontWidth);
}

// Text followed by options, all on the bottom screen. Returns the option index, or -1 for B.
static int BottomMessage(const std::string &text, const std::vector<std::string> &options) {
    std::vector<std::string> items = Wrap(text);
    size_t first = items.size();
    for (const std::string &o : options) items.push_back(o);
    int sel = (int)first;
    while (true) {
        int c = BottomList(items, sel);
        if (c < 0) return -1;
        if (c >= (int)first) return c - (int)first;
        sel = (int)first;  // a text line: stay, move the cursor to the first option
    }
}

// ---------------------------------------------------------------------------
// Full dictionary entry on the top screen: drawn over a darkened copy of the paused game, so
// the scene stays visible underneath. The keyboard flips between two top-screen buffers
// without drawing on them, so we draw into both.
// ---------------------------------------------------------------------------
struct TopSnapshot {
    bool valid = false;
    u32 bpp = 0;
    GSPFormat format = GSP_RGBA8_OES;
    std::vector<u8> px;  // 400x240 pixels in framebuffer format
};
static TopSnapshot *g_snapPtr = nullptr;
#define g_snap Leaky(g_snapPtr)

static void TakeTopSnapshot() {  // call while the game is paused and nothing is drawn on top
    const Screen &top = OSD::GetTopScreen();
    g_snap.bpp = top.BytesPerPixel;
    g_snap.format = top.Format;
    if (g_snap.bpp < 2 || g_snap.bpp > 4) { g_snap.valid = false; return; }
    g_snap.px.resize(400 * 240 * g_snap.bpp);
    for (u32 y = 0; y < 240; y++)
        for (u32 x = 0; x < 400; x++)
            memcpy(&g_snap.px[(y * 400 + x) * g_snap.bpp], top.GetFramebuffer(x, y), g_snap.bpp);
    g_snap.valid = true;
}

// The snapshot as R, G, B bytes (malloc'd; free() it), or nullptr.
static uint8_t *SnapshotRGB() {
    if (!g_snap.valid) return nullptr;
    uint8_t *rgb = (uint8_t *)malloc(400 * 240 * 3);
    if (rgb) kt::framebufferToRGB(g_snap.px.data(), 400 * 240, (int)g_snap.format, g_snap.bpp, rgb);
    return rgb;
}

static inline void Darken(u8 *d, const u8 *s, u32 bpp, GSPFormat fmt) {
    if (bpp >= 3) {  // 8 bits per channel: keep ~37%
        for (u32 i = 0; i < bpp; i++) d[i] = (u8)((s[i] >> 2) + (s[i] >> 3));
        return;
    }
    u16 v = (u16)(s[0] | (s[1] << 8));
    if (fmt == GSP_RGB565_OES) v = (u16)(((v >> 2) & 0x39E7) + ((v >> 3) & 0x18E3));
    else if (fmt == GSP_RGBA4_OES) v = (u16)((v >> 2) & 0x3333);
    else v = (u16)((v >> 2) & 0x39CE);  // RGB5A1
    d[0] = (u8)(v & 0xFF);
    d[1] = (u8)(v >> 8);
}

struct PanelLine { std::string text; Color color; };

// Draws `lines` (from `scroll`) over the darkened game; show=false puts the plain game back.
static void DrawTopPanel(const std::vector<PanelLine> &lines, int scroll, bool show) {
    if (!g_snap.valid) return;
    const int perScreen = 14;
    // When a menu closes, CTRPF queues a "clean" that refills the next shown buffer with the faded
    // game. Flip once first so that happens now, not over one of the two buffers drawn below
    // (otherwise the screen alternates between the definition and the faded game: flicker).
    OSD::SwapBuffers();
    for (int pass = 0; pass < 2; pass++) {
        const Screen &top = OSD::GetTopScreen();
        if (top.BytesPerPixel != g_snap.bpp) return;
        for (u32 y = 0; y < 240; y++)
            for (u32 x = 0; x < 400; x++) {
                const u8 *src = &g_snap.px[(y * 400 + x) * g_snap.bpp];
                u8 *dst = top.GetFramebuffer(x, y);
                if (show) Darken(dst, src, g_snap.bpp, g_snap.format);
                else memcpy(dst, src, g_snap.bpp);
            }
        if (show) {
            int y = 6;
            for (int i = scroll; i < (int)lines.size() && i < scroll + perScreen; i++, y += 16)
                top.DrawSysfont(lines[i].text, 8, y, lines[i].color);
            if (scroll > 0) top.DrawSysfont("\u25B2", 384, 4, Color::Gray);
            if (scroll + perScreen < (int)lines.size()) top.DrawSysfont("\u25BC", 384, 222, Color::Gray);
        }
        OSD::SwapBuffers();
    }
}

static std::vector<PanelLine> EntryLines(const kt::Entry &e, const kt::Result &r) {
    std::vector<PanelLine> out;
    for (const std::string &l : Wrap(e.headline, 380.f)) out.push_back({l, Color::Yellow});
    if (!r.reasons.empty())
        for (const std::string &l : Wrap("(" + r.reasons + ")", 380.f)) out.push_back({l, Color::Gray});
    for (const std::string &sense : e.senses)
        for (const std::string &l : Wrap(sense, 380.f)) out.push_back({l, Color::White});
    return out;
}

static void ShowEntryOnTop(const kt::Result &r, int scroll = 0) {
    kt::Entry e;
    if (g_dict.entry(r.entryId, e)) DrawTopPanel(EntryLines(e, r), scroll, true);
}

static void ClearTopPanel() { DrawTopPanel({}, 0, false); }

// ---------------------------------------------------------------------------
// The PC (tools/ocr_server.py): saved words become Anki cards right away
// ---------------------------------------------------------------------------
// Top screen of a saved screenshot, 16-bit, for uploading.
static bool CompactScreenshot(const std::string &shotName, std::string &out, std::string &error) {
    SdReader bmp;
    if (!bmp.open(std::string(kShotDir) + "/" + shotName)) {
        error = "could not open " + shotName;
        return false;
    }
    return kt::topScreenRGB565(bmp, out, error);
}

// Saved address first, then automatic search. Call between netStart()/netStop().
static bool ResolveServer(std::string &host, int &port, std::string &error) {
    std::string pingErr;
    port = kPcPort;
    if (!g_server.empty() && kt::parseHostPort(g_server, host, port) && kt::netPing(host, port, 1500, pingErr))
        return true;
    std::string found;
    int foundPort = kPcPort;
    if (kt::netDiscover(kPcPort, 1500, found, foundPort, error)) {
        host = found;
        port = foundPort;
        g_server = host + Utils::Format(":%d", port);  // remember it for next time
        SaveSettings();
        return true;
    }
    if (!pingErr.empty()) error = pingErr;
    return false;
}

// Sends one mined.tsv line to the PC, which turns it into an Anki card in the background.
// Returns a short status for the "Saved" notification.
static std::string SendCardToPc(const std::string &line, const std::string &shot) {
    if (!g_cardsEnabled) return "";
    if (g_pcDownUntil && osGetTime() < g_pcDownUntil) return "PC offline, run mine.py later";
    std::string err, host;
    int port = kPcPort;
    if (!kt::netStart(err)) return "no network, run mine.py later";
    bool ok = false, sent = false;
    std::string reply;
    std::vector<std::string> headers = {"X-Title-Id: " + TitleIdHex(), "X-Shot-Name: " + shot};
    if (ResolveServer(host, port, err)) {
        kt::MemReader body(line + "\n");
        ok = kt::netPost(host, port, "/mine", headers, body, 1500, 6000, reply, err);
        if (ok && reply.compare(0, 9, "NEED_SHOT") == 0 && !shot.empty()) {
            // the PC hasn't seen this screenshot: upload it, then ask again
            ShowBusy("Sending the screenshot to your PC...");
            std::string compact, r2;
            if (CompactScreenshot(shot, compact, err) &&
                [&] { kt::MemReader body2(compact); return kt::netPost(host, port, "/shot", headers, body2, 1500, 10000, r2, err); }()) {
                kt::MemReader again(line + "\n");
                ok = kt::netPost(host, port, "/mine", headers, again, 1500, 6000, reply, err);
            } else {
                ok = false;
            }
        }
        sent = ok && reply.compare(0, 2, "OK") == 0;
    }
    kt::netStop();
    if (sent) {
        g_pcDownUntil = 0;
        return "card sent to PC";
    }
    g_pcDownUntil = osGetTime() + 2 * 60 * 1000;
    return "PC not reachable, run mine.py later";
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------
static int CountSaved() {
    std::string s;
    if (!ReadWholeFile(kLogPath, s)) return 0;
    int n = 0;
    size_t start = 0;
    while (start < s.size()) {
        size_t e = s.find('\n', start);
        if (e == std::string::npos) e = s.size();
        if (e > start && s[start] != '#') n++;
        start = e + 1;
    }
    return n;
}

// The screenshot of the current lookup: written from the paused game's picture the first time
// a word from it is saved (no screenshot is taken for lookups that save nothing).
static std::string *g_lookupShotPtr = nullptr;
#define g_lookupShot Leaky(g_lookupShotPtr)

static std::string SaveScreenshot() {
    if (!g_lookupShot.empty()) return g_lookupShot;
    uint8_t *rgb = SnapshotRGB();
    if (!rgb) return "";
    std::string bmp = kt::bmpFromRGB(rgb, 400, 240);
    free(rgb);
    std::string ts = Timestamp();  // "2026-10-24 14:30:05"
    std::string name;
    for (char c : ts) name += (c == ' ') ? '_' : (c == ':' ? '-' : c);
    name += ".bmp";
    if (!WriteWholeFile(std::string(kShotDir) + "/" + name, bmp)) return "";
    g_lookupShot = name;
    return name;
}

static bool SaveWord(const kt::Entry &e, const kt::Result &r, const std::string &query,
                     const std::string &sentence) {
    ShowBusy("Saving...");
    std::string shot = SaveScreenshot();
    bool fresh = File::Exists(kLogPath) != 1;
    File f;
    if (File::Open(f, kLogPath, File::WRITE | File::CREATE | File::APPEND | File::SYNC) != File::SUCCESS) {
        MessageBox("Kotoba3DS", std::string("Could not write ") + kLogPath)();
        return false;
    }
    std::string text;
    if (fresh)
        text += "#time\ttitle_id\texpression\treading\tdefinition\tscreenshot\tquery\tinflection\tentry\tsentence\n";
    std::string line = kt::mineLogLine(Timestamp(), TitleIdHex(), e, r, shot, query, sentence);
    text += line + "\n";
    bool ok = f.Write(text.data(), text.size()) == File::SUCCESS;
    f.Close();
    if (ok) {
        int n = CountSaved();
        std::string status = SendCardToPc(line, shot);  // saved on the SD card first, so nothing is lost
        OSD::Notify(Utils::Format("Saved (%d word%s)", n, n == 1 ? "" : "s") + (status.empty() ? "" : " - " + status),
                    Color::White, status == "card sent to PC" || status.empty() ? Color::ForestGreen : Color::Orange);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Results & details
// ---------------------------------------------------------------------------
static void ShowDetails(const kt::Result &r, const std::string &query, const std::string &sentence) {
    kt::Entry e;
    if (!g_dict.entry(r.entryId, e)) {
        BottomMessage("Could not read this entry.", {"[ Back ]"});
        return;
    }
    std::vector<PanelLine> lines = EntryLines(e, r);
    if (!g_snap.valid) {  // no game picture to draw over (e.g. inside the Select menu): old way
        std::string body = kt::detailText(e, r);
        int c = BottomMessage(body, {"[ Save this word ]", "[ Back ]"});
        if (c == 0) SaveWord(e, r, r.surface.empty() ? query : r.surface, sentence);
        return;
    }
    const int perScreen = 14;
    int scroll = 0, sel = 0;
    bool shown = true;
    while (true) {
        DrawTopPanel(lines, scroll, shown);
        std::vector<std::string> opts = {"[ Save this word ]"};
        bool canDown = shown && scroll + perScreen < (int)lines.size(), canUp = shown && scroll > 0;
        if (canDown) opts.push_back("[ \u25BC More of the definition ]");
        if (canUp) opts.push_back("[ \u25B2 Back up ]");
        opts.push_back(shown ? "[ Hide definition (see the game) ]" : "[ Show definition ]");
        opts.push_back("[ Back ]");
        if (sel >= (int)opts.size()) sel = 0;
        int c = BottomList(opts, sel);
        if (c < 0 || opts[c] == "[ Back ]") return;
        sel = c;
        const std::string &o = opts[c];
        if (c == 0) {
            SaveWord(e, r, r.surface.empty() ? query : r.surface, sentence);  // surface = text as seen
            return;
        }
        if (o.find("More") != std::string::npos) scroll += perScreen - 2;
        else if (o.find("Back up") != std::string::npos) scroll = scroll - (perScreen - 2) < 0 ? 0 : scroll - (perScreen - 2);
        else shown = !shown;
    }
}

static std::string *g_lastSentencePtr = nullptr;  // sentence that goes with g_last ("" = manual)
#define g_lastSentence Leaky(g_lastSentencePtr)

static const kt::SearchOutput *g_listing = nullptr;  // results being browsed (for the cursor callback)

static void OnResultCursor(Keyboard &, KeyboardEvent &ev) {
    if (ev.type == KeyboardEvent::SelectionChanged && g_listing && ev.selectedIndex < g_listing->results.size())
        ShowEntryOnTop(g_listing->results[ev.selectedIndex]);
}

static void ShowResults(const kt::SearchOutput &so, const std::string &sentence = "") {
    if (so.results.empty()) {
        BottomMessage("No results for 「" + so.query + "」", {"[ Back ]"});
        return;
    }
    std::vector<std::string> labels;
    labels.reserve(so.results.size());
    for (const kt::Result &r : so.results) {
        kt::Entry e;
        std::string line = g_dict.entry(r.entryId, e) ? kt::summaryLine(e, r, 90) : "?";
        labels.push_back((r.prefixOnly ? "→ " : "") + line);
    }
    int sel = 0;
    while (true) {
        int choice;
        if (g_snap.valid) {  // the highlighted word's full entry is shown on the top screen
            ShowEntryOnTop(so.results[sel]);
            g_listing = &so;
            Keyboard kb(labels);
            kb.DisplayTopScreen = false;
            kb.OnKeyboardEvent(OnResultCursor);
            if (sel > 0) kb.ChangeSelectedEntry(sel);
            choice = kb.Open();
            g_listing = nullptr;
        } else {
            choice = BottomList(labels, sel);
        }
        if (choice < 0) { if (g_snap.valid) ClearTopPanel(); return; }
        sel = choice;
        ShowDetails(so.results[choice], so.query, sentence);
    }
}

// ---------------------------------------------------------------------------
// Reading the screen on the 3DS (VOCR): no PC, no internet. Needs the model on the SD card.
// ---------------------------------------------------------------------------
static const char *const kModelNames[] = {"VOCR5_N_w8.vocr", "VOCR5_S_w8.vocr", "VOCR5_L_w8.vocr", "model.vocr"};
static kt::LocalOcr *g_localOcrPtr = nullptr;
#define g_localOcr Leaky(g_localOcrPtr)
static std::string *g_modelNamePtr = nullptr;  // file the loaded model came from
#define g_modelName Leaky(g_modelNamePtr)

static std::string FindModel() {
    for (const char *n : kModelNames)
        if (File::Exists(std::string(kModelDir) + n) == 1) return n;
    return "";
}

static void LocalProgress(unsigned i, unsigned n) {
    ShowBusy("Reading the screen on the 3DS...", Utils::Format("line %u of %u", i + 1, n));
}

static void LogOcr(const std::string &line) {  // timings, to see how fast this console is
    File f;
    if (File::Open(f, "/3ds/kotoba3ds/ocr_log.txt", File::WRITE | File::CREATE | File::APPEND) == File::SUCCESS) {
        f.Write(line.c_str(), line.size());
        f.Write("\n", 1);
        f.Close();
    }
}

// Runs job(arg) on n CPU cores at once (CTRPF tasks: the New 3DS's extra app core 2, and core 0,
// where the paused game leaves room) and waits for all of them.
struct CoreJob { void (*job)(void *); void *arg; };
static s32 CoreTask(void *a) {
    CoreJob *j = (CoreJob *)a;
    j->job(j->arg);
    return 0;
}
static void RunOnCores(unsigned n, void (*job)(void *), void *arg) {
    static const s32 kAffinity[] = {Task::NewAppCore, Task::AppCore};
    CoreJob cj = {job, arg};
    std::vector<Task> tasks;
    for (unsigned i = 0; i < n && i < 2; i++) tasks.emplace_back(CoreTask, &cj, kAffinity[i]);
    for (Task &t : tasks) t.Start();
    for (Task &t : tasks) t.Wait();
}

// Reads the paused game's top screen -> text blocks.
static bool ReadSnapshot(std::vector<std::string> &paragraphs, std::string &error) {
    u64 t0 = osGetTime();
    std::string name = FindModel();
    if (name.empty()) {
        error = "No OCR model found.\nUnzip Kotoba3DS-ocr-model.zip to the root of the SD card\n"
                "(it goes to /3ds/kotoba3ds/ocr/).";
        return false;
    }
    ShowBusy("Reading the screen...");
    // 1. the picture: find the text box (by its colour), then grey for the OCR
    uint8_t *rgb = SnapshotRGB();
    if (!rgb) { error = "could not copy the screen"; return false; }
    std::vector<kt::TextBox> boxes = kt::findTextBoxes(rgb, 400, 240);
    uint8_t *grey = kt::greyFromRGB(rgb, 400, 240);
    free(rgb);
    if (!grey) { error = "not enough memory for the picture"; return false; }
    // 2. the model (loaded for this read only: it needs ~1.4 MB of the plugin's memory)
    u64 t1 = osGetTime();
    SdReader mf;
    if (!mf.open(kModelDir + name)) { free(grey); error = "could not open " + name; return false; }
    if (!g_localOcr.load(mf, error)) { free(grey); return false; }
    g_modelName = name;
    // 3. read the box (or the whole screen)
    u64 t2 = osGetTime();
    g_localOcr.setParallel(System::IsNew3DS() ? 2 : 1, RunOnCores);
    int used = -1;
    bool ok = kt::localReadScreen(g_localOcr, grey, 400, 240, boxes, paragraphs, error, LocalProgress, &used);
    free(grey);
    g_localOcr.unload();  // ~1.4 MB back right away
    u64 t3 = osGetTime();
    struct mallinfo mi = mallinfo();
    LogOcr(Utils::Format("%s: box %lu ms, load %lu ms, read %lu ms (%s, %u threads, %d MHz), heap free %lu KB, %s",
                         name.c_str(), (unsigned long)(t1 - t0), (unsigned long)(t2 - t1), (unsigned long)(t3 - t2),
                         used >= 0 ? "text box" : "whole screen", g_localOcr.lastWorkers(), CpuMHz(),
                         (unsigned long)(mi.fordblks / 1024), ok ? paragraphs[0].c_str() : error.c_str()));
    return ok;
}

// Several blocks of text: let the player pick one (the longest is preselected).
static bool PickParagraph(const std::vector<std::string> &paragraphs, std::string &out) {
    if (paragraphs.size() == 1) { out = paragraphs[0]; return true; }
    std::vector<std::string> labels;
    size_t best = 0;
    for (size_t i = 0; i < paragraphs.size(); i++) {
        std::vector<std::string> lines = Wrap(paragraphs[i]);
        labels.push_back(lines.empty() ? paragraphs[i] : lines[0] + (lines.size() > 1 ? "…" : ""));
        if (paragraphs[i].size() > paragraphs[best].size()) best = i;
    }
    if (g_snap.valid) ClearTopPanel();
    int c = BottomList(labels, (int)best);
    if (c < 0) return false;
    out = paragraphs[c];
    return true;
}

// Pick where the word starts, then the results.
static void TapWord(const std::string &sentence, const std::vector<std::string> &chunks) {
    int sel = 0;
    while (true) {
        if (g_snap.valid) ClearTopPanel();  // the plain game picture while picking a word
        int c = BottomList(chunks, sel);
        if (c < 0) return;
        sel = c;
        size_t offset = 0;
        for (int i = 0; i < c; i++) offset += chunks[i].size();
        kt::SearchOutput so = kt::scanAt(g_dict, sentence, offset);
        if (so.results.empty()) {
            BottomMessage("Nothing in the dictionary starts at 「" + chunks[c] + "」.", {"[ Back ]"});
            continue;
        }
        g_last = so;
        g_lastSentence = sentence;
        ShowResults(so, sentence);
    }
}

static void Lookup() {
    std::vector<std::string> paragraphs;
    std::string error, sentence;
    if (!ReadSnapshot(paragraphs, error)) {
        BottomMessage("Couldn't read the screen:\n" + error, {"[ Close ]"});
        return;
    }
    if (!PickParagraph(paragraphs, sentence)) return;
    ShowBusy("Finding the words...");
    std::vector<std::string> chunks = kt::chunkText(g_dict, sentence);
    TapWord(sentence, chunks);
}

static void ReleaseLookupMemory() {
    g_snap.valid = false;
    std::vector<u8>().swap(g_snap.px);
    if (g_localOcrPtr) g_localOcr.unload();
    g_lookupShot = "";
}

// The hotkey: pause the game, read the screen, look up a word. Called every frame.
static void HotkeyWatcher(void) {
    u32 keys = kHotkeys[g_hotkey].keys;
    if (!keys || !Controller::IsKeysPressed(keys)) return;
    if (!EnsureDict()) return;
    Process::Pause();
    BoostCpu();
    TakeTopSnapshot();  // the paused game: read, and drawn under the definitions
    Lookup();
    ReleaseLookupMemory();
    RestoreCpu();
    Process::Play();
}

// ---------------------------------------------------------------------------
// Saved words
// ---------------------------------------------------------------------------
static void SplitTabs(const std::string &line, std::vector<std::string> &cols) {
    cols.clear();
    size_t s = 0;
    while (true) {
        size_t p = line.find('\t', s);
        cols.push_back(line.substr(s, p == std::string::npos ? std::string::npos : p - s));
        if (p == std::string::npos) break;
        s = p + 1;
    }
}

static void SavedWords(MenuEntry *) {
    while (true) {
        std::string data;
        ReadWholeFile(kLogPath, data);
        std::vector<std::string> lines;
        size_t s = 0;
        while (s < data.size()) {
            size_t e = data.find('\n', s);
            if (e == std::string::npos) e = data.size();
            if (e > s) lines.push_back(data.substr(s, e - s));
            s = e + 1;
        }
        std::vector<int> idx;  // indexes of word lines, newest first
        for (int i = (int)lines.size() - 1; i >= 0; i--)
            if (lines[i][0] != '#') idx.push_back(i);
        if (idx.empty()) {
            MessageBox("Kotoba3DS", "No saved words yet.\nLook a word up and choose Yes to save it.")();
            return;
        }
        std::vector<std::string> labels;
        std::vector<std::string> cols;
        for (int i : idx) {
            SplitTabs(lines[i], cols);
            cols.resize(9);
            labels.push_back(cols[2] + "【" + cols[3] + "】" + (cols[5].empty() ? "" : "  ◎"));
        }
        Keyboard kb(Utils::Format("%d saved words (newest first)\n◎ = has a screenshot\n", (int)idx.size()) +
                        "Run tools/mine.py on your PC to make the Anki cards.",
                    labels);
        int c = kb.Open();
        if (c < 0) return;
        SplitTabs(lines[idx[c]], cols);
        cols.resize(9);
        std::string def = cols[4];
        for (size_t p; (p = def.find("<br>")) != std::string::npos;) def.replace(p, 4, "\n");
        std::string body = cols[2] + "【" + cols[3] + "】\n" + def + "\n\n" + std::string(Color::Gray) +
                           "Saved " + cols[0] + (cols[5].empty() ? "" : "\n" + cols[5]);
        if (MessageBox(std::string(Color::Orange) + "Delete this saved word?", body, DialogType::DialogYesNo)()) {
            lines.erase(lines.begin() + idx[c]);
            std::string out;
            for (const std::string &l : lines) out += l + "\n";
            WriteWholeFile(kLogPath, out);
        }
    }
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static std::string HotkeyEntryName() { return std::string("Hotkey: ") + kHotkeys[g_hotkey].name; }

static void ChooseHotkey(MenuEntry *entry) {
    std::vector<std::string> opts;
    for (const HotkeyChoice &h : kHotkeys) opts.push_back(h.name);
    Keyboard kb("Hotkey\nPressed in-game on a line: pauses the game, reads the screen and opens the\n"
                "lookup. Pick a combo your game doesn't use.",
                opts);
    kb.ChangeSelectedEntry(g_hotkey);
    int c = kb.Open();
    if (c < 0) return;
    g_hotkey = c;
    SaveSettings();
    entry->Name() = HotkeyEntryName();
}

static std::string CardsEntryName() {
    if (!g_cardsEnabled) return "Anki cards: with mine.py";
    return "Anki cards: send to PC " + (g_server.empty() ? std::string("(find automatically)") : g_server);
}

static void ChooseCards(MenuEntry *entry) {
    std::vector<std::string> opts = {
        g_cardsEnabled ? "Stop sending saved words to the PC (use mine.py later)" : "Send saved words to the PC right away",
        "Find my PC automatically now",
        "Type the address (shown by ocr_server.py)",
        "Forget the saved address",
    };
    Keyboard kb("Anki cards\nWith tools/ocr_server.py running on your PC (same Wi-Fi), each saved word\n"
                "becomes an Anki card right away. Otherwise run tools/mine.py later.\n" +
                    std::string(Color::Gray) + "PC: " + (g_server.empty() ? std::string("not set") : g_server),
                opts);
    int c = kb.Open();
    if (c < 0) return;
    std::string err;
    if (c == 0) {
        g_cardsEnabled = !g_cardsEnabled;
    } else if (c == 1) {
        std::string host;
        int port = kPcPort;
        bool ok = kt::netStart(err) && kt::netDiscover(kPcPort, 2000, host, port, err);
        kt::netStop();
        if (ok) {
            g_server = host + Utils::Format(":%d", port);
            g_cardsEnabled = true;
            MessageBox("Kotoba3DS", "Found your PC at " + g_server)();
        } else {
            MessageBox("Kotoba3DS", "Couldn't find it automatically:\n" + err +
                                        "\n\nCheck ocr_server.py is running, or type its address.")();
        }
    } else if (c == 2) {
        Keyboard in("Type the address shown by ocr_server.py, e.g. 192.168.1.20:7331");
        std::string addr = g_server;
        if (in.Open(addr, addr) >= 0) {
            std::string host;
            int port;
            if (!kt::parseHostPort(addr, host, port)) {
                MessageBox("Kotoba3DS", "That doesn't look like an address (example: 192.168.1.20:7331).")();
            } else {
                g_server = host + Utils::Format(":%d", port);
                g_cardsEnabled = true;
                bool ok = kt::netStart(err) && kt::netPing(host, port, 2000, err);
                kt::netStop();
                MessageBox("Kotoba3DS", ok ? "Connected to " + g_server
                                           : "Saved " + g_server + ", but it didn't answer:\n" + err)();
            }
        }
    } else {
        g_server = "";
    }
    g_pcDownUntil = 0;
    SaveSettings();
    entry->Name() = CardsEntryName();
}

static const char *kHelp =
    "1. In a game, press the hotkey (ZL+ZR) when a line with a word you don't\n"
    "   know is on screen. The game pauses and the 3DS reads the text.\n"
    "2. On the bottom screen, pick where the word starts. Its entry shows on the\n"
    "   top screen; move through the results with the D-pad.\n"
    "3. Choose 'Save this word'. It goes to mined.tsv with a screenshot.\n"
    "4. Anki cards: with tools/ocr_server.py running on your PC they're made\n"
    "   right away; otherwise run tools/mine.py later.\n\n"
    "Needs: /3ds/kotoba3ds/dict.bin and the OCR model in /3ds/kotoba3ds/ocr/.";

void PatchProcess(FwkSettings &settings) {
    settings.AllowActionReplay = false;
    settings.AllowSearchEngine = false;
}

void OnProcessExit(void) {}

int main(void) {
    PluginMenu *menu = new PluginMenu("Kotoba3DS", 2, 0, 0,
                                      "In-game Japanese dictionary that reads the screen and saves words for Anki.\n\n"
                                      "Press the hotkey (ZL+ZR) in a game to look a word up.");
    menu->SynchronizeWithFrame(true);

    Directory::Create(kRootDir);
    Directory::Create(kShotDir);
    LoadSettings();
    {   // CTRPF's own screenshot hotkey stays off: the plugin makes its screenshots itself
        bool *enabled = nullptr;
        u32 *hotkey = nullptr;
        PluginMenu::ScreenshotSettings(&enabled, &hotkey);
        if (enabled) *enabled = false;
    }

    *menu += new MenuEntry("Last results", nullptr,
                           [](MenuEntry *) {
                               if (g_last.results.empty())
                                   MessageBox("Kotoba3DS", "No lookup yet.")();
                               else if (EnsureDict())
                                   ShowResults(g_last, g_lastSentence);
                           },
                           "Reopen the words from your last lookup.");
    *menu += new MenuEntry("Saved words", nullptr, SavedWords, "Review or delete saved words.");
    *menu += new MenuEntry(HotkeyEntryName(), nullptr, ChooseHotkey, "The button combo that reads the screen.");
    *menu += new MenuEntry(CardsEntryName(), nullptr, ChooseCards, "Make Anki cards right away with your PC.");
    *menu += new MenuEntry("How to use", nullptr, [](MenuEntry *) { MessageBox("Kotoba3DS", kHelp)(); });

    *menu += HotkeyWatcher;  // runs every frame while the game plays
    menu->Run();
    // Don't `delete menu` (or free anything else) here: the game is exiting and the
    // plugin heap may already be unusable. See the note at Leaky().
    return 0;
}

}  // namespace CTRPluginFramework
