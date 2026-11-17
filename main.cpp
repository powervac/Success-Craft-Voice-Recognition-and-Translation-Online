// VoiceWindow: live speech recognition + text translation with a two-sided conversation mode.
//
//   channel 0 = microphone (you), channel 1 = system audio (the other side, WASAPI loopback)
//   Every channel has its own gain / noise suppression / speech detector; audio goes to worker.py,
//   which returns recognized text, translation and (optionally) the speaker number inside the channel.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <richedit.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Paths are resolved at start-up (see StartWorker): settings.ini [paths] python / root,
// otherwise <exe dir>\venv\Scripts\python.exe and <exe dir>\data.

enum { ID_MIC = 101, ID_LANG, ID_START, ID_CLEAR, ID_COPY, ID_LEVEL, ID_STATUS, ID_EDIT,
       ID_TGT, ID_SWAP, ID_COPYTR, ID_EDITTR, ID_SAVE,
       ID_GAIN, ID_NSSTR, ID_NS, ID_AGC, ID_BOOST,
       ID_SYS, ID_REV, ID_ECHO, ID_ONE, ID_MANY, ID_SENS, ID_LEVEL2, ID_SYSDEV };
enum { WM_WORKER = WM_APP + 1, WM_LEVEL = WM_APP + 2, WM_SYSERR = WM_APP + 3 };

// language table: code for the worker, label shown in the combos
struct Lang { const wchar_t* code; const wchar_t* name; };
static const Lang LANGS[] = {{L"ru", L"Russian"}, {L"en", L"English"}, {L"de", L"German"}, {L"fr", L"French"},
                             {L"es", L"Spanish"}, {L"uk", L"Ukrainian"}, {L"pl", L"Polish"}, {L"it", L"Italian"}};
static const int NLANGS = sizeof(LANGS) / sizeof(LANGS[0]);

// ---- settings shared with the audio threads ----
static std::atomic<int> g_gainDb{0}, g_ns{1}, g_nsStr{50}, g_agc{1}, g_boost{0};
static std::atomic<int> g_sysOn{0}, g_rev{1}, g_echo{0}, g_diar{0}, g_sens{60};
static std::atomic<long long> g_sysLastSpeechMs{0};   // for the speaker-echo filter

// ---- window handles and state ----
static HWND hMain, hMicLbl, hMic, hLang, hStart, hClear, hCopy, hLevel, hLevel2, hStatus, hEdit;
static HWND hGainLbl, hGain, hGainVal, hAgc, hNs, hNsStr, hNsVal, hBoost;
static HWND hSysOn, hRev, hEcho, hOne, hMany, hSens, hSensVal;
static HWND hSysLbl, hSysDev;
static std::vector<std::wstring> g_sysIds;   // endpoint ids of the output devices listed in hSysDev (item 0 = default)
static std::wstring g_sysIdSel;              // endpoint id used by the loopback thread ("" = default output)
static HWND hSave, hLangLbl, hTgtLbl, hTgt, hSwap, hCopyTr, hEditTr, hCapSrc, hCapTr;
static HFONT hFont, hBigFont, hBoldFont;
static bool g_trAvailable = true, g_spkAvailable = true;
static std::wstring g_ini;
static int g_dpi = 96;
static HANDLE hChildIn = nullptr, hJob = nullptr;
static PROCESS_INFORMATION g_pi{};
static std::mutex g_sendLock;
static std::thread g_reader, g_capture, g_loop;
static std::atomic<bool> g_capturing{false}, g_sysRun{false};
static bool g_ready = false;

static HWAVEIN g_wi = nullptr;
static HANDLE g_waveEv = nullptr;
static const int NB = 8, CHUNK = 1600;  // 8 buffers x 100 ms @16 kHz
static WAVEHDR g_hdr[NB];
static short g_bufs[NB][CHUNK];

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static long long NowMs() { return (long long)GetTickCount64(); }

static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static void SetStatus(const std::wstring& t) { SetWindowTextW(hStatus, t.c_str()); }

// ---- pipe to worker ----
static void SendFrame(char type, const void* data, uint32_t len) {
    if (!hChildIn) return;
    std::vector<char> f(5 + len);
    f[0] = type;
    memcpy(&f[1], &len, 4);
    if (len) memcpy(&f[5], data, len);
    std::lock_guard<std::mutex> g(g_sendLock);
    DWORD w;
    WriteFile(hChildIn, f.data(), (DWORD)f.size(), &w, nullptr);
}
static void SendAudio(int ch, const short* pcm, int n) {   // payload: channel byte + int16 samples
    std::vector<char> p(1 + n * 2);
    p[0] = (char)ch;
    memcpy(&p[1], pcm, n * 2);
    SendFrame('A', p.data(), (uint32_t)p.size());
}
static void SendEnd(int ch) { char c = (char)ch; SendFrame('E', &c, 1); }

// kind: S N V X (status) or P F T U (channel, speaker, text)
struct WorkerMsg { char kind; int ch = 0, spk = 0; std::string lang; std::wstring text; };

static void ReaderLoop(HANDLE hOut) {
    std::string acc;
    char tmp[4096];
    DWORD n;
    while (ReadFile(hOut, tmp, sizeof tmp, &n, nullptr) && n) {
        acc.append(tmp, n);
        size_t p;
        while ((p = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, p);
            acc.erase(0, p + 1);
            if (line.size() < 2) continue;
            auto* m = new WorkerMsg();
            m->kind = line[0];
            std::string rest = line.size() > 2 ? line.substr(2) : "";
            if (strchr("PFTU", m->kind)) {            // channel \t speaker \t language \t text
                size_t a = rest.find('\t');
                size_t b = a == std::string::npos ? a : rest.find('\t', a + 1);
                size_t c = b == std::string::npos ? b : rest.find('\t', b + 1);
                if (c != std::string::npos) {
                    m->ch = atoi(rest.substr(0, a).c_str());
                    m->spk = atoi(rest.substr(a + 1, b - a - 1).c_str());
                    m->lang = rest.substr(b + 1, c - b - 1);
                    rest = rest.substr(c + 1);
                }
            }
            m->text = Utf8ToW(rest);
            PostMessageW(hMain, WM_WORKER, 0, (LPARAM)m);
        }
    }
    auto* x = new WorkerMsg();
    x->kind = 'X';
    PostMessageW(hMain, WM_WORKER, 0, (LPARAM)x);
}

