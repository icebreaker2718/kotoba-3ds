// Kotoba3DS - in-game Japanese dictionary + Anki mining log, as a Luma3DS (CTRPF) plugin.
//
// In a game, press a hotkey (default ZL+ZR: the top screen; the touchscreen and both screens can
// have their own): the game pauses, the 3DS reads the screen itself (on-device OCR,
// plugin/Sources/vocr) and shows the text: tap a word (or move with the Circle Pad) and its entry
// shows over the dimmed game (the layout follows Rubi for Vita's overlay). Saved words go to
// SD:/3ds/kotoba3ds/mined.tsv with a screenshot; "Make Anki cards" in the Select menu sends them
// to tools/ocr_server.py on the PC (or tools/mine.py reads them later).
#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <csvc.h>  // Luma's extra system calls (cache maintenance for the memory patches)

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
#include "core/hotkeys.hpp"
#include "core/known_words.hpp"
#include "core/local_ocr.hpp"
#include "core/mem_share.hpp"
#include "core/lookup_view.hpp"
#include "core/menu_view.hpp"
#include "core/net.hpp"
#include "core/search.hpp"
#include "core/textbox.hpp"
#include "core/textlines.hpp"
#include "screen_ui.hpp"

extern "C" u32 __ctru_heap_size;  // libctru: the plugin's heap (what Luma gave it, minus code and CTRPF's own)

