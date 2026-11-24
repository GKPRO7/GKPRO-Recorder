#include "hook_capture.h"

namespace {
bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool InjectLoadLibrary(HANDLE proc, const std::wstring& dll) {
    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return false;
    bool ok = false;
    if (WriteProcessMemory(proc, mem, dll.c_str(), bytes, nullptr)) {
        auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, ll, mem, 0, nullptr);
        if (th) {
            DWORD ec = 0;
            ok = WaitForSingleObject(th, 8000) == WAIT_OBJECT_0 && GetExitCodeThread(th, &ec) && ec != 0;
            CloseHandle(th);
        }
    }
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    return ok;
}
}  // namespace

bool HookCapture::Start(DWORD pid, int fps, const std::wstring& dir, ID3D11Device* dev, const std::wstring& hookLogPath, std::string& err) {
    m_dev = dev;
    m_proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                         PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!m_proc) {
        err = "cannot open the game process (if the game runs as administrator, run GKPRO Recorder as administrator too)";
        return false;
    }

    wchar_t name[64];
    GkHookShmName(pid, name);
    m_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GkHookShared), name);
    if (!m_map) { err = "cannot create shared memory"; Stop(); return false; }
    m_shm = (GkHookShared*)MapViewOfFile(m_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GkHookShared));
    if (!m_shm) { err = "cannot map shared memory"; Stop(); return false; }

    const uint64_t baseFrames = m_shm->frameCounter;  // the hook may already be loaded from an earlier recording
    wcsncpy_s(m_shm->logPath, hookLogPath.c_str(), _TRUNCATE);
    m_shm->detected = 0;
    m_shm->magic = GKHOOK_MAGIC;
    m_shm->targetFps = (uint32_t)(fps > 0 ? fps : 30);
    m_shm->wantFrames = 1;

    BOOL wow = FALSE;
    IsWow64Process(m_proc, &wow);  // TRUE = a 32-bit game on 64-bit Windows
    if (!wow) {
        std::wstring dll = dir + L"\\gkhook64.dll";
        if (!FileExists(dll)) { err = "gkhook64.dll is missing next to GKPRORecorder.exe"; Stop(); return false; }
        if (!InjectLoadLibrary(m_proc, dll)) { err = "could not inject the hook (blocked by the game or by anti-cheat)"; Stop(); return false; }
    } else {
        std::wstring helper = dir + L"\\gkinject32.exe";
        std::wstring dll = dir + L"\\gkhook32.dll";
        if (!FileExists(helper) || !FileExists(dll)) { err = "gkinject32.exe / gkhook32.dll are missing next to GKPRORecorder.exe"; Stop(); return false; }
        std::wstring cmd = L"\"" + helper + L"\" " + std::to_wstring(pid) + L" \"" + dll + L"\"";
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            err = "could not start gkinject32.exe";
            Stop();
            return false;
        }
        DWORD ec = 1;
        WaitForSingleObject(pi.hProcess, 10000);
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (ec != 0) { err = "could not inject the 32-bit hook (blocked by the game or by anti-cheat)"; Stop(); return false; }
    }

    // Wait for the game to present frames through a supported API.
    for (int i = 0; i < 160; ++i) {  // ~4 s
        Update();
        if (m_tex && m_shm->frameCounter > baseFrames) return true;
        if (!Alive()) break;
        Sleep(25);
    }
    uint32_t det = m_shm ? m_shm->detected : 0;
    std::string what;
    if (det & 1) what += "Vulkan (including DXVK), ";
    if (det & 2) what += "DirectX 12, ";
    if (det & 4) what += "DirectX 8, ";
    if (det & 8) what += "DirectDraw (DirectX 7 and older), ";
    if (!what.empty()) {
        what.resize(what.size() - 2);
        err = "this game loads " + what + ", which game capture does not support yet";
    } else {
        err = "no frames came from the game (the game may block the hook, or uses an unsupported graphics API)";
    }
    Stop();
    return false;
}

void HookCapture::Stop() {
    if (m_locked) Unlock();
    if (m_shm) {
        m_shm->wantFrames = 0;
        UnmapViewOfFile(m_shm);
        m_shm = nullptr;
    }
    m_km.Reset();
    m_tex.Reset();
    if (m_map) { CloseHandle(m_map); m_map = nullptr; }
    if (m_proc) { CloseHandle(m_proc); m_proc = nullptr; }
}

bool HookCapture::Update() {
    if (!m_shm || !m_dev) return false;
    uint32_t g = m_shm->generation;
    if (g == m_gen && m_tex) return false;
    uint32_t h = m_shm->sharedHandle;
    if (!h) return false;
    m_gen = g;  // even on failure, so we do not retry every tick
    Microsoft::WRL::ComPtr<ID3D11Texture2D> t;
    if (FAILED(m_dev->OpenSharedResource((HANDLE)(uintptr_t)h, IID_PPV_ARGS(&t)))) return false;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    m_km.Reset();
    if (m_shm->hasKeyedMutex) t.As(&m_km);
    m_tex = t;
    m_w = d.Width;
    m_h = d.Height;
    return true;
}

bool HookCapture::Alive() const {
    return m_proc && WaitForSingleObject(m_proc, 0) == WAIT_TIMEOUT;
}

bool HookCapture::Lock(DWORD ms) {
    if (!m_km) return true;
    if (m_km->AcquireSync(0, ms) == S_OK) { m_locked = true; return true; }
    return false;
}

void HookCapture::Unlock() {
    if (m_km && m_locked) m_km->ReleaseSync(0);
    m_locked = false;
}