static bool StartWorker() {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inR, inW, outR, outW;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) return false;
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    HANDLE err = CreateFileW((dir + L"\\worker.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = err;
    auto iniStr = [](const wchar_t* key) {
        wchar_t b[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"paths", key, L"", b, MAX_PATH, g_ini.c_str());
        return std::wstring(b);
    };
    std::wstring py = iniStr(L"python");
    if (py.empty()) {
        std::wstring venvPy = dir + L"\\venv\\Scripts\\python.exe";
        py = GetFileAttributesW(venvPy.c_str()) != INVALID_FILE_ATTRIBUTES ? venvPy : L"python";
    }
    std::wstring root = iniStr(L"root");
    if (root.empty()) root = dir + L"\\data";
    CreateDirectoryW(root.c_str(), nullptr);
    SetEnvironmentVariableW(L"DICTATION_ROOT", root.c_str());   // models + temp files live here
    std::wstring cmd = L"\"" + py + L"\" -u \"" + dir + L"\\worker.py\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    // kill the whole python tree when the app exits
    hJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jl, sizeof jl);

    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, root.c_str(), &si, &g_pi);
    CloseHandle(inR);
    CloseHandle(outW);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok) { CloseHandle(inW); CloseHandle(outR); return false; }
    AssignProcessToJobObject(hJob, g_pi.hProcess);
    ResumeThread(g_pi.hThread);
    hChildIn = inW;
    g_reader = std::thread(ReaderLoop, outR);
    return true;
}

// =====================================================================================
//  Conversation model + colored rich-text panes
// =====================================================================================
struct Entry { int ch = 0, spk = 0; bool labeled = false; std::wstring text; };
struct Utt {                       // one finished utterance (for saving both language versions)
    int ch, spk; bool labeled;
    std::wstring orig, tr;
    std::string oLang, tLang;      // language codes of the original / translation ("auto" = unknown)
};
struct Pane {
    HWND h = nullptr;
    std::vector<Entry> done;       // committed lines
    Entry live[2];                 // current partial line of each channel
    bool has[2] = {false, false};
    LONG doneLen = 0;              // characters of the control occupied by committed lines
};
static Pane g_src, g_tr;
static std::vector<Utt> g_utts;
static std::deque<int> g_await[2]; // per channel: utterance index (or -1) waiting for its translation

static bool Labeled() { return g_sysOn || g_diar; }   // "several" mode or two-sided mode shows speaker labels

static std::wstring LabelOf(int ch, int spk) {
    std::wstring b = ch == 0 ? L"You" : L"Them";
    return spk >= 2 ? b + L" " + std::to_wstring(spk) : b;
}
static COLORREF ColorOf(int ch, int spk) {
    static const COLORREF P0[6] = {RGB(0, 84, 166), RGB(123, 63, 180), RGB(0, 130, 140), RGB(70, 70, 170), RGB(160, 50, 120), RGB(70, 110, 140)};
    static const COLORREF P1[6] = {RGB(20, 130, 50), RGB(205, 100, 10), RGB(190, 40, 50), RGB(120, 120, 0), RGB(130, 80, 40), RGB(190, 40, 160)};
    int i = (std::max(spk, 1) - 1) % 6;
    return ch == 0 ? P0[i] : P1[i];
}
static COLORREF Fade(COLORREF c) {   // partial (not yet final) text is drawn lighter
    auto mix = [](int a) { return (int)(a * 0.62 + 255 * 0.38); };
    return RGB(mix(GetRValue(c)), mix(GetGValue(c)), mix(GetBValue(c)));
}

static LONG TextLen(HWND h) {
    GETTEXTLENGTHEX g{GTL_DEFAULT, 1200};
    return (LONG)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&g, 0);
}
static void AppendRun(HWND h, const std::wstring& t, COLORREF c, bool bold) {
    if (t.empty()) return;
    LONG s = TextLen(h);
    SendMessageW(h, EM_SETSEL, s, s);
    SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
    LONG e = TextLen(h);
    SendMessageW(h, EM_SETSEL, s, e);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_BOLD;
    cf.crTextColor = c;
    cf.dwEffects = bold ? CFE_BOLD : 0;
    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(h, EM_SETSEL, e, e);
}
static void AppendEntry(HWND h, const Entry& e, bool live) {
    COLORREF c = e.labeled ? ColorOf(e.ch, e.spk) : RGB(30, 30, 30);
    if (live) c = Fade(c);
    if (e.labeled) AppendRun(h, LabelOf(e.ch, e.spk) + L": ", c, true);
    AppendRun(h, e.text, c, false);
    AppendRun(h, L"\r", c, false);
}
static bool AtBottom(HWND h) {
    SCROLLINFO si{sizeof si, SIF_ALL};
    if (!GetScrollInfo(h, SB_VERT, &si)) return true;
    return si.nMax <= 0 || si.nPos + (int)si.nPage >= si.nMax - 2;
}
// Redraws the volatile tail (partial lines), optionally committing one finished entry first.
static void PaneRender(Pane& p, const Entry* commit) {
    HWND h = p.h;
    bool bottom = AtBottom(h);
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SendMessageW(h, EM_SETSEL, p.doneLen, -1);
    SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)L"");
    if (commit) { AppendEntry(h, *commit, false); p.doneLen = TextLen(h); }
    for (int c = 0; c < 2; c++)
        if (p.has[c] && !p.live[c].text.empty()) AppendEntry(h, p.live[c], true);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    if (bottom) SendMessageW(h, WM_VSCROLL, SB_BOTTOM, 0);
    InvalidateRect(h, nullptr, TRUE);
}
static void PaneSetLive(Pane& p, int ch, const Entry& e) { p.live[ch] = e; p.has[ch] = !e.text.empty(); PaneRender(p, nullptr); }
static void PaneCommit(Pane& p, const Entry& e) { p.done.push_back(e); p.has[e.ch] = false; PaneRender(p, &e); }
static void PaneClearLive(Pane& p, int ch) { p.has[ch] = false; PaneRender(p, nullptr); }
static void PaneClear(Pane& p) {
    p.done.clear(); p.has[0] = p.has[1] = false; p.doneLen = 0;
    SetWindowTextW(p.h, L"");
}
static std::wstring PanePlain(const Pane& p) {
    std::wstring r;
    auto line = [&](const Entry& e) { r += (e.labeled ? L"[" + LabelOf(e.ch, e.spk) + L"] " : L"") + e.text + L"\r\n"; };
    for (auto& e : p.done) line(e);
    for (int c = 0; c < 2; c++) if (p.has[c]) line(p.live[c]);
    return r;
}

// Combo item data: language index (0..NLANGS-1) or -1 for "Auto" / "No translation".
static std::string CodeOf(HWND combo, const char* special) {
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    int idx = (int)SendMessageW(combo, CB_GETITEMDATA, sel, 0);
    if (sel < 0 || idx < 0 || idx >= NLANGS) return special;
    std::string c;
    for (const wchar_t* p = LANGS[idx].code; *p; ++p) c += (char)*p;
    return c;
}

