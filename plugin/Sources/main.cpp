// Kotoba3DS - in-game Japanese dictionary + Anki mining log, as a Luma3DS (CTRPF) plugin.
//
// In a game, press the hotkey (default ZL+ZR) on a line with a word you don't know: the game
// pauses, the 3DS reads the top screen itself (on-device OCR, plugin/Sources/vocr) and shows the
// text on the touchscreen: tap a word (or move with the D-pad) and its entry shows on the top
// screen over the dimmed game (the layout follows Rubi for Vita's overlay). Saved words go to
// SD:/3ds/kotoba3ds/mined.tsv with a screenshot; tools/mine.py on the PC (or ocr_server.py,
// right away) turns them into Anki cards through AnkiConnect.
#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <malloc.h>
#include <unistd.h>

#include "core/bmp.hpp"
#include "core/dict.hpp"
#include "core/correct.hpp"
#include "core/local_ocr.hpp"
#include "core/lookup_view.hpp"
#include "core/net.hpp"
#include "core/search.hpp"
#include "core/textbox.hpp"
#include "core/textlines.hpp"
#include "core/wrap.hpp"
#include "screen_ui.hpp"

extern "C" u32 __ctru_heap_size;  // libctru: the plugin's heap (what Luma gave it, minus code and CTRPF's own)

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
static std::vector<std::string> *g_lastBlocksPtr = nullptr;  // the text of the last lookup
#define g_reader Leaky(g_readerPtr)
#define g_dict Leaky(g_dictPtr)
#define g_dictError Leaky(g_dictErrorPtr)
#define g_lastBlocks Leaky(g_lastBlocksPtr)

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
// The paused game's top screen, kept while a lookup is open: read by the OCR, drawn (dimmed)
// under the definitions, saved as the card's screenshot. Kept in the framebuffer's own layout,
// 400 columns of 240 pixels from the bottom up, so it goes back with one copy per column.
// ---------------------------------------------------------------------------
struct TopSnapshot {
    bool valid = false;
    u32 bpp = 0;
    GSPFormat format = GSP_RGBA8_OES;
    std::vector<u8> px;
};
static TopSnapshot *g_snapPtr = nullptr;
#define g_snap Leaky(g_snapPtr)

static u8 *Column(const Screen &s, u32 x) { return s.GetFramebuffer(x, 239); }  // its pixel at y = 239

static void TakeTopSnapshot() {  // call while the game is paused and nothing is drawn on top
    const Screen &top = OSD::GetTopScreen();
    g_snap.bpp = top.BytesPerPixel;
    g_snap.format = top.Format;
    if (g_snap.bpp < 2 || g_snap.bpp > 4) { g_snap.valid = false; return; }
    const u32 col = 240 * g_snap.bpp;
    g_snap.px.resize(400 * col);
    for (u32 x = 0; x < 400; x++) memcpy(&g_snap.px[x * col], Column(top, x), col);
    g_snap.valid = true;
}

// The snapshot as R, G, B bytes, row by row from the top (malloc'd; free() it), or nullptr.
static uint8_t *SnapshotRGB() {
    if (!g_snap.valid) return nullptr;
    uint8_t *rgb = (uint8_t *)malloc(400 * 240 * 3);
    if (!rgb) return nullptr;
    for (u32 x = 0; x < 400; x++)
        for (u32 k = 0; k < 240; k++)  // k-th pixel of the column = y 239 - k
            kt::framebufferToRGB(&g_snap.px[(x * 240 + k) * g_snap.bpp], 1, (int)g_snap.format, g_snap.bpp,
                                 rgb + ((239 - k) * 400 + x) * 3);
    return rgb;
}

