// VoiceRecognitionSetup.exe - one-file installer for VoiceWindow.
//   * asks for the install folder
//   * unpacks VoiceWindow.exe and the Python scripts (embedded as resources)
//   * finds Python 3.10+ or downloads Python 3.12 from python.org into <folder>\python
//   * creates venv, installs the packages (CUDA libraries only with an NVIDIA GPU)
//   * downloads the models, creates a desktop shortcut and offers to start the program
// No compiler, winget or anything else is needed on the target machine.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <urlmon.h>
#include <cstdio>
#include <string>
#include <vector>

static const wchar_t* PY_ZIP_URL = L"https://www.nuget.org/api/v2/package/python/3.12.10";
static const wchar_t* PY_URL = L"https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe";

// ---------------------------------------------------------------- console output
static void Out(const std::wstring& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD w;
    WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &w, nullptr);
}
static void Line(const std::wstring& s = L"") { Out(s + L"\r\n"); }
static void Step(const std::wstring& s) { Line(); Line(L"===== " + s + L" ====="); }

static void WaitEnter() {
    Line();
    Out(L"Press Enter to close this window...");
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(in);
    wchar_t b[16];
    DWORD r;
    ReadConsoleW(in, b, 16, &r, nullptr);
}

[[noreturn]] static void Fail(const std::wstring& s) {
    Line();
    Line(L"ERROR: " + s);
    Line(L"The installation is not finished. Run the installer again - the steps that are already done are skipped.");
    WaitEnter();
    ExitProcess(1);
}

static bool Ask(const std::wstring& q) {
    Out(q + L" [Y/n] ");
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(in);
    wchar_t b[16] = {};
    DWORD r = 0;
    ReadConsoleW(in, b, 15, &r, nullptr);
    wchar_t c = b[0];
    return !(c == L'n' || c == L'N' || c == L'\u043d' || c == L'\u041d');
}

// ---------------------------------------------------------------- helpers
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::wstring Env(const wchar_t* name) {
    wchar_t b[32768];
    DWORD n = GetEnvironmentVariableW(name, b, 32768);
    return (n && n < 32768) ? std::wstring(b, n) : L"";
}