static bool Checked(HWND h);
static void SaveSettings() {
    if (g_ini.empty()) return;
    auto put = [](const wchar_t* k, long long v) {
        WritePrivateProfileStringW(L"app", k, std::to_wstring(v).c_str(), g_ini.c_str());
    };
    int mic = (int)SendMessageW(hMic, CB_GETCURSEL, 0, 0);
    wchar_t name[256] = L"";
    if (mic >= 0) SendMessageW(hMic, CB_GETLBTEXT, mic, (LPARAM)name);
    WritePrivateProfileStringW(L"app", L"mic", name, g_ini.c_str());
    put(L"src", SendMessageW(hLang, CB_GETCURSEL, 0, 0));
    put(L"tgt", SendMessageW(hTgt, CB_GETCURSEL, 0, 0));
    put(L"gain", g_gainDb + 10);
    put(L"ns", g_ns); put(L"nsv", g_nsStr); put(L"agc", g_agc); put(L"boost", g_boost);
    {
        int sd = (int)SendMessageW(hSysDev, CB_GETCURSEL, 0, 0);
        WritePrivateProfileStringW(L"app", L"sysdev", (sd > 0 && sd - 1 < (int)g_sysIds.size()) ? g_sysIds[sd - 1].c_str() : L"", g_ini.c_str());
    }
    put(L"sys", g_sysOn); put(L"rev", g_rev); put(L"echo", g_echo); put(L"diar", Checked(hMany)); put(L"sens", g_sens);
}

static bool Checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

static void SendLanguage() {
    std::string msg = CodeOf(hLang, "auto") + "|" + CodeOf(hTgt, "none") + "|" + (g_rev ? "1" : "0") + "|" +
                      (g_diar ? "1" : "0") + "|" + std::to_string(g_sens.load());
    SendFrame('L', msg.data(), (uint32_t)msg.size());
}

// read the controls into the audio-thread settings and refresh the value labels
static void UpdateFx(bool save) {
    g_gainDb = (int)SendMessageW(hGain, TBM_GETPOS, 0, 0);
    g_nsStr = (int)SendMessageW(hNsStr, TBM_GETPOS, 0, 0);
    g_ns = Checked(hNs);
    g_agc = Checked(hAgc);
    g_boost = Checked(hBoost);
    wchar_t t[48];
    swprintf(t, 48, L"%+d dB", g_gainDb.load()); SetWindowTextW(hGainVal, t);
    swprintf(t, 48, L"strength %d%%", g_nsStr.load()); SetWindowTextW(hNsVal, t);
    EnableWindow(hNsStr, g_ns);
    if (save) SaveSettings();
}
static void UpdateConv(bool save) {
    g_sysOn = Checked(hSysOn);
    g_rev = Checked(hRev);
    g_echo = Checked(hEcho);
    g_diar = Checked(hMany) && g_spkAvailable;
    g_sens = (int)SendMessageW(hSens, TBM_GETPOS, 0, 0);
    wchar_t t[48];
    swprintf(t, 48, L"%d%%", g_sens.load()); SetWindowTextW(hSensVal, t);
    EnableWindow(hRev, g_sysOn);
    EnableWindow(hEcho, g_sysOn);
    EnableWindow(hSens, g_diar);
    if (g_ready) SendLanguage();
    if (save) SaveSettings();
}

// =====================================================================================
//  Audio processing: gain / AGC / noise suppression, then an energy speech detector per channel
// =====================================================================================
struct Fx { float hx = 0, hy = 0, noise = 40, gate = 1, agc = 1, prevG = 1; };

// High-pass (rumble) + adaptive noise gate + manual gain / boost / automatic gain control.
// `mic` = false for system audio: it is clean digital sound, so the microphone gain / boost / noise gate are
// not applied to it (they would only clip it); automatic gain control still applies.
static void ProcessChunk(Fx& fx, short* s, int n, bool mic) {
    const int B = 160;  // 10 ms blocks
    const bool ns = mic && g_ns, agcOn = g_agc;
    const float st = g_nsStr / 100.0f;
    const float baseLin = mic ? std::pow(10.0f, (g_gainDb + (g_boost ? 12 : 0)) / 20.0f) : 1.0f;
    float v[B];
    for (int o = 0; o < n; o += B) {
        int m = std::min(B, n - o);
        double acc = 0;
        for (int i = 0; i < m; i++) {
            float x = s[o + i];
            if (ns) { float y = 0.97f * (fx.hy + x - fx.hx); fx.hx = x; fx.hy = y; x = y; }
            v[i] = x; acc += (double)x * x;
        }
        float r = (float)std::sqrt(acc / m);
        // noise floor: follows dips quickly, rises slowly (and is capped) so speech can't become "noise"
        fx.noise = r < fx.noise ? fx.noise * 0.8f + r * 0.2f : fx.noise * 1.0003f;
        fx.noise = std::min(800.0f, std::max(5.0f, fx.noise));
        float gt = 1;
        if (ns) {
            float lo = fx.noise * 1.6f, hi = fx.noise * 2.8f, depth = st * 0.97f;
            float k = r <= lo ? 0.f : r >= hi ? 1.f : (r - lo) / (hi - lo);
            gt = (1 - depth) + depth * k;
        }
        fx.gate += (gt - fx.gate) * (gt > fx.gate ? 0.6f : 0.08f);
        if (agcOn) {
            if (r > fx.noise * 2.8f) {  // adapt only while somebody is actually speaking
                float want = std::min(31.0f, std::max(0.3f, 2500.0f / std::max(r * baseLin, 1.0f)));
                fx.agc += (want - fx.agc) * 0.01f;
            }
        } else fx.agc += (1 - fx.agc) * 0.05f;
        float g = baseLin * fx.agc * fx.gate;
        for (int i = 0; i < m; i++) {
            float gi = fx.prevG + (g - fx.prevG) * (i + 1) / m;   // ramp: no clicks between blocks
            float y = v[i] * gi;
            s[o + i] = (short)std::max(-32000.f, std::min(32000.f, y));
        }
        fx.prevG = g;
    }
}

// One audio channel: processing + speech detector + framing towards the worker.
struct Channel {
    int id;
    Fx fx;
    std::vector<std::vector<short>> pre;
    std::vector<double> hist;
    size_t histPos = 0;
    bool speaking = false, suppressed = false;
    int silence = 0, total = 0, loud = 0, guard = 0, relLoud = 0;

    explicit Channel(int i = 0) : id(i) {}