static inline void Darken(u8 *d, const u8 *s, u32 bpp, GSPFormat fmt) {
    if (bpp >= 3) {  // 8 bits per channel: keep ~19%
        for (u32 i = 0; i < bpp; i++) d[i] = (u8)((s[i] >> 3) + (s[i] >> 4));
        if (fmt == GSP_RGBA8_OES) d[0] = s[0];  // alpha
        return;
    }
    // 16 bits: keep 1/4 (shift each field by 2; masks drop the bits that cross into a neighbour)
    u16 v = (u16)(s[0] | (s[1] << 8));
    if (fmt == GSP_RGB565_OES) v = (u16)((v >> 2) & 0x39E7);
    else if (fmt == GSP_RGBA4_OES) v = (u16)(((v >> 2) & 0x3330) | (v & 0xF));
    else v = (u16)(((v >> 2) & 0x39CE) | (v & 1));  // RGB5A1
    d[0] = (u8)(v & 0xFF);
    d[1] = (u8)(v >> 8);
}

// The paused game onto the top screen's draw buffer, dimmed or as it was.
static bool PaintGame(bool dim) {
    const Screen &top = OSD::GetTopScreen();
    if (!g_snap.valid || top.BytesPerPixel != g_snap.bpp) return false;
    const u32 bpp = g_snap.bpp, col = 240 * bpp;
    for (u32 x = 0; x < 400; x++) {
        u8 *d = Column(top, x);
        const u8 *src = &g_snap.px[x * col];
        if (!dim) memcpy(d, src, col);
        else for (u32 k = 0; k < col; k += bpp) Darken(d + k, src + k, bpp, g_snap.format);
    }
    return true;
}

// Shows what was drawn, and copies it into the other buffers (CTRPF alternates between two per
// screen; the right eye's too when the game's 3D is on), so one drawing serves them all.
static void Present() {
    const Screen &t = OSD::GetTopScreen(), &b = OSD::GetBottomScreen();
    u8 *top = (u8 *)t.LeftFramebuffer, *bottom = (u8 *)b.LeftFramebuffer;
    const u32 topBytes = 400 * t.Stride, bottomBytes = 320 * b.Stride;
    if (t.Is3DEnabled && t.RightFramebuffer && (u8 *)t.RightFramebuffer != top) memcpy((u8 *)t.RightFramebuffer, top, topBytes);
    OSD::SwapBuffers();
    const Screen &t2 = OSD::GetTopScreen(), &b2 = OSD::GetBottomScreen();
    if ((u8 *)t2.LeftFramebuffer != top && t2.Stride * 400 == topBytes) memcpy((u8 *)t2.LeftFramebuffer, top, topBytes);
    if (t2.Is3DEnabled && t2.RightFramebuffer && (u8 *)t2.RightFramebuffer != top) memcpy((u8 *)t2.RightFramebuffer, top, topBytes);
    if ((u8 *)b2.LeftFramebuffer != bottom && b2.Stride * 320 == bottomBytes) memcpy((u8 *)b2.LeftFramebuffer, bottom, bottomBytes);
}

static void ClearTopPanel() {  // the paused game as it was
    if (!PaintGame(false)) return;
    Present();
}

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

// Saves a word to mined.tsv (and sends it to the PC). `status` gets the line to show.
static bool SaveWord(const kt::Entry &e, const kt::Result &r, const std::string &query, const std::string &sentence,
                     std::string &status) {
    ShowBusy("Saving...");
    std::string shot = SaveScreenshot();
    bool fresh = File::Exists(kLogPath) != 1;
    File f;
    if (File::Open(f, kLogPath, File::WRITE | File::CREATE | File::APPEND | File::SYNC) != File::SUCCESS) {
        status = std::string("Could not write ") + kLogPath;
        return false;
    }
    std::string text;
    if (fresh)
        text += "#time\ttitle_id\texpression\treading\tdefinition\tscreenshot\tquery\tinflection\tentry\tsentence\n";
    std::string line = kt::mineLogLine(Timestamp(), TitleIdHex(), e, r, shot, query, sentence);
    text += line + "\n";
    bool ok = f.Write(text.data(), text.size()) == File::SUCCESS;
    f.Close();
    if (!ok) {
        status = "Could not save (is the SD card full?)";
        return false;
    }
    int n = CountSaved();
    std::string pc = SendCardToPc(line, shot);  // saved on the SD card first, so nothing is lost
    status = "Saved " + kt::cardExpressionFor(e, r.matchedKey) + Utils::Format(" (%d word%s)", n, n == 1 ? "" : "s") +
             (pc.empty() ? "" : " · " + pc);
    return true;
}

