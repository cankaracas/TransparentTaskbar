#pragma once

#include <windows.h>
#include <guiddef.h>

// Unique COM CLSID for our XAML Diagnostics TAP inside explorer.exe
// {6F2B9E41-8A3D-4C7E-9B12-4E7D5C8A3F19}
static constexpr CLSID CLSID_TransparentTaskbarTap = {
    0x6f2b9e41, 0x8a3d, 0x4c7e, {0x9b, 0x12, 0x4e, 0x7d, 0x5c, 0x8a, 0x3f, 0x19}
};

// Registered window message for UI-thread IPC when Shell_TrayWnd is directly reachable
static constexpr LPCWSTR kTaskbarCmdMsgName = L"TransparentTaskbar_ControlMsg_v1";

// Cross-desktop named Win32 events (same session, works from any desktop/terminal)
static constexpr LPCWSTR kEvtAliveV3 = L"Local\\TransparentTaskbar_Alive_v3";
static constexpr LPCWSTR kEvtApply   = L"Local\\TransparentTaskbar_Evt_Apply";
static constexpr LPCWSTR kEvtRestore = L"Local\\TransparentTaskbar_Evt_Restore";
static constexpr LPCWSTR kEvtRefresh = L"Local\\TransparentTaskbar_Evt_Refresh";
static constexpr LPCWSTR kEvtDump    = L"Local\\TransparentTaskbar_Evt_Dump";

enum TaskbarCmd : WPARAM {
    TB_CMD_PING    = 1,
    TB_CMD_APPLY   = 2,
    TB_CMD_RESTORE = 3,
    TB_CMD_REFRESH = 4,
    TB_CMD_DUMP    = 5,
};

static constexpr LRESULT TB_PING_MAGIC_V1 = 0x54424F4B; // "TBOK"
static constexpr LRESULT TB_PING_MAGIC    = 0x54423303; // "TB3\x03"