    // Feed one 100 ms chunk (16 kHz mono int16); modifies the samples in place.
    void Feed(short* s, int n) {
        const int PREROLL = 3;          // chunks kept before speech start
        const int HANG_CHUNKS = 7;      // 0.7 s of silence ends an utterance
        const int MAX_CHUNKS = 200;     // 20 s hard cap
        const size_t HIST = 150;
        ProcessChunk(fx, s, n, id == 0);
        double acc = 0;
        for (int i = 0; i < n; i++) acc += (double)s[i] * s[i];
        double rms = n ? std::sqrt(acc / n) : 0;
        PostMessageW(hMain, WM_LEVEL, (WPARAM)std::min(100.0, rms / 12.0), (speaking ? 1 : 0) | (id << 1));

        // Noise floor = low percentile of the last 15 s of levels (speech and silence alike), so quiet
        // speech can never drag the threshold upward; the threshold is also hard-capped.
        if (hist.size() < HIST) hist.push_back(rms); else hist[histPos++ % HIST] = rms;
        std::vector<double> sorted(hist);
        std::sort(sorted.begin(), sorted.end());
        double floorRms = sorted[sorted.size() * 15 / 100];
        double thr = std::min(700.0, std::max(80.0, floorRms * 3.0));
        bool isSpeech = rms > (speaking ? thr * 0.6 : thr);   // hysteresis
        if (id == 1 && isSpeech) g_sysLastSpeechMs = NowMs();

        pre.emplace_back(s, s + n);                       // rolling pre-roll (also used when an echo hold ends)
        if ((int)pre.size() > PREROLL) pre.erase(pre.begin());
        // Echo filter: with loudspeakers the microphone hears the other side too. While the system audio
        // carries speech, a microphone utterance is held back; once the other side is silent and you are
        // still talking, it is released (so your own words are kept, their echo is dropped).
        const bool echoNow = id == 0 && g_echo && g_sysOn && NowMs() - g_sysLastSpeechMs < 650;
        if (!speaking) {
            loud = isSpeech ? loud + 1 : 0;
            if (loud >= 2) {  // two loud chunks in a row = speech start
                speaking = true; silence = 0; total = 0; guard = 0;
                suppressed = echoNow;
                if (!suppressed)
                    for (auto& c : pre) { SendAudio(id, c.data(), (int)c.size()); total++; }
                pre.clear();
            }
        } else {
            if (echoNow && !suppressed) { SendEnd(id); suppressed = true; }   // the other side became audible: stop feeding the microphone
            relLoud = (suppressed && !echoNow && isSpeech) ? relLoud + 1 : 0;
            if (suppressed && relLoud >= 2) {              // the other side stopped and you keep talking: release
                suppressed = false; silence = 0; total = 0; relLoud = 0;
                for (auto& c : pre) { SendAudio(id, c.data(), (int)c.size()); total++; }
                pre.clear();
            } else if (!suppressed) {
                SendAudio(id, s, n);
                total++;
            }
            silence = isSpeech ? 0 : silence + 1;
            if (silence >= HANG_CHUNKS || ++guard >= MAX_CHUNKS + 40) {
                if (!suppressed) SendEnd(id);
                speaking = false; suppressed = false; loud = 0;
            }
        }
    }
    void Flush() { if (speaking && !suppressed) SendEnd(id); speaking = false; suppressed = false; }
};
static Channel g_ch[2] = {Channel(0), Channel(1)};

// ---- channel 0: microphone (WinMM) ----
static void MicLoop() {
    int cur = 0;
    while (g_capturing) {
        WaitForSingleObject(g_waveEv, 200);
        while (g_capturing && (g_hdr[cur].dwFlags & WHDR_DONE)) {
            WAVEHDR& h = g_hdr[cur];
            g_ch[0].Feed((short*)h.lpData, (int)(h.dwBytesRecorded / 2));
            waveInAddBuffer(g_wi, &h, sizeof h);
            cur = (cur + 1) % NB;
        }
    }
    g_ch[0].Flush();
}

// COM identifiers (declared here so no extra import library is needed)
static const GUID G_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID G_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID G_IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID G_IID_IAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};

// ---- channel 1: system audio = what the other side says (WASAPI loopback of the default output) ----
static void LoopbackLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* wfx = nullptr;
    auto fail = [](const wchar_t* msg) { PostMessageW(hMain, WM_SYSERR, 0, (LPARAM) new std::wstring(msg)); };
    do {
        if (FAILED(CoCreateInstance(G_CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, G_IID_IMMDeviceEnumerator, (void**)&en))) { fail(L"audio device enumerator"); break; }
        HRESULT hrd = g_sysIdSel.empty() ? en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)
                                         : en->GetDevice(g_sysIdSel.c_str(), &dev);
        if (FAILED(hrd)) { fail(L"the selected output device is not available"); break; }
        if (FAILED(dev->Activate(G_IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac))) { fail(L"cannot open the output device"); break; }
        if (FAILED(ac->GetMixFormat(&wfx))) { fail(L"cannot read the output format"); break; }
        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, wfx, nullptr))) { fail(L"loopback capture is not available"); break; }
        if (FAILED(ac->GetService(G_IID_IAudioCaptureClient, (void**)&cap))) { fail(L"cannot start loopback capture"); break; }
        const bool isFloat = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                             (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                              ((WAVEFORMATEXTENSIBLE*)wfx)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
        const int chn = std::max<int>(1, wfx->nChannels), bits = wfx->wBitsPerSample;
        const double inRate = wfx->nSamplesPerSec;
        if (FAILED(ac->Start())) { fail(L"cannot start loopback capture"); break; }

        std::vector<short> chunk;
        chunk.reserve(CHUNK);
        long long delivered = 0;
        LARGE_INTEGER freq, t0;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        double phase = 0, acc = 0;
        int cnt = 0;
        auto emit = [&](float v) {
            chunk.push_back((short)std::max(-32767.f, std::min(32767.f, v * 32767.f)));
            if ((int)chunk.size() == CHUNK) { g_ch[1].Feed(chunk.data(), CHUNK); delivered += CHUNK; chunk.clear(); }
        };
        while (g_sysRun) {
            Sleep(8);
            UINT32 np = 0;
            while (g_sysRun && SUCCEEDED(cap->GetNextPacketSize(&np)) && np > 0) {
                BYTE* d; UINT32 nf; DWORD fl;
                if (FAILED(cap->GetBuffer(&d, &nf, &fl, nullptr, nullptr))) break;
                for (UINT32 i = 0; i < nf; i++) {
                    float m = 0;
                    if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        for (int c = 0; c < chn; c++) {
                            size_t k = (size_t)i * chn + c;
                            if (isFloat && bits == 32) m += ((const float*)d)[k];
                            else if (bits == 16) m += ((const short*)d)[k] / 32768.f;
                            else if (bits == 32) m += ((const int32_t*)d)[k] / 2147483648.f;
                            else if (bits == 24) {
                                const BYTE* b = d + k * 3;
                                int32_t v = (b[0] << 8) | (b[1] << 16) | (b[2] << 24);
                                m += v / 2147483648.f;
                            }
                        }
                        m /= chn;
                    }
                    acc += m; cnt++; phase += 16000.0;
                    if (phase >= inRate) { phase -= inRate; emit((float)(acc / cnt)); acc = 0; cnt = 0; }   // box-filter downsampling
                }
                cap->ReleaseBuffer(nf);
            }
            // The device sends nothing while nothing plays: keep real-time pace with silence so the
            // speech detector can finish an utterance when the other side stops talking.
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            long long expected = (long long)((double)(t.QuadPart - t0.QuadPart) / freq.QuadPart * 16000.0);
            long long have = delivered + (long long)chunk.size();
            while (g_sysRun && expected - have > 2 * CHUNK) { emit(0.f); have++; }
        }
        ac->Stop();
    } while (0);
    if (cap) cap->Release();
    if (wfx) CoTaskMemFree(wfx);
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    g_ch[1].Flush();
    CoUninitialize();
}