// ---------------------------------------------------------------------------
// The lookup screens (core/lookup_view): the text on the touchscreen, the entry on the top screen
// ---------------------------------------------------------------------------
static void LogOcr(const std::string &line);
static SystemFont *g_fontPtr = nullptr;
#define g_font Leaky(g_fontPtr)
static kt::TextRenderer *g_textPtr = nullptr;
static kt::LookupView *g_viewPtr = nullptr;

static kt::LookupView &View() {
    if (!g_textPtr) g_textPtr = new kt::TextRenderer(g_font);
    if (!g_viewPtr) g_viewPtr = new kt::LookupView(g_dict, *g_textPtr);
    return *g_viewPtr;
}

// Frees the lookup screens and their caches (glyphs, matches) when they close: left in place,
// their many small blocks would split the free memory the next OCR model load needs whole.
static void DropView() {
    delete g_viewPtr;
    g_viewPtr = nullptr;
    delete g_textPtr;
    g_textPtr = nullptr;
}

// Draws both screens once and shows them. Returns the milliseconds it took.
static u32 DrawLookup(kt::LookupView &v) {
    u64 t0 = osGetTime();
    if (!PaintGame(!v.hidden())) OSD::GetTopScreen().DrawRect(0, 0, 400, 240, Color(15, 18, 22));
    ScreenCanvas tc(OSD::GetTopScreen());
    v.drawTop(tc);
    ScreenCanvas bc(OSD::GetBottomScreen());
    v.drawBottom(bc);
    Present();
    return (u32)(osGetTime() - t0);
}

static void SaveSelected(kt::LookupView &v) {
    const kt::Result *r = v.result();
    if (!r) return;
    kt::Entry e;
    std::string status;
    if (!g_dict.entry(r->entryId, e)) {
        v.setStatus("Could not read this entry.", kt::Rgb(255, 170, 90));
        return;
    }
    bool ok = SaveWord(e, *r, r->surface.empty() ? v.query() : r->surface, v.sentence(), status);
    if (ok) v.markSaved();
    bool pcProblem = status.find("run mine.py later") != std::string::npos;
    v.setStatus(status, !ok || pcProblem ? kt::Rgb(255, 170, 90) : kt::Rgb(123, 216, 143));
}

// Timings of the lookup screens, written to ocr_log.txt when they close.
struct ViewStats {
    u32 moves = 0, prefetched = 0;
    u64 lookSum = 0, lookMax = 0, drawSum = 0, drawMax = 0, draws = 0;
};

