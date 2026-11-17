// VoiceWindow: window with microphone picker, live speech-to-text via the Whisper worker in H:\Dictation.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Paths are resolved at start-up (see StartWorker): settings.ini [paths] python / root,
// otherwise <exe dir>\\venv\\Scripts\\python.exe and <exe dir>\\data.

enum { ID_MIC = 101, ID_LANG, ID_START, ID_CLEAR, ID_COPY, ID_LEVEL, ID_STATUS, ID_EDIT,
       ID_TGT, ID_SWAP, ID_COPYTR, ID_EDITTR, ID_SAVE,
       ID_GAIN, ID_NSSTR, ID_NS, ID_AGC, ID_BOOST };
enum { WM_WORKER = WM_APP + 1, WM_LEVEL = WM_APP + 2 };

// language table: code for the worker, label shown in the combos
struct Lang { const wchar_t* code; const wchar_t* name; };
static const Lang LANGS[] = {{L"ru", L"Russian"}, {L"en", L"English"}, {L"de", L"German"}, {L"fr", L"French"},
                             {L"es", L"Spanish"}, {L"uk", L"Ukrainian"}, {L"pl", L"Polish"}, {L"it", L"Italian"}};
static const int NLANGS = sizeof(LANGS) / sizeof(LANGS[0]);

// ---- globals ----
static HWND hMain, hMicLbl, hMic, hLang, hStart, hClear, hCopy, hLevel, hStatus, hEdit;
// ---- audio processing (settings shared with the UI thread) ----
static std::atomic<int> g_gainDb{0}, g_ns{1}, g_nsStr{50}, g_agc{1}, g_boost{0};
static HWND hGainLbl, hGain, hGainVal, hAgc, hNs, hNsStr, hNsVal, hBoost;
static HWND hSave, hLangLbl, hTgtLbl, hTgt, hSwap, hCopyTr, hEditTr, hCapSrc, hCapTr;
static HFONT hFont, hBigFont, hBoldFont;
static HBRUSH hTrBrush;
static std::wstring g_trCommitted, g_trPartial;
static bool g_trAvailable = true;
static std::wstring g_ini;
static int g_dpi = 96;
static HANDLE hChildIn = nullptr, hJob = nullptr;
static PROCESS_INFORMATION g_pi{};
static std::mutex g_sendLock;
static std::thread g_reader, g_capture;
static std::atomic<bool> g_capturing{false};
static bool g_ready = false;
static std::wstring g_committed, g_partial;

static HWAVEIN g_wi = nullptr;
static HANDLE g_waveEv = nullptr;
static const int NB = 8, CHUNK = 1600;  // 8 buffers x 100 ms @16 kHz
static WAVEHDR g_hdr[NB];
static short g_bufs[NB][CHUNK];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

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

struct WorkerMsg { char kind; std::wstring text; };

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
            auto* m = new WorkerMsg{line[0], Utf8ToW(line.size() > 2 ? line.substr(2) : "")};
            PostMessageW(hMain, WM_WORKER, 0, (LPARAM)m);
        }
    }
    PostMessageW(hMain, WM_WORKER, 0, (LPARAM) new WorkerMsg{'X', L""});
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

// ---- text view ----
static void ShowIn(HWND e, const std::wstring& all) {
    SetWindowTextW(e, all.c_str());
    int len = (int)all.size();
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}
static void RefreshText() { ShowIn(hEdit, g_committed + g_partial); }
static void RefreshTr() { ShowIn(hEditTr, g_trCommitted + g_trPartial); }

// Combo item data: language index (0..NLANGS-1) or -1 for "Auto" / "No translation".
static std::string CodeOf(HWND combo, const char* special) {
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    int idx = (int)SendMessageW(combo, CB_GETITEMDATA, sel, 0);
    if (sel < 0 || idx < 0 || idx >= NLANGS) return special;
    std::string c;
    for (const wchar_t* p = LANGS[idx].code; *p; ++p) c += (char)*p;
    return c;
}