static void StartLoopback() {
    if (g_loop.joinable()) { g_sysRun = false; g_loop.join(); }
    g_ch[1] = Channel(1);
    int sel = (int)SendMessageW(hSysDev, CB_GETCURSEL, 0, 0);
    g_sysIdSel = (sel > 0 && sel - 1 < (int)g_sysIds.size()) ? g_sysIds[sel - 1] : L"";
    g_sysRun = true;
    g_loop = std::thread(LoopbackLoop);
}
static void StopLoopback() {
    g_sysRun = false;
    if (g_loop.joinable()) g_loop.join();
    SendMessageW(hLevel2, PBM_SETPOS, 0, 0);
}

static bool StartCapture(UINT dev) {
    WAVEFORMATEX wf{WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
    g_waveEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    MMRESULT r = waveInOpen(&g_wi, dev, &wf, (DWORD_PTR)g_waveEv, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) { CloseHandle(g_waveEv); g_wi = nullptr; return false; }
    g_ch[0] = Channel(0);
    for (int i = 0; i < NB; i++) {
        g_hdr[i] = WAVEHDR{};
        g_hdr[i].lpData = (LPSTR)g_bufs[i];
        g_hdr[i].dwBufferLength = CHUNK * 2;
        waveInPrepareHeader(g_wi, &g_hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_wi, &g_hdr[i], sizeof(WAVEHDR));
    }
    g_capturing = true;
    waveInStart(g_wi);
    g_capture = std::thread(MicLoop);
    if (g_sysOn) StartLoopback();
    return true;
}

static void StopCapture() {
    if (!g_wi) return;
    g_capturing = false;
    StopLoopback();
    SetEvent(g_waveEv);
    if (g_capture.joinable()) g_capture.join();
    waveInStop(g_wi);
    waveInReset(g_wi);
    for (int i = 0; i < NB; i++) waveInUnprepareHeader(g_wi, &g_hdr[i], sizeof(WAVEHDR));
    waveInClose(g_wi);
    g_wi = nullptr;
    CloseHandle(g_waveEv);
    SendMessageW(hLevel, PBM_SETPOS, 0, 0);
}

static void UpdateUi() {
    bool on = g_capturing;
    SetWindowTextW(hStart, on ? L"Stop" : L"Start");
    EnableWindow(hStart, g_ready);
    EnableWindow(hMic, !on);
}

static void Layout(int w, int h) {
    int m = S(10), rowH = S(26), y = m;
    int lblW = S(130);
    // row 1: microphone
    MoveWindow(hMicLbl, m, y + S(4), lblW, rowH, TRUE);
    MoveWindow(hMic, m + lblW, y, w - 2 * m - lblW, S(300), TRUE);
    y += rowH + m;
    // row 1b: which output device carries the other side (sound card, HDMI, headphones, virtual cable)
    MoveWindow(hSysLbl, m, y + S(4), lblW, rowH, TRUE);
    MoveWindow(hSysDev, m + lblW, y, w - 2 * m - lblW, S(300), TRUE);
    y += rowH + m;
    // row 2: speech language <-> translation language
    int cw = S(150), sw = S(40), x = m;
    MoveWindow(hLangLbl, x, y + S(4), lblW, rowH, TRUE); x += lblW;
    MoveWindow(hLang, x, y, cw, S(300), TRUE); x += cw + S(6);
    MoveWindow(hSwap, x, y, sw, rowH, TRUE); x += sw + S(10);
    MoveWindow(hTgtLbl, x, y + S(4), S(100), rowH, TRUE); x += S(100);
    MoveWindow(hTgt, x, y, cw, S(300), TRUE);
    y += rowH + m;
    // row 3: buttons + level meters (microphone on top, system audio below)
    int bw = S(90), bh = rowH + S(2);
    MoveWindow(hStart, m, y, bw, bh, TRUE);
    MoveWindow(hClear, m + bw + m, y, bw, bh, TRUE);
    MoveWindow(hCopy, m + 2 * (bw + m), y, S(165), bh, TRUE);
    MoveWindow(hCopyTr, m + 2 * (bw + m) + S(165) + m, y, S(165), bh, TRUE);
    MoveWindow(hSave, m + 2 * (bw + m) + 2 * (S(165) + m), y, bw + S(20), bh, TRUE);
    int lx = m + 2 * (bw + m) + 2 * (S(165) + m) + bw + S(20) + m;
    MoveWindow(hLevel, lx, y + S(3), std::max(S(40), w - m - lx), S(10), TRUE);
    MoveWindow(hLevel2, lx, y + S(16), std::max(S(40), w - m - lx), S(10), TRUE);
    y += bh + m;
    // rows 4-5: sound controls (gain / auto-gain / boost / noise suppression)
    int colW = S(190), slW = S(250), vW = S(90), x2 = m + colW + slW + vW + S(10);
    MoveWindow(hGainLbl, m, y + S(4), colW, rowH, TRUE);
    MoveWindow(hGain, m + colW, y, slW, rowH, TRUE);
    MoveWindow(hGainVal, m + colW + slW + S(6), y + S(4), vW, rowH, TRUE);
    MoveWindow(hAgc, x2, y, S(220), rowH, TRUE);
    y += rowH + S(2);
    MoveWindow(hNs, m, y, colW, rowH, TRUE);
    MoveWindow(hNsStr, m + colW, y, slW, rowH, TRUE);
    MoveWindow(hNsVal, m + colW + slW + S(6), y + S(4), vW, rowH, TRUE);
    MoveWindow(hBoost, x2, y, S(260), rowH, TRUE);
    y += rowH + m;
    // rows 6-7: conversation mode (other side via system audio, speaker detection)
    MoveWindow(hSysOn, m, y, S(300), rowH, TRUE);
    MoveWindow(hRev, m + S(305), y, S(330), rowH, TRUE);
    MoveWindow(hEcho, m + S(640), y, S(240), rowH, TRUE);
    y += rowH + S(2);
    MoveWindow(hOne, m, y, S(150), rowH, TRUE);
    MoveWindow(hMany, m + S(155), y, S(210), rowH, TRUE);
    MoveWindow(hSens, m + S(380), y, S(240), rowH, TRUE);
    MoveWindow(hSensVal, m + S(626), y + S(4), S(80), rowH, TRUE);
    y += rowH + m;
    MoveWindow(hStatus, m, y, w - 2 * m, S(20), TRUE);
    y += S(24);
    // two panels share the rest
    int capH = S(20);
    int avail = std::max(S(120), h - y - m);
    int paneH = (avail - 2 * capH - m) / 2;
    MoveWindow(hCapSrc, m, y, w - 2 * m, capH, TRUE); y += capH;
    MoveWindow(hEdit, m, y, w - 2 * m, paneH, TRUE); y += paneH + m;
    MoveWindow(hCapTr, m, y, w - 2 * m, capH, TRUE); y += capH;
    MoveWindow(hEditTr, m, y, w - 2 * m, std::max(S(30), h - y - m), TRUE);
}

static HWND Mk(const wchar_t* cls, const wchar_t* txt, DWORD style, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hMain,
                             (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)hFont, TRUE);
    return c;
}

// Write text as UTF-8 with BOM (opens correctly in Notepad/Word).
static bool WriteTextFile(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &u[0], n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w; const char bom[3] = {'\xEF', '\xBB', '\xBF'};
    bool ok = WriteFile(f, bom, 3, &w, nullptr) && (u.empty() || WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr));
    CloseHandle(f);
    return ok;
}