// Runs a command in this console window and returns its exit code (-1 if it could not start).
static int Run(const std::wstring& cmdline, const std::wstring& cwd = L"") {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> b(cmdline.begin(), cmdline.end());
    b.push_back(0);
    if (!CreateProcessW(nullptr, b.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// Same, but silent (output discarded) - used for probing.
static int RunQuiet(const std::wstring& cmdline) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> b(cmdline.begin(), cmdline.end());
    b.push_back(0);
    BOOL ok = CreateProcessW(nullptr, b.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(nul);
    if (!ok) return -1;
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }

// ---------------------------------------------------------------- download with progress
class Progress : public IBindStatusCallback {
    ULONG last_ = 101;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) { *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG done, ULONG total, ULONG, LPCWSTR) override {
        if (total) {
            ULONG pct = (ULONG)((unsigned long long)done * 100 / total);
            if (pct != last_) {
                last_ = pct;
                Out(L"\r  " + std::to_wstring(pct) + L"%  (" + std::to_wstring(done / 1048576) + L" / " +
                    std::to_wstring(total / 1048576) + L" MB)   ");
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { Line(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD*, BINDINFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

static bool Download(const std::wstring& url, const std::wstring& dest) {
    Progress p;
    return SUCCEEDED(URLDownloadToFileW(nullptr, url.c_str(), dest.c_str(), 0, &p)) && Exists(dest);
}

// ---------------------------------------------------------------- embedded files
static bool Extract(const wchar_t* resName, const std::wstring& dest) {
    HRSRC r = FindResourceW(nullptr, resName, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    DWORD size = SizeofResource(nullptr, r);
    const void* data = LockResource(g);
    if (!data) return false;
    HANDLE f = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data, size, &w, nullptr);
    CloseHandle(f);
    return ok && w == size;
}

// ---------------------------------------------------------------- folder picker
static std::wstring PickFolder() {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Where to install Voice Recognition? (a folder named VoiceRecognition is created)");
    dlg->SetOkButtonLabel(L"Install here");
    if (SUCCEEDED(dlg->Show(GetConsoleWindow()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

// ---------------------------------------------------------------- desktop shortcut
static void MakeShortcut(const std::wstring& target, const std::wstring& workDir) {
    PWSTR desk = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) return;
    std::wstring lnk = std::wstring(desk) + L"\\Voice Recognition.lnk";
    CoTaskMemFree(desk);
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetDescription(L"Live speech recognition and translation");
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        pf->Save(lnk.c_str(), TRUE);
        pf->Release();
        Line(L"Desktop shortcut: Voice Recognition");
    }
    sl->Release();
}

// ---------------------------------------------------------------- Python
// A usable Python: 3.10 - 3.13 (wheels exist for these) with the modules the setup relies on.
static bool PythonWorks(const std::wstring& exe) {
    return RunQuiet(Q(exe) + L" -c \"import sys, ssl, venv, ensurepip, sqlite3; sys.exit(0 if (3, 10) <= sys.version_info[:2] <= (3, 13) else 1)\"") == 0;
}

static std::wstring FindPython(const std::wstring& dir) {
    std::vector<std::wstring> c;
    c.push_back(dir + L"\\python-pkg\\tools\\python.exe");   // our private copy from an earlier run
    c.push_back(dir + L"\\python\\python.exe");              // older installer versions
    std::wstring la = Env(L"LOCALAPPDATA"), pf = Env(L"ProgramFiles");
    for (const wchar_t* v : {L"313", L"312", L"311", L"310"}) {
        if (!la.empty()) c.push_back(la + L"\\Programs\\Python\\Python" + v + L"\\python.exe");
        if (!pf.empty()) c.push_back(pf + L"\\Python" + v + L"\\python.exe");
    }
    // registered installations (any location), per user and per machine
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
        for (const wchar_t* v : {L"3.13", L"3.12", L"3.11", L"3.10"}) {
            std::wstring key = std::wstring(L"Software\\Python\\PythonCore\\") + v + L"\\InstallPath";
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD n = sizeof buf - 2;
            if (RegGetValueW(root, key.c_str(), L"ExecutablePath", RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS && buf[0])
                c.push_back(buf);
        }
    for (auto& e : c)
        if (Exists(e) && PythonWorks(e)) return e;
    if (PythonWorks(L"python")) return L"python";   // from PATH (the Microsoft Store stub fails this check)
    return L"";
}

// Python is not "installed" with the python.org setup (it conflicts with existing registrations of the same
// version and silently does nothing). Instead the official CPython build is unpacked from its NuGet package:
// a plain zip with the complete interpreter (venv, pip, ssl, sqlite). No registry, no admin rights, no conflicts.
static std::wstring InstallPrivatePython(const std::wstring& dir) {
    std::wstring stage = dir + L"\\python-pkg";
    std::wstring py = stage + L"\\tools\\python.exe";
    std::wstring zip = Env(L"TEMP") + L"\\voicerecogn-python.zip";
    std::wstring tar = Env(L"SystemRoot") + L"\\System32\\tar.exe";
    if (Exists(tar)) {
        Line(L"Downloading Python 3.12 (about 15 MB)...");
        if (Download(PY_ZIP_URL, zip)) {
            Line(L"Unpacking...");
            CreateDirectoryW(stage.c_str(), nullptr);
            int rc = Run(Q(tar) + L" -xf " + Q(zip) + L" -C " + Q(stage));
            DeleteFileW(zip.c_str());
            if (rc == 0 && PythonWorks(py)) return py;
            Line(L"The unpacked Python does not start (code " + std::to_wstring(rc) + L"). Trying the python.org installer instead.");
        } else {
            Line(L"Download failed. Trying the python.org installer instead.");
        }
    }
    // fallback: the regular python.org installer (per-user), then look again for whatever got registered
    std::wstring tmp = Env(L"TEMP") + L"\\voicerecogn-python-setup.exe";
    Line(L"Downloading the Python installer from python.org (about 25 MB)...");
    if (!Download(PY_URL, tmp)) { Line(L"Could not download Python. Check your internet connection."); return L""; }
    std::wstring plog = dir + L"\\python-setup.log";
    Line(L"Installing Python for the current user...");
    int rc = Run(Q(tmp) + L" /quiet /log " + Q(plog) + L" InstallAllUsers=0 PrependPath=0 Include_launcher=0 Include_test=0"
                          L" Include_doc=0 Include_tcltk=0 Shortcuts=0 AssociateFiles=0");
    DeleteFileW(tmp.c_str());
    std::wstring found = FindPython(dir);
    if (found.empty()) Line(L"Python setup exit code " + std::to_wstring(rc) + L". Log: " + plog);
    return found;
}

// Rough free-space check: the models and CUDA libraries need several GB.
static void CheckDisk(const std::wstring& dir) {
    ULARGE_INTEGER freeB{};
    if (GetDiskFreeSpaceExW(dir.c_str(), &freeB, nullptr, nullptr)) {
        unsigned long long gb = freeB.QuadPart >> 30;
        Line(L"Free space on the target drive: " + std::to_wstring(gb) + L" GB (about 8 GB are needed).");
        if (gb < 8) Fail(L"not enough free disk space on the target drive. Choose another folder and run the installer again.");
    }
}

static bool HasNvidia() {
    if (Exists(Env(L"SystemRoot") + L"\\System32\\nvidia-smi.exe")) return true;
    return RunQuiet(L"nvidia-smi -L") == 0;
}

// ---------------------------------------------------------------- main
int wmain() {
    SetConsoleTitleW(L"Voice Recognition - setup");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Line(L"Voice Recognition and Translation - setup");
    Line(L"A folder picker opens now.");

    // ---- 1. folder
    std::wstring parent = PickFolder();
    if (parent.empty()) Fail(L"no folder was chosen.");
    while (!parent.empty() && (parent.back() == L'\\' || parent.back() == L'/')) parent.pop_back();
    std::wstring dir = parent;
    {
        std::wstring tail = dir.substr(dir.find_last_of(L"\\/") + 1);
        if (lstrcmpiW(tail.c_str(), L"VoiceRecognition") != 0) dir += L"\\VoiceRecognition";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!Exists(dir)) Fail(L"could not create the folder " + dir);
    Line(L"Install folder: " + dir);
    if (!Exists(dir + L"\\venv\\Scripts\\python.exe")) CheckDisk(dir);

    // ---- 2. program files
    Step(L"1/5  Program files");
    struct { const wchar_t* res; const wchar_t* name; } files[] = {
        {L"F_EXE", L"VoiceWindow.exe"},       {L"F_WORKER", L"worker.py"},
        {L"F_DLPROG", L"dl_progress.py"},     {L"F_REQ", L"requirements.txt"},
        {L"F_MODELS", L"install_models.py"},  {L"F_README", L"README.md"},
    };
    for (auto& f : files) {
        if (!Extract(f.res, dir + L"\\" + f.name)) Fail(std::wstring(L"could not write ") + f.name +
                                                        L" (is the program running? close VoiceWindow.exe)");
        Line(std::wstring(L"  ") + f.name);
    }

    // ---- 3. Python
    Step(L"2/5  Python");
    std::wstring venvPy = dir + L"\\venv\\Scripts\\python.exe";
    if (Exists(venvPy) && PythonWorks(venvPy)) {
        Line(L"The Python environment already exists, skipping.");
    } else {
        std::wstring py = FindPython(dir);
        if (py.empty()) {
            Line(L"No suitable Python (3.10+ with venv, ssl and pip) found on this PC.");
            py = InstallPrivatePython(dir);
            if (py.empty()) {
                Line(L"Alternative: install Python 3.12 from https://www.python.org/downloads/ yourself, then run this installer again.");
                Fail(L"could not set up Python.");
            }
        }
        Line(L"Python: " + py);
        Line(L"Creating the venv environment...");
        if (Run(Q(py) + L" -m venv " + Q(dir + L"\\venv")) != 0 || !Exists(venvPy)) Fail(L"could not create the venv.");
    }

    // ---- 4. packages
    Step(L"3/5  Python packages");
    bool gpu = HasNvidia();
    Line(gpu ? L"NVIDIA video card found - installing everything, including the CUDA libraries."
             : L"No NVIDIA video card found - installing without the CUDA libraries (runs on the processor).");
    Run(Q(venvPy) + L" -m pip install --disable-pip-version-check --upgrade pip");
    {
        // requirements.txt without the nvidia-* lines when there is no NVIDIA GPU
        std::wstring req = dir + L"\\requirements.txt";
        if (!gpu) {
            std::wstring tmpReq = dir + L"\\requirements-cpu.txt";
            FILE* in = _wfopen(req.c_str(), L"rb");
            FILE* out = _wfopen(tmpReq.c_str(), L"wb");
            if (in && out) {
                char buf[1024];
                while (fgets(buf, sizeof buf, in)) {
                    std::string l = buf;
                    std::string low = l;
                    for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
                    if (low.find("nvidia") == 0) continue;
                    fputs(l.c_str(), out);
                }
            }
            if (in) fclose(in);
            if (out) fclose(out);
            req = tmpReq;
        }
        if (Run(Q(venvPy) + L" -m pip install --disable-pip-version-check -r " + Q(req)) != 0)
            Fail(L"pip could not install the packages (see the messages above).");
    }

    // ---- 5. models
    Step(L"4/5  Models");
    Line(gpu ? L"Recognition: Whisper large-v3 (~3 GB), translation: NLLB-200 (~1.4 GB)."
             : L"Recognition: Whisper small (~0.5 GB), translation: NLLB-200 (~1.4 GB).");
    if (Run(Q(venvPy) + L" -u " + Q(dir + L"\\install_models.py") + (gpu ? L" --gpu" : L""), dir) != 0)
        Fail(L"could not download the models (see the messages above).");

    // ---- 6. shortcut + start
    Step(L"5/5  Done");
    MakeShortcut(dir + L"\\VoiceWindow.exe", dir);
    Line(L"The program is installed in " + dir);
    Line();
    if (Ask(L"Start Voice Recognition now?"))
        ShellExecuteW(nullptr, L"open", (dir + L"\\VoiceWindow.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    CoUninitialize();
    return 0;
}
