// tb_loader.exe (v3) — One-shot silent launcher
// Injects TransparentTaskbar.dll into explorer.exe via Windows.UI.Xaml.dll::InitializeXamlDiagnosticsEx
// and immediately exits (0 persistent background processes).
// Works both from the interactive desktop and from cross-desktop terminals in the same session.

#include <windows.h>
#include <tlhelp32.h>
#include <xamlom.h>
#include <string>
#include "common.h"

namespace {

using FnInitializeXamlDiagnosticsEx = HRESULT(WINAPI*)(
    LPCWSTR endPointName,
    DWORD pid,
    LPCWSTR wszDllXamlDiagnostics,
    LPCWSTR wszTAPDllName,
    CLSID tapClsid,
    LPCWSTR wszInitializationData);

std::wstring GetSiblingDllPath() {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath, len);
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        path.resize(pos + 1);
    }
    return path + L"TransparentTaskbar.dll";
}

DWORD FindExplorerPidInCurrentSession() {
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD foundPid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                DWORD ses = 0;
                if (ProcessIdToSessionId(pe.th32ProcessID, &ses) && ses == mySession) {
                    foundPid = pe.th32ProcessID;
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return foundPid;
}

bool SendControlCmd(HWND trayHwnd, UINT msg, TaskbarCmd cmd, DWORD_PTR* outResult = nullptr) {
    if (!trayHwnd) return false;
    DWORD_PTR res = 0;
    LRESULT ok = SendMessageTimeoutW(
        trayHwnd, msg, static_cast<WPARAM>(cmd), 0,
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &res);
    if (outResult) *outResult = res;
    return ok != 0;
}

bool IsNamedAliveEventSignaled() {
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, kEvtAliveV3);
    if (!h) return false;
    bool signaled = (WaitForSingleObject(h, 0) == WAIT_OBJECT_0);
    CloseHandle(h);
    return signaled;
}

bool SignalNamedEvent(LPCWSTR name) {
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!h) return false;
    BOOL ok = SetEvent(h);
    CloseHandle(h);
    return ok != FALSE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR lpCmdLine, int) {
    std::wstring args = lpCmdLine ? lpCmdLine : L"";

    bool isRestore = (args.find(L"--restore") != std::wstring::npos);
    bool isRefresh = (args.find(L"--refresh") != std::wstring::npos);
    bool isDump    = (args.find(L"--dump") != std::wstring::npos);
    bool isStatus  = (args.find(L"--status") != std::wstring::npos);

    UINT cmdMsg = RegisterWindowMessageW(kTaskbarCmdMsgName);
    HWND trayHwnd = FindWindowW(L"Shell_TrayWnd", nullptr);

    // If on interactive desktop at logon, wait briefly for Shell_TrayWnd if explorer is still starting
    if (!trayHwnd && !isRestore) {
        DWORD explorerPidNow = FindExplorerPidInCurrentSession();
        if (!explorerPidNow) {
            for (int i = 0; i < 40; ++i) {
                Sleep(250);
                explorerPidNow = FindExplorerPidInCurrentSession();
                if (explorerPidNow) break;
            }
        }
        // Wait briefly for Shell_TrayWnd in case we are on the interactive desktop
        for (int i = 0; i < 8; ++i) {
            trayHwnd = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (trayHwnd) break;
            Sleep(150);
        }
    }

    DWORD_PTR pingRes = 0;
    bool pingOk = SendControlCmd(trayHwnd, cmdMsg, TB_CMD_PING, &pingRes);
    bool loadedV3 = (pingOk && pingRes == static_cast<DWORD_PTR>(TB_PING_MAGIC)) || IsNamedAliveEventSignaled();
    bool loadedOld = pingOk && (pingRes == static_cast<DWORD_PTR>(TB_PING_MAGIC_V1));

    if (isStatus) {
        if (loadedV3) return 0;
        if (loadedOld) return 10;
        return 1;
    }

    if (isRestore) {
        if (loadedV3) {
            if (!SendControlCmd(trayHwnd, cmdMsg, TB_CMD_RESTORE)) {
                SignalNamedEvent(kEvtRestore);
            }
        } else if (loadedOld) {
            SendControlCmd(trayHwnd, cmdMsg, TB_CMD_RESTORE);
        } else if (trayHwnd) {
            SetWindowRgn(trayHwnd, nullptr, TRUE);
        }
        return 0;
    }

    if (loadedOld) {
        return 10;
    }

    if (isDump && loadedV3) {
        if (!SendControlCmd(trayHwnd, cmdMsg, TB_CMD_DUMP)) {
            SignalNamedEvent(kEvtDump);
        }
        return 0;
    }

    if (loadedV3) {
        TaskbarCmd cmd = isRefresh ? TB_CMD_REFRESH : TB_CMD_APPLY;
        if (!SendControlCmd(trayHwnd, cmdMsg, cmd)) {
            SignalNamedEvent(isRefresh ? kEvtRefresh : kEvtApply);
        }
        return 0;
    }

    DWORD explorerPid = 0;
    if (trayHwnd) {
        GetWindowThreadProcessId(trayHwnd, &explorerPid);
    }
    if (!explorerPid) {
        explorerPid = FindExplorerPidInCurrentSession();
    }
    if (!explorerPid) {
        return 2; // explorer.exe not found in current session
    }

    HMODULE hXaml = LoadLibraryW(L"Windows.UI.Xaml.dll");
    if (!hXaml) {
        return 3;
    }
    auto fnInitDiag = reinterpret_cast<FnInitializeXamlDiagnosticsEx>(
        GetProcAddress(hXaml, "InitializeXamlDiagnosticsEx"));
    if (!fnInitDiag) {
        FreeLibrary(hXaml);
        return 4;
    }

    std::wstring dllPath = GetSiblingDllPath();
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FreeLibrary(hXaml);
        return 6;
    }

    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 12; ++attempt) {
        for (int i = 1; i <= 8; ++i) {
            std::wstring endpoint = L"VisualDiagConnection" + std::to_wstring(i);
            hr = fnInitDiag(
                endpoint.c_str(),
                explorerPid,
                L"",
                dllPath.c_str(),
                CLSID_TransparentTaskbarTap,
                L"");
            if (SUCCEEDED(hr)) break;
        }
        if (SUCCEEDED(hr)) break;
        Sleep(500);
    }

    FreeLibrary(hXaml);

    if (FAILED(hr)) {
        return static_cast<int>(hr);
    }

    if (isDump) {
        Sleep(500);
        if (!SendControlCmd(trayHwnd, cmdMsg, TB_CMD_DUMP)) {
            SignalNamedEvent(kEvtDump);
        }
    }

    return 0;
}