// The whole conversation in one language: what was said in it, plus the translation of what was said otherwise.
static std::wstring DocumentIn(const std::string& key) {
    std::wstring r;
    for (auto& u : g_utts) {
        const std::wstring* t = nullptr;
        if (key == "orig" || u.oLang == key) t = &u.orig;
        else if (u.tLang == key) t = &u.tr;
        if (!t || t->empty()) continue;
        r += (u.labeled ? L"[" + LabelOf(u.ch, u.spk) + L"] " : L"") + *t + L"\r\n";
    }
    return r;
}

// "Save..." writes both language versions: <name>_<lang>.txt for the speech language and for the translation language
static void SaveDocuments(HWND hw) {
    if (g_utts.empty()) { SetStatus(L"Nothing to save yet."); return; }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t file[MAX_PATH];
    swprintf(file, MAX_PATH, L"Transcript_%04d-%02d-%02d_%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o; o.hwndOwner = hw;
    o.lpstrFilter = L"Text document (*.txt)\0*.txt\0";
    o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.lpstrDefExt = L"txt";
    o.lpstrTitle = L"Save both versions (a language code is appended to the name automatically)";
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&o)) return;
    std::wstring base = file;
    if (base.size() > 4 && _wcsicmp(base.c_str() + base.size() - 4, L".txt") == 0) base.resize(base.size() - 4);
    auto w = [](const std::string& c) { return std::wstring(c.begin(), c.end()); };
    std::string sc = CodeOf(hLang, "auto"), tc = CodeOf(hTgt, "none");
    std::string keyA = sc == "auto" ? "orig" : sc;
    std::wstring saved;
    std::wstring a = base + L"_" + w(keyA) + L".txt";
    bool ok = WriteTextFile(a, DocumentIn(keyA));
    if (ok) saved = a;
    if (ok && tc != "none") {
        std::wstring b = base + L"_" + w(tc) + L".txt";
        ok = WriteTextFile(b, DocumentIn(tc));
        if (ok) saved += L"  and  " + b;
    }
    SetStatus(ok ? L"Saved: " + saved : L"Could not save the file.");
}

static void FillSysDevices() {
    SendMessageW(hSysDev, CB_RESETCONTENT, 0, 0);
    g_sysIds.clear();
    SendMessageW(hSysDev, CB_ADDSTRING, 0, (LPARAM)L"Default output device");
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(G_CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, G_IID_IMMDeviceEnumerator, (void**)&en))) {
        IMMDeviceCollection* col = nullptr;
        UINT n = 0;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && SUCCEEDED(col->GetCount(&n))) {
            static const PROPERTYKEY KEY_NAME = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
            for (UINT i = 0; i < n; i++) {
                IMMDevice* d = nullptr;
                if (FAILED(col->Item(i, &d))) continue;
                LPWSTR id = nullptr;
                std::wstring name = L"(unnamed device)";
                if (SUCCEEDED(d->GetId(&id))) {
                    IPropertyStore* ps = nullptr;
                    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                        PROPVARIANT v;
                        PropVariantInit(&v);
                        if (SUCCEEDED(ps->GetValue(KEY_NAME, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
                        PropVariantClear(&v);
                        ps->Release();
                    }
                    g_sysIds.push_back(id);
                    SendMessageW(hSysDev, CB_ADDSTRING, 0, (LPARAM)name.c_str());
                    CoTaskMemFree(id);
                }
                d->Release();
            }
        }
        if (col) col->Release();
        en->Release();
    }
    SendMessageW(hSysDev, CB_SETCURSEL, 0, 0);
}

