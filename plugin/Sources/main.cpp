// Kotoba3DS - in-game Japanese dictionary + Anki mining log, as a Luma3DS (CTRPF) plugin.
//
// While a game runs: press the screenshot hotkey (default ZL+ZR) on a line you want to
// mine, open the plugin menu (Select), look the word up, and save it. Saved words go to
// SD:/3ds/kotoba3ds/mined.tsv together with the screenshot name; tools/mine.py on the PC
// turns them into Anki cards through AnkiConnect.
#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <malloc.h>

#include "core/dict.hpp"
#include "core/bmp.hpp"
#include "core/kana.hpp"
#include "core/net.hpp"
#include "core/lens.hpp"
#include "core/local_ocr.hpp"
#include "core/textbox.hpp"
#include "core/wrap.hpp"
#include "core/search.hpp"

namespace CTRPluginFramework {

// ---------------------------------------------------------------------------
// Paths & settings
// ---------------------------------------------------------------------------
static const char *kRootDir = "/3ds/kotoba3ds";
static const char *kDictPath = "/3ds/kotoba3ds/dict.bin";
static const char *kLogPath = "/3ds/kotoba3ds/mined.tsv";
static const char *kShotDir = "/3ds/kotoba3ds/shots";
static const char *kShotPrefix = "shot";
static const char *kSettingsPath = "/3ds/kotoba3ds/settings.txt";
static const u64 kShotMaxAgeMs = 15 * 60 * 1000;  // link a screenshot taken in the last 15 min

struct HotkeyChoice { const char *name; u32 keys; };
static const HotkeyChoice kHotkeys[] = {
    {"ZL + ZR", Key::ZL | Key::ZR},
    {"L + R + Down", Key::L | Key::R | Key::DPadDown},
    {"R + Down", Key::R | Key::DPadDown},
    {"Start + Select", Key::Start | Key::Select},
    {"Off (no screenshots)", 0},
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

// OCR ("Read screen") settings
static bool g_ocrEnabled = true;
static bool g_cardsEnabled = true;  // send saved words to the PC to become Anki cards right away
static bool g_hotkeyOpens = true;   // the screenshot hotkey also opens the lookup
static u64 g_pcDownUntil = 0;       // after a failed attempt, don't retry for a while
static bool g_lensEnabled = true;   // no PC? read the screen with Google Lens (needs internet)
static bool g_localEnabled = true;  // read the screen on the 3DS itself when a model is installed
static std::string *g_serverPtr = nullptr;  // "192.168.1.20:7331", empty = find automatically
#define g_server Leaky(g_serverPtr)
static const int kOcrPort = 7331;

static void SaveSettings() {
    WriteWholeFile(kSettingsPath, Utils::Format("hotkey=%d\nocr=%d\ncards=%d\nquick=%d\nlens=%d\nlocal=%d\nserver=", g_hotkey,
                                                g_ocrEnabled ? 1 : 0, g_cardsEnabled ? 1 : 0, g_hotkeyOpens ? 1 : 0,
                                                g_lensEnabled ? 1 : 0, g_localEnabled ? 1 : 0) +
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
    if (ReadWholeFile(kSettingsPath, s, 4096)) {
        std::string v = SettingValue(s, "hotkey");
        if (!v.empty()) {
            int h = atoi(v.c_str());
            if (h >= 0 && h < kHotkeyCount) g_hotkey = h;
        }
        v = SettingValue(s, "ocr");
        if (!v.empty()) g_ocrEnabled = v != "0";
        v = SettingValue(s, "cards");
        if (!v.empty()) g_cardsEnabled = v != "0";
        v = SettingValue(s, "quick");
        if (!v.empty()) g_hotkeyOpens = v != "0";
        v = SettingValue(s, "lens");
        if (!v.empty()) g_lensEnabled = v != "0";
        v = SettingValue(s, "local");
        if (!v.empty()) g_localEnabled = v != "0";
        g_server = SettingValue(s, "server");
    }
}

// ---------------------------------------------------------------------------
// Screenshots (CTRPF's built-in engine, pointed at our folder)
// We mirror the engine's file counter so we know each screenshot's file name.
// ---------------------------------------------------------------------------
// NOTE: OnScreenshot runs on the *game's* thread (inside the screenshot engine's frame hook).
// It must not allocate memory or touch std::string: the plugin heap is not safe to use from
// game threads, and doing so corrupts it (seen as a crash in free() when the game closes).
// So the callback only bumps a counter and stores a timestamp; names are built later on the
// plugin thread.
static u32 g_firstShot = 0;              // engine's file number of the first shot this session
static volatile u32 g_shotsTaken = 0;    // written on the game thread
static volatile u64 g_lastShotTime = 0;  // written on the game thread

static std::string ShotName(u32 n) { return std::string(kShotPrefix) + Utils::Format(" - %03d.bmp", (int)n); }

static void InitShotCounter() {
    // Same algorithm as the engine's Screenshot::UpdateFileCount()
    Directory dir(kShotDir);
    std::vector<std::string> files;
    dir.ListFiles(files, ".bmp");
    u32 next = 0;
    std::string name = ShotName(0);
    for (const std::string &f : files) {
        if (f == name) {
            ++next;
            name = ShotName(next);
        }
    }
    g_firstShot = next;
    g_shotsTaken = 0;
}

// Set while a lookup is open. A screenshot then would need ~0.6 MB of plugin memory at a moment
// the OCR may be using it, and CTRPF's screenshot task crashes when it can't get it.
static volatile bool g_lookupOpen = false;

static bool OnScreenshot() {
    // The engine fires while the hotkey is *held*: refuse a second shot within 1.5 s, so one
    // press = one screenshot. (Returning false cancels it.) No allocation here (see above).
    if (g_lookupOpen) return false;
    u64 now = osGetTime();
    if (g_shotsTaken > 0 && now - g_lastShotTime < 1500) return false;
    g_lastShotTime = now;
    g_shotsTaken = g_shotsTaken + 1;
    return true;
}

static void ApplyHotkey() {
    bool *enabled = nullptr;
    u32 *hotkey = nullptr;
    PluginMenu::ScreenshotSettings(&enabled, &hotkey);
    if (enabled && hotkey) {
        *enabled = kHotkeys[g_hotkey].keys != 0;
        if (kHotkeys[g_hotkey].keys) *hotkey = kHotkeys[g_hotkey].keys;
    }
}

static void InitScreenshots() {
    PluginMenu::ScreenshotPath() = std::string(kShotDir) + "/";
    PluginMenu::ScreenshotFilePrefix() = kShotPrefix;
    PluginMenu::ScreenshotUpdatePaths();
    PluginMenu::ScreenshotSetcallback(OnScreenshot);
    InitShotCounter();
    ApplyHotkey();
}

static std::string RecentScreenshot() {
    u32 taken = g_shotsTaken;
    u64 when = g_lastShotTime;
    if (taken == 0) return "";
    if (osGetTime() - when > kShotMaxAgeMs) return "";
    return ShotName(g_firstShot + taken - 1);
}

// ---------------------------------------------------------------------------
// Bottom-screen-only UI: the top screen keeps showing the (paused) game, so the player can
// compare it with what was read.
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

// Gives back what a lookup used (top-screen copy, OCR model), so the next screenshot has room.
static void ReleaseLookupMemory();

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

// Top screen of a saved screenshot, 16-bit: ~190 KB to upload instead of ~570 KB.
static bool CompactScreenshot(const std::string &shotName, std::string &out, std::string &error) {
    SdReader bmp;
    if (!bmp.open(std::string(kShotDir) + "/" + shotName)) {
        error = "could not open " + shotName;
        return false;
    }
    return kt::topScreenRGB565(bmp, out, error);
}

// ---------------------------------------------------------------------------
// The PC (tools/ocr_server.py): find it, and send saved words to it
// ---------------------------------------------------------------------------
// Saved address first, then automatic search. Call between netStart()/netStop().
static bool ResolveServer(std::string &host, int &port, std::string &error) {
    std::string pingErr;
    port = kOcrPort;
    if (!g_server.empty() && kt::parseHostPort(g_server, host, port) && kt::netPing(host, port, 1500, pingErr))
        return true;
    std::string found;
    int foundPort = kOcrPort;
    if (kt::netDiscover(kOcrPort, 1500, found, foundPort, error)) {
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
    if (!g_ocrEnabled || !g_cardsEnabled) return "";
    if (g_pcDownUntil && osGetTime() < g_pcDownUntil) return "PC offline, run mine.py later";
    std::string err, host;
    int port = kOcrPort;
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

// The screenshot the current lookup is about (set by Read screen / the hotkey). Saving uses it,
// so the word gets the screenshot that was read even if another one was taken since.
static std::string *g_lookupShotPtr = nullptr;
#define g_lookupShot Leaky(g_lookupShotPtr)

static bool SaveWord(const kt::Entry &e, const kt::Result &r, const std::string &query,
                     const std::string &sentence) {
    bool fresh = File::Exists(kLogPath) != 1;
    File f;
    if (File::Open(f, kLogPath, File::WRITE | File::CREATE | File::APPEND | File::SYNC) != File::SUCCESS) {
        MessageBox("Kotoba3DS", std::string("Could not write ") + kLogPath)();
        return false;
    }
    std::string text;
    if (fresh)
        text += "#time\ttitle_id\texpression\treading\tdefinition\tscreenshot\tquery\tinflection\tentry\tsentence\n";
    std::string shot = g_lookupShot.empty() ? RecentScreenshot() : g_lookupShot;
    std::string line = kt::mineLogLine(Timestamp(), TitleIdHex(), e, r, shot, query, sentence);
    text += line + "\n";
    bool ok = f.Write(text.data(), text.size()) == File::SUCCESS;
    f.Close();
    if (ok) {
        int n = CountSaved();
        std::string status = SendCardToPc(line, shot);  // saved on the SD card first, so nothing is lost
        OSD::Notify(Utils::Format("Saved (%d word%s)%s", n, n == 1 ? "" : "s",
                                  shot.empty() ? " - no screenshot" : "") +
                        (status.empty() ? "" : " - " + status),
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

static void RunSearch(const std::string &raw) {
    if (!EnsureDict()) return;
    g_last = kt::search(g_dict, raw);
    g_lastSentence = "";
    ShowResults(g_last);
}

// ---------------------------------------------------------------------------
// Typing (romaji with live kana preview)
// ---------------------------------------------------------------------------
static std::string *g_typePrefixPtr = nullptr;  // kanji picked before typing (okurigana mode)
#define g_typePrefix Leaky(g_typePrefixPtr)

// The bottom-screen keyboard shows the input only, so romaji is turned into kana right in the
// input field: when a syllable completes, the keyboard is reopened with the kana filled in
// (the keyboard has no API to edit its text and move the cursor). The game stays paused
// across the reopen so it doesn't advance a frame.
static std::string *g_kanaPendingPtr = nullptr;
#define g_kanaPending Leaky(g_kanaPendingPtr)

static void OnTypingEvent(Keyboard &kb, KeyboardEvent &ev) {
    if (ev.type != KeyboardEvent::CharacterAdded) return;
    std::string &in = kb.GetInput();
    std::string conv = kt::romajiToKana(in, false);
    if (conv != in) {
        g_kanaPending = conv;
        kb.Close();
    }
}

static bool TypeRomaji(const std::string &prefix, std::string &out) {
    Process::Pause();
    std::string text = prefix;
    int r = -1;
    std::string input;
    while (true) {
        Keyboard kb;
        kb.DisplayTopScreen = false;
        kb.OnKeyboardEvent(OnTypingEvent);
        kb.SetMaxLength(40);
        g_kanaPending = "";
        input.clear();
        r = kb.Open(input, text);
        if (!g_kanaPending.empty()) {  // a syllable completed: reopen with kana
            text = g_kanaPending;
            continue;
        }
        break;
    }
    Process::Play();
    if (r < 0) return false;
    out = kt::romajiToKana(input, true);
    return !out.empty();
}

static void LookupTyped(MenuEntry *) {
    std::string q;
    if (TypeRomaji("", q) && !q.empty()) RunSearch(q);
}

// ---------------------------------------------------------------------------
// Kanji by components (RADKFILE)
// ---------------------------------------------------------------------------
static bool PickKanji(std::string &picked) {
    const std::vector<kt::Radical> &rads = g_dict.radicals();
    if (rads.empty()) {
        BottomMessage("This dict.bin has no radical data. Rebuild it with a radkfile.", {"[ Back ]"});
        return false;
    }
    std::vector<int> chosen;
    int sel = 0;
    while (true) {
        std::vector<std::string> kanji;
        uint32_t total = 0;
        g_dict.kanjiWithRadicals(chosen, 300, kanji, total);

        std::string parts;
        for (int i : chosen) parts += rads[i].glyph + " ";
        std::vector<std::string> labels;
        int fixed = 0;
        if (!chosen.empty()) {
            labels.push_back(Utils::Format("→ Show %u kanji with  ", (unsigned)total) + parts);
            labels.push_back("× Clear parts");
            fixed = 2;
        }
        for (size_t i = 0; i < rads.size(); i++) {
            bool on = false;
            for (int c : chosen) on |= (c == (int)i);
            std::string l = Utils::Format("%s%2d画  ", on ? "● " : "   ", rads[i].strokes) + rads[i].glyph;
            if (!rads[i].label.empty()) l += "   (" + rads[i].label + ")";
            labels.push_back(l);
        }
        int c = BottomList(labels, sel);  // parts sorted by stroke count; select again to remove
        if (c < 0) return false;
        sel = c;
        if (fixed && c == 0) {
            if (kanji.empty()) continue;
            int k = BottomList(kanji);
            if (k < 0) continue;
            picked = kanji[k];
            return true;
        }
        if (fixed && c == 1) { chosen.clear(); sel = 0; continue; }
        int ri = c - fixed;
        bool removed = false;
        for (size_t i = 0; i < chosen.size(); i++)
            if (chosen[i] == ri) { chosen.erase(chosen.begin() + i); removed = true; break; }
        if (!removed) {
            chosen.push_back(ri);
            sel = 0;  // jump to "Show matching kanji"
        } else {
            sel = c - (chosen.empty() ? 2 : 0);
            if (sel < 0) sel = 0;
        }
    }
}

static void LookupByParts(MenuEntry *) {
    if (!EnsureDict()) return;
    std::string word;
    while (true) {
        if (word.empty()) {
            std::string k;
            if (!PickKanji(k)) return;
            word += k;
        }
        std::vector<std::string> opts = {
            "Search  " + word,
            "Add another kanji",
            "Type the rest in romaji (okurigana)",
            "Delete last character",
        };
        int c = BottomList(opts);
        if (c < 0) return;
        if (c == 0) { RunSearch(word); return; }
        if (c == 1) {
            std::string k;
            if (PickKanji(k)) word += k;
        } else if (c == 2) {
            std::string q;
            if (TypeRomaji(word, q)) { RunSearch(q); return; }
        } else if (c == 3) {
            kt::utf8PopBack(word);
        }
    }
}

// ---------------------------------------------------------------------------
// Read screen: send the last screenshot to tools/ocr_server.py on the PC, then tap a word.
// Every failure offers the manual (romaji) lookup instead.
// ---------------------------------------------------------------------------
struct OcrCache {
    std::string shot, sentence;
    std::vector<std::string> chunks;
};
static OcrCache *g_ocrPtr = nullptr;
#define g_ocr Leaky(g_ocrPtr)

// Offered whenever OCR isn't possible. Returns true if a manual lookup was run.
static bool OfferManual(const std::string &title, const std::string &why) {
    int c = BottomMessage(title + "\n" + why, {"[ Type the word (romaji) ]", "[ Kanji parts ]", "[ Close ]"});
    if (c == 0) { LookupTyped(nullptr); return true; }
    if (c == 1) { LookupByParts(nullptr); return true; }
    return false;
}

// Google Lens: the top screen as a JPEG, straight to Google (internet needed, no PC).
static bool LensRead(const std::string &shotName, std::vector<std::string> &paragraphs, std::string &error) {
    ShowBusy("Reading the screen...", "Asking Google Lens");
    SdReader bmp;
    if (!bmp.open(std::string(kShotDir) + "/" + shotName)) {
        error = "could not open " + shotName;
        return false;
    }
    std::string jpeg;
    int w = 0, h = 0;
    if (!kt::topScreenJpeg(bmp, 2, 88, jpeg, w, h, error)) return false;
    uint64_t rnd = svcGetSystemTick() ^ ((uint64_t)osGetTime() << 24);
    return kt::lensOcr(jpeg, w, h, rnd, 20000, paragraphs, error);
}

// ---------------------------------------------------------------------------
// On-device OCR (VOCR, plugin/Sources/vocr): no PC, no internet. Needs a model file on the SD.
// ---------------------------------------------------------------------------
static const char *kModelDir = "/3ds/kotoba3ds/ocr/";
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

static bool LocalRead(const std::string &shotName, std::vector<std::string> &paragraphs, std::string &error) {
    u64 t0 = osGetTime();
    std::string name = FindModel();
    if (name.empty()) { error = "no OCR model on the SD card (see the README)"; return false; }
    ShowBusy("Reading the screen on the 3DS...");
    // 1. the picture: find the text box (by its colour), then grey for the OCR
    SdReader bmp;
    if (!bmp.open(std::string(kShotDir) + "/" + shotName)) { error = "could not open " + shotName; return false; }
    unsigned w = 0, h = 0;
    uint8_t *rgb = kt::topScreenRGB888(bmp, w, h, error);
    if (!rgb) return false;
    std::vector<kt::TextBox> boxes = kt::findTextBoxes(rgb, w, h);
    uint8_t *grey = kt::greyFromRGB(rgb, w, h);
    free(rgb);
    if (!grey) { error = "not enough memory for the picture"; return false; }
    // 2. the model (loaded for this read only: it needs ~1.4 MB of the plugin's memory)
    u64 t1 = osGetTime();
    ShowBusy("Reading the screen on the 3DS...", "loading " + name);
    SdReader mf;
    if (!mf.open(kModelDir + name)) { free(grey); error = "could not open " + name; return false; }
    if (!g_localOcr.load(mf, error)) { free(grey); return false; }
    g_modelName = name;
    // 3. read the box (or the whole screen)
    u64 t2 = osGetTime();
    osSetSpeedupEnable(true);  // New 3DS: full CPU speed while reading (ignored where not allowed)
    int used = -1;
    bool ok = kt::localReadScreen(g_localOcr, grey, w, h, boxes, paragraphs, error, LocalProgress, &used);
    free(grey);
    u64 t3 = osGetTime();
    struct mallinfo mi = mallinfo();
    LogOcr(Utils::Format("%s: box %lu ms, load %lu ms, read %lu ms (%s), heap free %lu KB, %s", name.c_str(),
                         (unsigned long)(t1 - t0), (unsigned long)(t2 - t1), (unsigned long)(t3 - t2),
                         used >= 0 ? "text box" : "whole screen", (unsigned long)(mi.fordblks / 1024),
                         ok ? paragraphs[0].c_str() : error.c_str()));
    return ok;
}

static void ReleaseLookupMemory() {
    g_snap.valid = false;
    std::vector<u8>().swap(g_snap.px);
    if (g_localOcrPtr) g_localOcr.unload();
}

// Several blocks of text on screen: let the player pick the line. With `offerOther`, a last item
// lets them read it again with the PC / Google Lens instead. 1 = picked, 0 = cancelled, 2 = other.
static int PickParagraph(const std::vector<std::string> &paragraphs, std::string &out, bool offerOther = false) {
    if (paragraphs.size() == 1 && !offerOther) { out = paragraphs[0]; return 1; }
    std::vector<std::string> labels;
    size_t best = 0;
    for (size_t i = 0; i < paragraphs.size(); i++) {
        std::vector<std::string> lines = Wrap(paragraphs[i]);
        labels.push_back(lines.empty() ? paragraphs[i] : lines[0] + (lines.size() > 1 ? "…" : ""));
        if (paragraphs[i].size() > paragraphs[best].size()) best = i;  // the dialogue is usually the longest
    }
    if (offerOther) labels.push_back("[ Wrong? Read it with the PC / Google Lens ]");
    if (g_snap.valid) ClearTopPanel();
    int c = BottomList(labels, (int)best);
    if (c < 0) return 0;
    if (c >= (int)paragraphs.size()) return 2;
    out = paragraphs[c];
    return 1;
}

// OCRs a saved screenshot: on the 3DS itself when a model is installed (and that's on), else on
// the PC (tools/ocr_server.py) if it can be reached, else with Google Lens when that's on.
// `chunks` are the word starts to offer.
static bool RunOcr(const std::string &shotName, std::string &sentence, std::vector<std::string> &chunks,
                   std::string &error) {
    std::string localError;
    if (g_localEnabled && (g_localOcr.loaded() || !FindModel().empty())) {
        std::vector<std::string> paragraphs;
        bool read = LocalRead(shotName, paragraphs, localError);
        g_localOcr.unload();  // ~1.4 MB back right away (loading it again takes a moment, not a crash)
        if (read) {
            int pick = PickParagraph(paragraphs, sentence, true);
            if (pick == 0) { error = "cancelled"; return false; }
            if (pick == 1) {
                ShowBusy("Finding the words...");
                chunks = kt::chunkText(g_dict, sentence);
                return true;
            }
            localError = "you chose another reader";
        }
    }
    std::string netError;
    if (!kt::netStart(netError)) {
        error = localError.empty() ? netError : "On the 3DS: " + localError + "\nNetwork: " + netError;
        return false;
    }
    bool ok = false;
    std::string pcError;
    bool tryPc = !(g_pcDownUntil && osGetTime() < g_pcDownUntil);
    if (tryPc) {
        ShowBusy("Reading the screen...", "Sending to your PC");
        std::string compact, host;
        int port = kOcrPort;
        if (CompactScreenshot(shotName, compact, pcError) && ResolveServer(host, port, pcError)) {
            ShowBusy("Reading the screen...", "Waiting for OCR on " + host);
            std::string body;
            kt::MemReader upload(compact);
            std::vector<std::string> headers = {"X-Title-Id: " + TitleIdHex(), "X-Shot-Name: " + shotName};
            ok = kt::netPost(host, port, "/ocr", headers, upload, 2500, 25000, body, pcError) &&
                 kt::parseOcrReply(body, sentence, chunks, pcError);
            if (ok) g_pcDownUntil = 0;
        }
    } else {
        pcError = "PC not reachable a moment ago";
    }
    std::string prefix = localError.empty() ? "" : "On the 3DS: " + localError + "\n";
    std::vector<std::string> paragraphs;
    bool viaLens = false;
    if (!ok && g_lensEnabled) {
        if (tryPc) g_pcDownUntil = osGetTime() + 2 * 60 * 1000;  // don't wait for the PC again for a while
        std::string lensError;
        viaLens = LensRead(shotName, paragraphs, lensError);
        if (!viaLens) error = prefix + "PC: " + pcError + "\nGoogle Lens: " + lensError;
    } else if (!ok) {
        error = prefix + (prefix.empty() ? "" : "PC: ") + pcError;
    }
    kt::netStop();  // never leave the network running while the game plays
    if (viaLens) {
        if (PickParagraph(paragraphs, sentence) != 1) { error = "cancelled"; return false; }
        ShowBusy("Finding the words...");
        chunks = kt::chunkText(g_dict, sentence);
        ok = true;
    }
    return ok;
}

static void TapWord(const std::string &sentence, const std::vector<std::string> &chunks) {
    std::vector<std::string> labels;
    for (const std::string &c : chunks) labels.push_back(c);  // pick where the word starts
    labels.push_back("[ Type a word instead ]");
    labels.push_back("[ Kanji parts ]");
    int sel = 0;
    while (true) {
        if (g_snap.valid) ClearTopPanel();  // the plain game picture while picking a word
        int c = BottomList(labels, sel);
        if (c < 0) return;
        sel = c;
        if (c == (int)chunks.size()) { LookupTyped(nullptr); continue; }
        if (c == (int)chunks.size() + 1) { LookupByParts(nullptr); continue; }
        size_t offset = 0;
        for (int i = 0; i < c; i++) offset += chunks[i].size();
        kt::SearchOutput so = kt::scanAt(g_dict, sentence, offset);
        if (so.results.empty()) {
            OfferManual("No match", "Nothing in the dictionary starts at 「" + chunks[c] + "」.");
            continue;
        }
        g_last = so;
        g_lastSentence = sentence;
        ShowResults(so, sentence);
    }
}

// OCR the given screenshot (or reuse the last result for it), then let the player pick a word.
static void ReadShot(const std::string &shot) {
    g_lookupShot = shot;
    if (g_ocr.shot != shot || g_ocr.sentence.empty()) {
        std::string sentence, error;
        std::vector<std::string> chunks;
        if (!RunOcr(shot, sentence, chunks, error)) {
            if (error == "cancelled") return;
            OfferManual("Couldn't read the screen:", error +
                        (g_lensEnabled ? "" : "\nIs ocr_server.py running on your PC, on the same Wi-Fi?"));
            return;
        }
        g_ocr.shot = shot;
        g_ocr.sentence = sentence;
        g_ocr.chunks = chunks;
    }
    TapWord(g_ocr.sentence, g_ocr.chunks);
}

static void ReadScreen(MenuEntry *) {
    if (!EnsureDict()) return;
    if (!g_ocrEnabled) {
        LookupTyped(nullptr);
        return;
    }
    std::string shot = RecentScreenshot();
    if (shot.empty()) {
        OfferManual("No screenshot yet.", std::string("Close this menu and press ") + kHotkeys[g_hotkey].name +
                                              " while the line is on screen.");
        return;
    }
    g_lookupOpen = true;
    ReadShot(shot);
    g_lookupShot = "";
    ReleaseLookupMemory();
    g_lookupOpen = false;
}

// ---------------------------------------------------------------------------
// One-button lookup: the screenshot hotkey also opens the lookup, with the game paused and
// still visible on the top screen.
// ---------------------------------------------------------------------------
static bool WaitForScreenshot(u32 takenBefore, std::string &shot) {
    const u32 fullSize = 54 + 400 * 480 * 3;  // CTRPF writes both screens, 24-bit
    for (int i = 0; i < 60; i++) {           // up to ~3 s
        if (g_shotsTaken > takenBefore) {
            shot = RecentScreenshot();
            File f;
            if (!shot.empty() && File::Open(f, std::string(kShotDir) + "/" + shot, File::READ) == File::SUCCESS) {
                u64 size = f.GetSize();
                f.Close();
                if (size >= fullSize) return true;
            }
        }
        Sleep(Milliseconds(50));
    }
    return false;
}

static void HotkeyWatcher(void) {  // called every frame by the plugin menu while the game runs
    u32 keys = kHotkeys[g_hotkey].keys;
    if (!g_hotkeyOpens || !keys || !Controller::IsKeysPressed(keys)) return;
    u32 before = g_shotsTaken;
    std::string shot;
    bool haveShot = WaitForScreenshot(before, shot);
    if (!EnsureDict()) return;
    g_lookupOpen = true;
    Process::Pause();
    TakeTopSnapshot();  // the paused game, to draw definitions over
    g_lookupShot = haveShot ? shot : "";
    if (!g_ocrEnabled) {
        LookupTyped(nullptr);
    } else if (!haveShot) {
        OfferManual("The screenshot wasn't saved in time.", "Try the hotkey again, or look the word up:");
    } else {
        ReadShot(shot);
    }
    g_lookupShot = "";
    ReleaseLookupMemory();
    g_lookupOpen = false;
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
static MenuEntry *g_hotkeyEntry = nullptr;

static std::string HotkeyEntryName() {
    return std::string("Hotkey: ") + kHotkeys[g_hotkey].name + (g_hotkeyOpens ? " (screenshot + lookup)" : " (screenshot)");
}

static void ChooseHotkey(MenuEntry *entry) {
    std::vector<std::string> opts;
    for (const HotkeyChoice &h : kHotkeys) opts.push_back(h.name);
    opts.push_back(g_hotkeyOpens ? "[ Hotkey only takes a screenshot ]" : "[ Hotkey takes a screenshot AND opens the lookup ]");
    Keyboard kb("Hotkey\nPressed in-game on a line: saves a screenshot and (if on) opens the lookup\n"
                "with the game still visible on the top screen. Pick a combo your game doesn't use.",
                opts);
    kb.ChangeSelectedEntry(g_hotkey);
    int c = kb.Open();
    if (c < 0) return;
    if (c == kHotkeyCount) g_hotkeyOpens = !g_hotkeyOpens;
    else g_hotkey = c;
    ApplyHotkey();
    SaveSettings();
    entry->Name() = HotkeyEntryName();
}

static MenuEntry *g_serverEntry = nullptr;

static std::string ServerEntryName() {
    if (!g_ocrEnabled) return "OCR server: off (manual lookup only)";
    return "OCR server: " + (g_server.empty() ? std::string("find automatically") : g_server);
}

static void ChooseServer(MenuEntry *entry) {
    std::vector<std::string> opts = {
        "Find my PC automatically now",
        "Type the address (shown by ocr_server.py)",
        "Forget the saved address",
        g_ocrEnabled ? "Turn OCR off (Read screen = typing)" : "Turn OCR on",
        g_cardsEnabled ? "Stop making Anki cards in-game (use mine.py)" : "Make Anki cards in-game when saving",
        g_lensEnabled ? "Turn off Google Lens (when the PC isn't found)" : "Use Google Lens when the PC isn't found",
        g_localEnabled ? "Turn off reading on the 3DS (offline OCR)" : "Read on the 3DS first (offline OCR)",
    };
    std::string model = FindModel();
    Keyboard kb("OCR server\nRun tools/ocr_server.py on your PC, on the same Wi-Fi as the 3DS.\n" +
                    std::string(Color::Gray) + "Now: " + (g_server.empty() ? std::string("not set") : g_server) +
                    "\nOffline OCR model: " + (model.empty() ? std::string("none in /3ds/kotoba3ds/ocr/") : model),
                opts);
    int c = kb.Open();
    if (c < 0) return;
    std::string err;
    if (c == 0) {
        std::string host;
        int port = kOcrPort;
        bool ok = kt::netStart(err) && kt::netDiscover(kOcrPort, 2000, host, port, err);
        kt::netStop();
        if (ok) {
            g_server = host + Utils::Format(":%d", port);
            g_ocrEnabled = true;
            MessageBox("Kotoba3DS", "Found your PC at " + g_server)();
        } else {
            MessageBox("Kotoba3DS", "Couldn't find it automatically:\n" + err +
                                        "\n\nCheck the server is running, or type its address.")();
        }
    } else if (c == 1) {
        Keyboard in("Type the address shown by ocr_server.py, e.g. 192.168.1.20:7331");
        std::string addr = g_server;
        if (in.Open(addr, addr) >= 0) {
            std::string host;
            int port;
            if (!kt::parseHostPort(addr, host, port)) {
                MessageBox("Kotoba3DS", "That doesn't look like an address (example: 192.168.1.20:7331).")();
            } else {
                g_server = host + Utils::Format(":%d", port);
                g_ocrEnabled = true;
                bool ok = kt::netStart(err) && kt::netPing(host, port, 2000, err);
                kt::netStop();
                MessageBox("Kotoba3DS", ok ? "Connected to " + g_server
                                           : "Saved " + g_server + ", but it didn't answer:\n" + err)();
            }
        }
    } else if (c == 2) {
        g_server = "";
    } else if (c == 3) {
        g_ocrEnabled = !g_ocrEnabled;
    } else if (c == 4) {
        g_cardsEnabled = !g_cardsEnabled;
    } else if (c == 5) {
        g_lensEnabled = !g_lensEnabled;
    } else {
        g_localEnabled = !g_localEnabled;
    }
    g_pcDownUntil = 0;
    SaveSettings();
    entry->Name() = ServerEntryName();
}

static const char *kHelp =
    "1. In-game, press the hotkey (ZL+ZR) on a line with a word you don't know.\n"
    "   It saves a screenshot and opens the lookup on the bottom screen, with the\n"
    "   game still visible on top. (Or press Select later and use this menu.)\n"
    "2. Settings and saved words are in this menu (Select).\n"
    "3. 'Read screen': with tools/ocr_server.py running on your PC, the line is\n"
    "   read for you; pick where the word starts. With an OCR model in\n"
    "   /3ds/kotoba3ds/ocr/ the 3DS reads it by itself, offline. Otherwise\n"
    "   Google Lens (internet), or type the word.\n"
    "   'Look up word': type it in romaji. Conjugated forms work (tabenakatta).\n"
    "   'Look up by kanji parts': pick the parts of a kanji you can't read.\n"
    "4. Pick a result, read it, choose Yes to save it.\n"
    "5. On your PC, run tools/mine.py to turn saved words into Anki cards.\n\n"
    "Files: SD:/3ds/kotoba3ds/";

void PatchProcess(FwkSettings &settings) {
    settings.AllowActionReplay = false;
    settings.AllowSearchEngine = false;
}

void OnProcessExit(void) {}

int main(void) {
    PluginMenu *menu = new PluginMenu("Kotoba3DS", 1, 0, 0,
                                      "In-game Japanese dictionary that saves words for Anki.\n\n"
                                      "Press Select in-game to open this menu.");
    menu->SynchronizeWithFrame(true);

    Directory::Create(kRootDir);
    Directory::Create(kShotDir);
    LoadSettings();
    InitScreenshots();

    *menu += new MenuEntry("Read screen (tap a word)", nullptr, ReadScreen,
                           "Reads your last screenshot with OCR on your PC (tools/ocr_server.py),\n"
                           "then you pick the word. Falls back to typing if the PC isn't reachable.");
    *menu += new MenuEntry("Look up word (romaji)", nullptr, LookupTyped,
                           "Type a word in romaji; you'll see the kana as you type.\n"
                           "Conjugated verbs and adjectives are understood.");
    *menu += new MenuEntry("Look up by kanji parts", nullptr, LookupByParts,
                           "For kanji you can't read: pick the parts you can see.");
    *menu += new MenuEntry("Last results", nullptr,
                           [](MenuEntry *) {
                               if (g_last.results.empty())
                                   MessageBox("Kotoba3DS", "No search yet.")();
                               else if (EnsureDict())
                                   ShowResults(g_last, g_lastSentence);
                           },
                           "Reopen your previous search.");
    *menu += new MenuEntry("Saved words", nullptr, SavedWords, "Review or delete saved words.");
    g_hotkeyEntry = new MenuEntry(HotkeyEntryName(), nullptr, ChooseHotkey,
                                  "Button combo that saves a screenshot to SD:/3ds/kotoba3ds/shots/.");
    *menu += g_hotkeyEntry;
    g_serverEntry = new MenuEntry(ServerEntryName(), nullptr, ChooseServer,
                                  "Where Read screen sends screenshots for OCR.");
    *menu += g_serverEntry;
    *menu += new MenuEntry("How to use", nullptr, [](MenuEntry *) { MessageBox("Kotoba3DS", kHelp)(); });

    *menu += HotkeyWatcher;  // runs every frame while the game plays
    menu->Run();
    // Don't `delete menu` (or free anything else) here: the game is exiting and the
    // plugin heap may already be unusable. See the note at Leaky().
    return 0;
}

}  // namespace CTRPluginFramework