// Runs the lookup screens until B / Close. Tap a character; D-pad or Circle Pad: words and rows;
// L/R: matches; C-stick: scroll; A save, X hide.
static void RunLookupView(kt::LookupView &v) {
    ViewStats st;
    OSD::SwapBuffers();  // let CTRPF finish any pending redraw of its own first
    DrawLookup(v);
    bool touchWas = true;  // a touch held from before doesn't count
    u32 repeatKey = 0;
    u64 repeatAt = 0;
    const u32 kLeft = Key::DPadLeft | Key::CPadLeft, kRight = Key::DPadRight | Key::CPadRight,
              kUp = Key::DPadUp | Key::CPadUp, kDown = Key::DPadDown | Key::CPadDown,
              kScroll = Key::CStickUp | Key::CStickDown, kMoves = kLeft | kRight | kUp | kDown | kScroll;
    while (true) {
        Controller::Update();
        u32 down = Controller::GetKeysPressed(), held = Controller::GetKeysDown();
        bool changed = false;
        u64 t0 = osGetTime();
        bool moved = false;
        if (down & Key::B) break;
        if (down & Key::A) { SaveSelected(v); changed = true; }
        if (down & Key::X) { v.setHidden(!v.hidden()); changed = true; }
        if (down & Key::L) moved |= v.page(-1);
        if (down & Key::R) moved |= v.page(1);
        // moving: on the press, then repeating while held
        u32 k = down & kMoves;
        if (k) {
            repeatKey = k;
            repeatAt = t0 + 300;
        } else if (repeatKey && (held & repeatKey)) {
            if (t0 >= repeatAt) {
                k = held & repeatKey;
                repeatAt = t0 + ((repeatKey & kScroll) ? 50 : 90);
            }
        } else {
            repeatKey = 0;
        }
        if (k & kRight) moved |= v.moveWord(1);
        else if (k & kLeft) moved |= v.moveWord(-1);
        if (k & kDown) moved |= v.moveRow(1);
        else if (k & kUp) moved |= v.moveRow(-1);
        if (k & Key::CStickDown) changed |= v.scroll(1);
        else if (k & Key::CStickUp) changed |= v.scroll(-1);
        // touch: acts on the press
        bool touch = Touch::IsDown();
        if (touch && !touchWas) {
            UIntVector p = Touch::GetPosition();
            kt::LookupView::Button b = kt::LookupView::kNone;
            moved |= v.tap((int)p.x, (int)p.y, b);
            if (b == kt::LookupView::kClose) break;
            if (b == kt::LookupView::kSave) { SaveSelected(v); changed = true; }
            if (b == kt::LookupView::kHide) { v.setHidden(!v.hidden()); changed = true; }
        }
        touchWas = touch;
        if (moved) {
            u64 ms = osGetTime() - t0;
            st.moves++;
            st.lookSum += ms;
            if (ms > st.lookMax) st.lookMax = ms;
        }
        if (moved || changed) {
            u32 ms = DrawLookup(v);
            st.draws++;
            st.drawSum += ms;
            if (ms > st.drawMax) st.drawMax = ms;
            continue;  // straight back to the buttons
        }
        // idle: look up the words the next press could go to, one per pass
        if (!held && v.prefetch()) { st.prefetched++; continue; }
        Sleep(Milliseconds(10));
    }
    ClearTopPanel();  // the game as it was
    if (st.draws)
        LogOcr(Utils::Format("view: %lu moves, look-up avg %lu ms (max %lu), draw avg %lu ms (max %lu), %lu looked up ahead",
                             (unsigned long)st.moves, (unsigned long)(st.moves ? st.lookSum / st.moves : 0),
                             (unsigned long)st.lookMax, (unsigned long)(st.drawSum / st.draws), (unsigned long)st.drawMax,
                             (unsigned long)st.prefetched));
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

// OCR misreads the dictionary can fix: 造跡 -> 遺跡 (遺 was the recognizer's second guess).
static unsigned g_fixedChars = 0;
static void FixLine(std::string &text, const std::vector<kt::OcrAlt> &alts, void *) {
    g_fixedChars += kt::fixOcrLine(g_dict, text, alts);
}

// Free heap: what malloc has free inside the heap plus what it hasn't claimed yet (nothing is
// allocated to find out).
extern "C" char *fake_heap_end;
static u32 HeapFree() {
    struct mallinfo mi = mallinfo();
    char *top = (char *)sbrk(0);
    return (u32)mi.fordblks + (top && top < fake_heap_end ? (u32)(fake_heap_end - top) : 0);
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
    // 1. the model first, while the plugin's memory is in one piece (it needs ~1.55 MB in a few
    //    big blocks; it is loaded for this read only)
    u64 t1 = osGetTime();
    SdReader mf;
    if (!mf.open(kModelDir + name)) { error = "could not open " + name; return false; }
    if (!g_localOcr.load(mf, error)) {
        LogOcr(Utils::Format("%s: load failed (%s), heap free %lu KB", name.c_str(), error.c_str(),
                             (unsigned long)(HeapFree() / 1024)));
        return false;
    }
    g_modelName = name;
    u64 t2 = osGetTime();
    // 2. the picture: find the text box (by its colour), then grey for the OCR
    uint8_t *rgb = SnapshotRGB();
    if (!rgb) { g_localOcr.unload(); error = "not enough memory to copy the screen"; return false; }
    std::vector<kt::TextBox> boxes = kt::findTextBoxes(rgb, 400, 240);
    uint8_t *grey = kt::greyFromRGB(rgb, 400, 240);
    free(rgb);
    if (!grey) { g_localOcr.unload(); error = "not enough memory for the picture"; return false; }
    // 3. read the box (or the whole screen)
    u64 t2b = osGetTime();
    g_localOcr.setParallel(System::IsNew3DS() ? 2 : 1, RunOnCores);
    g_localOcr.setFixer(FixLine, nullptr);
    g_fixedChars = 0;
    int used = -1;
    bool ok = kt::localReadScreen(g_localOcr, grey, 400, 240, boxes, paragraphs, error, LocalProgress, &used);
    free(grey);
    g_localOcr.unload();  // ~1.4 MB back right away
    u64 t3 = osGetTime();
    LogOcr(Utils::Format("%s: load %lu ms, box %lu ms, read %lu ms (%s, %u threads, %d MHz, %u fixed), heap free %lu KB, %s",
                         name.c_str(), (unsigned long)(t2 - t1 + (t1 - t0)), (unsigned long)(t2b - t2),
                         (unsigned long)(t3 - t2b), used >= 0 ? "text box" : "whole screen", g_localOcr.lastWorkers(),
                         CpuMHz(), g_fixedChars, (unsigned long)(HeapFree() / 1024),
                         ok ? paragraphs[0].c_str() : error.c_str()));
    return ok;
}

static void Lookup() {
    std::vector<std::string> paragraphs;
    std::string error;
    if (!ReadSnapshot(paragraphs, error)) {
        BottomMessage("Couldn't read the screen:\n" + error, {"[ Close ]"});
        return;
    }
    // the longest block first (the dialogue, usually)
    std::stable_sort(paragraphs.begin(), paragraphs.end(),
                     [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    ShowBusy("Finding the words...");
    g_lastBlocks = paragraphs;
    static bool fontLogged = false;
    if (!fontLogged) { LogOcr(g_font.describe()); fontLogged = true; }
    kt::LookupView &v = View();
    v.setText(paragraphs);
    RunLookupView(v);
    DropView();
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
    "2. The text shows on the touchscreen. Tap a word (or move with the D-pad\n"
    "   or Circle Pad) and its entry shows on the top screen.\n"
    "   L/R: other meanings, C-stick: scroll, X: hide it to see the game.\n"
    "3. A saves the word to mined.tsv with a screenshot. B closes.\n"
    "4. Anki cards: with tools/ocr_server.py running on your PC they're made\n"
    "   right away; otherwise run tools/mine.py later.\n\n"
    "Needs: /3ds/kotoba3ds/dict.bin and the OCR model in /3ds/kotoba3ds/ocr/.";

// Runs before the game's first instruction: records the game's memory budget as the game will
// see it (Nintendo's SDK sizes the game's heap from these), for games that crash at start.
static void LogBootMemory() {
    s64 appRegion = 0, appUsed = 0, sysUsed = 0, baseUsed = 0;
    svcGetSystemInfo(&appRegion, 0x10000, 0x80);  // Luma: the APPLICATION region's size
    svcGetSystemInfo(&appUsed, 0, 1);
    svcGetSystemInfo(&sysUsed, 0, 2);
    svcGetSystemInfo(&baseUsed, 0, 3);
    u32 appMemAlloc = *(vu32 *)0x1FF80040;  // what the game is told it may use
    s64 commitMax = -1, commitNow = -1;
    Handle rl = 0;
    if (R_SUCCEEDED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) {
        ResourceLimitType t = RESLIMIT_COMMIT;
        svcGetResourceLimitLimitValues(&commitMax, rl, &t, 1);
        svcGetResourceLimitCurrentValues(&commitNow, rl, &t, 1);
        svcCloseHandle(rl);
    }
    LogOcr(Utils::Format("boot: game %s, APPMEMALLOC %lu KB, commit limit %ld KB, committed %ld KB, app region %ld KB "
                         "(used %ld KB), system used %ld KB, base used %ld KB, plugin heap %lu KB",
                         TitleIdHex().c_str(), (unsigned long)(appMemAlloc / 1024), (long)(commitMax / 1024),
                         (long)(commitNow / 1024), (long)(appRegion / 1024), (long)(appUsed / 1024),
                         (long)(sysUsed / 1024), (long)(baseUsed / 1024), (unsigned long)(__ctru_heap_size / 1024)));
}

// ---------------------------------------------------------------------------
// Giving the game back the memory the plugin takes
//
// On a New 3DS, Luma maps the plugin's memory (5 MiB) into the game and it counts against the
// game's 64 MB budget (its "commit" limit). Nintendo's SDK sizes a game's heap at start as
//     heap = min(APPMEMALLOC, 64 MB) - memory already committed - device memory
// so the heap comes out 5 MiB short; a game that needs all of it (大逆転裁判2) fails an
// allocation and crashes. The console has the memory (the app region is 124 MB), so before the
// game's first instruction: raise the game's commit limit by the plugin's size, and make the SDK's
// formula leave the plugin's share out of "already committed". Done only when the numbers say it
// is safe, and the formula only when its instructions are found exactly once.
// ---------------------------------------------------------------------------
static s64 g_commitLimitBefore = -1;  // to put back when the game exits

static bool ArmImmediate(u32 v, u32 &enc) {  // ARM data-processing immediate (8 bits rotated)
    for (u32 rot = 0; rot < 16; rot++) {
        u32 r = (v << (2 * rot)) | (rot ? v >> (32 - 2 * rot) : 0);  // rotate left by 2*rot
        if (r < 256) { enc = (rot << 8) | r; return true; }
    }
    return false;
}

static void GiveGameItsMemory() {
    PluginHeader *h = FwkSettings::Header;
    if (!h) return;
    const u32 plugin = h->heapSize + h->exeSize;  // the block Luma took from the game
    const u32 appMemAlloc = *(vu32 *)0x1FF80040;
    s64 appRegion = 0;
    svcGetSystemInfo(&appRegion, 0x10000, 0x80);
    Handle rl = 0;
    if (R_FAILED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) return;
    ResourceLimitType t = RESLIMIT_COMMIT;
    s64 limit = -1, committed = -1;
    svcGetResourceLimitLimitValues(&limit, rl, &t, 1);
    svcGetResourceLimitCurrentValues(&committed, rl, &t, 1);
    const u32 codeSize = Process::GetTextSize() + Process::GetRoDataSize() + Process::GetRwDataSize();
    std::string why;
    // only for a game told it has at most 64 MB (the SDK's cap, so the formula's cap does nothing),
    // whose commit already holds the plugin's block, with room left in the app region
    const s64 want = (s64)appMemAlloc + plugin;
    if (appMemAlloc == 0 || appMemAlloc > 0x4000000) why = "APPMEMALLOC above 64 MB";
    else if (limit < (s64)appMemAlloc) why = "unusual commit limit";
    else if (committed < (s64)(codeSize + plugin)) why = "the plugin isn't counted in the game's memory";
    else if (want > appRegion) why = "no room in the app region";
    u32 subImm = 0;
    if (why.empty() && !ArmImmediate(plugin, subImm)) why = "plugin size not encodable";
    // the SDK's  cmp rN, #0x4000000 / movhi rN, #0x4000000 / sub r0, rN, r0
    u32 at = 0, found = 0;
    if (why.empty()) {
        const u32 *text = (const u32 *)0x00100000;
        const u32 words = Process::GetTextSize() / 4;
        for (u32 i = 0; i + 2 < words; i++) {
            u32 w0 = text[i];
            if ((w0 & 0xFFF0FFFF) != 0xE3500301) continue;
            u32 n = (w0 >> 16) & 0xF;
            if (text[i + 1] == (0x83A00301 | (n << 12)) && text[i + 2] == (0xE0400000 | (n << 16))) {
                at = 0x00100000 + i * 4;
                found++;
            }
        }
        if (found != 1) why = Utils::Format("SDK heap sizing found %lu times", (unsigned long)found);
    }
    if (why.empty() && limit < want) {
        // the high word is what the kernel writes to APPMEMALLOC: keep it as it is
        s64 value = want | ((s64)appMemAlloc << 32);
        Result r = svcSetResourceLimitValues(rl, &t, &value, 1);
        if (R_FAILED(r)) why = Utils::Format("could not raise the limit (%08lX)", (unsigned long)r);
        else g_commitLimitBefore = limit;
    }
    svcCloseHandle(rl);
    if (why.empty()) {
        Process::Patch(at, 0xE2400000 | subImm);  // sub r0, r0, #plugin  (committed - plugin)
        Process::Patch(at + 4, 0xE1A00000);       // nop (the 64 MB cap can't apply: checked above)
        LogOcr(Utils::Format("boot: gave the game back %lu KB (commit limit %ld -> %ld KB, heap sizing at %08lX)",
                             (unsigned long)(plugin / 1024), (long)(limit / 1024), (long)(want / 1024),
                             (unsigned long)at));
    } else {
        LogOcr("boot: memory left as it is: " + why);
    }
}

void OnProcessExit(void) {
    if (g_commitLimitBefore < 0) return;  // put the game's limit back for whatever runs next
    Handle rl = 0;
    if (R_SUCCEEDED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) {
        ResourceLimitType t = RESLIMIT_COMMIT;
        s64 value = g_commitLimitBefore | ((s64)*(vu32 *)0x1FF80040 << 32);
        svcSetResourceLimitValues(rl, &t, &value, 1);
        svcCloseHandle(rl);
    }
}

void PatchProcess(FwkSettings &settings) {
    LogBootMemory();
    GiveGameItsMemory();
    settings.AllowActionReplay = false;
    settings.AllowSearchEngine = false;
}


int main(void) {
    PluginMenu *menu = new PluginMenu("Kotoba3DS", 2, 0, 0,
                                      "In-game Japanese dictionary that reads the screen and saves words for Anki.\n\n"
                                      "Press the hotkey (ZL+ZR) in a game to look a word up.");
    menu->SynchronizeWithFrame(true);

    Directory::Create(kRootDir);
    Directory::Create(kShotDir);
    LoadSettings();
    {   // what this game leaves the plugin (the OCR needs ~2.5 MB at its peak)
        LogOcr(Utils::Format("start: game %s, plugin heap %lu KB, heap free %lu KB", TitleIdHex().c_str(),
                             (unsigned long)(__ctru_heap_size / 1024), (unsigned long)(HeapFree() / 1024)));
    }
    {   // CTRPF's own screenshot hotkey stays off: the plugin makes its screenshots itself
        bool *enabled = nullptr;
        u32 *hotkey = nullptr;
        PluginMenu::ScreenshotSettings(&enabled, &hotkey);
        if (enabled) *enabled = false;
    }

    *menu += new MenuEntry("Last results", nullptr,
                           [](MenuEntry *) {
                               if (g_lastBlocks.empty()) {
                                   MessageBox("Kotoba3DS", "No lookup yet.")();
                               } else if (EnsureDict()) {
                                   kt::LookupView &v = View();
                                   v.setText(g_lastBlocks);
                                   RunLookupView(v);
                                   DropView();
                               }
                           },
                           "Reopen the text of your last lookup.");
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