static void SetCheck(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

static void StyleRich(HWND e, COLORREF bg) {
    SendMessageW(e, EM_SETBKGNDCOLOR, 0, bg);
    SendMessageW(e, EM_SETREADONLY, TRUE, 0);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    cf.yHeight = 14 * 20;   // 14 pt in twips
    cf.crTextColor = RGB(30, 30, 30);
    wcscpy(cf.szFaceName, L"Segoe UI");
    SendMessageW(e, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
}

static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        hMain = hw;
        HDC dc = GetDC(nullptr); g_dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc);
        hFont = CreateFontW(-MulDiv(11, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        hBigFont = CreateFontW(-MulDiv(14, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        hBoldFont = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        hMicLbl = Mk(L"STATIC", L"Microphone:", 0, 0);
        hMic = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_MIC);
        hSysLbl = Mk(L"STATIC", L"Other side from:", 0, 0);
        hSysDev = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_SYSDEV);
        hLangLbl = Mk(L"STATIC", L"Speech language:", 0, 0);
        hLang = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_LANG);
        hSwap = Mk(L"BUTTON", L"⇄", BS_PUSHBUTTON | WS_TABSTOP, ID_SWAP);
        hTgtLbl = Mk(L"STATIC", L"Translate to:", 0, 0);
        hTgt = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_TGT);
        hStart = Mk(L"BUTTON", L"Start", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, ID_START);
        hClear = Mk(L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, ID_CLEAR);
        hCopy = Mk(L"BUTTON", L"Copy text", BS_PUSHBUTTON | WS_TABSTOP, ID_COPY);
        hCopyTr = Mk(L"BUTTON", L"Copy translation", BS_PUSHBUTTON | WS_TABSTOP, ID_COPYTR);
        hSave = Mk(L"BUTTON", L"Save...", BS_PUSHBUTTON | WS_TABSTOP, ID_SAVE);
        hLevel = Mk(PROGRESS_CLASSW, L"", 0, ID_LEVEL);
        hLevel2 = Mk(PROGRESS_CLASSW, L"", 0, ID_LEVEL2);
        SendMessageW(hLevel, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(hLevel2, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        hGainLbl = Mk(L"STATIC", L"Microphone gain:", 0, 0);
        hGain = Mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, ID_GAIN);
        SendMessageW(hGain, TBM_SETRANGE, TRUE, MAKELONG(-10, 30));
        hGainVal = Mk(L"STATIC", L"+0 dB", 0, 0);
        hAgc = Mk(L"BUTTON", L"Auto gain (AGC)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_AGC);
        hNs = Mk(L"BUTTON", L"Noise suppression", BS_AUTOCHECKBOX | WS_TABSTOP, ID_NS);
        hNsStr = Mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, ID_NSSTR);
        SendMessageW(hNsStr, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        hNsVal = Mk(L"STATIC", L"strength 50%", 0, 0);
        hBoost = Mk(L"BUTTON", L"Boost +12 dB (quiet sources)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_BOOST);
        hSysOn = Mk(L"BUTTON", L"Capture the other side (system audio)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_SYS);
        hRev = Mk(L"BUTTON", L"Other side speaks the translation language", BS_AUTOCHECKBOX | WS_TABSTOP, ID_REV);
        hEcho = Mk(L"BUTTON", L"Speakers mode (echo filter)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_ECHO);
        hOne = Mk(L"BUTTON", L"One interlocutor", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, ID_ONE);
        hMany = Mk(L"BUTTON", L"Several (detect voices)", BS_AUTORADIOBUTTON, ID_MANY);
        hSens = Mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, ID_SENS);
        SendMessageW(hSens, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        hSensVal = Mk(L"STATIC", L"60%", 0, 0);
        hStatus = Mk(L"STATIC", L"Starting the recognizer...", 0, ID_STATUS);
        hCapSrc = Mk(L"STATIC", L"Recognized speech", 0, 0);
        LoadLibraryW(L"Msftedit.dll");
        hEdit = Mk(L"RICHEDIT50W", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_EDIT, WS_EX_CLIENTEDGE);
        hCapTr = Mk(L"STATIC", L"Translation", 0, 0);
        hEditTr = Mk(L"RICHEDIT50W", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_EDITTR, WS_EX_CLIENTEDGE);
        SendMessageW(hCapSrc, WM_SETFONT, (WPARAM)hBoldFont, TRUE);
        SendMessageW(hCapTr, WM_SETFONT, (WPARAM)hBoldFont, TRUE);
        SendMessageW(hSwap, WM_SETFONT, (WPARAM)hBigFont, TRUE);
        for (HWND e : {hEdit, hEditTr}) SendMessageW(e, EM_SETLIMITTEXT, 0x7FFFFFFF, 0);
        StyleRich(hEdit, RGB(255, 255, 255));
        StyleRich(hEditTr, RGB(238, 245, 255));
        g_src.h = hEdit;
        g_tr.h = hEditTr;

        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        g_ini = exePath;
        g_ini = g_ini.substr(0, g_ini.find_last_of(L"\\/")) + L"\\settings.ini";
        if (GetFileAttributesW(g_ini.c_str()) == INVALID_FILE_ATTRIBUTES ||
            GetPrivateProfileIntW(L"app", L"v", 0, g_ini.c_str()) != 2) {
            // a UTF-16 (BOM) ini keeps non-ASCII microphone names intact
            int oldSrc = GetPrivateProfileIntW(L"app", L"src", -1, g_ini.c_str());
            int oldTgt = GetPrivateProfileIntW(L"app", L"tgt", -1, g_ini.c_str());
            HANDLE f = CreateFileW(g_ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                const wchar_t bom = 0xFEFF; DWORD w;
                WriteFile(f, &bom, 2, &w, nullptr);
                CloseHandle(f);
                WritePrivateProfileStringW(L"app", L"v", L"2", g_ini.c_str());
                if (oldSrc >= 0) WritePrivateProfileStringW(L"app", L"src", std::to_wstring(oldSrc).c_str(), g_ini.c_str());
                if (oldTgt >= 0) WritePrivateProfileStringW(L"app", L"tgt", std::to_wstring(oldTgt).c_str(), g_ini.c_str());
            }
        }

        SendMessageW(hMic, CB_ADDSTRING, 0, (LPARAM)L"System default");
        SendMessageW(hMic, CB_SETITEMDATA, 0, (LPARAM)-1);
        UINT n = waveInGetNumDevs();
        for (UINT i = 0; i < n; i++) {
            WAVEINCAPSW c;
            if (waveInGetDevCapsW(i, &c, sizeof c) == MMSYSERR_NOERROR) {
                int idx = (int)SendMessageW(hMic, CB_ADDSTRING, 0, (LPARAM)c.szPname);
                SendMessageW(hMic, CB_SETITEMDATA, idx, (LPARAM)i);
            }
        }
        SendMessageW(hMic, CB_SETCURSEL, 0, 0);
        SendMessageW(hLang, CB_ADDSTRING, 0, (LPARAM)L"Auto");
        SendMessageW(hLang, CB_SETITEMDATA, 0, (LPARAM)-1);
        SendMessageW(hTgt, CB_ADDSTRING, 0, (LPARAM)L"No translation");
        SendMessageW(hTgt, CB_SETITEMDATA, 0, (LPARAM)-1);
        for (int i = 0; i < NLANGS; i++) {
            int a = (int)SendMessageW(hLang, CB_ADDSTRING, 0, (LPARAM)LANGS[i].name);
            SendMessageW(hLang, CB_SETITEMDATA, a, i);
            int b = (int)SendMessageW(hTgt, CB_ADDSTRING, 0, (LPARAM)LANGS[i].name);
            SendMessageW(hTgt, CB_SETITEMDATA, b, i);
        }
        // defaults: speech Russian -> translation English; then restore the saved choice
        auto geti = [](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"app", k, d, g_ini.c_str()); };
        int srcSel = geti(L"src", 1), tgtSel = geti(L"tgt", 2);
        SendMessageW(hLang, CB_SETCURSEL, (srcSel >= 0 && srcSel <= NLANGS) ? srcSel : 1, 0);
        SendMessageW(hTgt, CB_SETCURSEL, (tgtSel >= 0 && tgtSel <= NLANGS) ? tgtSel : 2, 0);
        SendMessageW(hGain, TBM_SETPOS, TRUE, geti(L"gain", 10) - 10);
        SendMessageW(hNsStr, TBM_SETPOS, TRUE, geti(L"nsv", 50));
        SetCheck(hNs, geti(L"ns", 1)); SetCheck(hAgc, geti(L"agc", 1)); SetCheck(hBoost, geti(L"boost", 0));
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        FillSysDevices();
        {
            wchar_t sid[512];
            GetPrivateProfileStringW(L"app", L"sysdev", L"", sid, 512, g_ini.c_str());
            for (size_t i = 0; sid[0] && i < g_sysIds.size(); i++)
                if (g_sysIds[i] == sid) SendMessageW(hSysDev, CB_SETCURSEL, i + 1, 0);
        }
        SetCheck(hSysOn, geti(L"sys", 0)); SetCheck(hRev, geti(L"rev", 1)); SetCheck(hEcho, geti(L"echo", 1));
        SetCheck(hMany, geti(L"diar", 0)); SetCheck(hOne, !geti(L"diar", 0));
        SendMessageW(hSens, TBM_SETPOS, TRUE, geti(L"sens", 60));
        UpdateFx(false);
        UpdateConv(false);
        wchar_t savedMic[256];
        GetPrivateProfileStringW(L"app", L"mic", L"", savedMic, 256, g_ini.c_str());
        if (savedMic[0]) {
            int f = (int)SendMessageW(hMic, CB_FINDSTRINGEXACT, -1, (LPARAM)savedMic);
            if (f >= 0) SendMessageW(hMic, CB_SETCURSEL, f, 0);
        }

        if (!StartWorker()) SetStatus(L"Could not start Python. Run setup.ps1 or set [paths] python= in settings.ini.");
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == hGain || (HWND)lp == hNsStr) UpdateFx(LOWORD(wp) == TB_ENDTRACK);   // save when the drag ends
        if ((HWND)lp == hSens) UpdateConv(LOWORD(wp) == TB_ENDTRACK);
        return 0;
    case WM_SIZE: Layout(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_GETMINMAXINFO: { auto* m = (MINMAXINFO*)lp; m->ptMinTrackSize = {S(860), S(740)}; return 0; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_START:
            if (g_capturing) {
                StopCapture();
                SetStatus(L"Stopped.");
            } else {
                int sel = (int)SendMessageW(hMic, CB_GETCURSEL, 0, 0);
                UINT dev = (UINT)SendMessageW(hMic, CB_GETITEMDATA, sel, 0);
                if (StartCapture(dev)) SetStatus(g_sysOn ? L"Listening to you and to the other side..." : L"Listening... speak now.");
                else SetStatus(L"Could not open the microphone.");
            }
            UpdateUi();
            return 0;
        case ID_CLEAR:
            PaneClear(g_src); PaneClear(g_tr);
            g_utts.clear(); g_await[0].clear(); g_await[1].clear();
            SendFrame('R', nullptr, 0);      // new conversation: forget the known voices
            return 0;
        case ID_SAVE: SaveDocuments(hw); return 0;
        case ID_NS: case ID_AGC: case ID_BOOST: UpdateFx(true); return 0;
        case ID_REV: case ID_ECHO: case ID_ONE: case ID_MANY: UpdateConv(true); return 0;
        case ID_SYSDEV:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                SaveSettings();
                if (g_capturing && g_sysOn) StartLoopback();
            }
            return 0;
        case ID_SYS:
            UpdateConv(true);
            if (g_capturing) { if (g_sysOn) StartLoopback(); else StopLoopback(); }
            return 0;
        case ID_COPY: case ID_COPYTR: {
            std::wstring t = PanePlain(LOWORD(wp) == ID_COPY ? g_src : g_tr);
            if (OpenClipboard(hw)) {
                EmptyClipboard();
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * 2);
                memcpy(GlobalLock(g), t.c_str(), (t.size() + 1) * 2);
                GlobalUnlock(g);
                SetClipboardData(CF_UNICODETEXT, g);
                CloseClipboard();
            }
            return 0;
        }
        case ID_SWAP: {  // swap speech language and translation language (not for Auto / no translation)
            int s = (int)SendMessageW(hLang, CB_GETCURSEL, 0, 0), t = (int)SendMessageW(hTgt, CB_GETCURSEL, 0, 0);
            if (s > 0 && t > 0) {
                SendMessageW(hLang, CB_SETCURSEL, t, 0);
                SendMessageW(hTgt, CB_SETCURSEL, s, 0);
                g_tr.has[0] = g_tr.has[1] = false; PaneRender(g_tr, nullptr);
                SendLanguage(); SaveSettings();
            } else SetStatus(L"Swap works when both languages are concrete (not \"Auto\" and not \"No translation\").");
            return 0;
        }
        case ID_LANG: case ID_TGT: case ID_MIC:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                if (LOWORD(wp) != ID_MIC) { g_tr.has[0] = g_tr.has[1] = false; PaneRender(g_tr, nullptr); SendLanguage(); }
                SaveSettings();
            }
            return 0;
        }
        break;
    case WM_LEVEL: {
        int ch = (int)(lp >> 1) & 1;
        SendMessageW(ch ? hLevel2 : hLevel, PBM_SETPOS, wp, 0);
        static int speaking[2] = {0, 0}, shown = -1;
        speaking[ch] = (int)lp & 1;
        int state = speaking[0] | (speaking[1] << 1);
        if (g_capturing && state != shown) {   // live feedback: which side does the speech detector hear?
            shown = state;
            SetStatus(state == 0 ? (g_sysOn ? L"Listening to you and to the other side..." : L"Listening... speak now.")
                      : state == 1 ? L"Hearing you..." : state == 2 ? L"Hearing the other side..." : L"Hearing both sides...");
        }
        return 0;
    }
    case WM_SYSERR: {
        std::unique_ptr<std::wstring> m((std::wstring*)lp);
        g_sysRun = false;
        SetStatus(L"System audio capture failed: " + *m + L". Microphone capture continues.");
        return 0;
    }
    case WM_WORKER: {
        std::unique_ptr<WorkerMsg> m((WorkerMsg*)lp);
        int ch = m->ch & 1;
        switch (m->kind) {
        case 'S':
            if (m->text.rfind(L"ready", 0) == 0) {
                g_ready = true;
                SendLanguage();
                SetStatus(L"Ready " + m->text.substr(6) + (g_trAvailable ? L", NLLB translation enabled." : L", translation unavailable.") +
                          L" Choose a microphone and press Start.");
                UpdateUi();
            } else SetStatus(m->text);
            break;
        case 'N':  // translation engine failed to load
            g_trAvailable = false;
            SendMessageW(hTgt, CB_SETCURSEL, 0, 0);
            EnableWindow(hTgt, FALSE); EnableWindow(hSwap, FALSE);
            SaveSettings();
            break;
        case 'V':  // speaker detection engine unavailable
            g_spkAvailable = false;
            SetCheck(hMany, false); SetCheck(hOne, true);
            EnableWindow(hMany, FALSE);
            UpdateConv(true);
            break;
        case 'P': {
            Entry e{ch, m->spk, Labeled(), m->text};
            PaneSetLive(g_src, ch, e);
            break;
        }
        case 'F': {
            if (m->text.empty()) { g_await[ch].push_back(-1); PaneClearLive(g_src, ch); break; }
            Entry e{ch, m->spk, Labeled(), m->text};
            Utt u{ch, m->spk, e.labeled, m->text, L"", m->lang, ""};
            g_utts.push_back(u);
            g_await[ch].push_back((int)g_utts.size() - 1);
            PaneCommit(g_src, e);
            break;
        }
        case 'T': {
            Entry e{ch, m->spk, Labeled(), m->text};
            PaneSetLive(g_tr, ch, e);
            break;
        }
        case 'U': {
            int idx = -1;
            if (!g_await[ch].empty()) { idx = g_await[ch].front(); g_await[ch].pop_front(); }
            if (idx >= 0 && idx < (int)g_utts.size() && !m->text.empty()) {
                g_utts[idx].tr = m->text;
                g_utts[idx].tLang = m->lang;
                PaneCommit(g_tr, Entry{ch, g_utts[idx].spk, g_utts[idx].labeled, m->text});
            } else PaneClearLive(g_tr, ch);
            break;
        }
        case 'X':
            g_ready = false;
            if (g_capturing) StopCapture();
            SetStatus(L"The recognizer stopped (see worker.log next to the program).");
            UpdateUi();
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        if (g_capturing) StopCapture();
        SendFrame('Q', nullptr, 0);
        if (hJob) CloseHandle(hJob);  // kills python tree
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&ic);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VoiceWindow";
    RegisterClassW(&wc);
    HDC dc = GetDC(nullptr); int dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc);
    // WS_EX_TOPMOST: always above other windows; it leaves only when minimized with the "-" button
    HWND w = CreateWindowExW(WS_EX_TOPMOST, L"VoiceWindow", L"Voice Recognition and Translation", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(900, dpi, 96), MulDiv(1000, dpi, 96),
                             nullptr, nullptr, hInst, nullptr);
    ShowWindow(w, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (g_reader.joinable()) g_reader.detach();
    return 0;
}