static void UpdateFx(bool save);
static void SaveSettings() {
    if (g_ini.empty()) return;
    int mic = (int)SendMessageW(hMic, CB_GETCURSEL, 0, 0);
    wchar_t name[256] = L"";
    if (mic >= 0) SendMessageW(hMic, CB_GETLBTEXT, mic, (LPARAM)name);
    WritePrivateProfileStringW(L"app", L"mic", name, g_ini.c_str());
    WritePrivateProfileStringW(L"app", L"src", std::to_wstring(SendMessageW(hLang, CB_GETCURSEL, 0, 0)).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"app", L"tgt", std::to_wstring(SendMessageW(hTgt, CB_GETCURSEL, 0, 0)).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"app", L"gain", std::to_wstring(g_gainDb + 10).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"app", L"ns", std::to_wstring(g_ns.load()).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"app", L"nsv", std::to_wstring(g_nsStr.load()).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"app", L"agc", std::to_wstring(g_agc.load()).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"app", L"boost", std::to_wstring(g_boost.load()).c_str(), g_ini.c_str());
}

// read the controls into the audio-thread settings and refresh the value labels
static void UpdateFx(bool save) {
    g_gainDb = (int)SendMessageW(hGain, TBM_GETPOS, 0, 0);
    g_nsStr = (int)SendMessageW(hNsStr, TBM_GETPOS, 0, 0);
    g_ns = SendMessageW(hNs, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_agc = SendMessageW(hAgc, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_boost = SendMessageW(hBoost, BM_GETCHECK, 0, 0) == BST_CHECKED;
    wchar_t t[48];
    swprintf(t, 48, L"%+d dB", g_gainDb.load()); SetWindowTextW(hGainVal, t);
    swprintf(t, 48, L"strength %d%%", g_nsStr.load()); SetWindowTextW(hNsVal, t);
    EnableWindow(hNsStr, g_ns);
    if (save) SaveSettings();
}

static void SendLanguage() {
    std::string msg = CodeOf(hLang, "auto") + "|" + CodeOf(hTgt, "none");
    SendFrame('L', msg.data(), (uint32_t)msg.size());
}

// ---- audio processing ----

struct Fx { float hx = 0, hy = 0, noise = 40, gate = 1, agc = 1, prevG = 1; };

// High-pass (rumble) + adaptive noise gate + manual gain / boost / automatic gain control.
static void ProcessChunk(Fx& fx, short* s, int n) {
    const int B = 160;  // 10 ms blocks
    const bool ns = g_ns, agcOn = g_agc;
    const float st = g_nsStr / 100.0f;
    const float baseLin = std::pow(10.0f, (g_gainDb + (g_boost ? 12 : 0)) / 20.0f);
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

// ---- audio capture + simple energy VAD ----
static void CaptureLoop() {
    const int PREROLL = 3;          // chunks kept before speech start
    const int HANG_CHUNKS = 7;      // 0.7 s of silence ends an utterance
    const int MAX_CHUNKS = 200;     // 20 s hard cap
    std::vector<std::vector<short>> pre;
    bool speaking = false;
    int silence = 0, total = 0, cur = 0, loud = 0;
    // Noise floor = low percentile of the last 15 s of levels (speech and silence alike), so quiet
    // speech can never drag the threshold upward; the threshold is also hard-capped.
    std::vector<double> hist;
    size_t histPos = 0;
    Fx fx;
    const size_t HIST = 150;

    while (g_capturing) {
        WaitForSingleObject(g_waveEv, 200);
        while (g_capturing && (g_hdr[cur].dwFlags & WHDR_DONE)) {
            WAVEHDR& h = g_hdr[cur];
            int n = (int)(h.dwBytesRecorded / 2);
            short* s = (short*)h.lpData;
            ProcessChunk(fx, s, n);
            double acc = 0;
            for (int i = 0; i < n; i++) acc += (double)s[i] * s[i];
            double rms = n ? std::sqrt(acc / n) : 0;
            PostMessageW(hMain, WM_LEVEL, (WPARAM)std::min(100.0, rms / 12.0), speaking ? 1 : 0);

            if (hist.size() < HIST) hist.push_back(rms); else hist[histPos++ % HIST] = rms;
            std::vector<double> sorted(hist);
            std::sort(sorted.begin(), sorted.end());
            double floorRms = sorted[sorted.size() * 15 / 100];
            double thr = std::min(700.0, std::max(80.0, floorRms * 3.0));
            bool isSpeech = rms > (speaking ? thr * 0.6 : thr);   // hysteresis
            if (!speaking) {
                loud = isSpeech ? loud + 1 : 0;
                pre.emplace_back(s, s + n);
                if ((int)pre.size() > PREROLL) pre.erase(pre.begin());
                if (loud >= 2) {  // two loud chunks in a row = speech start
                    speaking = true; silence = 0; total = 0;
                    for (auto& c : pre) { SendFrame('A', c.data(), (uint32_t)(c.size() * 2)); total++; }
                    pre.clear();
                }
            } else {
                SendFrame('A', s, (uint32_t)(n * 2));
                total++;
                silence = isSpeech ? 0 : silence + 1;
                if (silence >= HANG_CHUNKS || total >= MAX_CHUNKS) {
                    SendFrame('E', nullptr, 0);
                    speaking = false; loud = 0;
                }
            }
            waveInAddBuffer(g_wi, &h, sizeof h);
            cur = (cur + 1) % NB;
        }
    }
    if (speaking) SendFrame('E', nullptr, 0);
}

static bool StartCapture(UINT dev) {
    WAVEFORMATEX wf{WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
    g_waveEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    MMRESULT r = waveInOpen(&g_wi, dev, &wf, (DWORD_PTR)g_waveEv, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) { CloseHandle(g_waveEv); g_wi = nullptr; return false; }
    for (int i = 0; i < NB; i++) {
        g_hdr[i] = WAVEHDR{};
        g_hdr[i].lpData = (LPSTR)g_bufs[i];
        g_hdr[i].dwBufferLength = CHUNK * 2;
        waveInPrepareHeader(g_wi, &g_hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_wi, &g_hdr[i], sizeof(WAVEHDR));
    }
    g_capturing = true;
    waveInStart(g_wi);
    g_capture = std::thread(CaptureLoop);
    return true;
}

static void StopCapture() {
    if (!g_wi) return;
    g_capturing = false;
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
    // row 2: speech language <-> translation language
    int cw = S(150), sw = S(40), x = m;
    MoveWindow(hLangLbl, x, y + S(4), lblW, rowH, TRUE); x += lblW;
    MoveWindow(hLang, x, y, cw, S(300), TRUE); x += cw + S(6);
    MoveWindow(hSwap, x, y, sw, rowH, TRUE); x += sw + S(10);
    MoveWindow(hTgtLbl, x, y + S(4), S(100), rowH, TRUE); x += S(100);
    MoveWindow(hTgt, x, y, cw, S(300), TRUE);
    y += rowH + m;
    // row 3: buttons + level meter
    int bw = S(90), bh = rowH + S(2);
    MoveWindow(hStart, m, y, bw, bh, TRUE);
    MoveWindow(hClear, m + bw + m, y, bw, bh, TRUE);
    MoveWindow(hCopy, m + 2 * (bw + m), y, S(165), bh, TRUE);
    MoveWindow(hCopyTr, m + 2 * (bw + m) + S(165) + m, y, S(165), bh, TRUE);
    MoveWindow(hSave, m + 2 * (bw + m) + 2 * (S(165) + m), y, bw + S(20), bh, TRUE);
    int lx = m + 2 * (bw + m) + 2 * (S(165) + m) + bw + S(20) + m;
    MoveWindow(hLevel, lx, y + S(6), std::max(S(40), w - m - lx), S(14), TRUE);
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
    MoveWindow(hBoost, x2, y, S(220), rowH, TRUE);
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

// "Save..." writes both versions: <name>_<speech lang>.txt and <name>_<translation lang>.txt
static void SaveDocuments(HWND hw) {
    std::wstring src = g_committed + g_partial, tr = g_trCommitted + g_trPartial;
    if (src.empty() && tr.empty()) { SetStatus(L"Nothing to save yet."); return; }
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
    auto wcode = [](const std::string& c) { return std::wstring(c.begin(), c.end()); };
    std::wstring a = base + L"_" + wcode(CodeOf(hLang, "orig")) + L".txt";
    std::wstring saved;
    bool ok = WriteTextFile(a, src);
    if (ok) saved = a;
    std::string tc = CodeOf(hTgt, "");
    if (ok && !tc.empty() && !tr.empty()) {
        std::wstring b = base + L"_" + wcode(tc) + L".txt";
        ok = WriteTextFile(b, tr);
        if (ok) saved += L"  and  " + b;
    }
    SetStatus(ok ? L"Saved: " + saved : L"Could not save the file.");
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
        hTrBrush = CreateSolidBrush(RGB(238, 245, 255));
        hMicLbl = Mk(L"STATIC", L"Microphone:", 0, 0);
        hMic = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_MIC);
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
        SendMessageW(hLevel, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
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
        hStatus = Mk(L"STATIC", L"Starting the recognizer...", 0, ID_STATUS);
        hCapSrc = Mk(L"STATIC", L"Recognized speech", 0, 0);
        hEdit = Mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_EDIT, WS_EX_CLIENTEDGE);
        hCapTr = Mk(L"STATIC", L"Translation", 0, 0);
        hEditTr = Mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_EDITTR, WS_EX_CLIENTEDGE);
        SendMessageW(hCapSrc, WM_SETFONT, (WPARAM)hBoldFont, TRUE);
        SendMessageW(hCapTr, WM_SETFONT, (WPARAM)hBoldFont, TRUE);
        SendMessageW(hSwap, WM_SETFONT, (WPARAM)hBigFont, TRUE);
        for (HWND e : {hEdit, hEditTr}) {
            SendMessageW(e, WM_SETFONT, (WPARAM)hBigFont, TRUE);
            SendMessageW(e, EM_SETLIMITTEXT, 0x7FFFFFFF, 0);
        }

        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        g_ini = exePath;
        g_ini = g_ini.substr(0, g_ini.find_last_of(L"\\/")) + L"\\settings.ini";
        if (GetFileAttributesW(g_ini.c_str()) == INVALID_FILE_ATTRIBUTES ||
            GetPrivateProfileIntW(L"app", L"v", 0, g_ini.c_str()) != 2) {
            // a UTF-16 (BOM) ini keeps Cyrillic microphone names intact
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
        int srcSel = GetPrivateProfileIntW(L"app", L"src", 1, g_ini.c_str());
        int tgtSel = GetPrivateProfileIntW(L"app", L"tgt", 2, g_ini.c_str());
        SendMessageW(hLang, CB_SETCURSEL, (srcSel >= 0 && srcSel <= NLANGS) ? srcSel : 1, 0);
        SendMessageW(hTgt, CB_SETCURSEL, (tgtSel >= 0 && tgtSel <= NLANGS) ? tgtSel : 2, 0);
        SendMessageW(hGain, TBM_SETPOS, TRUE, (int)GetPrivateProfileIntW(L"app", L"gain", 10, g_ini.c_str()) - 10);
        SendMessageW(hNsStr, TBM_SETPOS, TRUE, GetPrivateProfileIntW(L"app", L"nsv", 50, g_ini.c_str()));
        SendMessageW(hNs, BM_SETCHECK, GetPrivateProfileIntW(L"app", L"ns", 1, g_ini.c_str()) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(hAgc, BM_SETCHECK, GetPrivateProfileIntW(L"app", L"agc", 1, g_ini.c_str()) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(hBoost, BM_SETCHECK, GetPrivateProfileIntW(L"app", L"boost", 0, g_ini.c_str()) ? BST_CHECKED : BST_UNCHECKED, 0);
        UpdateFx(false);
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
        return 0;
    case WM_SIZE: Layout(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_GETMINMAXINFO: { auto* m = (MINMAXINFO*)lp; m->ptMinTrackSize = {S(860), S(600)}; return 0; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_START:
            if (g_capturing) {
                StopCapture();
                SetStatus(L"Stopped.");
            } else {
                int sel = (int)SendMessageW(hMic, CB_GETCURSEL, 0, 0);
                UINT dev = (UINT)SendMessageW(hMic, CB_GETITEMDATA, sel, 0);
                if (StartCapture(dev)) SetStatus(L"Listening... speak now.");
                else SetStatus(L"Could not open the microphone.");
            }
            UpdateUi();
            return 0;
        case ID_CLEAR:
            g_committed.clear(); g_partial.clear(); g_trCommitted.clear(); g_trPartial.clear();
            RefreshText(); RefreshTr();
            return 0;
        case ID_SAVE: SaveDocuments(hw); return 0;
        case ID_NS: case ID_AGC: case ID_BOOST: UpdateFx(true); return 0;
        case ID_COPY: case ID_COPYTR: {
            std::wstring t = LOWORD(wp) == ID_COPY ? g_committed + g_partial : g_trCommitted + g_trPartial;
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
                g_trPartial.clear(); RefreshTr();
                SendLanguage(); SaveSettings();
            } else SetStatus(L"Swap works when both languages are concrete (not \"Auto\" and not \"No translation\").");
            return 0;
        }
        case ID_LANG: case ID_TGT: case ID_MIC:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                if (LOWORD(wp) != ID_MIC) { g_trPartial.clear(); RefreshTr(); SendLanguage(); }
                SaveSettings();
            }
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == hEdit) { SetBkColor((HDC)wp, RGB(255, 255, 255)); return (LRESULT)GetStockObject(WHITE_BRUSH); }
        if ((HWND)lp == hEditTr) { SetBkColor((HDC)wp, RGB(238, 245, 255)); return (LRESULT)hTrBrush; }
        break;
    case WM_LEVEL: {
        SendMessageW(hLevel, PBM_SETPOS, wp, 0);
        static int lastSpeaking = -1;
        if (g_capturing && (int)lp != lastSpeaking) {   // live feedback: is the speech detector hearing you?
            lastSpeaking = (int)lp;
            SetStatus(lp ? L"Hearing speech..." : L"Listening... speak now.");
        }
        return 0;
    }
    case WM_WORKER: {
        std::unique_ptr<WorkerMsg> m((WorkerMsg*)lp);
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
        case 'P': g_partial = m->text; RefreshText(); break;
        case 'F':
            if (!m->text.empty()) g_committed += m->text + L"\r\n";
            g_partial.clear();
            RefreshText();
            break;
        case 'T': g_trPartial = m->text; RefreshTr(); break;
        case 'U':
            if (!m->text.empty()) g_trCommitted += m->text + L"\r\n";
            g_trPartial.clear();
            RefreshTr();
            break;
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
    // WS_EX_TOPMOST: always above other windows; it leaves only when minimized with the "–" button
    HWND w = CreateWindowExW(WS_EX_TOPMOST, L"VoiceWindow", L"Voice Recognition and Translation", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(860, dpi, 96), MulDiv(860, dpi, 96),
                             nullptr, nullptr, hInst, nullptr);
    ShowWindow(w, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (g_reader.joinable()) g_reader.detach();
    return 0;
}