namespace CTRPluginFramework {
extern FS_Archive _sdmcArchive;  // CTRPF's SD card archive (File uses it)
// CTRPF's own record of the system's sleep request (the lid closed), as its menus use it: in
// libctrpf, though its header isn't installed with it.
class SystemImpl {
public:
    static bool WantsToSleep(void);
    static void ReadyToSleep(void);
};

// ---------------------------------------------------------------------------
// Paths & settings
// ---------------------------------------------------------------------------
static const char *kRootDir = "/3ds/kotoba3ds";
static const char *kDictPath = "/3ds/kotoba3ds/dict.bin";
static const char *kLogPath = "/3ds/kotoba3ds/mined.tsv";
static const char *kShotDir = "/3ds/kotoba3ds/shots";
static const char *kSettingsPath = "/3ds/kotoba3ds/settings.txt";
static const char *kModelDir = "/3ds/kotoba3ds/ocr/";
static const char *kKnownPath = "/3ds/kotoba3ds/known.txt";
static const char *kMarkedPath = "/3ds/kotoba3ds/marked.txt";  // words marked known with Y

// What a read covers. Each has its own hotkey (0 = off).
enum ReadMode { kReadTop = 0, kReadBottom = 1, kReadBoth = 2, kModes = 3 };
static const char *const kModeNames[kModes] = {"Read the top screen", "Read the touchscreen", "Read both screens"};
static u32 g_combo[kModes] = {Key::ZL | Key::ZR, 0, 0};
static bool g_kanjiForm = true;  // saved words in their kanji form (or as the game wrote them)
static bool g_textOnTop = false;  // the touchscreen was read: show its text on the top screen
enum HideMode { kHideSeeThrough = 0, kHideHidden = 1, kHideCycle = 2 };
static int g_hideMode = kHideHidden;  // what X does to the screen(s) that were read
static std::string *g_serverPtr = nullptr;  // "192.168.1.20:7331", empty = find automatically
static const int kPcPort = 7331;

template <typename T>
static T &Leaky(T *&slot);
#define g_server Leaky(g_serverPtr)

// Long-lived state is created on first use and deliberately never destroyed. When the game
// closes, the plugin's memory can already be in the middle of being torn down, and freeing
// anything at that point crashes in free() (seen in StreetPass Mii Plaza). The console
// reclaims all plugin memory when the game ends, so nothing is lost by not freeing it.
template <typename T>
static T &Leaky(T *&slot) {
    if (!slot) slot = new T();
    return *slot;
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

static void SaveSettings() {
    WriteWholeFile(kSettingsPath, Utils::Format("key_top=%lu\nkey_bottom=%lu\nkey_both=%lu\ntext_on_top=%d\nhide=%d\n"
                                                "kanji_form=%d\nserver=",
                                                (unsigned long)g_combo[0], (unsigned long)g_combo[1],
                                                (unsigned long)g_combo[2], g_textOnTop ? 1 : 0, g_hideMode,
                                                g_kanjiForm ? 1 : 0) +
                                      g_server + "\n");
}

static std::string SettingValue(const std::string &s, const char *key) {
    std::string k = std::string(key) + "=";
    size_t p = s.find(k);
    while (p != std::string::npos && p > 0 && s[p - 1] != '\n') p = s.find(k, p + 1);  // at a line start
    if (p == std::string::npos) return "";
    size_t e = s.find('\n', p);
    std::string v = s.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

static void LoadSettings() {
    std::string s;
    if (!ReadWholeFile(kSettingsPath, s, 4096)) return;
    static const char *const kKeys[kModes] = {"key_top", "key_bottom", "key_both"};
    bool any = false;
    for (int m = 0; m < kModes; m++) {
        std::string v = SettingValue(s, kKeys[m]);
        if (v.empty()) continue;
        g_combo[m] = (u32)strtoul(v.c_str(), nullptr, 10) & kt::keys::Usable;
        any = true;
    }
    if (!any) {  // settings from before: "hotkey=" picked one of a few combinations
        static const u32 kOld[] = {Key::ZL | Key::ZR, Key::L | Key::R | Key::DPadDown, Key::R | Key::DPadDown,
                                   Key::Start | Key::Select, 0};
        std::string v = SettingValue(s, "hotkey");
        int h = v.empty() ? 0 : atoi(v.c_str());
        if (h >= 0 && h < 5) g_combo[0] = kOld[h] & kt::keys::Usable;  // Select now opens the menu
    }
    std::string v = SettingValue(s, "text_on_top");
    if (!v.empty()) g_textOnTop = v == "1";
    v = SettingValue(s, "hide");
    if (!v.empty()) g_hideMode = std::max(0, std::min(2, atoi(v.c_str())));
    v = SettingValue(s, "kanji_form");
    if (!v.empty()) g_kanjiForm = v != "0";
    g_server = SettingValue(s, "server");
}

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
        return file_.Seek(off, File::SET) == File::SUCCESS && file_.Read(dst, len) == File::SUCCESS;
    }
    uint32_t size() override { return size_; }
private:
    File file_;
    u32 size_ = 0;
};

static SdReader *g_readerPtr = nullptr;
static kt::Dictionary *g_dictPtr = nullptr;
static std::string *g_dictErrorPtr = nullptr;
static std::vector<std::string> *g_lastBlocksPtr = nullptr;  // the text of the last lookup
#define g_reader Leaky(g_readerPtr)
#define g_dict Leaky(g_dictPtr)
#define g_dictError Leaky(g_dictErrorPtr)
#define g_lastBlocks Leaky(g_lastBlocksPtr)
static bool g_lastTextOnTop = false;  // how the last lookup was laid out

static bool OpenDict() {
    if (g_dict.isOpen()) return true;
    if (!g_reader.open(kDictPath)) {
        g_dictError = std::string("The dictionary isn't on the SD card: ") + kDictPath +
                      "\n\nDownload dict.bin from the Kotoba3DS release and copy it there.";
    } else if (!g_dict.open(&g_reader, g_dictError)) {
        // g_dictError set by open()
    }
    return g_dict.isOpen();
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

static void LogOcr(const std::string &line) {  // each read's timing, for troubleshooting
    File f;
    if (File::Open(f, "/3ds/kotoba3ds/ocr_log.txt", File::WRITE | File::CREATE | File::APPEND) == File::SUCCESS) {
        f.Write(line.c_str(), line.size());
        f.Write("\n", 1);
        f.Close();
    }
}

// Free heap: what malloc has free inside the heap plus what it hasn't claimed yet (nothing is
// allocated to find out).
extern "C" char *fake_heap_end;
static u32 HeapFree() {
    struct mallinfo mi = mallinfo();
    char *top = (char *)sbrk(0);
    return (u32)mi.fordblks + (top && top < fake_heap_end ? (u32)(fake_heap_end - top) : 0);
}

// The biggest block malloc can give now (KB): the free memory may be in pieces.
static uint64_t OsTimeMs() { return osGetTime(); }  // for the reader's timings

// Alpha: also time the current reader and the whole screen on each read, for the log.
#define KT_READER_BENCH 1

static u32 LargestFreeKB() {
    u32 lo = 0, hi = HeapFree() / 1024;  // never ask for more than there is
    while (lo < hi) {
        u32 mid = (lo + hi + 1) / 2;
        void *p = malloc((size_t)mid * 1024);
        if (p) { free(p); lo = mid; } else hi = mid - 1;
    }
    return lo;
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
// Drawing: the system font at any size (core/ui_text), the screens' framebuffers
// ---------------------------------------------------------------------------
static SystemFont *g_fontPtr = nullptr;
#define g_font Leaky(g_fontPtr)
static kt::TextRenderer *g_textPtr = nullptr;

static kt::TextRenderer &Text() {
    if (!g_textPtr) g_textPtr = new kt::TextRenderer(g_font);
    return *g_textPtr;
}

static const Screen &ScreenOf(int i) { return i == 0 ? OSD::GetTopScreen() : OSD::GetBottomScreen(); }
static u32 ScreenW(int i) { return i == 0 ? 400 : 320; }
static u8 *Column(const Screen &s, u32 x) { return s.GetFramebuffer(x, 239); }  // its pixel at y = 239

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

static const kt::Rgb kPanel(15, 18, 22), kPanelEdge(70, 78, 90), kInk(237, 237, 237), kGreyInk(138, 144, 153),
    kGood(123, 216, 143), kWarn(255, 170, 90);

// "Reading the screen..." in a box in the middle of the touchscreen, shown at once.
static void ShowBusy(const std::string &line1, const std::string &line2 = "") {
    ScreenCanvas c(OSD::GetBottomScreen());
    c.fill(16, 84, 288, 68, kPanelEdge);
    c.fill(17, 85, 286, 66, kPanel);
    kt::TextRenderer &tr = Text();
    tr.draw(c, 30, line2.empty() ? 107 : 95, line1, 18, kInk);
    if (!line2.empty()) tr.draw(c, 30, 121, line2, 15, kGreyInk);
    Present();
}

// ---------------------------------------------------------------------------
// The paused game's screens, kept while a lookup is open: drawn (dimmed) under the text and the
// entry, saved as the card's screenshot. In the framebuffer's column order (each column bottom to
// top), as R, G, B, or RGB565 when both screens are kept (less memory: the OCR reads the
// framebuffers themselves, before anything is drawn on them).
// ---------------------------------------------------------------------------
struct Picture {
    bool valid = false;
    u32 w = 0, bytes = 3;
    u8 *px = nullptr;  // malloc'd: short of memory, there is just no picture (no abort)
};
static Picture *g_picPtr[2] = {nullptr, nullptr};
static Picture &Pic(int i) { return Leaky(g_picPtr[i]); }

static void TakePicture(int i, u32 bytes) {  // call while the game is paused and nothing is drawn on it
    Picture &p = Pic(i);
    const Screen &s = ScreenOf(i);
    p.valid = false;
    free(p.px);
    p.px = nullptr;
    if (s.BytesPerPixel < 2 || s.BytesPerPixel > 4) return;
    p.w = ScreenW(i);
    p.bytes = bytes;
    p.px = (u8 *)malloc((size_t)p.w * 240 * bytes);
    if (!p.px) return;
    for (u32 x = 0; x < p.w; x++)
        kt::framebufferColumnToPicture(Column(s, x), 240, (int)s.Format, s.BytesPerPixel, bytes, &p.px[(size_t)x * 240 * bytes]);
    p.valid = true;
}

static void DropPictures() {
    for (int i = 0; i < 2; i++) {
        Pic(i).valid = false;
        free(Pic(i).px);
        Pic(i).px = nullptr;
    }
}

// The kept picture onto screen i's draw buffer: tone 0 as it was, 1 dimmed (behind the lookup),
// 2 half (see-through); its top `band` rows darkened further. False (nothing drawn) without one.
static bool PaintScreen(int i, int tone, int band = 0, unsigned shade = 0) {
    Picture &p = Pic(i);
    const Screen &s = ScreenOf(i);
    if (!p.valid || p.w != ScreenW(i) || s.BytesPerPixel < 2) return false;
    const u32 from = band > 0 ? 240 - (u32)std::min(band, 240) : 240, keep = 255 - shade;
    for (u32 x = 0; x < p.w; x++)
        kt::pictureColumnToFramebuffer(&p.px[(size_t)x * 240 * p.bytes], 240, p.bytes, tone, from, keep, (int)s.Format,
                                       s.BytesPerPixel, Column(s, x));
    return true;
}

static void RestoreScreens() {  // the paused game as it was, where it was kept
    bool any = PaintScreen(0, 0);
    any |= PaintScreen(1, 0);
    if (any) Present();
}

// ---------------------------------------------------------------------------
// The plugin's own screens (core/menu_view): a list on the touchscreen, its explanation above
// ---------------------------------------------------------------------------
static void DrawMenu(kt::MenuView &m) {
    ScreenCanvas top(OSD::GetTopScreen()), bottom(OSD::GetBottomScreen());
    m.drawTop(top);
    m.drawBottom(bottom);
    Present();
}

// What happened on a menu screen.
struct MenuEvent {
    enum Type { kChoose, kButton, kKey } type = kKey;
    int index = -1;  // kChoose: the row; kButton: the button
    u32 key = 0;     // kKey: B, Y, Left, Right, or a key from `keys`
};

// Shows the screen and waits: A or a tapped row chooses, a tapped button, or B / Left / Right /
// any of `keys`. Up / down (held: repeating) move the highlight.
// The lid was closed (the system wants to sleep): the game must go on to answer it, or the
// console hangs with the screens off. Every screen of the plugin closes, as if B were pressed.
// The paused game can't answer the system's sleep query (that takes its main thread, which the
// pause holds), so the lid itself is watched: closed, and the plugin lets the game go on at once.
static bool LidClosed() {
    static bool ptmOk = R_SUCCEEDED(ptmuInit());
    static u64 lastAt = 0;
    static bool closed = false;
    const u64 now = osGetTime();
    if (ptmOk && now - lastAt >= 30) {  // a quick question to the power service, ~30 times a second at most
        lastAt = now;
        u8 shell = 1;
        closed = R_SUCCEEDED(PTMU_GetShellState(&shell)) && shell == 0;
    }
    return closed;
}
static bool SleepRequested() { return LidClosed() || SystemImpl::WantsToSleep(); }

static MenuEvent MenuInput(kt::MenuView &m, u32 keys = 0) {
    DrawMenu(m);
    bool touchWas = true;  // a touch held from before doesn't count
    u32 repeatKey = 0;
    u64 repeatAt = 0;
    const u32 kUp = Key::DPadUp | Key::CPadUp, kDown = Key::DPadDown | Key::CPadDown;
    while (true) {
        Controller::Update();
        u32 down = Controller::GetKeysPressed(), held = Controller::GetKeysDown();
        u64 now = osGetTime();
        MenuEvent ev;
        if (SleepRequested()) {
            ev.key = Key::B;
            return ev;
        }
        if (down & Key::A) {
            if (m.selected() >= 0) {
                ev.type = MenuEvent::kChoose;
                ev.index = m.selected();
                return ev;
            }
        }
        u32 k = down & (Key::B | Key::Y | Key::DPadLeft | Key::DPadRight | Key::CPadLeft | Key::CPadRight | keys);
        if (k) {
            ev.key = k;
            return ev;
        }
        u32 mv = down & (kUp | kDown);
        if (mv) {
            repeatKey = mv;
            repeatAt = now + 300;
        } else if (repeatKey && (held & repeatKey) && now >= repeatAt) {
            mv = held & repeatKey;
            repeatAt = now + 90;
        } else if (!(held & repeatKey)) {
            repeatKey = 0;
        }
        bool moved = false;
        if (mv & kDown) moved = m.move(1);
        else if (mv & kUp) moved = m.move(-1);
        bool touch = Touch::IsDown();
        if (touch && !touchWas) {
            UIntVector p = Touch::GetPosition();
            int button = -1, row = m.tap((int)p.x, (int)p.y, button);
            if (button >= 0) {
                ev.type = MenuEvent::kButton;
                ev.index = button;
                return ev;
            }
            if (row >= 0) {
                m.select(row);
                DrawMenu(m);
                ev.type = MenuEvent::kChoose;
                ev.index = row;
                return ev;
            }
        }
        touchWas = touch;
        if (moved) DrawMenu(m);
        else Sleep(Milliseconds(16));
    }
}

// A message with a Close button (B or A also close it).
static void Notice(const std::string &title, const std::string &text, const std::string &help = "") {
    kt::MenuView m(Text());
    m.setTitle(title);
    m.setMessage(text);
    m.setHelp(help);
    m.setButtons({" Close"});
    while (true) {
        MenuEvent ev = MenuInput(m, Key::A);
        if (ev.type == MenuEvent::kButton || (ev.key & (Key::A | Key::B))) return;
    }
}

// A question: the choices as rows (A or a tap picks one). -1 for B.
static int Ask(const std::string &title, const std::string &question, const std::vector<std::string> &choices) {
    kt::MenuView m(Text());
    m.setTitle(title);
    m.setHelp(question);
    std::vector<kt::MenuItem> items;
    for (const std::string &c : choices) {
        kt::MenuItem it;
        it.label = c;
        items.push_back(it);
    }
    m.setItems(items);
    m.setButtons({" Choose", " Back"});
    while (true) {
        MenuEvent ev = MenuInput(m);
        if (ev.type == MenuEvent::kChoose) return ev.index;
        if ((ev.type == MenuEvent::kButton && ev.index == 1) || (ev.key & Key::B)) return -1;
        if (ev.type == MenuEvent::kButton && ev.index == 0) return m.selected();
    }
}

// ---------------------------------------------------------------------------
// Saving words
// ---------------------------------------------------------------------------
static bool g_readTop = true, g_readBottom = false;  // what the open lookup read

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

// mined.tsv's lines (the header and comments too, in order).
static std::vector<std::string> SavedLines() {
    std::string data;
    ReadWholeFile(kLogPath, data);
    std::vector<std::string> lines;
    size_t s = 0;
    while (s < data.size()) {
        size_t e = data.find('\n', s);
        if (e == std::string::npos) e = data.size();
        std::string l = data.substr(s, e - s);
        while (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) lines.push_back(l);
        s = e + 1;
    }
    return lines;
}

static int CountSaved() {
    int n = 0;
    for (const std::string &l : SavedLines()) n += l[0] != '#';
    return n;
}

// The words the player has already, green in lookups: known.txt (the words of their Anki cards,
// from the PC) and the words saved here. Loaded once, kept (reloaded after it changes).
static kt::KnownWords *g_knownPtr = nullptr;
#define g_known Leaky(g_knownPtr)
static bool g_knownLoaded = false;
static size_t g_knownFromAnki = 0;  // words in known.txt
static std::vector<std::string> *g_markedPtr = nullptr;  // marked.txt's words
#define g_marked Leaky(g_markedPtr)

static std::vector<std::string> FileLines(const char *path) {
    std::string data;
    ReadWholeFile(path, data);
    std::vector<std::string> lines;
    for (size_t s = 0; s < data.size();) {
        size_t e = data.find('\n', s);
        if (e == std::string::npos) e = data.size();
        std::string l = data.substr(s, e - s);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty() && l[0] != '#') lines.push_back(l);
        s = e + 1;
    }
    return lines;
}
static const u32 kKnownMax = 1024 * 1024;

static void LoadKnown() {
    if (g_knownLoaded) return;
    g_knownLoaded = true;
    g_known.clear();
    g_knownFromAnki = 0;
    File f;
    if (File::Open(f, kKnownPath, File::READ) == File::SUCCESS) {
        const u64 size = f.GetSize();
        const u32 len = (u32)std::min<u64>(size, kKnownMax);
        char *buf = (char *)malloc(len + 1);
        if (buf && f.Read(buf, len) == File::SUCCESS) {
            g_known.adopt(buf, len, (size_t)size);  // keeps buf
            g_knownFromAnki = g_known.size();
        } else {
            free(buf);
            LogOcr(Utils::Format("known.txt (%lu KB) not loaded: not enough memory", (unsigned long)(size / 1024)));
        }
        f.Close();
    }
    std::vector<std::string> cols, saved;
    for (const std::string &l : SavedLines()) {
        if (l[0] == '#') continue;
        SplitTabs(l, cols);
        if (cols.size() > 2) saved.push_back(cols[2]);
    }
    g_marked = FileLines(kMarkedPath);
    g_marked.reserve(g_marked.size() + 64);
    // room for this lookup session's saves and marks: nothing allocated for them later, among
    // the pictures and the read's buffers
    g_known.reserve(saved.size() + g_marked.size() + 128);
    for (const std::string &w : saved) g_known.add(w);
    for (const std::string &w : g_marked) g_known.add(w);
}

// Y in a lookup: marks the selected word known without saving it (a grammar point, a word you
// know already), or takes the mark back. The spellings go to marked.txt, which mine.py known and
// "Known words" leave alone.
static void MarkKnown(kt::LookupView &v) {
    std::vector<std::string> forms = v.selectedForms();
    if (forms.empty()) {
        v.setStatus("No word here to mark.", kWarn);
        return;
    }
    const std::string word = forms[0];
    bool marked = false;
    for (const std::string &f : forms)
        marked = marked || std::find(g_marked.begin(), g_marked.end(), f) != g_marked.end();
    if (marked) {  // Y again: unmark
        for (const std::string &f : forms) {
            auto it = std::find(g_marked.begin(), g_marked.end(), f);
            if (it != g_marked.end()) g_marked.erase(it);
            g_known.remove(f);
        }
        std::string out = "# Kotoba3DS: words marked known with Y (one per line)\n";
        for (const std::string &w : g_marked) out += w + "\n";
        WriteWholeFile(kMarkedPath, out);
        v.refreshKnown();
        v.setStatus(word + " is no longer marked known.", kGreyInk);
        return;
    }
    for (const std::string &f : forms)
        if (g_known.has(f)) {
            v.setStatus(word + " is known already (saved, or an Anki card).", kGreyInk);
            return;
        }
    std::string add = File::Exists(kMarkedPath) == 1 ? "" : "# Kotoba3DS: words marked known with Y (one per line)\n";
    for (const std::string &f : forms) {
        add += f + "\n";
        g_marked.push_back(f);
        g_known.add(f);
    }
    File f;
    bool ok = File::Open(f, kMarkedPath, File::WRITE | File::CREATE | File::APPEND) == File::SUCCESS &&
              f.Write(add.data(), add.size()) == File::SUCCESS;
    f.Close();
    v.refreshKnown();
    v.setStatus(ok ? "Marked " + word + " known (Y again to undo)." : std::string("Could not write ") + kMarkedPath,
                ok ? kGood : kWarn);
}

// The screenshot of the current lookup: the screen(s) that were read (top 400x240, touchscreen
// 320x240, both 400x480 with the touchscreen centred under the top screen), as a QOI image (a
// third of a BMP). Saving a word only picks its name; the file is written from the kept pictures
// once the lookup closes, on another CPU core while the game plays (the SD card is slow to write
// to, and the lookup's dictionary reads would wait behind it). FinishScreenshot() waits for it.
static std::string *g_lookupShotPtr = nullptr;
#define g_lookupShot Leaky(g_lookupShotPtr)
static bool g_shotKept = false;  // Start in the lookup: keep its screenshot even with no word saved
static bool g_benchDiffer = false;  // benchmark build: the readers disagreed on this read

struct ShotJob {
    char path[96];
    bool top, bottom, ok, started;
    u32 ms, bytes;
    Picture pic[2];  // handed over by the lookup: freed when written
    File *file;
};
static ShotJob g_shotJob;
static Thread g_shotThread = nullptr;

static bool ShotSink(const uint8_t *data, size_t n, void *) {
    g_shotJob.bytes += n;
    return g_shotJob.file->Write(data, n) == File::SUCCESS;
}

static void WriteShot(void *) {
    ShotJob &j = g_shotJob;
    const u64 t0 = osGetTime();
    const bool top = j.top, bottom = j.bottom;
    const u32 W = top ? 400 : 320, H = top && bottom ? 480 : 240;
    const size_t kBuf = 16 * 1024;
    uint8_t *buf = (uint8_t *)malloc(kBuf);
    File f;
    j.ok = buf && File::Open(f, j.path, File::WRITE | File::CREATE | File::TRUNCATE) == File::SUCCESS;
    if (j.ok) {
        j.file = &f;
        kt::QoiEncoder q(W, H, ShotSink, nullptr, buf, kBuf);
        for (u32 y = 0; y < H; y++) {
            // rows from the top: the top screen's, then the touchscreen's (centred, black beside it)
            const bool fromBottom = bottom && (!top || y >= 240);
            const Picture &p = j.pic[fromBottom ? 1 : 0];
            const u32 k = 239 - y % 240, x0 = (fromBottom && top) ? 40 : 0;
            for (u32 x = 0; x < W; x++) {
                uint8_t rgb[3] = {0, 0, 0};
                if (x >= x0 && x - x0 < p.w) kt::picturePixelRGB(p.px + ((size_t)(x - x0) * 240 + k) * p.bytes, p.bytes, rgb);
                q.push(rgb[0], rgb[1], rgb[2]);
            }
        }
        j.ok = q.finish();
        f.Close();
    }
    free(buf);
    for (Picture &p : j.pic) {
        free(p.px);
        p.px = nullptr;
        p.valid = false;
    }
    j.ms = (u32)(osGetTime() - t0);
}

// Waits for the screenshot being written (if any).
static bool g_shotDeferred = false;  // a screenshot to write once the console is awake again
static void StartScreenshotThread();

static void FinishScreenshot() {
    if (g_shotDeferred) {  // still waiting for the console to wake: written now
        g_shotDeferred = false;
        StartScreenshotThread();
    }
    if (!g_shotThread) return;
    const u64 t0 = osGetTime();
    threadJoin(g_shotThread, U64_MAX);
    threadFree(g_shotThread);
    g_shotThread = nullptr;
    if (!g_shotJob.ok) File::Remove(g_shotJob.path);  // a half-written file is no use to anyone
    LogOcr(Utils::Format("screenshot %s: %lu KB in %lu ms (waited %lu ms for it)", g_shotJob.ok ? "written" : "FAILED",
                         (unsigned long)(g_shotJob.bytes / 1024), (unsigned long)g_shotJob.ms,
                         (unsigned long)(osGetTime() - t0)));
}

// The screenshot's file name for a word saved now: picked the first time, written at the close.
static std::string SaveScreenshot() {
    if (!g_lookupShot.empty()) return g_lookupShot;
    const bool top = g_readTop && Pic(0).valid, bottom = g_readBottom && Pic(1).valid;
    if (!top && !bottom) return "";
    std::string ts = Timestamp();  // "2026-10-24 14:30:05"
    std::string name;
    for (char c : ts) name += (c == ' ') ? '_' : (c == ':' ? '-' : c);
    g_lookupShot = name + ".qoi";
    return g_lookupShot;
}

// At the lookup's close: when a word was saved, its screenshot goes to the SD card in the
// background, the pictures with it; otherwise they are just freed.
static void WriteScreenshotLater() {
    const bool top = g_readTop && Pic(0).valid, bottom = g_readBottom && Pic(1).valid;
    if (g_lookupShot.empty() || (!top && !bottom)) {
        DropPictures();
        return;
    }
    FinishScreenshot();
    ShotJob &j = g_shotJob;
    j.started = false;
    snprintf(j.path, sizeof(j.path), "%s/%s", kShotDir, g_lookupShot.c_str());
    j.top = top;
    j.bottom = bottom;
    j.ok = false;
    j.ms = j.bytes = 0;
    for (int i = 0; i < 2; i++) {  // the pictures move to the job
        j.pic[i] = Pic(i);
        Pic(i).px = nullptr;
        Pic(i).valid = false;
    }
    // its own thread with room on its stack (CTRPF's tasks have 4 KB), below the game's priority so
    // the game goes on as usual; on the game's core: it mostly waits for the SD card
    if (SleepRequested()) {  // the lid is closing: not now (the SD card and the system are busy with that)
        g_shotDeferred = true;
        return;
    }
    StartScreenshotThread();
}

static void StartScreenshotThread() {
    g_shotJob.started = true;
    g_shotThread = threadCreate(WriteShot, nullptr, 32 * 1024, 0x3F, -2, false);
    if (!g_shotThread) WriteShot(nullptr);  // no thread: now, then
}

// Saves a word to mined.tsv. `status` gets the line to show.
static bool SaveWord(const kt::Entry &e, const kt::Result &r, const std::string &query, const std::string &sentence,
                     std::string &status, std::string &line) {
    const u64 t0 = osGetTime();
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
    line = kt::mineLogLine(Timestamp(), TitleIdHex(), e, r, shot, query, sentence, g_kanjiForm);
    text += line + "\n";
    bool ok = f.Write(text.data(), text.size()) == File::SUCCESS;
    f.Close();
    if (!ok) {
        status = "Could not save (is the SD card full?)";
        return false;
    }
    int n = CountSaved();
    status = "Saved " + (g_kanjiForm ? kt::cardExpressionFor(e, r.matchedKey) : kt::writtenExpressionFor(e, r)) + Utils::Format(" (%d word%s saved)", n, n == 1 ? "" : "s");
    LogOcr(Utils::Format("saved a word in %lu ms", (unsigned long)(osGetTime() - t0)));
    return true;
}

// ---------------------------------------------------------------------------
// The lookup screens (core/lookup_view): the text that was read, and the selected word's entry
// ---------------------------------------------------------------------------
static kt::LookupView *g_viewPtr = nullptr;

static kt::LookupView &View() {
    if (!g_viewPtr) g_viewPtr = new kt::LookupView(g_dict, Text());
    return *g_viewPtr;
}

// Frees the lookup screens and their caches of matches when they close: left in place, their
// many small blocks would split the free memory the next reads need whole. The scaled glyphs stay
// (two blocks, allocated once): the next lookup draws its text without making them again.
static void DropView() {
    delete g_viewPtr;
    g_viewPtr = nullptr;
}

// X / Hide: what the screen(s) that were read show. 0 the lookup, 1 see-through, 2 the game.
static int g_hide = 0;
static int NextHide(int h) {
    if (g_hideMode == kHideSeeThrough) return h ? 0 : 1;
    if (g_hideMode == kHideHidden) return h ? 0 : 2;
    return (h + 1) % 3;
}

static kt::LookupView::Panel PanelFor(int screen) {
    const bool read = screen == 0 ? g_readTop : g_readBottom;
    if (!read || g_hide == 0) return kt::LookupView::Panel::Shown;
    return g_hide == 1 ? kt::LookupView::Panel::SeeThrough : kt::LookupView::Panel::Hidden;
}

// Draws both screens once and shows them. Returns the milliseconds it took.
static u32 DrawLookup(kt::LookupView &v) {
    using Panel = kt::LookupView::Panel;
    u64 t0 = osGetTime();
    v.setHidden(g_hide != 0);
    for (int s = 0; s < 2; s++) {
        const Panel p = PanelFor(s);
        const bool entryHere = (s == 0) != v.textOnTop();
        ScreenCanvas c(ScreenOf(s));
        bool bandDone = false;
        if (p == Panel::Hidden) {
            PaintScreen(s, 0);
        } else if (p == Panel::SeeThrough) {
            if (!PaintScreen(s, 2)) c.fill(0, 0, ScreenW(s), 240, kPanel);
        } else if (entryHere) {  // the game dimmed under the entry, the band behind it darker still
            bandDone = PaintScreen(s, 1, v.entryBand(), kt::LookupView::kBandShade);
            if (!bandDone) c.fill(0, 0, ScreenW(s), 240, kPanel);
        }
        if (s == 0) v.drawTop(c, p, bandDone);
        else v.drawBottom(c, p, bandDone);
    }
    Present();
    return (u32)(osGetTime() - t0);
}

static void SaveSelected(kt::LookupView &v) {
    const kt::Result *r = v.result();
    if (!r) return;
    kt::Entry e;
    std::string status;
    if (!g_dict.entry(r->entryId, e)) {
        v.setStatus("Could not read this entry.", kWarn);
        return;
    }
    std::string line;
    bool ok = SaveWord(e, *r, r->surface.empty() ? v.query() : r->surface, v.sentence(), status, line);
    if (ok) v.markSaved(line);
    v.setStatus(status, ok ? kGood : kWarn);
}

// A (or Undo) on a word saved in this lookup: the save is taken back (saved by mistake).
static void UndoSave(kt::LookupView &v, const std::string line) {
    std::vector<std::string> lines = SavedLines();
    bool found = false;
    for (size_t i = lines.size(); i-- > 0;)
        if (lines[i] == line) {
            lines.erase(lines.begin() + i);
            found = true;
            break;
        }
    std::string out;
    for (const std::string &l : lines) out += l + "\n";
    if (found && !WriteWholeFile(kLogPath, out)) {
        v.setStatus(std::string("Could not write ") + kLogPath, kWarn);
        return;
    }
    // no longer known, unless saved before or marked with Y
    std::vector<std::string> cols, still;
    for (const std::string &l : lines) {
        if (l[0] == '#') continue;
        SplitTabs(l, cols);
        if (cols.size() > 2) still.push_back(cols[2]);
    }
    std::vector<std::string> forms = v.selectedForms();
    for (const std::string &f : forms)
        if (std::find(still.begin(), still.end(), f) == still.end() &&
            std::find(g_marked.begin(), g_marked.end(), f) == g_marked.end())
            g_known.remove(f);
    v.unmarkSaved();
    v.refreshKnown();
    if (!v.anySaved() && !g_shotKept) g_lookupShot = "";  // nothing saved or kept: no screenshot
    int n = 0;
    for (const std::string &l : lines) n += l[0] != '#';
    v.setStatus("Took back " + (forms.empty() ? std::string("the word") : forms[0]) +
                    Utils::Format(" (%d word%s saved)", n, n == 1 ? "" : "s"),
                kGreyInk);
}

// A / Save: saves the selected word, or takes back its save from this lookup.
static void SaveOrUndo(kt::LookupView &v) {
    if (const std::string *tag = v.savedTag()) UndoSave(v, *tag);
    else SaveSelected(v);
}

// Start: keeps this lookup's screenshot without saving a word (to look at the screen later), or
// takes that back. A word saved from the lookup keeps the screenshot either way.
static void KeepScreenshot(kt::LookupView &v) {
    if (g_shotKept) {
        g_shotKept = false;
        if (v.anySaved()) {
            v.setStatus("The screenshot stays: a word was saved with it", kGreyInk);
        } else {
            g_lookupShot = "";
            v.setStatus("Screenshot won't be saved", kGreyInk);
        }
        return;
    }
    const std::string name = SaveScreenshot();
    if (name.empty()) {
        v.setStatus("No picture of the screen to save", kWarn);
        return;
    }
    g_shotKept = true;
    LogOcr("screenshot kept without a word: " + name);
    v.setStatus("Screenshot will be saved: shots/" + name + " (Start: undo)", kGood);
}

// Runs the lookup screens until B / Close. Tap a character (text on the touchscreen); Circle Pad:
// words and rows; D-pad up/down (or C-stick): scroll the entry; D-pad left/right or L/R: the other
// matches; A save (again on a word just saved: undo), X hide, Y mark known, Start keep the
// screenshot without saving a word.
static u64 g_viewOpenedAt = 0;  // when the read finished, for the log
static void RunLookupView(kt::LookupView &v) {
    g_hide = 0;
    OSD::SwapBuffers();  // let CTRPF finish any pending redraw of its own first
    DrawLookup(v);
    u64 openMs = g_viewOpenedAt ? osGetTime() - g_viewOpenedAt : 0;
    g_viewOpenedAt = 0;
    u32 moves = 0, draws = 0;
    u64 drawSum = 0;
    bool touchWas = true;  // a touch held from before doesn't count
    u32 repeatKey = 0;
    u64 repeatAt = 0;
    const u32 kScrollUp = Key::DPadUp | Key::CStickUp, kScrollDown = Key::DPadDown | Key::CStickDown,
              kScroll = kScrollUp | kScrollDown,
              kMoves = Key::CPadLeft | Key::CPadRight | Key::CPadUp | Key::CPadDown | kScroll;
    u64 lastDraw = 0;
    while (true) {
        Controller::Update();
        u32 down = Controller::GetKeysPressed(), held = Controller::GetKeysDown();
        bool changed = false, moved = false;
        u64 t0 = osGetTime();
        if ((down & Key::B) || SleepRequested()) break;
        if (down & Key::A) { SaveOrUndo(v); changed = true; }
        if (down & Key::X) { g_hide = NextHide(g_hide); changed = true; }
        if (down & Key::Y) { MarkKnown(v); changed = true; }
        if (down & Key::Start) { KeepScreenshot(v); changed = true; }
        if (down & (Key::L | Key::DPadLeft)) moved |= v.page(-1);
        if (down & (Key::R | Key::DPadRight)) moved |= v.page(1);
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
        if (k & Key::CPadRight) moved |= v.moveWord(1);
        else if (k & Key::CPadLeft) moved |= v.moveWord(-1);
        if (k & Key::CPadDown) moved |= v.moveRow(1);
        else if (k & Key::CPadUp) moved |= v.moveRow(-1);
        if (k & kScrollDown) changed |= v.scroll(1);
        else if (k & kScrollUp) changed |= v.scroll(-1);
        // touch: acts on the press
        bool touch = Touch::IsDown();
        if (touch && !touchWas) {
            if (PanelFor(1) == kt::LookupView::Panel::Hidden) {  // the touchscreen shows the game: a tap brings the lookup back
                g_hide = 0;
                changed = true;
            } else {
                UIntVector p = Touch::GetPosition();
                kt::LookupView::Button b = kt::LookupView::kNone;
                moved |= v.tap((int)p.x, (int)p.y, b);
                if (b == kt::LookupView::kClose) break;
                if (b == kt::LookupView::kSave) { SaveOrUndo(v); changed = true; }
                if (b == kt::LookupView::kHide) { g_hide = NextHide(g_hide); changed = true; }
            }
        }
        touchWas = touch;
        if (moved) moves++;
        if (moved || changed) {
            drawSum += DrawLookup(v);
            draws++;
            lastDraw = osGetTime();
            continue;  // straight back to the buttons
        }
        // idle: look up the words the next press could go to, one per pass, then which words of
        // the text the player has already (green: drawn when done, or every half second)
        const bool more = !held && v.prefetch();
        if (v.takeRedraw()) {
            if (more && osGetTime() - lastDraw < 500) v.setRedraw();
            else { DrawLookup(v); lastDraw = osGetTime(); }
        }
        if (more) continue;
        Sleep(Milliseconds(10));
    }
    RestoreScreens();
    static const char *kFormats[5] = {"RGBA8", "BGR8", "RGB565", "RGB5A1", "RGBA4"};
    const int f0 = (int)ScreenOf(0).Format, f1 = (int)ScreenOf(1).Format;
    LogOcr(Utils::Format("view: opened in %lu ms, %lu moves, draw avg %lu ms (screens %s / %s)", (unsigned long)openMs,
                         (unsigned long)moves, (unsigned long)(draws ? drawSum / draws : 0),
                         f0 >= 0 && f0 < 5 ? kFormats[f0] : "?", f1 >= 0 && f1 < 5 ? kFormats[f1] : "?"));
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
    ShowBusy("Reading the screen...", Utils::Format("line %u of %u", i + 1, n));
}

// OCR misreads the dictionary can fix: 造跡 -> 遺跡 (遺 was the recognizer's second guess).
static unsigned g_fixedChars = 0;
static void FixLine(std::string &text, const std::vector<kt::OcrAlt> &alts, void *) {
    g_fixedChars += kt::fixOcrLine(g_dict, text, alts);
}

// Runs job(arg) on n CPU cores at once (CTRPF tasks: the New 3DS's extra app core 2, and core 0,
// where the paused game leaves room) and waits for all of them.
struct CoreJob { void (*job)(void *); void *arg; };
static s32 CoreTask(void *a) {
    CoreJob *j = (CoreJob *)a;
    j->job(j->arg);
    return 0;
}
// A third worker can go on the New 3DS's core 3, when the kernel lets this process start a thread
// there (it may refuse: then 2 workers do all the lines, as they share them out as they go).
static int g_core3 = -1;  // -1 not tried yet, 0 refused, 1 works
static void RunOnCores(unsigned n, void (*job)(void *), void *arg) {
    static const s32 kAffinity[] = {Task::NewAppCore, Task::AppCore};
    CoreJob cj = {job, arg};
    Thread third = nullptr;
    if (n >= 3 && g_core3 != 0) {
        third = threadCreate(job, arg, 16 * 1024, 0x18, 3, false);
        g_core3 = third ? 1 : 0;
    }
    std::vector<Task> tasks;
    for (unsigned i = 0; i < n && i < 2; i++) tasks.emplace_back(CoreTask, &cj, kAffinity[i]);
    for (Task &t : tasks) t.Start();
    for (Task &t : tasks) t.Wait();
    if (third) {
        threadJoin(third, U64_MAX);
        threadFree(third);
    }
}
static void Nothing(void *) {}
static unsigned OcrWorkers() {  // as many as this console has: New 3DS 2 or 3, Old 1
    if (!System::IsNew3DS()) return 1;
    if (g_core3 < 0) {  // once: may this process run a thread on core 3?
        Thread t = threadCreate(Nothing, nullptr, 4096, 0x18, 3, false);
        g_core3 = t ? 1 : 0;
        if (t) {
            threadJoin(t, U64_MAX);
            threadFree(t);
        }
    }
    return g_core3 ? 3 : 2;
}

static bool LoadModel(const std::string &name, std::string &error) {
    g_localOcr.unload();
    SdReader mf;
    if (!mf.open(kModelDir + name)) { error = "could not open " + name; return false; }
    if (!g_localOcr.load(mf, error)) {
        LogOcr(Utils::Format("%s: load failed (%s), heap free %lu KB", name.c_str(), error.c_str(),
                             (unsigned long)(HeapFree() / 1024)));
        return false;
    }
    g_modelName = name;
    return true;
}

// The OCR's input for screen i, straight from its framebuffer (call before anything is drawn on
// it): the text boxes found by their colour, and the grey picture (malloc'd; nullptr on failure).
static uint8_t *ScreenGrey(int i, std::vector<kt::TextBox> &boxes) {
    const Screen &s = ScreenOf(i);
    const u32 W = ScreenW(i), bpp = s.BytesPerPixel;
    if (bpp < 2 || bpp > 4) return nullptr;
    uint8_t *rgb = (uint8_t *)malloc((size_t)W * 240 * 3);
    if (!rgb) return nullptr;
    u8 *base = Column(s, 0);
    if (W > 1 && Column(s, 1) - base == (int)(240 * bpp)) {  // columns back to back: in one go
        kt::framebufferColumnsToRGB(base, W, 240, (int)s.Format, bpp, rgb);
    } else {
        for (u32 x = 0; x < W; x++) {
            const u8 *col = Column(s, x);
            for (u32 k = 0; k < 240; k++) kt::framebufferToRGB(col + k * bpp, 1, (int)s.Format, bpp, &rgb[((239 - k) * W + x) * 3]);
        }
    }
    boxes = kt::findTextBoxes(rgb, W, 240);
    uint8_t *grey = kt::greyFromRGB(rgb, W, 240);
    free(rgb);
    return grey;
}

// Reads the screens of `mode` -> text blocks (the top screen's first; each screen's text boxes
// top to bottom, or, read whole, its longest text first). Call with the game paused and nothing
// drawn yet.
static bool ReadScreens(int mode, std::vector<std::string> &blocks, std::string &error) {
    u64 t0 = osGetTime();
    std::string name = FindModel();
    if (name.empty()) {
        error = "No OCR model found.\nUnzip Kotoba3DS-ocr-model.zip to the root of the SD card (it goes to /3ds/kotoba3ds/ocr/).";
        return false;
    }
    // What stays for the whole session goes first, while the plugin's memory is still in one
    // piece, side by side: the glyph cache, then the model with the workers' working memory
    // (loaded at the first read and kept: 1.2 MB from the SD card each time was 150-250 ms).
    // A read's pictures then come and go above them.
    Text().reserve();
    g_localOcr.setParallel(OcrWorkers(), RunOnCores);
    g_localOcr.setFixer(FixLine, nullptr);
    if ((!g_localOcr.loaded() || g_modelName != name) && !LoadModel(name, error)) return false;
    u64 t1 = osGetTime();
    const u32 largestBefore = LargestFreeKB();  // for the log: room for the read's buffers?
    const u64 tMem = osGetTime();
    // the screens' pictures for the OCR, before anything is drawn on them
    const bool screens[2] = {mode != kReadBottom, mode != kReadTop};
    uint8_t *grey[2] = {nullptr, nullptr};
    std::vector<kt::TextBox> boxes[2];
    for (int s = 0; s < 2; s++)
        if (screens[s]) grey[s] = ScreenGrey(s, boxes[s]);
    const u64 tBoxes = osGetTime();
    // then the game's screens as they are, for drawing under the lookup and the card's
    // screenshot (after the colour copies above are freed: the pictures take their place): the
    // top screen always (the entry goes over it), the touchscreen when it is read
    const u32 bytes = g_readBottom ? 2 : 3;
    TakePicture(0, bytes);
    if (g_readBottom) TakePicture(1, bytes);
    u64 t2 = osGetTime();
    ShowBusy("Reading the screen...");
    g_fixedChars = 0;
    bool any = false;
    std::string firstError;
    for (int s = 0; s < 2; s++) {
        if (!screens[s]) continue;
        if (!grey[s]) {
            if (firstError.empty()) firstError = "not enough memory for the picture";
            continue;
        }
        std::vector<std::string> paras;
        std::string err;
        kt::ScoredReadStats rst;
        rst.clock = OsTimeMs;
#if KT_READER_BENCH
        // alpha benchmark: the current reader and the whole screen on the same picture, for the log;
        // the readers take turns going first (the first pays for a cold cache)
        static unsigned benchRuns = 0;
        const bool currentFirst = (benchRuns++ & 1) != 0;
        std::vector<std::string> pa, pb;
        std::string ea, eb;
        u64 ta0 = 0, ta1 = 0;
        auto runCurrent = [&]() {
            ta0 = osGetTime();
            kt::localReadScreen(g_localOcr, grey[s], ScreenW(s), 240, boxes[s], pa, ea);
            ta1 = osGetTime();
        };
        if (currentFirst) runCurrent();
#endif
        const u64 tc0 = osGetTime();
        const bool ok = kt::readScreenScored(g_localOcr, &g_dict, grey[s], ScreenW(s), 240, boxes[s], paras, err,
                                             LocalProgress, &rst);
        const u64 tc1 = osGetTime();
        LogOcr(Utils::Format("reader: %s %lu ms: boxes %lu ms (%u lines, %u dropped)%s, dictionary %u checks %lu ms",
                             s == 0 ? "top" : "touchscreen", (unsigned long)(tc1 - tc0), (unsigned long)rst.boxMs,
                             rst.boxLines, rst.boxDropped,
                             rst.usedBands ? Utils::Format("; bands: find %lu ms, %u candidates, read %u in %lu ms, kept %u",
                                                           (unsigned long)rst.bandFindMs, rst.bandCandidates, rst.bandRead,
                                                           (unsigned long)rst.bandReadMs, rst.bandKept).c_str()
                                           : "; no bands",
                             rst.dictChecks, (unsigned long)rst.dictMs));
#if KT_READER_BENCH
        {
            if (!currentFirst) runCurrent();
            const u64 tb0 = osGetTime();
            kt::localReadScreen(g_localOcr, grey[s], ScreenW(s), 240, std::vector<kt::TextBox>(), pb, eb);
            const u64 tb1 = osGetTime();
            auto flat = [](const std::vector<std::string> &v) {
                std::string o;
                for (const std::string &x : v) { if (!o.empty()) o += " / "; for (char c : x) o += c == '\n' ? '|' : c; }
                return o;
            };
            LogOcr(Utils::Format("bench: %s option1 %lu ms, current %lu ms (+ box search %lu ms), whole %lu ms; same text: "
                                 "current %d, whole %d; %s first",
                                 s == 0 ? "top" : "touchscreen", (unsigned long)(tc1 - tc0), (unsigned long)(ta1 - ta0),
                                 (unsigned long)(tBoxes - tMem), (unsigned long)(tb1 - tb0), pa == paras, pb == paras,
                                 currentFirst ? "current" : "option1"));
            if (pa != paras) {
                LogOcr("bench: option1: " + flat(paras) + "  ||  current: " + flat(pa));
                g_benchDiffer = true;
            }
        }
#endif
        if (ok) {
            blocks.insert(blocks.end(), paras.begin(), paras.end());
            any = true;
        } else if (firstError.empty()) {
            firstError = err;
        }
        free(grey[s]);
        grey[s] = nullptr;
    }
    u64 t3 = osGetTime();
    if (!any) error = firstError.empty() ? "no text found" : firstError;
    std::string first = any ? blocks[0] : error;
    for (char &c : first) if (c == '\n') c = '/';
    // in pieces: Utils::Format stops at 255 characters
    LogOcr(Utils::Format("%s: %s, model %lu ms, box %lu ms (%u+%u found; memory check %lu, boxes %lu, pictures %lu), ",
                         name.c_str(), kModeNames[mode], (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
                         (unsigned)boxes[0].size(), (unsigned)boxes[1].size(), (unsigned long)(tMem - t1),
                         (unsigned long)(tBoxes - tMem), (unsigned long)(t2 - tBoxes)) +
           Utils::Format("read %lu ms (%u threads, %d MHz, %u fixed), heap free %lu KB (largest %lu KB, before the "
                         "read %lu KB), %u block%s, ",
                         (unsigned long)(t3 - t2), g_localOcr.lastWorkers(), CpuMHz(), g_fixedChars,
                         (unsigned long)(HeapFree() / 1024), (unsigned long)LargestFreeKB(), (unsigned long)largestBefore,
                         (unsigned)blocks.size(), blocks.size() == 1 ? "" : "s") +
           first);
    return any;
}

static void OpenLookup(const std::vector<std::string> &blocks, bool textOnTop) {
    g_lastBlocks = blocks;
    g_lastTextOnTop = textOnTop;
    kt::LookupView &v = View();
    v.setTextOnTop(textOnTop);
    LoadKnown();
    v.setKnown(&g_known);
    v.setText(blocks);
    RunLookupView(v);
    DropView();
}

// Before the game goes on: wait until every button (and the touchscreen) is let go, and a few
// frames more. Otherwise the B (or A) that closed the plugin still reaches the game as a press
// and skips the line you were reading.
static void WaitForRelease() {
    const u64 until = osGetTime() + 3000;  // a button held on purpose doesn't keep the game paused
    while (osGetTime() < until && !SleepRequested()) {
        Controller::Update();
        if (!Controller::GetKeysDown(true) && !Touch::IsDown()) break;
        Sleep(Milliseconds(10));
    }
    if (SleepRequested()) return;
    Sleep(Milliseconds(80));
    Controller::Update();  // the game's next reading is "nothing pressed"
}

// The game goes on; if the lid was closed meanwhile, the system's sleep goes ahead now.
static void ResumeGame() {
    const bool lid = SleepRequested();
    Process::Play();
    int extra = 0;
    while (Process::IsPaused() && extra < 8) {  // paused twice somewhere: the game must go on all the same
        Process::Play();
        extra++;
    }
    if (lid)
        LogOcr(Utils::Format("power %llu: lid closed with the plugin open: the game goes on (paused %s, %d extra), "
                             "sleep flag %d", (unsigned long long)osGetTime(), Process::IsPaused() ? "STILL" : "no",
                             extra, SystemImpl::WantsToSleep() ? 1 : 0));
    if (SystemImpl::WantsToSleep()) SystemImpl::ReadyToSleep();
}

// A hotkey: pause the game, read its screen(s), look a word up.
static void ReadAndLookUp(int mode) {
    Process::Pause();
    BoostCpu();
    FinishScreenshot();  // the last lookup's pictures are still being written out
    LoadKnown();  // kept for the session: before the pictures, which come and go
    g_readTop = mode != kReadBottom;
    g_readBottom = mode != kReadTop;
    g_lookupShot = "";
    g_shotKept = false;
    g_benchDiffer = false;
    std::vector<std::string> blocks;
    std::string error;
    if (!OpenDict()) {
        Notice("Kotoba3DS", g_dictError);
    } else if (!ReadScreens(mode, blocks, error)) {
        Notice("Couldn't read the screen", error + "\n\n(The screen is saved as shots/failed_*.qoi on the SD card.)");
        std::string ts = Timestamp(), name = "failed_";
        for (char c : ts) name += (c == ' ') ? '_' : (c == ':' ? '-' : c);
        g_lookupShot = name + ".qoi";  // written like a lookup's screenshot, below
    } else {
        g_viewOpenedAt = osGetTime();
        OpenLookup(blocks, g_readBottom && g_textOnTop);
#if KT_READER_BENCH
        if (g_benchDiffer && g_lookupShot.empty()) {  // the readers disagreed: keep the screen to replay
            const std::string shot = SaveScreenshot();
            if (!shot.empty()) LogOcr("bench: readers disagreed, screenshot kept: " + shot);
        }
#endif
    }
    RestoreScreens();
    WriteScreenshotLater();  // the game goes on while it is written
    g_lookupShot = "";
    g_shotKept = false;
    RestoreCpu();
    WaitForRelease();
    ResumeGame();
}

// ---------------------------------------------------------------------------
// Anki cards: saved words to tools/ocr_server.py on the PC, all at once from the menu
// ---------------------------------------------------------------------------
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

// One saved word (a mined.tsv line) to the PC, which makes the card in the background.
// 1: sent, 2: it was a card already, 0: failed (error says why).
static int SendWordToPc(const std::string &host, int port, const std::string &line, std::string &error) {
    std::vector<std::string> cols;
    SplitTabs(line, cols);
    cols.resize(10);
    const std::string &title = cols[1], &shot = cols[5];
    std::vector<std::string> headers = {"X-Title-Id: " + title, "X-Shot-Name: " + shot};
    std::string reply;
    kt::MemReader body(line + "\n");
    if (!kt::netPost(host, port, "/mine", headers, body, 1500, 6000, reply, error)) return 0;
    if (reply.compare(0, 9, "NEED_SHOT") == 0 && !shot.empty()) {
        // the PC hasn't seen this screenshot: upload it, then ask again
        SdReader file;
        std::string compact, r2;
        if (!file.open(std::string(kShotDir) + "/" + shot)) {
            error = "screenshot " + shot + " is missing";
            return 0;
        }
        if (shot.size() > 4 && shot.compare(shot.size() - 4, 4, ".qoi") == 0) {  // small already: as it is
            if (!kt::netPost(host, port, "/shot", headers, file, 1500, 10000, r2, error)) return 0;
        } else {  // an older BMP: sent as RGB565, a third of its size
            if (!kt::screenshotRGB565(file, compact, error)) return 0;
            kt::MemReader body2(compact);
            if (!kt::netPost(host, port, "/shot", headers, body2, 1500, 10000, r2, error)) return 0;
        }
        kt::MemReader again(line + "\n");
        if (!kt::netPost(host, port, "/mine", headers, again, 1500, 6000, reply, error)) return 0;
    }
    if (reply.compare(0, 2, "OK") != 0) {
        size_t nl = reply.find('\n');
        error = nl == std::string::npos ? reply : reply.substr(nl + 1);
        while (!error.empty() && (error.back() == '\n' || error.back() == '\r')) error.pop_back();
        if (error.empty()) error = "the PC said no";
        return 0;
    }
    return reply.find("already") != std::string::npos ? 2 : 1;
}

static const char *kCardsHelp =
    "Sends your saved words to tools/ocr_server.py on your PC, which makes them into Anki cards "
    "(with the definition, audio and the screenshot). The PC must be on the same Wi-Fi, with Anki open.\n\n"
    "Words that are cards already are skipped. Without a PC nearby, keep saving: they wait here, "
    "or tools/mine.py can read them from the SD card later.";

static void MakeCards() {
    std::vector<std::string> lines = SavedLines();
    std::vector<int> words;
    for (int i = 0; i < (int)lines.size(); i++)
        if (lines[i][0] != '#') words.push_back(i);
    if (words.empty()) {
        Notice("Make Anki cards", "No saved words yet.\n\nIn a lookup, A saves the selected word.", kCardsHelp);
        return;
    }
    kt::MenuView m(Text());
    m.setTitle("Make Anki cards");
    m.setHelp(kCardsHelp);
    m.setMessage("Looking for your PC...");
    DrawMenu(m);
    std::string err, host;
    int port = kPcPort;
    if (!kt::netStart(err) || !ResolveServer(host, port, err)) {
        kt::netStop();
        Notice("Make Anki cards",
               "Couldn't reach your PC:\n" + err +
                   "\n\nStart ocr_server.py on the PC (same Wi-Fi), or set its address under \"PC\".",
               kCardsHelp);
        return;
    }
    int sent = 0, already = 0, failed = 0;
    std::string firstError;
    std::vector<int> done;  // lines that are cards now
    for (size_t k = 0; k < words.size() && !SleepRequested(); k++) {
        m.setMessage(Utils::Format("Sending word %u of %u to %s...", (unsigned)(k + 1), (unsigned)words.size(),
                                   g_server.c_str()));
        DrawMenu(m);
        std::string e;
        int r = SendWordToPc(host, port, lines[words[k]], e);
        if (r == 1) sent++;
        else if (r == 2) already++;
        else {
            failed++;
            if (firstError.empty()) firstError = e;
            if (failed >= 3 && sent + already == 0) break;  // the PC isn't taking them: stop trying
        }
        if (r) done.push_back(words[k]);
    }
    kt::netStop();
    std::string result = Utils::Format("%d new card%s", sent, sent == 1 ? "" : "s");
    if (already) result += Utils::Format(", %d already in Anki", already);
    if (failed) result += Utils::Format(", %d failed:\n%s", failed, firstError.c_str());
    if (done.empty()) {
        Notice("Make Anki cards", result, kCardsHelp);
        return;
    }
    int c = Ask("Make Anki cards",
                result + "\n\nRemove the " + Utils::Format("%u", (unsigned)done.size()) +
                    " word(s) that are cards now from Saved words? (Their screenshots stay on the SD card.)",
                {"Remove them", "Keep them"});
    if (c != 0) return;
    std::string out, cards;
    std::vector<std::string> cols;
    for (int i = 0; i < (int)lines.size(); i++) {
        if (std::find(done.begin(), done.end(), i) == done.end()) {
            out += lines[i] + "\n";
        } else {  // a card now: it stays green as one of the words with Anki cards
            SplitTabs(lines[i], cols);
            if (cols.size() > 2 && !cols[2].empty()) cards += cols[2] + "\n";
        }
    }
    WriteWholeFile(kLogPath, out);
    if (!cards.empty()) {
        File f;
        if (File::Open(f, kKnownPath, File::WRITE | File::CREATE | File::APPEND) == File::SUCCESS) {
            f.Write(cards.data(), cards.size());
            f.Close();
        }
    }
    g_knownLoaded = false;
}

// ---------------------------------------------------------------------------
// Known words: the list of words with Anki cards, from tools/ocr_server.py on the PC
// ---------------------------------------------------------------------------
static const char *kKnownHelp =
    "Words you have already show in green in lookups: the words you saved, the words you marked "
    "known with Y in a lookup (grammar, words you know), and the words of your Anki cards.\n\n"
    "Get the words of your Anki cards from tools/ocr_server.py on your PC (same Wi-Fi, Anki open), "
    "or run  mine.py known  on the PC: it puts the list on the SD card (SD:/3ds/kotoba3ds/known.txt).";

static void KnownWordsMenu() {
    LoadKnown();
    while (true) {
        std::string now = g_knownFromAnki ? Utils::Format("%u words with Anki cards.", (unsigned)g_knownFromAnki)
                                          : std::string("No list of your Anki cards' words yet.");
        now += Utils::Format("\n%u spellings marked known with Y.", (unsigned)g_marked.size());
        int c = Ask("Known words", now, {"Get them from my PC", "Forget the list", "Forget the Y marks"});
        if (c < 0) return;
        if (c == 2) {
            if (Ask("Known words", "Forget every word you marked known with Y?", {"Forget them", "Keep them"}) == 0) {
                File::Remove(kMarkedPath);
                g_knownLoaded = false;
                LoadKnown();
            }
            continue;
        }
        if (c == 1) {
            if (Ask("Known words", "Forget the words of your Anki cards? (Saved words stay green.)",
                    {"Forget them", "Keep them"}) == 0) {
                File::Remove(kKnownPath);
                g_knownLoaded = false;
                LoadKnown();
            }
            continue;
        }
        kt::MenuView m(Text());
        m.setTitle("Known words");
        m.setHelp(kKnownHelp);
        m.setMessage("Looking for your PC...");
        DrawMenu(m);
        g_known.clear();  // room for the new list
        g_knownLoaded = false;
        std::string err, host, reply;
        int port = kPcPort;
        bool ok = kt::netStart(err) && ResolveServer(host, port, err);
        if (ok) {
            m.setMessage("Getting the words of your Anki cards from " + g_server + "...\n\n(A big collection takes a moment.)");
            DrawMenu(m);
            kt::MemReader none("");
            ok = kt::netPost(host, port, "/known", {}, none, 1500, 60000, reply, err, kKnownMax + 64);
        }
        kt::netStop();
        if (ok && reply.compare(0, 7, "KNOWN1\n") != 0) {
            ok = false;
            err = "the PC's ocr_server.py is too old: update it (it comes with this release)";
        }
        if (ok && !WriteWholeFile(kKnownPath, reply.substr(7))) {
            ok = false;
            err = "could not write " + std::string(kKnownPath);
        }
        reply.clear();
        reply.shrink_to_fit();
        LoadKnown();
        if (!ok) {
            Notice("Known words", "Couldn't get the list:\n" + err, kKnownHelp);
            return;
        }
        Notice("Known words",
               Utils::Format("%u words with Anki cards. They show in green in lookups.", (unsigned)g_knownFromAnki) +
                   (g_known.truncated() ? "\n\n(The list is too long for the 3DS: only its first part is used.)" : ""),
               kKnownHelp);
        return;
    }
}

// ---------------------------------------------------------------------------
// Saved words: the list, each word's entry above it, Y deletes
// ---------------------------------------------------------------------------
static void SavedWords() {
    kt::MenuView m(Text());
    m.setTitle("Saved words");
    m.setButtons({" Delete", " Back"});
    int armed = -1;  // the line a first Y asked about
    while (true) {
        std::vector<std::string> lines = SavedLines();
        std::vector<int> idx;  // word lines, newest first
        for (int i = (int)lines.size() - 1; i >= 0; i--)
            if (lines[i][0] != '#') idx.push_back(i);
        std::vector<kt::MenuItem> items;
        std::vector<std::string> cols;
        for (int i : idx) {
            SplitTabs(lines[i], cols);
            cols.resize(10);
            kt::MenuItem it;
            it.label = cols[2] + (cols[3].empty() || cols[3] == cols[2] ? "" : "【" + cols[3] + "】");
            it.value = cols[0].size() >= 10 ? cols[0].substr(5, 5) : cols[0];  // "10-24"
            std::string def = cols[4];
            for (size_t p; (p = def.find("<br>")) != std::string::npos;) def.replace(p, 4, "\n");
            for (size_t p; (p = def.find("<b>")) != std::string::npos;) def.erase(p, 3);
            for (size_t p; (p = def.find("</b>")) != std::string::npos;) def.erase(p, 4);
            it.help = it.label + "\n" + def + (cols[9].empty() ? "" : "\n\n「" + cols[9] + "」");
            items.push_back(it);
        }
        m.setItems(items);
        if (items.empty()) {
            m.setMessage("No saved words yet.\n\nIn a lookup, A saves the selected word.");
            m.setButtons({" Back"});
        }
        MenuEvent ev = MenuInput(m);
        const bool back = (ev.key & Key::B) || (ev.type == MenuEvent::kButton && (items.empty() || ev.index == 1));
        if (back) return;
        const bool del = (ev.key & Key::Y) || (ev.type == MenuEvent::kButton && ev.index == 0);
        if (!del || items.empty()) {
            armed = -1;
            m.setStatus("", kInk);
            continue;
        }
        const int line = idx[m.selected()];
        if (armed != line) {  // the first press asks
            armed = line;
            m.setStatus("Press  again to delete " + items[m.selected()].label, kWarn);
            continue;
        }
        lines.erase(lines.begin() + line);
        std::string out;
        for (const std::string &l : lines) out += l + "\n";
        WriteWholeFile(kLogPath, out);
        g_knownLoaded = false;  // it may not be green any more
        armed = -1;
        m.setStatus("Deleted.", kGreyInk);
    }
}

// ---------------------------------------------------------------------------
// Hotkeys: hold the buttons, let go
// ---------------------------------------------------------------------------
static void RecordHotkey(int mode) {
    kt::MenuView m(Text());
    m.setTitle(kModeNames[mode]);
    m.setHelp("Hold the button or buttons you want (one works too), then let go.\n\n"
              "Pick buttons your game doesn't need at that moment: the game sees them as well. "
              "Select opens this menu, so it can't be used.\n\n"
              "Tap \"Turn off\" for no hotkey, \"Cancel\" to keep it as it is.");
    m.setButtons({"Turn off", "Cancel"});
    const std::string now = "Now: " + kt::keyComboName(g_combo[mode]);
    m.setMessage("Hold the buttons...\n\n" + now);
    DrawMenu(m);
    u32 held = 0;
    bool released = false;  // wait for the A that chose this to be let go
    bool touchWas = true;
    while (true) {
        if (SleepRequested()) return;
        Controller::Update();
        const u32 down = Controller::GetKeysDown() & (kt::keys::Usable | Key::Select);
        bool touch = Touch::IsDown();
        if (touch && !touchWas) {
            UIntVector p = Touch::GetPosition();
            int button = -1;
            m.tap((int)p.x, (int)p.y, button);
            if (button == 1) return;
            if (button == 0) {
                g_combo[mode] = 0;
                SaveSettings();
                return;
            }
        }
        touchWas = touch;
        if (!released) {
            released = down == 0;
        } else if (down) {
            if ((held | down) != held) {
                held |= down;
                m.setMessage(kt::keyComboName(held) + "\n\n(let go to keep it)");
                DrawMenu(m);
            }
        } else if (held) {  // all let go: that's the hotkey
            if (held & Key::Select) {
                m.setMessage("Select opens this menu: pick other buttons.\n\n" + now);
                m.setStatus("", kInk);
                DrawMenu(m);
                held = 0;
                continue;
            }
            g_combo[mode] = held;
            for (int o = 0; o < kModes; o++)  // two modes can't share one hotkey
                if (o != mode && g_combo[o] == held) g_combo[o] = 0;
            SaveSettings();
            return;
        }
        Sleep(Milliseconds(16));
    }
}

// ---------------------------------------------------------------------------
// The PC's address
// ---------------------------------------------------------------------------
static void ChoosePc() {
    std::string help = "Your PC runs tools/ocr_server.py (same Wi-Fi). Its address is found automatically "
                       "the first time; set it here if that doesn't work (ocr_server.py shows it).";
    while (true) {
        int c = Ask("PC", help + "\n\nNow: " + (g_server.empty() ? std::string("found automatically") : g_server),
                    {"Find my PC now", "Type its address", "Forget the address"});
        if (c < 0) return;
        std::string err;
        if (c == 0) {
            kt::MenuView m(Text());
            m.setTitle("PC");
            m.setMessage("Looking for your PC...");
            DrawMenu(m);
            std::string host;
            int port = kPcPort;
            bool ok = kt::netStart(err) && kt::netDiscover(kPcPort, 2000, host, port, err);
            kt::netStop();
            if (ok) {
                g_server = host + Utils::Format(":%d", port);
                SaveSettings();
                Notice("PC", "Found your PC at " + g_server);
            } else {
                Notice("PC", "Couldn't find it:\n" + err + "\n\nCheck ocr_server.py is running, or type its address.");
            }
        } else if (c == 1) {
            Keyboard in("The address ocr_server.py shows, e.g. 192.168.1.20:7331");
            std::string addr = g_server;
            if (in.Open(addr, addr) >= 0) {
                std::string host;
                int port;
                if (!kt::parseHostPort(addr, host, port)) {
                    Notice("PC", "That doesn't look like an address (example: 192.168.1.20:7331).");
                } else {
                    g_server = host + Utils::Format(":%d", port);
                    SaveSettings();
                    bool ok = kt::netStart(err) && kt::netPing(host, port, 2000, err);
                    kt::netStop();
                    Notice("PC", ok ? "Connected to " + g_server : "Saved " + g_server + ", but it didn't answer:\n" + err);
                }
            }
        } else {
            g_server = "";
            SaveSettings();
        }
    }
}

// ---------------------------------------------------------------------------
// How to use
// ---------------------------------------------------------------------------
static void HowToUse() {
    struct Page { const char *title, *text; };
    static const Page kPages[] = {
        {"Reading a screen",
         "In a game, press a hotkey when there is text you want to read. The game pauses and the 3DS "
         "reads the screen itself: no PC or internet needed.\n\n"
         "Each of \"Read the top screen\", \"Read the touchscreen\" and \"Read both screens\" can have "
         "its own hotkey (Select, then the item). The top screen's is ZL + ZR at first."},
        {"Looking words up",
         "The text shows on the touchscreen (or on the top screen: \"Touchscreen text shows on\"), "
         "broken into lines as in the game. Tap a word, or move with the Circle Pad; its entry shows "
         "on the other screen.\n\n"
         "D-pad up / down (or C-stick): scroll a long entry. D-pad left / right (or L / R): other "
         "meanings of the same spot. B: back to the game."},
        {"Seeing the game",
         "X (or Hide) uncovers the screen that was read: see-through (the text over the game) or "
         "hidden (the game alone), as set under \"Hide (X)\". X again brings the lookup back; so "
         "does a tap on a hidden touchscreen."},
        {"Saving words, Anki cards",
         "A saves the selected word with its sentence and a screenshot (SD:/3ds/kotoba3ds/mined.tsv); "
         "saved by mistake? A again on it (Undo) takes it back. "
         "\"Saved words\" lists them; Y deletes one.\n\n"
         "\"Make Anki cards\" sends them to tools/ocr_server.py on your PC (same Wi-Fi, Anki open). "
         "tools/mine.py can also read them from the SD card."},
        {"Words you know",
         "Words you saved show in green, and so do the words of your Anki cards once \"Known words\" "
         "has their list (from your PC, or mine.py known).\n\n"
         "Y in a lookup marks the word known without saving it (grammar, words you know already); "
         "Y again takes it back."},
        {"Files",
         "The dictionary: SD:/3ds/kotoba3ds/dict.bin. The OCR model: SD:/3ds/kotoba3ds/ocr/ "
         "(from Kotoba3DS-ocr-model.zip). Both come with the Kotoba3DS release."},
    };
    kt::MenuView m(Text());
    m.setTitle("How to use");
    std::vector<kt::MenuItem> items;
    for (const Page &p : kPages) {
        kt::MenuItem it;
        it.label = p.title;
        it.help = std::string(p.text);
        items.push_back(it);
    }
    m.setItems(items);
    m.setButtons({" Back"});
    while (true) {
        MenuEvent ev = MenuInput(m);
        if ((ev.key & Key::B) || ev.type == MenuEvent::kButton) return;
    }
}

// ---------------------------------------------------------------------------
// The Select menu
// ---------------------------------------------------------------------------
static const char *const kHideNames[] = {"See-through", "Hidden", "See-through, then hidden"};

static void Settings() {
    kt::MenuView m(Text());
    m.setTitle("Kotoba3DS");
    m.setButtons({" Choose", " Close"});
    enum Row { kKeyTop, kKeyBottom, kKeyBoth, kTextWhere, kHide, kSpelling, kLast, kSaved, kCards, kKnown, kPc, kHelp, kRows };
    FinishScreenshot();  // "Make Anki cards" may send it
    while (true) {
        LoadKnown();
        const int saved = CountSaved();
        std::vector<kt::MenuItem> items(kRows);
        static const char *const kKeyHelp[kModes] = {
            "The button(s) that pause the game and read the top screen.",
            "The button(s) that pause the game and read the touchscreen.",
            "The button(s) that pause the game and read both screens: the top screen's text first.",
        };
        for (int mode = 0; mode < kModes; mode++) {
            items[mode].label = kModeNames[mode];
            items[mode].value = kt::keyComboName(g_combo[mode]);
            items[mode].help = std::string(kKeyHelp[mode]) + "\n\nA: set the buttons (hold them, then let go).";
        }
        items[kTextWhere].label = "Touchscreen text shows on";
        items[kTextWhere].value = g_textOnTop ? "the top screen" : "the touchscreen";
        items[kTextWhere].help =
            "When the touchscreen was read, its text can show where it was (tap the words) or on the "
            "top screen (move with the Circle Pad), so the touchscreen shows the entry.\n\nA,  or : change.";
        items[kHide].label = "Hide (X)";
        items[kHide].value = kHideNames[g_hideMode];
        items[kHide].help =
            "What X (or Hide) does to the screen that was read: see-through shows the text over the game, "
            "hidden shows the game alone. Both: the first press see-through, the second hidden.\n\n"
            "A,  or : change.";
        items[kSpelling].label = "Saved words are written";
        items[kSpelling].value = g_kanjiForm ? "in kanji" : "as in the game";
        items[kSpelling].help =
            "How a word you save is written (on its Anki card too). In kanji: its usual kanji form, "
            "even when the game wrote it in kana (ちょっと -> 一寸). As in the game: in kanji only when "
            "the game wrote it in kanji, else in kana (たべた -> たべる).\n\n"
            "A,  or : change.";
        items[kLast].label = "Last lookup";
        items[kLast].dim = g_lastBlocks.empty();
        items[kLast].help = "Opens the text of your last lookup again.";
        items[kSaved].label = "Saved words";
        items[kSaved].value = Utils::Format("%d", saved);
        items[kSaved].help = "The words you saved, newest first, with their entry. Y deletes one.";
        items[kCards].label = "Make Anki cards";
        items[kCards].dim = saved == 0;
        items[kCards].help = kCardsHelp;
        items[kKnown].label = "Known words";
        items[kKnown].value = Utils::Format("%u", (unsigned)g_knownFromAnki);
        items[kKnown].help = std::string(kKnownHelp) + "\n\nA: get or forget the list.";
        items[kPc].label = "PC";
        items[kPc].value = g_server.empty() ? "find automatically" : g_server;
        items[kPc].help = "The address of your PC running tools/ocr_server.py, for \"Make Anki cards\" and \"Known words\".";
        items[kHelp].label = "How to use";
        items[kHelp].help = "The buttons, the settings and the files, in short.";
        m.setItems(items);
        MenuEvent ev = MenuInput(m);
        if ((ev.key & Key::B) || (ev.type == MenuEvent::kButton && ev.index == 1)) return;
        int row = ev.type == MenuEvent::kChoose ? ev.index : (ev.type == MenuEvent::kButton ? m.selected() : -1);
        const int dir = (ev.key & (Key::DPadLeft | Key::CPadLeft)) ? -1 : (ev.key & (Key::DPadRight | Key::CPadRight)) ? 1 : 0;
        if (dir) row = m.selected();
        if (row < 0) continue;
        if (row <= kKeyBoth) {
            if (!dir) RecordHotkey(row);
        } else if (row == kTextWhere) {
            g_textOnTop = !g_textOnTop;
            SaveSettings();
        } else if (row == kHide) {
            g_hideMode = (g_hideMode + (dir < 0 ? 2 : 1)) % 3;
            SaveSettings();
        } else if (row == kSpelling) {
            g_kanjiForm = !g_kanjiForm;
            SaveSettings();
        } else if (dir) {
            continue;
        } else if (row == kLast) {
            if (g_lastBlocks.empty()) continue;
            if (!OpenDict()) {
                Notice("Kotoba3DS", g_dictError);
                continue;
            }
            g_readTop = g_readBottom = false;  // nothing of the game to show under it
            std::vector<std::string> blocks = g_lastBlocks;
            OpenLookup(blocks, g_lastTextOnTop);
        } else if (row == kSaved) {
            SavedWords();
        } else if (row == kCards) {
            MakeCards();
        } else if (row == kKnown) {
            KnownWordsMenu();
        } else if (row == kPc) {
            ChoosePc();
        } else if (row == kHelp) {
            HowToUse();
        }
    }
}

// ---------------------------------------------------------------------------
// Every frame while the game plays: the hotkeys, and Select (the menu)
// ---------------------------------------------------------------------------
static bool g_openMenu = false;
static kt::HotkeyDispatcher *g_hotkeysPtr = nullptr;
#define g_hotkeys Leaky(g_hotkeysPtr)

static bool OnMenuOpening() {  // CTRPF's menu key (Select) opens the plugin's own menu instead
    g_openMenu = true;
    return false;
}

static void Watcher(void) {
    if (SleepRequested()) return;  // the console is going to sleep: not now
    if (g_shotDeferred) {
        g_shotDeferred = false;
        StartScreenshotThread();
    }
    if (g_openMenu) {
        g_openMenu = false;
        Process::Pause();
        Settings();
        g_hotkeys.reset();
        WaitForRelease();
        ResumeGame();
        return;
    }
    for (int m = 0; m < kModes; m++) g_hotkeys.set(m, g_combo[m]);
    int mode = g_hotkeys.update(Controller::GetKeysDown(true), Controller::GetKeysPressed(), osGetTime());
    if (mode < 0) return;
    ReadAndLookUp(mode);
    g_hotkeys.reset();
}

// ---------------------------------------------------------------------------
// Start of the game
// ---------------------------------------------------------------------------
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
// game's 64 MB budget (its "commit" limit). A game that asks for fixed amounts (逆転裁判123: a
// 512 KB heap and 56.5 MB of device memory) goes over that limit; one that sizes its heap from
// what's left (大逆転裁判2: min(APPMEMALLOC, 64 MB) - memory in use - device memory) comes out
// 5 MiB short. Either way the game fails an allocation at start. The console has the memory (the
// app region is 124 MB), so before the game's first instruction:
//   1. the game's commit limit is raised by the plugin's share (fixed amounts now fit), and
//   2. every system call the game could use to ask how much memory it uses, or what its limit is,
//      is patched to leave the plugin's share out of the answer (mem_hooks.s, core/mem_share.cpp).
// The game then sees a normal 64 MB console whichever way its start-up code was written. Step 2
// only follows step 1, and either all the calls found are patched or none: the game must never be
// told it has memory it can't get. Only answers measured at boot to include the share are changed.
// ---------------------------------------------------------------------------
static s64 g_commitLimitBefore = -1;  // to put back when the game exits

static kt::memshare::Plan g_memPlan;
static kt::memshare::Counts g_memCounts;

extern "C" {
void KotobaHookGetSystemInfo(void);
void KotobaHookGetProcessInfo(void);
void KotobaHookLimitValues(void);
void KotobaHookCurrentValues(void);

// called by the stand-ins in mem_hooks.s, on the game's threads, right after the real call
void KotobaMemHookAdjust(u32 svc, u32 *regs) {
    if (svc == kt::memshare::kLimitValues || svc == kt::memshare::kCurrentValues)
        kt::memshare::adjustLimits(g_memPlan, g_memCounts, svc, (s32)regs[0], (s64 *)(uintptr_t)regs[4],
                                   (const u32 *)(uintptr_t)regs[6], (s32)regs[7]);
    else
        kt::memshare::adjustRegisters(g_memPlan, g_memCounts, svc, regs);
}
}

// svcGetProcessInfo's type is a u32 in some libctru versions and an enum in others
template <typename T>
static Result AskProcessInfo(Result (*fn)(s64 *, Handle, T), s64 *out, u32 type) {
    return fn(out, CUR_PROCESS_HANDLE, (T)type);
}

// Step 2: patch the memory questions. Returns what happened, for the log.
static std::string HookMemoryQuestions(u32 plugin, s64 committed) {
    using namespace kt::memshare;
    // which answers include the plugin's share, asked now with the plugin's own (unpatched) calls
    Plan plan;
    plan.plugin = plugin;
    plan.limits = true;  // committed includes it (checked by the caller) and the limit was raised by it
    for (int type = 0; type < kProcessInfoTypes; type++) {
        s64 v = -1;
        if (R_SUCCEEDED(AskProcessInfo(svcGetProcessInfo, &v, (u32)type)))
            plan.processInfo[type] = countsPlugin(v, committed, plugin);
    }
    s64 appUsed = -1;
    if (R_SUCCEEDED(svcGetSystemInfo(&appUsed, 0, 1))) plan.appUsed = countsPlugin(appUsed, committed, plugin);

    // find the calls in the game's code
    struct Kind { u32 svc; void (*standIn)(void); const char *name; };
    static const Kind kinds[] = {
        {kGetSystemInfo, KotobaHookGetSystemInfo, "system info"},
        {kGetProcessInfo, KotobaHookGetProcessInfo, "process info"},
        {kLimitValues, KotobaHookLimitValues, "limit"},
        {kCurrentValues, KotobaHookCurrentValues, "in use"},
    };
    constexpr int kMaxSites = 16;
    u32 siteAt[kMaxSites];
    void (*siteFn[kMaxSites])(void);
    int sites = 0, perKind[4] = {0, 0, 0, 0};
    const u32 *text = (const u32 *)0x00100000;
    const u32 words = Process::GetTextSize() / 4;
    for (u32 i = 0; i < words; i++) {  // one pass for all four (svc #n: 0xEF0000nn)
        const u32 word = text[i];
        if ((word & 0xFFFFFFC0) != 0xEF000000) continue;  // svc 0x00..0x3F
        int k = 0;
        while (k < 4 && kinds[k].svc != (word & 0xFF)) k++;
        if (k == 4) continue;
        // a call left as it is would give the game a second, different answer
        if (!isPatchableCall(text, words, i, kinds[k].svc))
            return Utils::Format("not patched: a %s call can't be patched", kinds[k].name);
        if (sites == kMaxSites) return "not patched: more calls than expected";
        siteAt[sites] = 0x00100000 + i * 4;
        siteFn[sites] = kinds[k].standIn;
        sites++;
        perKind[k]++;
    }
    std::string found;
    for (int k = 0; k < 4; k++) found += Utils::Format("%s%s %d", k ? ", " : "", kinds[k].name, perKind[k]);
    if (sites == 0) return "the game asks none of these";

    // the trampolines, in plugin memory the game can run (never freed: the game uses them until it exits)
    const u32 bytes = (sites * kTrampolineWords * 4 + 0xFFF) & ~0xFFFu;
    u32 *tramp = (u32 *)memalign(0x1000, bytes);
    if (!tramp) return "not patched: no memory for the trampolines";
    if (!Process::ProtectMemory((u32)tramp, bytes)) return "not patched: trampolines can't be made runnable";
    for (int s = 0; s < sites; s++) {
        u32 *t = tramp + s * kTrampolineWords;
        buildTrampoline(t, (u32)t, (u32)siteFn[s], *(const u32 *)(siteAt[s] + 4), siteAt[s] + 8);
    }
    svcFlushProcessDataCache(Process::GetHandle(), (u32)tramp, bytes);

    // all or nothing: patch every site; if one fails, put back the ones done
    g_memPlan = plan;
    u32 original[kMaxSites][2];
    for (int s = 0; s < sites; s++) {
        u32 patch[2];
        buildSitePatch(patch, (u32)(tramp + s * kTrampolineWords));
        original[s][0] = *(const u32 *)siteAt[s];
        original[s][1] = *(const u32 *)(siteAt[s] + 4);
        if (!Process::Patch(siteAt[s], patch, 8)) {
            for (int t = 0; t < s; t++) Process::Patch(siteAt[t], original[t], 8);
            svcInvalidateEntireInstructionCache();
            g_memPlan = Plan();
            return Utils::Format("not patched: could not write the call at %08lX", (unsigned long)siteAt[s]);
        }
    }
    svcInvalidateEntireInstructionCache();
    return "patched " + found;
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
    // only for a game told it has at most 64 MB (the SDK's cap), whose commit already holds the
    // plugin's block, with room left in the app region
    const s64 want = (s64)appMemAlloc + plugin;
    if (appMemAlloc == 0 || appMemAlloc > 0x4000000) why = "APPMEMALLOC above 64 MB";
    else if (limit < (s64)appMemAlloc) why = "unusual commit limit";
    else if (committed < (s64)(codeSize + plugin)) why = "the plugin isn't counted in the game's memory";
    else if (want > appRegion) why = "no room in the app region";
    // 1. The commit limit: at least APPMEMALLOC + the plugin's share. Taking the share off both
    //    the limit and the memory in use (step 2) leaves the game's free memory exactly as it is.
    std::string raised;
    if (why.empty() && limit < want) {
        // the high word is what the kernel writes to APPMEMALLOC: keep it as it is
        s64 value = want | ((s64)appMemAlloc << 32);
        Result r = svcSetResourceLimitValues(rl, &t, &value, 1);
        if (R_FAILED(r)) why = Utils::Format("could not raise the limit (%08lX)", (unsigned long)r);
        else {
            g_commitLimitBefore = limit;
            raised = Utils::Format("commit limit %ld -> %ld KB", (long)(limit / 1024), (long)(want / 1024));
        }
    }
    svcCloseHandle(rl);
    if (!why.empty()) {
        LogOcr("boot: memory left as it is: " + why);
        return;
    }
    LogOcr(Utils::Format("boot: gave the game back %lu KB (%s)", (unsigned long)(plugin / 1024),
                         raised.empty() ? "commit limit already high enough" : raised.c_str()));
    // 2. The questions about memory: only once the limit really is at least APPMEMALLOC + the share
    LogOcr("boot: memory answers: " + HookMemoryQuestions(plugin, committed));
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
    PluginMenu *menu = new PluginMenu("Kotoba3DS", 3, 0, 0,
                                      "In-game Japanese dictionary that reads the screen and saves words for Anki.");
    menu->SynchronizeWithFrame(true);
    menu->OnOpening = OnMenuOpening;  // Select: the plugin's own menu, not CTRPF's list

    Directory::Create(kRootDir);
    Directory::Create(kShotDir);
    LoadSettings();
    LogOcr(Utils::Format("start: game %s, plugin heap %lu KB, heap free %lu KB", TitleIdHex().c_str(),
                         (unsigned long)(__ctru_heap_size / 1024), (unsigned long)(HeapFree() / 1024)));
    {   // CTRPF's own screenshot hotkey stays off: the plugin makes its screenshots itself
        bool *enabled = nullptr;
        u32 *hotkey = nullptr;
        PluginMenu::ScreenshotSettings(&enabled, &hotkey);
        if (enabled) *enabled = false;
    }
    *menu += Watcher;  // runs every frame while the game plays
    menu->Run();
    // Don't `delete menu` (or free anything else) here: the game is exiting and the
    // plugin heap may already be unusable. See the note at Leaky().
    return 0;
}

}  // namespace CTRPluginFramework
