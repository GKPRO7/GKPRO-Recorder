// gkinject32.exe <pid> <dll path>  -  injects the 32-bit hook into a 32-bit game.
#include <windows.h>
#include <shellapi.h>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) return 2;
    DWORD pid = wcstoul(argv[1], nullptr, 10);
    std::wstring dll = argv[2];
    LocalFree(argv);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return 3;
    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int rc = 4;
    if (mem && WriteProcessMemory(proc, mem, dll.c_str(), bytes, nullptr)) {
        auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, ll, mem, 0, nullptr);
        if (th) {
            DWORD ec = 0;
            if (WaitForSingleObject(th, 8000) == WAIT_OBJECT_0 && GetExitCodeThread(th, &ec) && ec != 0) rc = 0;
            CloseHandle(th);
        }
    }
    if (mem) VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return rc;
}
