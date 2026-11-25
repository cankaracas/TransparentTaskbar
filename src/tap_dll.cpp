// TransparentTaskbar.dll (v3) — Native C++ In-Process XAML TAP for Windows 11 Taskbar
// Zero IAT/inline hooks (100% CFG/XFG safe), zero persistent .exe, event-driven, ~0% idle CPU.
// Architecture:
//  - Uses native Windows ABS_AUTOHIDE via SHAppBarMessage(ABM_SETSTATE) so Windows itself
//    expands WorkArea to full monitor height (1536x960) with zero SPI_SETWORKAREA fighting.
//  - Keeps Shell_TrayWnd clamped visible at the bottom edge (except during true D3D/exclusive
//    fullscreen apps) and ensures XAML TaskbarFrame stays untranslated and visible.
//  - Collapses BackgroundFill/BackgroundStroke + clears DWM accent for 100% transparency.
//  - Applies diff-cached SetWindowRgn around icon/tray clusters so empty space clicks through.

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <unknwn.h>
#include <ocidl.h>
#include <commctrl.h>
#include <xamlom.h>

#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"

namespace xaml = winrt::Windows::UI::Xaml;
namespace media = winrt::Windows::UI::Xaml::Media;
namespace controls = winrt::Windows::UI::Xaml::Controls;
namespace primitives = winrt::Windows::UI::Xaml::Controls::Primitives;
namespace comp = winrt::Windows::UI::Composition;

namespace {

HMODULE g_module = nullptr;
std::atomic<LONG> g_refCount{0};
UINT g_cmdMsg = 0;

constexpr UINT_PTR kSubclassId = 0x54420005;
constexpr UINT_PTR kBridgeSubclassId = 0x54420006;
constexpr UINT_PTR kTimerRefreshRegion = 0x54420101;
constexpr UINT_PTR kTimerKeepVisible   = 0x54420103;
constexpr UINT_PTR kTimerHoverCheck    = 0x54420104;
constexpr UINT_PTR kTimerHideFallback  = 0x54420105;
constexpr UINT_PTR kTimerInitialHide   = 0x54420106;
constexpr UINT_PTR kTrayUITimerHide    = 2;
constexpr UINT_PTR kTrayUITimerUnhide  = 3;
constexpr UINT kWmXamlReapply        = WM_APP + 0x310;
constexpr UINT kWmXamlRestore        = WM_APP + 0x311;
constexpr UINT kWmXamlDump           = WM_APP + 0x312;
constexpr UINT kWmXamlProcessPending = WM_APP + 0x313;
constexpr UINT kWmInitAppBar         = WM_APP + 0x314;
constexpr UINT kWmKeepIconsVisible   = WM_APP + 0x315;
constexpr UINT kWmHideAnimDone       = WM_APP + 0x316;
constexpr UINT kWmForegroundChanged  = WM_APP + 0x317;
constexpr UINT kWmEdgeHover          = WM_APP + 0x318;
constexpr WPARAM kBootstrapMagic     = 0x54424253; // "TBBS"

// Lock-free atomics for WH_MOUSE_LL bottom-edge hover detection (zero syscalls/mutexes in hook)
std::atomic<bool> g_anyTrayHidden{false};
std::atomic<bool> g_fullscreenSuppressed{false};
std::atomic<bool> g_edgeHoverQueued{false};
std::atomic<HWND> g_primaryTrayHwnd{nullptr};
std::atomic<LONG> g_monLeft{0};
std::atomic<LONG> g_monRight{0};
std::atomic<LONG> g_monBottom{0};

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE &&
        g_anyTrayHidden.load(std::memory_order_relaxed) &&
        !g_fullscreenSuppressed.load(std::memory_order_relaxed)) {
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        LONG b = g_monBottom.load(std::memory_order_relaxed);
        if (b > 0 && ms->pt.y >= b - 3 && ms->pt.y <= b &&
            ms->pt.x >= g_monLeft.load(std::memory_order_relaxed) &&
            ms->pt.x < g_monRight.load(std::memory_order_relaxed)) {
            HWND tray = g_primaryTrayHwnd.load(std::memory_order_relaxed);
            if (tray) {
                bool expected = false;
                if (g_edgeHoverQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    if (!PostMessageW(tray, kWmEdgeHover, 0, 0)) {
                        g_edgeHoverQueued.store(false, std::memory_order_release);
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#endif
#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#endif
#ifndef WM_POINTERENTER
#define WM_POINTERENTER 0x0249
#endif

bool IsWindowCloaked(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

bool UncloakWindowIfNeeded(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsWindowCloaked(hwnd)) {
        BOOL uncloak = FALSE;
        DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &uncloak, sizeof(uncloak));
        return true;
    }
    return false;
}

// ============================================================================
// Native XAML MacBook Liquid Glass Brushes (Zero SWCA hacks on Shell_TrayWnd!)
// ============================================================================

media::AcrylicBrush MakeMacGlassBrush() {
    media::AcrylicBrush brush;
    brush.BackgroundSource(media::AcrylicBackgroundSource::HostBackdrop);
    brush.TintColor(winrt::Windows::UI::Color{0xFF, 0x24, 0x24, 0x2C});
    brush.TintOpacity(0.24);
    brush.TintLuminosityOpacity(0.22);
    brush.FallbackColor(winrt::Windows::UI::Color{0x44, 0x24, 0x24, 0x2C});
    return brush;
}

media::LinearGradientBrush MakeGlassRimBrush() {
    media::LinearGradientBrush rim;
    rim.StartPoint({0.0f, 0.0f});
    rim.EndPoint({1.0f, 1.0f});

    media::GradientStop s0;
    s0.Color(winrt::Windows::UI::Color{0x66, 0xFF, 0xFF, 0xFF});
    s0.Offset(0.0);
    media::GradientStop s1;
    s1.Color(winrt::Windows::UI::Color{0x1C, 0xFF, 0xFF, 0xFF});
    s1.Offset(0.5);
    media::GradientStop s2;
    s2.Color(winrt::Windows::UI::Color{0x40, 0xFF, 0xFF, 0xFF});
    s2.Offset(1.0);

    rim.GradientStops().Append(s0);
    rim.GradientStops().Append(s1);
    rim.GradientStops().Append(s2);
    return rim;
}

void SaveBmp24(const std::wstring& path, int w, int h, const BYTE* bgra) {
    if (!bgra || w <= 0 || h <= 0) return;
    int rowBytes = ((w * 3 + 3) / 4) * 4;
    DWORD imgSize = static_cast<DWORD>(rowBytes * h);

    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42; // "BM"
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + imgSize;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = w;
    bih.biHeight = h; // bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;

    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hf, &bfh, sizeof(bfh), &written, nullptr);
    WriteFile(hf, &bih, sizeof(bih), &written, nullptr);

    std::vector<BYTE> row(rowBytes, 0);
    for (int y = h - 1; y >= 0; --y) {
        const BYTE* src = bgra + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        WriteFile(hf, row.data(), rowBytes, &written, nullptr);
    }
    CloseHandle(hf);
}

bool RectVectorsEqual(const std::vector<RECT>& a, const std::vector<RECT>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!EqualRect(&a[i], &b[i])) return false;
    }
    return true;
}

bool IsShellFlyoutProcess(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    wchar_t path[MAX_PATH]{};
    DWORD len = MAX_PATH;
    bool match = false;
    if (QueryFullProcessImageNameW(hProc, 0, path, &len)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? (base + 1) : path;
        if (_wcsicmp(base, L"StartMenuExperienceHost.exe") == 0 ||
            _wcsicmp(base, L"SearchHost.exe") == 0 ||
            _wcsicmp(base, L"ShellExperienceHost.exe") == 0) {
            match = true;
        }
    }
    CloseHandle(hProc);
    return match;
}

bool IsShellClass(const wchar_t* cls) {
    return wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           wcscmp(cls, L"NotifyIconOverflowWindow") == 0 ||
           wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
           wcscmp(cls, L"TaskListThumbnailWnd") == 0 ||
           wcscmp(cls, L"Xaml_WindowedPopupClass") == 0 ||
           wcscmp(cls, L"#32768") == 0 ||
           wcscmp(cls, L"ControlCenterWindow") == 0;
}

bool IsShellFlyoutOrMenuOpen(HWND trayHwnd) {
    HWND fg = GetForegroundWindow();
    if (fg && IsWindowVisible(fg)) {
        if (fg == trayHwnd || IsChild(trayHwnd, fg)) return true;
        wchar_t cls[64]{};
        GetClassNameW(fg, cls, 64);
        if (wcscmp(cls, L"NotifyIconOverflowWindow") == 0 ||
            wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
            wcscmp(cls, L"TaskListThumbnailWnd") == 0 ||
            wcscmp(cls, L"Xaml_WindowedPopupClass") == 0 ||
            wcscmp(cls, L"#32768") == 0 ||
            wcscmp(cls, L"ControlCenterWindow") == 0 ||
            IsShellFlyoutProcess(fg)) {
            return true;
        }
    }

    auto isVisClass = [](const wchar_t* cls) {
        HWND w = FindWindowW(cls, nullptr);
        return w && IsWindowVisible(w);
    };
    if (isVisClass(L"TaskListThumbnailWnd") ||
        isVisClass(L"TopLevelWindowForOverflowXamlIsland") ||
        isVisClass(L"NotifyIconOverflowWindow")) {
        return true;
    }

    if (trayHwnd) {
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        DWORD tid = GetWindowThreadProcessId(trayHwnd, nullptr);
        if (tid && GetGUIThreadInfo(tid, &gti) && (gti.flags & GUI_INMENUMODE)) {
            return true;
        }
    }
    return false;
}

bool IsWindowAboveTray(HWND fg, HWND trayHwnd) {
    HWND cur = GetWindow(trayHwnd, GW_HWNDPREV);
    for (int i = 0; cur && i < 64; ++i) {
        if (cur == fg) return true;
        cur = GetWindow(cur, GW_HWNDPREV);
    }
    return false;
}

bool IsFullscreenCoveringTray(HWND trayHwnd) {
    QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&quns))) {
        if (quns == QUNS_RUNNING_D3D_FULL_SCREEN) {
            return true;
        }
    }

    HWND fg = GetForegroundWindow();
    if (!fg || fg == trayHwnd || !IsWindowVisible(fg)) return false;

    wchar_t cls[64]{};
    GetClassNameW(fg, cls, 64);
    if (IsShellClass(cls) || IsShellFlyoutProcess(fg)) {
        return false;
    }

    HMONITOR trayMon = MonitorFromWindow(trayHwnd, MONITOR_DEFAULTTONEAREST);
    HMONITOR fgMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (trayMon != fgMon) return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT rcFg{};
    if (!GetMonitorInfoW(trayMon, &mi) || !GetWindowRect(fg, &rcFg)) return false;

    bool coversMonitor = (rcFg.left <= mi.rcMonitor.left && rcFg.top <= mi.rcMonitor.top &&
                          rcFg.right >= mi.rcMonitor.right && rcFg.bottom >= mi.rcMonitor.bottom);
    if (!coversMonitor) return false;

    bool trayLostTopmost = (GetWindowLongPtrW(trayHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;
    if (trayLostTopmost || IsWindowAboveTray(fg, trayHwnd)) {
        return true;
    }

    LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
    if ((style & WS_CAPTION) == 0) {
        return true; // Borderless fullscreen / Chromium / VLC / games
    }

    // Firefox YouTube fullscreen keeps WS_CAPTION internally but is NOT zoomed and matches exact monitor rect
    if (!IsZoomed(fg) &&
        rcFg.left == mi.rcMonitor.left && rcFg.top == mi.rcMonitor.top &&
        rcFg.right == mi.rcMonitor.right && rcFg.bottom == mi.rcMonitor.bottom) {
        return true;
    }
    return false;
}

enum class VisState { Hidden, Revealing, Visible, Hiding };

struct TrayAnimState {
    VisState state{VisState::Visible};
    uint64_t animGen{0};
    ULONGLONG leaveStartTick{0};
    bool triggerRgnApplied{false};
    bool fullscreenEmptyRgn{false};
    bool animTargetInitialized{false};
    bool initialHideScheduled{false};
    winrt::weak_ref<xaml::FrameworkElement> animTarget;
};

// ============================================================================
// TaskbarController — Core In-Process Controller (v6)
// ============================================================================

struct SavedElementState {
    winrt::weak_ref<xaml::UIElement> element;
    xaml::Visibility origVisibility{xaml::Visibility::Visible};
    double origOpacity{1.0};
};

struct TrackedRoot {
    winrt::weak_ref<xaml::FrameworkElement> root;
    winrt::event_token sizeToken{};
    HWND trayHwnd{nullptr};
    HWND bridgeHwnd{nullptr};
};

class TaskbarController {
public:
    static TaskbarController& Instance() {
        static TaskbarController inst;
        return inst;
    }

    void InitializeFromSite(IUnknown* pSite) {
        winrt::com_ptr<IVisualTreeService3> vt;
        winrt::com_ptr<TreeCallback> cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_initialized) {
                m_active = true;
                m_lastGroups.clear();
                m_lastCenterPillSig.clear();
                m_trayPillStyled.clear();
            } else {
                if (!pSite) return;

                // Pin module inside explorer.exe so it stays alive after tb_loader.exe exits
                HMODULE pinned = nullptr;
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&TaskbarController::Instance),
                    &pinned);

                pSite->QueryInterface(IID_PPV_ARGS(m_xamlDiag.put()));
                pSite->QueryInterface(IID_PPV_ARGS(m_visualTree.put()));

                m_initialized = true;
                m_active = true;

                if (m_visualTree) {
                    m_callback = winrt::make_self<TreeCallback>();
                    vt = m_visualTree;
                    cb = m_callback;
                }
            }
        }

        SetupNamedControlEvents();
        if (m_evtAlive) {
            SetEvent(m_evtAlive);
        }

        // Subclass all tray windows synchronously on their own UI threads
        BootstrapSubclassAllTrays();

        // Subscribe to XAML visual tree (first time only)
        if (vt && cb) {
            vt->AdviseVisualTreeChange(cb.get());
        }

        // Initialize native ABS_AUTOHIDE on Shell_TrayWnd UI thread, then apply transparency + regions
        ScheduleAllTrays(kWmInitAppBar);
        ScheduleAllTrays(kWmXamlReapply);
    }

    void InitAppBarOnUiThread(HWND trayHwnd) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;

        HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (trayHwnd != primary) return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Capture normal taskbar height once (48px at 100% / 125% scaling etc.)
            if (m_normalTrayHeight <= 0) {
                RECT rc{};
                GetWindowRect(trayHwnd, &rc);
                int h = rc.bottom - rc.top;
                if (h >= 24 && h <= 120) {
                    m_normalTrayHeight = h;
                } else {
                    UINT dpi = GetDpiForWindow(trayHwnd);
                    m_normalTrayHeight = MulDiv(48, dpi ? static_cast<int>(dpi) : 96, 96);
                }
            }

            if (!m_appBarSaved) {
                APPBARDATA abd{};
                abd.cbSize = sizeof(abd);
                abd.hWnd = trayHwnd;
                m_origAppBarState = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
                m_appBarSaved = true;
            }
        }

        // Enable native ABS_AUTOHIDE | ABS_ALWAYSONTOP so Windows natively releases the 48px WorkArea!
        APPBARDATA abd{};
        abd.cbSize = sizeof(abd);
        abd.hWnd = trayHwnd;
        abd.lParam = ABS_AUTOHIDE | ABS_ALWAYSONTOP;
        SHAppBarMessage(ABM_SETSTATE, &abd);

        EnsureTrayClampedVisible(trayHwnd);
        KillTimer(trayHwnd, kTrayUITimerHide);
        PostMessageW(trayHwnd, WM_TIMER, kTrayUITimerUnhide, 0);
    }

    void OnVisualTreeMutation(InstanceHandle handle, LPCWSTR typeName, LPCWSTR elemName, VisualMutationType mutationType) {
        if (!typeName && !elemName) return;

        std::wstring typeStr = typeName ? typeName : L"";
        std::wstring nameStr = elemName ? elemName : L"";

        bool isRootCandidate =
            (mutationType == Add) &&
            ((typeStr.find(L"TaskbarFrame") != std::wstring::npos) ||
             (nameStr == L"TaskbarFrame") ||
             (nameStr == L"RootGrid"));

        bool isBgCandidate =
            (mutationType == Add) &&
            ((nameStr == L"BackgroundFill") ||
             (nameStr == L"BackgroundStroke") ||
             (nameStr == L"TaskbarBackground") ||
             (nameStr == L"ScreenEdgeStroke") ||
             (typeStr.find(L"TaskbarBackground") != std::wstring::npos));

        bool isInteractiveChange =
            (typeStr.find(L"TaskListButton") != std::wstring::npos) ||
            (typeStr.find(L"NotifyIconView") != std::wstring::npos) ||
            (typeStr.find(L"OmniButton") != std::wstring::npos) ||
            (typeStr.find(L"TaskbarExtensionElement") != std::wstring::npos);

        if (!isRootCandidate && !isBgCandidate && !isInteractiveChange) return;

        if (isRootCandidate) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pendingRootHandles.push_back(handle);
            }
            ScheduleAllTrays(kWmXamlProcessPending);
        } else {
            auto trays = EnumAllTrayWindows();
            for (HWND h : trays) {
                ScheduleRefreshDebounced(h, 150);
            }
        }
    }

    void ProcessPendingHandlesOnUiThread(HWND trayHwnd) {
        std::vector<InstanceHandle> handles;
        winrt::com_ptr<IXamlDiagnostics> diag;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            handles.swap(m_pendingRootHandles);
            diag = m_xamlDiag;
        }
        if (!diag) return;

        for (InstanceHandle handle : handles) {
            try {
                winrt::com_ptr<IInspectable> insp;
                if (FAILED(diag->GetIInspectableFromHandle(handle, insp.put())) || !insp) {
                    continue;
                }
                winrt::Windows::Foundation::IInspectable winrtInsp;
                winrt::copy_from_abi(winrtInsp, insp.get());
                if (auto fe = winrtInsp.try_as<xaml::FrameworkElement>()) {
                    RegisterPotentialRootOnUiThread(fe, trayHwnd);
                }
            } catch (...) {}
        }

        if (m_active.load()) {
            ApplyOnUiThread(trayHwnd);
        }
    }

    // Runs strictly on Shell_TrayWnd UI thread! Idempotent — zero redundant Win32/XAML mutations!
    void ApplyOnUiThread(HWND trayHwnd) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;

        UpdatePrimaryMonitorCache(trayHwnd);
        EnsureTrayClampedVisible(trayHwnd);

        xaml::FrameworkElement animTarget{nullptr};
        try {
            std::vector<xaml::FrameworkElement> roots = GetLiveRootsForTray(trayHwnd);
            for (auto& root : roots) {
                if (!animTarget) {
                    animTarget = FindOrInitAnimTarget(trayHwnd, root);
                }
                KeepVisualsVisibleRecursive(root, 0, animTarget);
                ApplyTransparencyRecursive(root, trayHwnd);
            }
            UpdateClickThroughAndPills(trayHwnd, roots);
        } catch (...) {}

        bool scheduleInitHide = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& st = m_trayStates[trayHwnd];
            if (!st.initialHideScheduled) {
                st.initialHideScheduled = true;
                scheduleInitHide = true;
            }
        }
        if (scheduleInitHide) {
            if (IsFullscreenCoveringTray(trayHwnd)) {
                HideTaskbarOnUiThread(trayHwnd, true, true);
            } else {
                SetTimer(trayHwnd, kTimerInitialHide, 650, nullptr);
            }
        }
    }

    void KeepIconsVisibleOnUiThread(HWND trayHwnd, bool fromTimer) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        if (IsFullscreenCoveringTray(trayHwnd)) return;

        EnsureTrayClampedVisible(trayHwnd);

        bool hadSlide = false;
        try {
            std::vector<xaml::FrameworkElement> roots = GetLiveRootsForTray(trayHwnd);
            xaml::FrameworkElement animTarget = GetAnimTarget(trayHwnd);
            for (auto& root : roots) {
                if (!animTarget) animTarget = FindOrInitAnimTarget(trayHwnd, root);
                if (KeepVisualsVisibleRecursive(root, 0, animTarget)) {
                    hadSlide = true;
                }
            }
        } catch (...) {}

        if (hadSlide && !fromTimer) {
            SetTimer(trayHwnd, kTimerKeepVisible, 180, nullptr);
        }
    }

    void RevealTaskbarOnUiThread(HWND trayHwnd, bool fromSystemTrigger) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        if (!fromSystemTrigger && IsFullscreenCoveringTray(trayHwnd)) return;

        KillTimer(trayHwnd, kTimerHideFallback);
        KillTimer(trayHwnd, kTimerInitialHide);

        EnsureTrayClampedVisible(trayHwnd);

        xaml::FrameworkElement animTarget = GetAnimTarget(trayHwnd);
        std::vector<xaml::FrameworkElement> roots;
        if (!animTarget) {
            roots = GetLiveRootsForTray(trayHwnd);
            for (auto& r : roots) {
                animTarget = FindOrInitAnimTarget(trayHwnd, r);
                if (animTarget) break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& st = m_trayStates[trayHwnd];
            st.animGen++;
            st.leaveStartTick = 0;
            st.state = VisState::Visible;
            st.triggerRgnApplied = false;
            st.fullscreenEmptyRgn = false;
        }
        g_anyTrayHidden.store(false, std::memory_order_release);
        g_edgeHoverQueued.store(false, std::memory_order_release);

        // Restore pill click-through region immediately so icons are clickable during reveal
        std::vector<RECT> cachedGroups;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_lastGroups.find(trayHwnd);
            if (it != m_lastGroups.end()) cachedGroups = it->second;
        }
        if (!cachedGroups.empty()) {
            ApplyRoundRgnToTray(trayHwnd, cachedGroups);
        } else {
            if (roots.empty()) roots = GetLiveRootsForTray(trayHwnd);
            UpdateClickThroughAndPills(trayHwnd, roots);
        }

        if (animTarget) {
            try {
                auto vis = xaml::Hosting::ElementCompositionPreview::GetElementVisual(animTarget);
                if (vis) {
                    auto c = vis.Compositor();
                    auto easeOut = c.CreateCubicBezierEasingFunction({0.16f, 1.0f}, {0.30f, 1.0f});

                    auto slideAnim = c.CreateVector3KeyFrameAnimation();
                    slideAnim.InsertKeyFrame(1.0f, {0.0f, 0.0f, 0.0f}, easeOut);
                    slideAnim.Duration(std::chrono::milliseconds(220));

                    auto opAnim = c.CreateScalarKeyFrameAnimation();
                    opAnim.InsertKeyFrame(1.0f, 1.0f, easeOut);
                    opAnim.Duration(std::chrono::milliseconds(165));

                    vis.StartAnimation(L"Translation", slideAnim);
                    vis.StartAnimation(L"Opacity", opAnim);
                }
            } catch (...) {}
        }

        SetTimer(trayHwnd, kTimerHoverCheck, 80, nullptr);
    }

    void HideTaskbarOnUiThread(HWND trayHwnd, bool immediate, bool forFullscreen) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;

        KillTimer(trayHwnd, kTimerHoverCheck);
        KillTimer(trayHwnd, kTimerInitialHide);
        KillTimer(trayHwnd, kTimerHideFallback);

        uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& st = m_trayStates[trayHwnd];
            gen = ++st.animGen;
            st.leaveStartTick = 0;
        }

        UINT dpi = GetDpiForWindow(trayHwnd);
        double scale = dpi ? (dpi / 96.0) : 1.0;
        double dipH = GetNormalHeightForTray(trayHwnd) / scale;
        float hideOffsetY = static_cast<float>(dipH + 8.0);

        xaml::FrameworkElement animTarget = GetAnimTarget(trayHwnd);
        if (!animTarget) {
            auto roots = GetLiveRootsForTray(trayHwnd);
            for (auto& r : roots) {
                animTarget = FindOrInitAnimTarget(trayHwnd, r);
                if (animTarget) break;
            }
        }

        if (immediate || !animTarget) {
            if (animTarget) {
                try {
                    auto vis = xaml::Hosting::ElementCompositionPreview::GetElementVisual(animTarget);
                    if (vis) {
                        vis.StopAnimation(L"Translation");
                        vis.StopAnimation(L"Opacity");
                        vis.Properties().InsertVector3(L"Translation", {0.0f, hideOffsetY, 0.0f});
                        vis.Opacity(0.0f);
                    }
                } catch (...) {}
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_trayStates[trayHwnd].state = VisState::Hidden;
            }
            g_anyTrayHidden.store(true, std::memory_order_release);
            g_edgeHoverQueued.store(false, std::memory_order_release);
            ApplyEmptyRgnToTray(trayHwnd);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_trayStates[trayHwnd].state = VisState::Hiding;
        }
        g_anyTrayHidden.store(true, std::memory_order_release);
        g_edgeHoverQueued.store(false, std::memory_order_release);

        try {
            auto vis = xaml::Hosting::ElementCompositionPreview::GetElementVisual(animTarget);
            if (vis) {
                auto c = vis.Compositor();
                auto batch = c.CreateScopedBatch(comp::CompositionBatchTypes::Animation);
                auto easeInOut = c.CreateCubicBezierEasingFunction({0.40f, 0.0f}, {0.20f, 1.0f});

                auto slideAnim = c.CreateVector3KeyFrameAnimation();
                slideAnim.InsertKeyFrame(1.0f, {0.0f, hideOffsetY, 0.0f}, easeInOut);
                slideAnim.Duration(std::chrono::milliseconds(190));

                auto opAnim = c.CreateScalarKeyFrameAnimation();
                opAnim.InsertKeyFrame(1.0f, 0.0f, easeInOut);
                opAnim.Duration(std::chrono::milliseconds(150));

                vis.StartAnimation(L"Translation", slideAnim);
                vis.StartAnimation(L"Opacity", opAnim);

                batch.Completed([trayHwnd, gen](auto const&, auto const&) {
                    PostMessageW(trayHwnd, kWmHideAnimDone,
                        static_cast<WPARAM>(gen & 0xFFFF'FFFFu),
                        static_cast<LPARAM>(gen >> 32));
                });
                batch.End();
                SetTimer(trayHwnd, kTimerHideFallback, 250, nullptr);
                return;
            }
        } catch (...) {}

        // Fallback if composition failed
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_trayStates[trayHwnd].state = VisState::Hidden;
        }
        g_anyTrayHidden.store(true, std::memory_order_release);
        ApplyEmptyRgnToTray(trayHwnd);
    }

    void OnHideAnimDoneOnUiThread(HWND trayHwnd, uint64_t gen) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& st = m_trayStates[trayHwnd];
            if (st.animGen != gen || st.state != VisState::Hiding) return;
            st.state = VisState::Hidden;
        }
        KillTimer(trayHwnd, kTimerHideFallback);
        g_anyTrayHidden.store(true, std::memory_order_release);
        ApplyEmptyRgnToTray(trayHwnd);
    }

    bool IsCursorInBottomBand(HWND trayHwnd) const {
        POINT pt{};
        if (!GetCursorPos(&pt)) return false;
        HMONITOR hMon = MonitorFromWindow(trayHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hMon, &mi)) return false;
        int normalH = GetNormalHeightForTray(trayHwnd);
        int topEdge = mi.rcMonitor.bottom - normalH - 8;
        return pt.x >= mi.rcMonitor.left && pt.x < mi.rcMonitor.right &&
               pt.y >= topEdge && pt.y <= mi.rcMonitor.bottom;
    }

    void OnHoverCheckTimer(HWND trayHwnd) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) {
            KillTimer(trayHwnd, kTimerHoverCheck);
            return;
        }

        if (IsFullscreenCoveringTray(trayHwnd)) {
            HideTaskbarOnUiThread(trayHwnd, false, true);
            return;
        }

        bool stayVisible = IsCursorInBottomBand(trayHwnd) || IsShellFlyoutOrMenuOpen(trayHwnd);
        if (!stayVisible) {
            bool mouseBtnDown = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
                                ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
            if (mouseBtnDown && IsCursorInBottomBand(trayHwnd)) {
                stayVisible = true;
            }
        }

        ULONGLONG now = GetTickCount64();
        bool shouldHide = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& st = m_trayStates[trayHwnd];
            if (st.state == VisState::Hidden || st.state == VisState::Hiding) {
                KillTimer(trayHwnd, kTimerHoverCheck);
                return;
            }
            if (stayVisible) {
                st.leaveStartTick = 0;
            } else {
                if (st.leaveStartTick == 0) {
                    st.leaveStartTick = now;
                } else if (now - st.leaveStartTick >= 260) {
                    shouldHide = true;
                }
            }
        }

        if (shouldHide) {
            HideTaskbarOnUiThread(trayHwnd, false, false);
        }
    }

    void OnInitialHideTimer(HWND trayHwnd) {
        KillTimer(trayHwnd, kTimerInitialHide);
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        if (IsFullscreenCoveringTray(trayHwnd)) {
            HideTaskbarOnUiThread(trayHwnd, true, true);
            return;
        }
        if (!IsCursorInBottomBand(trayHwnd) && !IsShellFlyoutOrMenuOpen(trayHwnd)) {
            HideTaskbarOnUiThread(trayHwnd, false, false);
        } else {
            SetTimer(trayHwnd, kTimerHoverCheck, 80, nullptr);
        }
    }

    void PostEdgeHoverIfHidden(HWND trayHwnd) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        VisState curState = VisState::Visible;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_trayStates.find(trayHwnd);
            if (it != m_trayStates.end()) {
                curState = it->second.state;
            }
        }
        if (curState != VisState::Hidden && curState != VisState::Hiding) {
            return;
        }
        POINT pt{};
        if (!GetCursorPos(&pt)) return;
        HMONITOR hMon = MonitorFromWindow(trayHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hMon, &mi)) return;
        if (pt.x < mi.rcMonitor.left || pt.x >= mi.rcMonitor.right ||
            pt.y < mi.rcMonitor.bottom - 8 || pt.y > mi.rcMonitor.bottom) {
            return;
        }
        bool expected = false;
        if (g_edgeHoverQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (!PostMessageW(trayHwnd, kWmEdgeHover, 0, 0)) {
                g_edgeHoverQueued.store(false, std::memory_order_release);
            }
        }
    }

    void ResetLeaveTimerIfVisible(HWND trayHwnd) {
        if (!m_active.load() || !trayHwnd) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_trayStates.find(trayHwnd);
        if (it != m_trayStates.end() && it->second.state == VisState::Visible) {
            it->second.leaveStartTick = 0;
        }
    }

    void OnEdgeHover(HWND trayHwnd) {
        g_edgeHoverQueued.store(false, std::memory_order_release);
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        VisState curState = VisState::Visible;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            curState = m_trayStates[trayHwnd].state;
        }
        if (curState == VisState::Hidden || curState == VisState::Hiding) {
            RevealTaskbarOnUiThread(trayHwnd, false);
        } else {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_trayStates[trayHwnd].leaveStartTick = 0;
        }
    }

    void OnForegroundChangedOnUiThread(HWND trayHwnd) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        VisState curState = VisState::Visible;
        bool wasFsEmpty = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& st = m_trayStates[trayHwnd];
            curState = st.state;
            wasFsEmpty = st.fullscreenEmptyRgn;
        }

        bool isFs = IsFullscreenCoveringTray(trayHwnd);
        g_fullscreenSuppressed.store(isFs, std::memory_order_release);

        if (isFs) {
            if (curState == VisState::Visible || curState == VisState::Revealing) {
                HideTaskbarOnUiThread(trayHwnd, false, true);
            } else if (curState == VisState::Hidden && !wasFsEmpty) {
                ApplyEmptyRgnToTray(trayHwnd);
            }
            return;
        }

        if (IsShellFlyoutOrMenuOpen(trayHwnd)) {
            if (curState == VisState::Hidden || curState == VisState::Hiding) {
                RevealTaskbarOnUiThread(trayHwnd, true);
            }
            return;
        }
    }

    void RestoreOnUiThread(HWND trayHwnd) {
        m_active = false;
        g_anyTrayHidden.store(false, std::memory_order_release);
        g_fullscreenSuppressed.store(false, std::memory_order_release);
        g_edgeHoverQueued.store(false, std::memory_order_release);
        KillTimer(trayHwnd, kTimerRefreshRegion);
        KillTimer(trayHwnd, kTimerKeepVisible);
        KillTimer(trayHwnd, kTimerHoverCheck);
        KillTimer(trayHwnd, kTimerHideFallback);
        KillTimer(trayHwnd, kTimerInitialHide);
        if (m_evtAlive) {
            ResetEvent(m_evtAlive);
        }
        if (m_cloakHook) {
            UnhookWinEvent(m_cloakHook);
            m_cloakHook = nullptr;
        }
        if (m_fgHook) {
            UnhookWinEvent(m_fgHook);
            m_fgHook = nullptr;
        }
        if (m_mouseHook) {
            UnhookWindowsHookEx(m_mouseHook);
            m_mouseHook = nullptr;
        }

        // Stop GPU auto-hide animations & reset Translation/Opacity
        if (auto animTarget = GetAnimTarget(trayHwnd)) {
            try {
                auto vis = xaml::Hosting::ElementCompositionPreview::GetElementVisual(animTarget);
                if (vis) {
                    vis.StopAnimation(L"Translation");
                    vis.StopAnimation(L"Opacity");
                    vis.Properties().InsertVector3(L"Translation", {0.0f, 0.0f, 0.0f});
                    vis.Opacity(1.0f);
                }
            } catch (...) {}
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_trayStates.clear();
        }

        // 1. Restore XAML backgrounds & remove custom glass pills
        try {
            std::vector<SavedElementState> toRestore;
            std::vector<winrt::weak_ref<controls::Grid>> centerPills;
            std::vector<winrt::weak_ref<controls::Panel>> trayPills;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                toRestore.swap(m_savedElements);
                centerPills.swap(m_centerPillGrids);
                trayPills.swap(m_trayPillPanels);
                m_lastGroups.clear();
                m_lastCenterPillSig.clear();
                m_trayPillStyled.clear();
            }
            for (auto& s : toRestore) {
                if (auto el = s.element.get()) {
                    try {
                        el.Visibility(s.origVisibility);
                        el.Opacity(s.origOpacity);
                    } catch (...) {}
                }
            }
            for (auto& wg : centerPills) {
                if (auto g = wg.get()) {
                    try {
                        g.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
                        g.Width(std::numeric_limits<double>::quiet_NaN());
                        g.Margin({0.0, 0.0, 0.0, 0.0});
                        g.CornerRadius({0.0, 0.0, 0.0, 0.0});
                        g.BorderThickness({0.0, 0.0, 0.0, 0.0});
                        g.Background(nullptr);
                        g.BorderBrush(nullptr);
                    } catch (...) {}
                }
            }
            for (auto& wp : trayPills) {
                if (auto p = wp.get()) {
                    try {
                        if (auto sp = p.try_as<controls::StackPanel>()) {
                            sp.Margin({0.0, 0.0, 0.0, 0.0});
                            sp.Padding({0.0, 0.0, 0.0, 0.0});
                            sp.CornerRadius({0.0, 0.0, 0.0, 0.0});
                            sp.BorderThickness({0.0, 0.0, 0.0, 0.0});
                            sp.Background(nullptr);
                            sp.BorderBrush(nullptr);
                        } else if (auto gr = p.try_as<controls::Grid>()) {
                            gr.Margin({0.0, 0.0, 0.0, 0.0});
                            gr.Padding({0.0, 0.0, 0.0, 0.0});
                            gr.CornerRadius({0.0, 0.0, 0.0, 0.0});
                            gr.BorderThickness({0.0, 0.0, 0.0, 0.0});
                            gr.Background(nullptr);
                            gr.BorderBrush(nullptr);
                        }
                    } catch (...) {}
                }
            }
        } catch (...) {}

        // 2. Clear SetWindowRgn on tray and child windows
        ClearWindowRegions(trayHwnd);

        // 3. Restore original AppBar auto-hide state on primary tray
        HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (trayHwnd == primary && m_appBarSaved) {
            APPBARDATA abd{};
            abd.cbSize = sizeof(abd);
            abd.hWnd = trayHwnd;
            abd.lParam = m_origAppBarState;
            SHAppBarMessage(ABM_SETSTATE, &abd);
            m_appBarSaved = false;
        }

        InvalidateRect(trayHwnd, nullptr, TRUE);
        RedrawWindow(trayHwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void DumpVisualTreeOnUiThread(HWND trayHwnd) {
        try {
            wchar_t tempPath[MAX_PATH]{};
            GetTempPathW(MAX_PATH, tempPath);
            std::wstring logFile = std::wstring(tempPath) + L"tb_diag.log";
            std::wofstream out(logFile, std::ios::trunc);
            if (!out.is_open()) return;

            APPBARDATA abd{};
            abd.cbSize = sizeof(abd);
            abd.hWnd = trayHwnd;
            UINT abState = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));

            VisState stState = VisState::Visible;
            uint64_t stGen = 0;
            bool stTrig = false;
            bool stFsEmpty = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_trayStates.find(trayHwnd);
                if (it != m_trayStates.end()) {
                    stState = it->second.state;
                    stGen = it->second.animGen;
                    stTrig = it->second.triggerRgnApplied;
                    stFsEmpty = it->second.fullscreenEmptyRgn;
                }
            }
            const wchar_t* stateStr =
                (stState == VisState::Hidden) ? L"Hidden" :
                (stState == VisState::Revealing) ? L"Revealing" :
                (stState == VisState::Hiding) ? L"Hiding" : L"Visible";

            float animTransY = 0.0f;
            float animOp = 1.0f;
            if (auto at = GetAnimTarget(trayHwnd)) {
                try {
                    if (auto v = xaml::Hosting::ElementCompositionPreview::GetElementVisual(at)) {
                        winrt::Windows::Foundation::Numerics::float3 tr{};
                        if (v.Properties().TryGetVector3(L"Translation", tr) == winrt::Windows::UI::Composition::CompositionGetValueStatus::Succeeded) {
                            animTransY = tr.y;
                        }
                        animOp = v.Opacity();
                    }
                } catch (...) {}
            }

            RECT rgnBox{};
            int rgnType = GetWindowRgnBox(trayHwnd, &rgnBox);

            out << L"=== TransparentTaskbar v6 Diagnostics ===\n";
            out << L"Version=v6 Active=" << (m_active.load() ? 1 : 0)
                << L" VisState=" << stateStr
                << L" AnimGen=" << stGen
                << L" AnimTransY=" << animTransY
                << L" AnimOpacity=" << animOp
                << L" TriggerRgn=" << (stTrig ? 1 : 0)
                << L" FullscreenEmptyRgn=" << (stFsEmpty ? 1 : 0)
                << L" RgnType=" << rgnType
                << L" RgnBox=(" << rgnBox.left << L"," << rgnBox.top << L"," << rgnBox.right << L"," << rgnBox.bottom << L")"
                << L" AppBarState=0x" << std::hex << abState << std::dec
                << L" NormalH=" << m_normalTrayHeight
                << L" FullscreenActive=" << (IsFullscreenCoveringTray(trayHwnd) ? 1 : 0)
                << L" ClampCount=" << m_clampCount.load()
                << L" LastSwpOk=" << (m_lastSwpOk.load() ? 1 : 0)
                << L" LastPostTop=" << m_lastPostTop.load()
                << L" MouseHook=" << (m_mouseHook ? 1 : 0)
                << L" MonBottom=" << g_monBottom.load() << L"\n";
            if (HWND fg = GetForegroundWindow()) {
                wchar_t fgCls[64]{};
                GetClassNameW(fg, fgCls, 64);
                LONG_PTR fgStyle = GetWindowLongPtrW(fg, GWL_STYLE);
                out << L"FG=" << fgCls << L" style=0x" << std::hex << fgStyle << std::dec << L"\n";
            }

            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(hMon, &mi)) {
                    auto* o = reinterpret_cast<std::wofstream*>(lp);
                    *o << L"Monitor: Full=(" << mi.rcMonitor.left << L"," << mi.rcMonitor.top
                       << L"," << mi.rcMonitor.right << L"," << mi.rcMonitor.bottom
                       << L") Work=(" << mi.rcWork.left << L"," << mi.rcWork.top
                       << L"," << mi.rcWork.right << L"," << mi.rcWork.bottom << L")\n";
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&out));

            RECT rcTray{};
            GetWindowRect(trayHwnd, &rcTray);
            bool trayTopmost = (GetWindowLongPtrW(trayHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            out << L"Tray HWND=0x" << std::hex << reinterpret_cast<uintptr_t>(trayHwnd)
                << std::dec << L" Rect=(" << rcTray.left << L"," << rcTray.top
                << L"," << rcTray.right << L"," << rcTray.bottom << L") Visible="
                << (IsWindowVisible(trayHwnd) ? 1 : 0)
                << L" TrayCloaked=" << (IsWindowCloaked(trayHwnd) ? 1 : 0)
                << L" TrayTopmost=" << (trayTopmost ? 1 : 0) << L"\n";

            HWND bridge = FindBridgeChild(trayHwnd);
            if (bridge) {
                RECT rcB{};
                GetWindowRect(bridge, &rcB);
                out << L"Bridge HWND=0x" << std::hex << reinterpret_cast<uintptr_t>(bridge)
                    << std::dec << L" Rect=(" << rcB.left << L"," << rcB.top
                    << L"," << rcB.right << L"," << rcB.bottom << L") BridgeVisible="
                    << (IsWindowVisible(bridge) ? 1 : 0)
                    << L" BridgeCloaked=" << (IsWindowCloaked(bridge) ? 1 : 0) << L"\n";
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_lastGroups.find(trayHwnd);
                if (it != m_lastGroups.end()) {
                    out << L"Applied Click-Through Groups (" << it->second.size() << L"):\n";
                    for (const auto& g : it->second) {
                        out << L"  Group=(" << g.left << L"," << g.top << L"," << g.right << L"," << g.bottom << L")\n";
                    }

                    // Live desktop pixel sampling + bottom strip screenshot capture
                    HMONITOR hMon = MonitorFromWindow(trayHwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    GetMonitorInfoW(hMon, &mi);
                    int sampleY = mi.rcMonitor.bottom - std::max<int>(GetNormalHeightForTray(trayHwnd) / 2, 10);
                    sampleY = std::clamp<int>(sampleY, mi.rcMonitor.top, mi.rcMonitor.bottom - 1);
                    int aboveY = std::clamp<int>(mi.rcMonitor.bottom - GetNormalHeightForTray(trayHwnd) - 40, mi.rcMonitor.top, mi.rcMonitor.bottom - 1);

                    if (HDC hdc = GetDC(nullptr)) {
                        auto printPxAt = [&](int px, int py) {
                            COLORREF c = GetPixel(hdc, px, py);
                            out << L"(" << GetRValue(c) << L"," << GetGValue(c) << L"," << GetBValue(c) << L")";
                        };
                        out << L"AboveBar y=" << aboveY << L" EmptySpace: x=300=";
                        printPxAt(300, aboveY); out << L" x=500="; printPxAt(500, aboveY); out << L" x=1300="; printPxAt(1300, aboveY); out << L"\n";
                        out << L"PixelSamples y=" << sampleY << L" EmptySpace: x=300=";
                        printPxAt(300, sampleY); out << L" x=500="; printPxAt(500, sampleY); out << L" x=1300="; printPxAt(1300, sampleY); out << L"\n";

                        for (size_t gi = 0; gi < it->second.size(); ++gi) {
                            const auto& g = it->second[gi];
                            int x0 = g.left + 12;
                            int x1 = g.right - 12;
                            if (x1 <= x0) continue;
                            int blackCount = 0;
                            std::unordered_set<COLORREF> uniq;
                            out << L"  Group" << gi << L" Pixels: ";
                            for (int k = 0; k < 12; ++k) {
                                int sx = x0 + (x1 - x0) * k / 11;
                                COLORREF c = GetPixel(hdc, sx, sampleY);
                                int r = GetRValue(c), gr = GetGValue(c), b = GetBValue(c);
                                if (r == 0 && gr == 0 && b == 0) blackCount++;
                                uniq.insert(c);
                                out << L"x" << sx << L"=(" << r << L"," << gr << L"," << b << L") ";
                            }
                            out << L"| pureBlack=" << blackCount << L"/12 uniqueColors=" << uniq.size() << L"\n";
                        }

                        // Capture 140px bottom strip to BMP for visual inspection
                        int capW = mi.rcMonitor.right - mi.rcMonitor.left;
                        int capH = std::min<int>(140, mi.rcMonitor.bottom - mi.rcMonitor.top);
                        int capY = mi.rcMonitor.bottom - capH;
                        if (HDC memDc = CreateCompatibleDC(hdc)) {
                            BITMAPINFO bmi{};
                            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                            bmi.bmiHeader.biWidth = capW;
                            bmi.bmiHeader.biHeight = -capH; // top-down
                            bmi.bmiHeader.biPlanes = 1;
                            bmi.bmiHeader.biBitCount = 32;
                            bmi.bmiHeader.biCompression = BI_RGB;
                            void* bits = nullptr;
                            if (HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0)) {
                                HGDIOBJ old = SelectObject(memDc, hBmp);
                                BitBlt(memDc, 0, 0, capW, capH, hdc, mi.rcMonitor.left, capY, SRCCOPY);
                                SelectObject(memDc, old);
                                std::wstring bmpPath = std::wstring(tempPath) + L"tb_verify.bmp";
                                SaveBmp24(bmpPath, capW, capH, static_cast<const BYTE*>(bits));
                                DeleteObject(hBmp);
                            }
                            DeleteDC(memDc);
                        }
                        ReleaseDC(nullptr, hdc);
                    }
                } else {
                    out << L"Applied Click-Through Groups: NONE\n";
                }
            }

            auto roots = GetLiveRootsForTray(trayHwnd);
            out << L"Tracked roots count: " << roots.size() << L"\n";
            for (auto& r : roots) {
                auto tr = r.Translation();
                out << L"Root: " << winrt::get_class_name(r).c_str()
                    << L" #" << r.Name().c_str()
                    << L" Translation=(" << tr.x << L"," << tr.y << L"," << tr.z << L")"
                    << L" Opacity=" << r.Opacity()
                    << L" Vis=" << static_cast<int>(r.Visibility()) << L"\n";
                try {
                    auto vis = xaml::Hosting::ElementCompositionPreview::GetElementVisual(r);
                    auto p = vis ? vis.Parent() : nullptr;
                    for (int pi = 0; p && pi < 8; ++pi) {
                        auto po = p.Offset();
                        out << L"  Parent[" << pi << L"]: isVis=" << (p.IsVisible() ? 1 : 0)
                            << L" op=" << p.Opacity()
                            << L" off=(" << po.x << L"," << po.y << L"," << po.z << L")\n";
                        p = p.Parent();
                    }
                } catch (...) {}
                DumpElementRecursive(r, 0, out);
            }
            out.flush();
        } catch (...) {}
    }

    void OnDisplayOrThemeChanged(HWND trayHwnd) {
        if (!m_active.load()) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastGroups.erase(trayHwnd);
            m_lastCenterPillSig.erase(trayHwnd);
            m_trayPillStyled.erase(trayHwnd);
            m_normalTrayHeight = 0;
        }
        InitAppBarOnUiThread(trayHwnd);
        ScheduleRefreshDebounced(trayHwnd);
    }

    void OnRefreshTimer(HWND trayHwnd) {
        KillTimer(trayHwnd, kTimerRefreshRegion);
        if (!m_active.load()) return;
        ApplyOnUiThread(trayHwnd);
    }

    void OnKeepVisibleTimer(HWND trayHwnd) {
        KillTimer(trayHwnd, kTimerKeepVisible);
        if (!m_active.load()) return;
        KeepIconsVisibleOnUiThread(trayHwnd, true);
    }

    void ScheduleKeepVisibleDebounced(HWND trayHwnd) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        SetTimer(trayHwnd, kTimerKeepVisible, 120, nullptr);
    }

    void ScheduleRefreshDebounced(HWND trayHwnd, UINT delayMs = 120) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        SetTimer(trayHwnd, kTimerRefreshRegion, delayMs, nullptr);
    }

    void HandleControlCommand(HWND /*trayHwnd*/, WPARAM cmd) {
        switch (cmd) {
        case TB_CMD_APPLY:
            m_active = true;
            if (m_evtAlive) SetEvent(m_evtAlive);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastGroups.clear();
                m_lastCenterPillSig.clear();
                m_trayPillStyled.clear();
            }
            BootstrapSubclassAllTrays();
            ScheduleAllTrays(kWmInitAppBar);
            ScheduleAllTrays(kWmXamlReapply);
            break;
        case TB_CMD_RESTORE:
            ScheduleAllTrays(kWmXamlRestore);
            break;
        case TB_CMD_REFRESH:
            if (m_active.load()) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lastGroups.clear();
                    m_lastCenterPillSig.clear();
                    m_trayPillStyled.clear();
                }
                ScheduleAllTrays(kWmInitAppBar);
                ScheduleAllTrays(kWmXamlReapply);
            }
            break;
        case TB_CMD_DUMP:
            ScheduleAllTrays(kWmXamlDump);
            break;
        default:
            break;
        }
    }

    bool IsActive() const { return m_active.load(); }
    bool IsInClamp() const { return m_inClamp.load(); }
    bool IsApplyingRgn() const { return m_applyingRgn.load(); }
    int  GetClampCount() const { return m_clampCount.load(); }

    void ReapplyRegionIfReset(HWND trayHwnd) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        RECT box{};
        int rgnType = GetWindowRgnBox(trayHwnd, &box);
        bool hasRgn = (rgnType != ERROR);
        VisState stState = VisState::Visible;
        bool fsEmpty = false;
        std::vector<RECT> groups;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto stIt = m_trayStates.find(trayHwnd);
            if (stIt != m_trayStates.end()) {
                stState = stIt->second.state;
                fsEmpty = stIt->second.fullscreenEmptyRgn;
            }
            if (stState == VisState::Hidden) {
                // While hidden, never restore pill regions; ensure trigger strip or empty rgn stays intact
                if (fsEmpty || IsFullscreenCoveringTray(trayHwnd)) {
                    if (rgnType != NULLREGION) {
                        PostMessageW(trayHwnd, kWmForegroundChanged, 0, 0);
                    }
                }
                return;
            }
            auto it = m_lastGroups.find(trayHwnd);
            if (it == m_lastGroups.end() || it->second.empty()) return;
            auto boxIt = m_appliedRgnBoxes.find(trayHwnd);
            if (hasRgn && boxIt != m_appliedRgnBoxes.end() && EqualRect(&box, &boxIt->second)) {
                return;
            }
            groups = it->second;
        }
        if (stState == VisState::Hidden) {
            if (fsEmpty || IsFullscreenCoveringTray(trayHwnd)) ApplyEmptyRgnToTray(trayHwnd);
            else ApplyTriggerStripRgn(trayHwnd);
            return;
        }
        ApplyRoundRgnToTray(trayHwnd, groups);
    }

private:
    class TreeCallback : public winrt::implements<TreeCallback, IVisualTreeServiceCallback2> {
    public:
        HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
            ParentChildRelation /*relation*/,
            VisualElement element,
            VisualMutationType mutationType) noexcept override {
            try {
                TaskbarController::Instance().OnVisualTreeMutation(
                    element.Handle, element.Type, element.Name, mutationType);
            } catch (...) {}
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnElementStateChanged(
            InstanceHandle /*element*/,
            VisualElementState /*elementState*/,
            LPCWSTR /*context*/) noexcept override {
            return S_OK;
        }
    };

    static VOID CALLBACK OnNamedEventSignaled(PVOID lpParam, BOOLEAN /*timerOrWaitFired*/) {
        WPARAM cmd = reinterpret_cast<WPARAM>(lpParam);
        TaskbarController::Instance().HandleControlCommand(nullptr, cmd);
    }

    void SetupNamedControlEvents() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_eventsCreated) return;
        m_eventsCreated = true;

        m_evtAlive   = CreateEventW(nullptr, TRUE, TRUE, kEvtAliveV3);
        m_evtApply   = CreateEventW(nullptr, FALSE, FALSE, kEvtApply);
        m_evtRestore = CreateEventW(nullptr, FALSE, FALSE, kEvtRestore);
        m_evtRefresh = CreateEventW(nullptr, FALSE, FALSE, kEvtRefresh);
        m_evtDump    = CreateEventW(nullptr, FALSE, FALSE, kEvtDump);

        if (m_evtApply) {
            RegisterWaitForSingleObject(&m_waitApply, m_evtApply, OnNamedEventSignaled,
                reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(TB_CMD_APPLY)), INFINITE, WT_EXECUTEDEFAULT);
        }
        if (m_evtRestore) {
            RegisterWaitForSingleObject(&m_waitRestore, m_evtRestore, OnNamedEventSignaled,
                reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(TB_CMD_RESTORE)), INFINITE, WT_EXECUTEDEFAULT);
        }
        if (m_evtRefresh) {
            RegisterWaitForSingleObject(&m_waitRefresh, m_evtRefresh, OnNamedEventSignaled,
                reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(TB_CMD_REFRESH)), INFINITE, WT_EXECUTEDEFAULT);
        }
        if (m_evtDump) {
            RegisterWaitForSingleObject(&m_waitDump, m_evtDump, OnNamedEventSignaled,
                reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(TB_CMD_DUMP)), INFINITE, WT_EXECUTEDEFAULT);
        }
    }

    int GetNormalHeightForTray(HWND trayHwnd) const {
        if (m_normalTrayHeight > 0) return m_normalTrayHeight;
        UINT dpi = GetDpiForWindow(trayHwnd);
        return MulDiv(48, dpi ? static_cast<int>(dpi) : 96, 96);
    }

    void ClampWindowPosChanging(HWND trayHwnd, WINDOWPOS* wp) {
        if (!wp || !m_active.load()) return;
        if (IsFullscreenCoveringTray(trayHwnd)) return;

        HMONITOR hMon = MonitorFromWindow(trayHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hMon, &mi)) return;

        int normalH = GetNormalHeightForTray(trayHwnd);
        int targetX = mi.rcMonitor.left;
        int targetY = mi.rcMonitor.bottom - normalH;
        int targetW = mi.rcMonitor.right - mi.rcMonitor.left;

        RECT curRc{};
        GetWindowRect(trayHwnd, &curRc);

        // Override even if CTray::v_WndProc set SWP_NOMOVE / SWP_NOSIZE
        if ((!(wp->flags & SWP_NOMOVE) && wp->y > targetY) ||
            ((wp->flags & SWP_NOMOVE) && curRc.top > targetY)) {
            wp->flags &= ~SWP_NOMOVE;
            wp->x = targetX;
            wp->y = targetY;
        }
        if ((!(wp->flags & SWP_NOSIZE) && ((wp->cy > 0 && wp->cy < normalH) || (wp->cx > 0 && wp->cx < targetW))) ||
            ((wp->flags & SWP_NOSIZE) && (curRc.bottom - curRc.top) < normalH)) {
            wp->flags &= ~SWP_NOSIZE;
            wp->cx = targetW;
            wp->cy = normalH;
        }
        if (wp->flags & SWP_HIDEWINDOW) {
            wp->flags &= ~SWP_HIDEWINDOW;
            wp->flags |= SWP_SHOWWINDOW;
        }
    }

    void EnsureTrayClampedVisible(HWND trayHwnd) {
        if (!m_active.load() || !trayHwnd || !IsWindow(trayHwnd)) return;
        if (IsFullscreenCoveringTray(trayHwnd)) return;

        bool expected = false;
        if (!m_inClamp.compare_exchange_strong(expected, true)) return;

        HMONITOR hMon = MonitorFromWindow(trayHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi)) {
            int normalH = GetNormalHeightForTray(trayHwnd);
            int targetX = mi.rcMonitor.left;
            int targetY = mi.rcMonitor.bottom - normalH;
            int targetW = mi.rcMonitor.right - mi.rcMonitor.left;

            UncloakWindowIfNeeded(trayHwnd);
            RECT rc{};
            GetWindowRect(trayHwnd, &rc);
            bool visible = (IsWindowVisible(trayHwnd) != FALSE);

            // IMPORTANT: Never fight Z-order (always SWP_NOZORDER) so fullscreen apps (YouTube, games) can sit above Shell_TrayWnd!
            if (!visible || rc.top != targetY || (rc.bottom - rc.top) != normalH || (rc.right - rc.left) != targetW) {
                BOOL ok = SetWindowPos(
                    trayHwnd, nullptr,
                    targetX, targetY, targetW, normalH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
                RECT postRc{};
                GetWindowRect(trayHwnd, &postRc);
                m_clampCount.fetch_add(1);
                m_lastSwpOk.store(ok != FALSE);
                m_lastPostTop.store(postRc.top);
            }

            // Ensure child DesktopWindowContentBridge fills trayHwnd, is visible & uncloaked
            if (HWND bridge = FindBridgeChild(trayHwnd)) {
                UncloakWindowIfNeeded(bridge);
                RECT rcB{};
                GetWindowRect(bridge, &rcB);
                bool bridgeVisible = (IsWindowVisible(bridge) != FALSE);
                if (!bridgeVisible || rcB.left != targetX || rcB.top != targetY ||
                    (rcB.right - rcB.left) != targetW || (rcB.bottom - rcB.top) != normalH) {
                    SetWindowPos(
                        bridge, nullptr, 0, 0, targetW, normalH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
                }
            }
        }

        m_inClamp.store(false, std::memory_order_release);
    }

    xaml::FrameworkElement GetAnimTarget(HWND trayHwnd) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_trayStates.find(trayHwnd);
        if (it != m_trayStates.end()) {
            return it->second.animTarget.get();
        }
        return nullptr;
    }

    xaml::FrameworkElement FindOrInitAnimTarget(HWND trayHwnd, const xaml::FrameworkElement& root) {
        if (!root) return nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_trayStates.find(trayHwnd);
            if (it != m_trayStates.end()) {
                if (auto ex = it->second.animTarget.get()) {
                    return ex;
                }
            }
        }

        // Walk down ScrollViewer -> ScrollContentPresenter -> Border -> Grid (parent of TaskbarFrame + SystemTrayFrame)
        xaml::FrameworkElement target = root;
        try {
            xaml::DependencyObject cur = root;
            for (int d = 0; d < 4; ++d) {
                int cCount = media::VisualTreeHelper::GetChildrenCount(cur);
                if (cCount <= 0) break;
                auto ch = media::VisualTreeHelper::GetChild(cur, 0);
                if (auto g = ch.try_as<controls::Grid>()) {
                    // Check if this Grid contains TaskbarFrame or SystemTrayFrame
                    int gc = media::VisualTreeHelper::GetChildrenCount(g);
                    if (gc >= 1) {
                        target = g;
                    }
                    break;
                }
                if (auto fe = ch.try_as<xaml::FrameworkElement>()) {
                    target = fe;
                }
                cur = ch;
            }
        } catch (...) {}

        try {
            xaml::Hosting::ElementCompositionPreview::SetIsTranslationEnabled(target, true);
            if (auto vis = xaml::Hosting::ElementCompositionPreview::GetElementVisual(target)) {
                winrt::Windows::Foundation::Numerics::float3 curTr{};
                if (vis.Properties().TryGetVector3(L"Translation", curTr) != comp::CompositionGetValueStatus::Succeeded) {
                    vis.Properties().InsertVector3(L"Translation", {0.0f, 0.0f, 0.0f});
                }
            }
        } catch (...) {}

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& st = m_trayStates[trayHwnd];
            st.animTarget = winrt::make_weak(target);
            st.animTargetInitialized = true;
        }
        return target;
    }

    static bool KeepVisualsVisibleRecursive(const xaml::DependencyObject& node, int depth, const xaml::FrameworkElement& animTarget) {
        if (!node || depth > 4) return false;
        bool fixedAny = false;

        if (auto fe = node.try_as<xaml::FrameworkElement>()) {
            std::wstring name = fe.Name().c_str();
            std::wstring typeName = winrt::get_class_name(fe).c_str();

            if (name == L"BackgroundControl" || name == L"BackgroundFill" ||
                name == L"BackgroundStroke" || name == L"TaskbarBackground" ||
                name == L"BackgroundBorder" || name == L"ScreenEdgeStroke" ||
                name == L"SystemTrayFrame" || typeName.find(L"SystemTray") != std::wstring::npos ||
                typeName.find(L"TaskbarBackground") != std::wstring::npos) {
                return false;
            }

            bool isAnimTarget = (animTarget && winrt::get_abi(fe) == winrt::get_abi(animTarget));

            try {
                if (fe.Visibility() != xaml::Visibility::Visible) {
                    fe.Visibility(xaml::Visibility::Visible);
                    fixedAny = true;
                }
                if (!isAnimTarget) {
                    if (fe.Opacity() < 0.99) {
                        fe.Opacity(1.0);
                        fixedAny = true;
                    }
                    auto tr = fe.Translation();
                    if (std::abs(tr.y) > 0.1f || std::abs(tr.z) > 0.1f) {
                        fe.Translation({tr.x, 0.0f, 0.0f});
                        fixedAny = true;
                    }
                }
                if (auto rt = fe.RenderTransform()) {
                    if (auto tt = rt.try_as<media::TranslateTransform>()) {
                        if (std::abs(tt.Y()) > 0.1) {
                            tt.Y(0.0);
                            fixedAny = true;
                        }
                    }
                }
                auto vis = xaml::Hosting::ElementCompositionPreview::GetElementVisual(fe);
                if (vis) {
                    if (!isAnimTarget) {
                        vis.StopAnimation(L"Offset");
                        vis.StopAnimation(L"Translation");
                        vis.StopAnimation(L"Opacity");
                        vis.StopAnimation(L"IsVisible");
                        if (!vis.IsVisible()) {
                            vis.IsVisible(true);
                            fixedAny = true;
                        }
                        auto off = vis.Offset();
                        if (std::abs(off.y) > 0.1f) {
                            vis.Offset({off.x, 0.0f, off.z});
                            fixedAny = true;
                        }
                        if (vis.Opacity() < 0.99f) {
                            vis.Opacity(1.0f);
                            fixedAny = true;
                        }
                    } else {
                        // On animTarget itself, never stop our GPU Translation/Opacity animations!
                        if (!vis.IsVisible()) {
                            vis.IsVisible(true);
                            fixedAny = true;
                        }
                    }
                    // Also check ancestor island CompositionVisuals above ScrollViewer
                    if (depth == 0) {
                        auto p = vis.Parent();
                        for (int pi = 0; p && pi < 8; ++pi) {
                            p.StopAnimation(L"Offset");
                            p.StopAnimation(L"Opacity");
                            p.StopAnimation(L"IsVisible");
                            if (!p.IsVisible()) {
                                p.IsVisible(true);
                                fixedAny = true;
                            }
                            if (p.Opacity() < 0.99f) {
                                p.Opacity(1.0f);
                                fixedAny = true;
                            }
                            auto po = p.Offset();
                            if (std::abs(po.y) > 0.1f) {
                                p.Offset({po.x, 0.0f, po.z});
                                fixedAny = true;
                            }
                            p = p.Parent();
                        }
                    }
                }
            } catch (...) {}
        }

        int count = media::VisualTreeHelper::GetChildrenCount(node);
        for (int i = 0; i < count; ++i) {
            if (KeepVisualsVisibleRecursive(media::VisualTreeHelper::GetChild(node, i), depth + 1, animTarget)) {
                fixedAny = true;
            }
        }
        return fixedAny;
    }

    static HWND FindBridgeChild(HWND trayHwnd) {
        struct Ctx { HWND found{nullptr}; } ctx;
        EnumChildWindows(trayHwnd, [](HWND child, LPARAM lp) -> BOOL {
            wchar_t cls[128]{};
            GetClassNameW(child, cls, 128);
            if (wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
                reinterpret_cast<Ctx*>(lp)->found = child;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.found;
    }

    static std::vector<HWND> EnumAllTrayWindows() {
        std::vector<HWND> result;
        if (HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr)) {
            result.push_back(primary);
        }
        HWND sec = nullptr;
        while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
            result.push_back(sec);
        }
        return result;
    }

    static LRESULT CALLBACK BootstrapHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && lParam) {
            auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
            if (cwp->message == g_cmdMsg && cwp->wParam == kBootstrapMagic) {
                TaskbarController::Instance().AttachTrayWindowOnUiThread(cwp->hwnd);
            }
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    void BootstrapSubclassAllTrays() {
        auto trays = EnumAllTrayWindows();
        for (HWND h : trays) {
            DWORD tid = GetWindowThreadProcessId(h, nullptr);
            if (!tid) continue;
            if (tid == GetCurrentThreadId()) {
                AttachTrayWindowOnUiThread(h);
            } else {
                HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, BootstrapHookProc, nullptr, tid);
                if (hook) {
                    DWORD_PTR res = 0;
                    SendMessageTimeoutW(h, g_cmdMsg, kBootstrapMagic, 0, SMTO_NORMAL, 2000, &res);
                    UnhookWindowsHookEx(hook);
                }
            }
        }
    }

    static VOID CALLBACK CloakEventProc(
        HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd,
        LONG idObject, LONG /*idChild*/, DWORD /*idThread*/, DWORD /*time*/) {
        if (event != EVENT_OBJECT_CLOAKED || idObject != OBJID_WINDOW || !hwnd) return;
        auto& ctrl = TaskbarController::Instance();
        if (!ctrl.IsActive()) return;
        HWND trayHwnd = ctrl.ResolveTrackedTray(hwnd);
        if (trayHwnd && !IsFullscreenCoveringTray(trayHwnd)) {
            UncloakWindowIfNeeded(hwnd);
            PostMessageW(trayHwnd, kWmKeepIconsVisible, 0, 0);
        }
    }

    static VOID CALLBACK FgEventProc(
        HWINEVENTHOOK /*hook*/, DWORD event, HWND /*hwnd*/,
        LONG idObject, LONG /*idChild*/, DWORD /*idThread*/, DWORD /*time*/) {
        if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW) return;
        auto& ctrl = TaskbarController::Instance();
        if (!ctrl.IsActive()) return;
        ctrl.ScheduleAllTrays(kWmForegroundChanged);
    }

    HWND ResolveTrackedTray(HWND hwnd) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_subclassedTrays.find(hwnd) != m_subclassedTrays.end()) return hwnd;
        if (m_subclassedBridges.find(hwnd) != m_subclassedBridges.end()) {
            return GetParent(hwnd);
        }
        return nullptr;
    }

    static LRESULT CALLBACK BridgeSubclassProc(
        HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {

        HWND trayHwnd = reinterpret_cast<HWND>(dwRefData);
        auto& ctrl = TaskbarController::Instance();

        switch (uMsg) {
        case WM_NCHITTEST: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (ctrl.IsActive() && trayHwnd) {
                ctrl.PostEdgeHoverIfHidden(trayHwnd);
            }
            return res;
        }

        case WM_MOUSEMOVE:
        case WM_NCMOUSEMOVE:
        case WM_POINTERUPDATE:
        case WM_POINTERENTER:
            if (ctrl.IsActive() && trayHwnd) {
                ctrl.PostEdgeHoverIfHidden(trayHwnd);
                ctrl.ResetLeaveTimerIfVisible(trayHwnd);
            }
            break;

        case WM_SETCURSOR:
            if (ctrl.IsActive() && trayHwnd && HIWORD(lParam) == WM_MOUSEMOVE) {
                ctrl.PostEdgeHoverIfHidden(trayHwnd);
            }
            break;

        case WM_MOUSELEAVE:
            if (ctrl.IsActive() && trayHwnd) {
                ctrl.ScheduleKeepVisibleDebounced(trayHwnd);
            }
            break;

        case WM_SHOWWINDOW:
            if (ctrl.IsActive() && wParam == FALSE && trayHwnd && !IsFullscreenCoveringTray(trayHwnd)) {
                ctrl.ScheduleKeepVisibleDebounced(trayHwnd);
                return 0;
            }
            break;

        case WM_WINDOWPOSCHANGING: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (ctrl.IsActive() && trayHwnd && !IsFullscreenCoveringTray(trayHwnd)) {
                auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
                if (wp) {
                    if (!(wp->flags & SWP_NOMOVE) && (wp->y != 0 || wp->x != 0)) {
                        wp->x = 0;
                        wp->y = 0;
                    }
                    if (wp->flags & SWP_HIDEWINDOW) {
                        wp->flags &= ~SWP_HIDEWINDOW;
                        wp->flags |= SWP_SHOWWINDOW;
                    }
                }
                ctrl.ScheduleKeepVisibleDebounced(trayHwnd);
            }
            return res;
        }

        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, BridgeSubclassProc, kBridgeSubclassId);
            break;
        }

        return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    }

    void UpdatePrimaryMonitorCache(HWND trayHwnd) {
        HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!primary || trayHwnd != primary) return;
        g_primaryTrayHwnd.store(primary, std::memory_order_release);
        HMONITOR hMon = MonitorFromWindow(primary, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi)) {
            g_monLeft.store(mi.rcMonitor.left, std::memory_order_release);
            g_monRight.store(mi.rcMonitor.right, std::memory_order_release);
            g_monBottom.store(mi.rcMonitor.bottom, std::memory_order_release);
        }
    }

    void AttachTrayWindowOnUiThread(HWND h) {
        if (!h || !IsWindow(h)) return;
        if (GetWindowThreadProcessId(h, nullptr) != GetCurrentThreadId()) return;

        UpdatePrimaryMonitorCache(h);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_subclassedTrays.find(h) == m_subclassedTrays.end()) {
                if (SetWindowSubclass(h, TraySubclassProc, kSubclassId, 0)) {
                    m_subclassedTrays.insert(h);
                }
            }
            if (!m_cloakHook) {
                m_cloakHook = SetWinEventHook(
                    EVENT_OBJECT_CLOAKED, EVENT_OBJECT_CLOAKED,
                    nullptr, CloakEventProc, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
            }
            if (!m_fgHook) {
                m_fgHook = SetWinEventHook(
                    EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                    nullptr, FgEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            }
            if (!m_mouseHook) {
                m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_module, 0);
            }
        }

        if (HWND bridge = FindBridgeChild(h)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_subclassedBridges.find(bridge) == m_subclassedBridges.end()) {
                if (SetWindowSubclass(bridge, BridgeSubclassProc, kBridgeSubclassId, reinterpret_cast<DWORD_PTR>(h))) {
                    m_subclassedBridges.insert(bridge);
                }
            }
        }
    }

    void ScheduleAllTrays(UINT msg) {
        auto trays = EnumAllTrayWindows();
        for (HWND h : trays) {
            PostMessageW(h, msg, 0, 0);
        }
    }

    static LRESULT CALLBACK TraySubclassProc(
        HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/) {

        auto& ctrl = TaskbarController::Instance();

        if (uMsg == g_cmdMsg && g_cmdMsg != 0) {
            if (wParam == TB_CMD_PING) {
                return TB_PING_MAGIC;
            }
            if (wParam == kBootstrapMagic) {
                return 1;
            }
            ctrl.HandleControlCommand(hwnd, wParam);
            return 1;
        }

        switch (uMsg) {
        case kWmInitAppBar:
            ctrl.InitAppBarOnUiThread(hwnd);
            return 0;

        case kWmXamlProcessPending:
            ctrl.ProcessPendingHandlesOnUiThread(hwnd);
            return 0;

        case kWmXamlReapply:
            ctrl.ApplyOnUiThread(hwnd);
            return 0;

        case kWmXamlRestore:
            ctrl.RestoreOnUiThread(hwnd);
            return 0;

        case kWmKeepIconsVisible:
            ctrl.KeepIconsVisibleOnUiThread(hwnd, false);
            return 0;

        case kWmHideAnimDone: {
            uint64_t gen = (static_cast<uint64_t>(static_cast<uint32_t>(lParam)) << 32) |
                           static_cast<uint32_t>(wParam);
            ctrl.OnHideAnimDoneOnUiThread(hwnd, gen);
            return 0;
        }

        case kWmForegroundChanged:
            ctrl.OnForegroundChangedOnUiThread(hwnd);
            return 0;

        case kWmEdgeHover:
            ctrl.OnEdgeHover(hwnd);
            return 0;

        case kWmXamlDump:
            ctrl.DumpVisualTreeOnUiThread(hwnd);
            return 0;

        case WM_NCHITTEST: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (ctrl.IsActive()) {
                ctrl.PostEdgeHoverIfHidden(hwnd);
            }
            return res;
        }

        case WM_MOUSEMOVE:
        case WM_NCMOUSEMOVE:
        case WM_POINTERUPDATE:
        case WM_POINTERENTER:
            if (ctrl.IsActive()) {
                ctrl.PostEdgeHoverIfHidden(hwnd);
                ctrl.ResetLeaveTimerIfVisible(hwnd);
            }
            break;

        case WM_SETCURSOR:
            if (ctrl.IsActive() && HIWORD(lParam) == WM_MOUSEMOVE) {
                ctrl.PostEdgeHoverIfHidden(hwnd);
            }
            break;

        case WM_MOUSELEAVE:
            if (ctrl.IsActive()) {
                ctrl.ScheduleKeepVisibleDebounced(hwnd);
            }
            break;

        case WM_WINDOWPOSCHANGING: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (ctrl.IsActive()) {
                ctrl.ClampWindowPosChanging(hwnd, reinterpret_cast<WINDOWPOS*>(lParam));
            }
            return res;
        }

        case WM_WINDOWPOSCHANGED: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (ctrl.IsActive() && !ctrl.IsApplyingRgn()) {
                if (!ctrl.IsInClamp() && !IsFullscreenCoveringTray(hwnd)) {
                    int prevClamp = ctrl.GetClampCount();
                    ctrl.EnsureTrayClampedVisible(hwnd);
                    if (ctrl.GetClampCount() != prevClamp) {
                        KillTimer(hwnd, kTrayUITimerHide);
                        PostMessageW(hwnd, WM_TIMER, kTrayUITimerUnhide, 0);
                        ctrl.ScheduleKeepVisibleDebounced(hwnd);
                    }
                }
                ctrl.ReapplyRegionIfReset(hwnd);
            }
            return res;
        }

        case WM_MOVE: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (ctrl.IsActive() && !ctrl.IsInClamp() && !IsFullscreenCoveringTray(hwnd)) {
                ctrl.EnsureTrayClampedVisible(hwnd);
            }
            return res;
        }

        case WM_SHOWWINDOW:
            if (ctrl.IsActive() && wParam == FALSE && !IsFullscreenCoveringTray(hwnd)) {
                // Reject auto-hide SW_HIDE; re-clamp on next message cycle
                PostMessageW(hwnd, kWmXamlReapply, 0, 0);
                return 0;
            }
            break;

        case WM_TIMER:
            if (wParam == kTrayUITimerHide && ctrl.IsActive()) {
                KillTimer(hwnd, kTrayUITimerHide);
                return 0; // We manage hiding smoothly via GPU Composition
            }
            if (wParam == kTrayUITimerUnhide && ctrl.IsActive()) {
                LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
                ctrl.RevealTaskbarOnUiThread(hwnd, true);
                return res;
            }
            if (wParam == kTimerHoverCheck) {
                ctrl.OnHoverCheckTimer(hwnd);
                return 0;
            }
            if (wParam == kTimerHideFallback) {
                KillTimer(hwnd, kTimerHideFallback);
                uint64_t curGen = ctrl.GetAnimGen(hwnd);
                ctrl.OnHideAnimDoneOnUiThread(hwnd, curGen);
                return 0;
            }
            if (wParam == kTimerInitialHide) {
                ctrl.OnInitialHideTimer(hwnd);
                return 0;
            }
            if (wParam == kTimerRefreshRegion) {
                ctrl.OnRefreshTimer(hwnd);
                return 0;
            }
            if (wParam == kTimerKeepVisible) {
                ctrl.OnKeepVisibleTimer(hwnd);
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
        case WM_DWMCOMPOSITIONCHANGED:
        case WM_THEMECHANGED:
            if (ctrl.IsActive()) {
                ctrl.OnDisplayOrThemeChanged(hwnd);
            }
            break;

        case WM_SIZE:
            if (ctrl.IsActive()) {
                ctrl.ScheduleRefreshDebounced(hwnd);
            }
            break;

        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, TraySubclassProc, kSubclassId);
            break;
        }

        return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    }

    bool RegisterPotentialRootOnUiThread(const xaml::FrameworkElement& elem, HWND fallbackTray) {
        if (!elem) return false;
        try {
            xaml::DependencyObject cur = elem;
            xaml::FrameworkElement topFe = elem;
            while (cur) {
                auto parent = media::VisualTreeHelper::GetParent(cur);
                if (!parent) break;
                if (auto pFe = parent.try_as<xaml::FrameworkElement>()) {
                    topFe = pFe;
                }
                cur = parent;
            }

            void* rawId = winrt::get_abi(topFe);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& tr : m_trackedRoots) {
                    if (auto existing = tr.root.get()) {
                        if (winrt::get_abi(existing) == rawId) {
                            return false;
                        }
                    }
                }
            }

            HWND matchedTray = MatchTrayForRoot(topFe);
            if (!matchedTray) matchedTray = fallbackTray;
            AttachTrayWindowOnUiThread(matchedTray);
            HWND bridge = matchedTray ? FindBridgeChild(matchedTray) : nullptr;

            TrackedRoot tr{};
            tr.root = winrt::make_weak(topFe);
            tr.trayHwnd = matchedTray;
            tr.bridgeHwnd = bridge;

            tr.sizeToken = topFe.SizeChanged([this, matchedTray](auto const&, auto const&) {
                if (matchedTray && m_active.load()) {
                    ScheduleRefreshDebounced(matchedTray, 120);
                }
            });

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_trackedRoots.push_back(std::move(tr));
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    HWND MatchTrayForRoot(const xaml::FrameworkElement& root) {
        auto trays = EnumAllTrayWindows();
        if (trays.empty()) return nullptr;
        if (trays.size() == 1) return trays[0];

        double rw = root.ActualWidth();
        HWND best = trays[0];
        double bestDiff = 1e9;
        for (HWND h : trays) {
            RECT rc{};
            GetWindowRect(h, &rc);
            UINT dpi = GetDpiForWindow(h);
            double scale = dpi ? (dpi / 96.0) : 1.0;
            double logicalW = (rc.right - rc.left) / scale;
            double diff = std::abs(logicalW - rw);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = h;
            }
        }
        return best;
    }

    std::vector<xaml::FrameworkElement> GetLiveRootsForTray(HWND trayHwnd) {
        std::vector<xaml::FrameworkElement> out;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_trackedRoots.begin(); it != m_trackedRoots.end();) {
            auto fe = it->root.get();
            if (!fe) {
                it = m_trackedRoots.erase(it);
                continue;
            }
            if (it->trayHwnd == trayHwnd || it->trayHwnd == nullptr) {
                out.push_back(fe);
            }
            ++it;
        }
        return out;
    }

    void SaveAndHideElement(const xaml::UIElement& el) {
        if (!el) return;

        bool alreadyHidden = (el.Visibility() == xaml::Visibility::Collapsed && el.Opacity() == 0.0);

        void* raw = winrt::get_abi(el);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            bool alreadySaved = false;
            for (auto& s : m_savedElements) {
                if (auto existing = s.element.get()) {
                    if (winrt::get_abi(existing) == raw) {
                        alreadySaved = true;
                        break;
                    }
                }
            }
            if (!alreadySaved) {
                SavedElementState st{};
                st.element = winrt::make_weak(el);
                st.origVisibility = el.Visibility();
                st.origOpacity = el.Opacity();
                m_savedElements.push_back(std::move(st));
            }
        }

        if (!alreadyHidden) {
            el.Opacity(0.0);
            el.Visibility(xaml::Visibility::Collapsed);
        }
    }

    void SubscribeContainerSizeChanged(const xaml::FrameworkElement& container, HWND trayHwnd) {
        if (!container || !trayHwnd) return;
        void* raw = winrt::get_abi(container);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_subscribedContainers.find(raw) != m_subscribedContainers.end()) {
                return;
            }
            m_subscribedContainers.insert(raw);
        }
        container.SizeChanged([this, trayHwnd](auto const&, auto const&) {
            if (m_active.load()) {
                ScheduleRefreshDebounced(trayHwnd, 120);
            }
        });
    }

    void ApplyTransparencyRecursive(const xaml::DependencyObject& node, HWND trayHwnd) {
        if (!node) return;

        if (auto fe = node.try_as<xaml::FrameworkElement>()) {
            std::wstring name = fe.Name().c_str();
            std::wstring typeName = winrt::get_class_name(fe).c_str();

            if (name == L"TaskbarFrameRepeater" || typeName.find(L"TaskbarFrameRepeater") != std::wstring::npos ||
                name == L"SystemTrayFrame" || typeName.find(L"SystemTrayFrame") != std::wstring::npos) {
                SubscribeContainerSizeChanged(fe, trayHwnd);
            }

            if (name == L"BackgroundFill" ||
                name == L"BackgroundStroke" ||
                name == L"ScreenEdgeStroke") {
                SaveAndHideElement(fe);
                return;
            }

            // Capture Center Dock Pill container (Taskbar.TaskbarBackground > Grid)
            if (typeName.find(L"TaskbarBackground") != std::wstring::npos) {
                int cCount = media::VisualTreeHelper::GetChildrenCount(node);
                for (int i = 0; i < cCount; ++i) {
                    if (auto g = media::VisualTreeHelper::GetChild(node, i).try_as<controls::Grid>()) {
                        RegisterCenterPillGrid(trayHwnd, g);
                    }
                }
            }

            // Style Right System Tray Pill (StackPanel#SystemTrayFrameGrid / Grid#SystemTrayFrameGrid)
            if (name == L"SystemTrayFrameGrid") {
                if (auto p = fe.try_as<controls::Panel>()) {
                    StyleSystemTrayPill(trayHwnd, p);
                }
            }

            if (name == L"TaskbarFrame" || name == L"RootGrid" ||
                typeName.find(L"TaskbarFrame") != std::wstring::npos) {
                if (auto panel = fe.try_as<controls::Panel>()) {
                    if (panel.Background()) {
                        panel.Background(nullptr);
                    }
                }
                if (auto border = fe.try_as<controls::Border>()) {
                    if (border.Background()) {
                        border.Background(nullptr);
                    }
                }
            }
        }

        int count = media::VisualTreeHelper::GetChildrenCount(node);
        for (int i = 0; i < count; ++i) {
            ApplyTransparencyRecursive(media::VisualTreeHelper::GetChild(node, i), trayHwnd);
        }
    }

    static bool TryAddElementRect(
        const xaml::FrameworkElement& fe,
        const xaml::UIElement& rootVisual,
        double scale,
        int trayW,
        int trayH,
        std::vector<RECT>& rects) {

        double w = fe.ActualWidth();
        double h = fe.ActualHeight();
        if (w <= 2.0 || h <= 2.0) return false;

        try {
            auto xform = fe.TransformToVisual(rootVisual);
            auto pt = xform.TransformPoint(winrt::Windows::Foundation::Point{0.0f, 0.0f});
            RECT r{};
            r.left   = static_cast<LONG>(std::floor(pt.X * scale));
            r.top    = 0;
            r.right  = static_cast<LONG>(std::ceil((pt.X + w) * scale));
            r.bottom = trayH;

            int rw = r.right - r.left;
            if (rw > 2 && rw < static_cast<int>(trayW * 0.70)) {
                r.left  = std::max<LONG>(0, r.left);
                r.right = std::min<LONG>(trayW, r.right);
                if (r.right > r.left) {
                    rects.push_back(r);
                    return true;
                }
            }
        } catch (...) {}
        return false;
    }

    void CollectInteractiveRects(
        const xaml::DependencyObject& node,
        const xaml::UIElement& rootVisual,
        double scale,
        int trayW,
        int trayH,
        std::vector<RECT>& rects) {

        if (!node) return;

        if (auto fe = node.try_as<xaml::FrameworkElement>()) {
            if (fe.Visibility() != xaml::Visibility::Visible || fe.Opacity() <= 0.01) {
                return;
            }

            std::wstring name = fe.Name().c_str();
            std::wstring typeName = winrt::get_class_name(fe).c_str();

            if (name == L"BackgroundFill" || name == L"BackgroundStroke" ||
                name == L"TaskbarBackground" || name == L"BackgroundBorder" ||
                name == L"ScreenEdgeStroke") {
                return;
            }

            if (name == L"TaskbarFrameRepeater" || typeName.find(L"TaskbarFrameRepeater") != std::wstring::npos) {
                int childCount = media::VisualTreeHelper::GetChildrenCount(node);
                bool capturedAny = false;
                for (int i = 0; i < childCount; ++i) {
                    auto ch = media::VisualTreeHelper::GetChild(node, i);
                    if (auto chFe = ch.try_as<xaml::FrameworkElement>()) {
                        if (chFe.Visibility() == xaml::Visibility::Visible && chFe.Opacity() > 0.01) {
                            if (TryAddElementRect(chFe, rootVisual, scale, trayW, trayH, rects)) {
                                capturedAny = true;
                            }
                        }
                    }
                }
                if (capturedAny) return;
            }

            bool isButtonBase = (fe.try_as<primitives::ButtonBase>() != nullptr);
            bool isKnownLeaf =
                isButtonBase ||
                (typeName.find(L"TaskListButton") != std::wstring::npos) ||
                (typeName.find(L"ExperienceToggleButton") != std::wstring::npos) ||
                (typeName.find(L"SearchBoxButton") != std::wstring::npos) ||
                (typeName.find(L"AugmentedEntryPointButton") != std::wstring::npos) ||
                (typeName.find(L"OmniButton") != std::wstring::npos) ||
                (typeName.find(L"NotifyIconView") != std::wstring::npos) ||
                (typeName.find(L"ChevronIconView") != std::wstring::npos) ||
                (typeName.find(L"ShowDesktop") != std::wstring::npos) ||
                (name == L"ShowDesktopStack") ||
                (name == L"SystemTrayIcon");

            if (isKnownLeaf) {
                if (TryAddElementRect(fe, rootVisual, scale, trayW, trayH, rects)) {
                    return;
                }
            }
        }

        int count = media::VisualTreeHelper::GetChildrenCount(node);
        for (int i = 0; i < count; ++i) {
            CollectInteractiveRects(media::VisualTreeHelper::GetChild(node, i), rootVisual, scale, trayW, trayH, rects);
        }
    }

    static std::vector<RECT> MergeIntoIconGroups(std::vector<RECT> rects, double scale, int trayW, int trayH) {
        if (rects.empty()) return {};

        const LONG pad = static_cast<LONG>(std::ceil(4.0 * scale));
        const LONG mergeGap = static_cast<LONG>(std::ceil(24.0 * scale));

        for (auto& r : rects) {
            r.left   = std::max<LONG>(0, r.left - pad);
            r.right  = std::min<LONG>(trayW, r.right + pad);
            r.top    = 0;
            r.bottom = trayH;
        }

        std::sort(rects.begin(), rects.end(), [](const RECT& a, const RECT& b) {
            return a.left < b.left;
        });

        std::vector<RECT> merged;
        merged.push_back(rects[0]);
        for (size_t i = 1; i < rects.size(); ++i) {
            RECT& back = merged.back();
            if (rects[i].left <= back.right + mergeGap) {
                back.right = std::max<LONG>(back.right, rects[i].right);
            } else {
                merged.push_back(rects[i]);
            }
        }
        return merged;
    }

    void RegisterCenterPillGrid(HWND trayHwnd, const controls::Grid& g) {
        if (!g) return;
        void* raw = winrt::get_abi(g);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_trayToCenterPill[trayHwnd] = winrt::make_weak(g);
        for (auto& w : m_centerPillGrids) {
            if (auto ex = w.get()) {
                if (winrt::get_abi(ex) == raw) return;
            }
        }
        m_centerPillGrids.push_back(winrt::make_weak(g));
    }

    void StyleSystemTrayPill(HWND trayHwnd, const controls::Panel& p) {
        if (!p) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_trayPillStyled.find(trayHwnd) != m_trayPillStyled.end()) return;
            m_trayPillStyled.insert(trayHwnd);

            void* raw = winrt::get_abi(p);
            bool found = false;
            for (auto& w : m_trayPillPanels) {
                if (auto ex = w.get()) {
                    if (winrt::get_abi(ex) == raw) { found = true; break; }
                }
            }
            if (!found) m_trayPillPanels.push_back(winrt::make_weak(p));
        }

        try {
            if (auto sp = p.try_as<controls::StackPanel>()) {
                sp.Margin({0.0, 3.0, 6.0, 3.0});
                sp.Padding({4.0, 0.0, 4.0, 0.0});
                sp.CornerRadius({12.0, 12.0, 12.0, 12.0});
                sp.BorderThickness({1.0, 1.0, 1.0, 1.0});
                sp.BorderBrush(MakeGlassRimBrush());
                sp.Background(MakeMacGlassBrush());
            } else if (auto gr = p.try_as<controls::Grid>()) {
                gr.Margin({0.0, 3.0, 6.0, 3.0});
                gr.Padding({4.0, 0.0, 4.0, 0.0});
                gr.CornerRadius({12.0, 12.0, 12.0, 12.0});
                gr.BorderThickness({1.0, 1.0, 1.0, 1.0});
                gr.BorderBrush(MakeGlassRimBrush());
                gr.Background(MakeMacGlassBrush());
            }
        } catch (...) {}
    }

    void ApplyCenterPillGeometry(HWND trayHwnd, double leftDip, double widthDip) {
        if (widthDip <= 10.0) return;
        controls::Grid g{nullptr};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_trayToCenterPill.find(trayHwnd);
            if (it == m_trayToCenterPill.end()) return;
            g = it->second.get();
            if (!g) return;

            long lKey = std::lround(leftDip * 10.0);
            long wKey = std::lround(widthDip * 10.0);
            auto sigIt = m_lastCenterPillSig.find(trayHwnd);
            if (sigIt != m_lastCenterPillSig.end() &&
                sigIt->second.first == lKey && sigIt->second.second == wKey) {
                return;
            }
            m_lastCenterPillSig[trayHwnd] = {lKey, wKey};
        }

        try {
            g.HorizontalAlignment(xaml::HorizontalAlignment::Left);
            g.Width(widthDip);
            g.Margin({leftDip, 3.0, 0.0, 3.0});
            g.CornerRadius({12.0, 12.0, 12.0, 12.0});
            g.BorderThickness({1.0, 1.0, 1.0, 1.0});
            g.BorderBrush(MakeGlassRimBrush());
            g.Background(MakeMacGlassBrush());
        } catch (...) {}
    }

    uint64_t GetAnimGen(HWND trayHwnd) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_trayStates.find(trayHwnd);
        return (it != m_trayStates.end()) ? it->second.animGen : 0;
    }

    void ApplyTriggerStripRgn(HWND trayHwnd) {
        RECT rcTray{};
        GetWindowRect(trayHwnd, &rcTray);
        int trayW = rcTray.right - rcTray.left;
        int trayH = rcTray.bottom - rcTray.top;
        if (trayW <= 0 || trayH <= 0) return;

        int stripTop = std::max<int>(0, trayH - 3);
        HRGN rgn = CreateRectRgn(0, stripTop, trayW, trayH);
        if (!rgn) return;

        m_applyingRgn.store(true, std::memory_order_release);
        BOOL applied = SetWindowRgn(trayHwnd, rgn, TRUE);
        m_applyingRgn.store(false, std::memory_order_release);
        if (!applied) {
            DeleteObject(rgn);
        } else {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& st = m_trayStates[trayHwnd];
            st.triggerRgnApplied = true;
            st.fullscreenEmptyRgn = false;
        }
        if (HWND bridge = FindBridgeChild(trayHwnd)) {
            SetWindowRgn(bridge, nullptr, FALSE);
        }
    }

    void ApplyEmptyRgnToTray(HWND trayHwnd) {
        HRGN rgn = CreateRectRgn(0, 0, 0, 0);
        if (!rgn) return;
        m_applyingRgn.store(true, std::memory_order_release);
        BOOL applied = SetWindowRgn(trayHwnd, rgn, TRUE);
        m_applyingRgn.store(false, std::memory_order_release);
        if (!applied) {
            DeleteObject(rgn);
        } else {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& st = m_trayStates[trayHwnd];
            st.triggerRgnApplied = false;
            st.fullscreenEmptyRgn = true;
        }
    }

    void ApplyRoundRgnToTray(HWND trayHwnd, const std::vector<RECT>& groups) {
        RECT rcTray{};
        GetWindowRect(trayHwnd, &rcTray);
        int trayH = rcTray.bottom - rcTray.top;
        if (trayH <= 0) return;

        UINT dpi = GetDpiForWindow(trayHwnd);
        double scale = dpi ? (dpi / 96.0) : 1.0;

        const int marginY = std::lround(2.0 * scale);
        const int corner  = std::lround(24.0 * scale);

        HRGN trayRgn = CreateRectRgn(0, 0, 0, 0);
        for (const auto& g : groups) {
            int topY = std::min<int>(marginY, trayH / 4);
            int botY = std::max<int>(topY + 4, trayH - marginY);
            HRGN part = CreateRoundRectRgn(g.left, topY, g.right, botY, corner, corner);
            CombineRgn(trayRgn, trayRgn, part, RGN_OR);
            DeleteObject(part);
        }

        RECT rgnBox{};
        if (GetRgnBox(trayRgn, &rgnBox) == NULLREGION) {
            DeleteObject(trayRgn);
            return;
        }

        m_applyingRgn.store(true, std::memory_order_release);
        BOOL applied = SetWindowRgn(trayHwnd, trayRgn, TRUE);
        m_applyingRgn.store(false, std::memory_order_release);

        if (applied) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_appliedRgnBoxes[trayHwnd] = rgnBox;
        } else {
            DeleteObject(trayRgn);
        }

        if (HWND bridge = FindBridgeChild(trayHwnd)) {
            SetWindowRgn(bridge, nullptr, FALSE);
        }
    }

    void UpdateClickThroughAndPills(HWND trayHwnd, const std::vector<xaml::FrameworkElement>& roots) {
        if (!trayHwnd || !IsWindow(trayHwnd) || roots.empty()) return;

        RECT rcTray{};
        GetWindowRect(trayHwnd, &rcTray);
        int trayW = rcTray.right - rcTray.left;
        int trayH = rcTray.bottom - rcTray.top;
        if (trayW <= 0 || trayH <= 0) return;

        UINT dpi = GetDpiForWindow(trayHwnd);
        double scale = dpi ? (dpi / 96.0) : 1.0;

        HWND bridge = FindBridgeChild(trayHwnd);
        POINT bridgeOffset{0, 0};
        if (bridge) {
            RECT rcBridge{};
            GetWindowRect(bridge, &rcBridge);
            bridgeOffset.x = rcBridge.left - rcTray.left;
            bridgeOffset.y = rcBridge.top - rcTray.top;
        }

        std::vector<RECT> rawRects;
        for (auto& root : roots) {
            double rootScale = scale;
            try {
                if (auto xr = root.XamlRoot()) {
                    double rs = xr.RasterizationScale();
                    if (rs > 0.1) rootScale = rs;
                }
            } catch (...) {}

            CollectInteractiveRects(root, root, rootScale, trayW, trayH, rawRects);
        }

        if (rawRects.empty()) return;

        for (auto& r : rawRects) {
            r.left  = std::clamp<LONG>(r.left + bridgeOffset.x, 0, trayW);
            r.right = std::clamp<LONG>(r.right + bridgeOffset.x, 0, trayW);
        }

        auto groups = MergeIntoIconGroups(std::move(rawRects), scale, trayW, trayH);
        if (groups.empty()) return;

        // Style the Center Icons Pill in XAML using the center group's DIP coordinates
        const RECT& centerGrp = groups[0];
        double leftDip  = std::max<double>(0.0, (centerGrp.left - bridgeOffset.x) / scale - 4.0);
        double rightDip = ((centerGrp.right - bridgeOffset.x) / scale) + 4.0;
        ApplyCenterPillGeometry(trayHwnd, leftDip, rightDip - leftDip);

        // Expand click-through region slightly to match the 4 DIP pill padding
        std::vector<RECT> rgnGroups = groups;
        LONG padPx = std::lround(4.0 * scale);
        rgnGroups[0].left  = std::max<LONG>(0, rgnGroups[0].left - padPx);
        rgnGroups[0].right = std::min<LONG>(trayW, rgnGroups[0].right + padPx);
        if (rgnGroups.size() >= 2) {
            rgnGroups.back().left = std::max<LONG>(0, rgnGroups.back().left - padPx);
        }

        bool skipPillRgn = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto stIt = m_trayStates.find(trayHwnd);
            if (stIt != m_trayStates.end() && stIt->second.state == VisState::Hidden) {
                skipPillRgn = true;
            }
            auto it = m_lastGroups.find(trayHwnd);
            if (it != m_lastGroups.end() && RectVectorsEqual(it->second, rgnGroups)) {
                return;
            }
            m_lastGroups[trayHwnd] = rgnGroups;
        }

        if (skipPillRgn) {
            if (IsFullscreenCoveringTray(trayHwnd)) ApplyEmptyRgnToTray(trayHwnd);
            else ApplyTriggerStripRgn(trayHwnd);
            return;
        }

        ApplyRoundRgnToTray(trayHwnd, rgnGroups);
    }

    static void ClearWindowRegions(HWND trayHwnd) {
        if (!trayHwnd || !IsWindow(trayHwnd)) return;
        SetWindowRgn(trayHwnd, nullptr, TRUE);
        EnumChildWindows(trayHwnd, [](HWND child, LPARAM) -> BOOL {
            SetWindowRgn(child, nullptr, TRUE);
            return TRUE;
        }, 0);
    }

    static void DumpElementRecursive(const xaml::DependencyObject& node, int depth, std::wofstream& out) {
        if (!node || depth > 6) return;
        std::wstring indent(depth * 2, L' ');
        std::wstring cls = winrt::get_class_name(node).c_str();
        out << indent << cls;
        if (auto fe = node.try_as<xaml::FrameworkElement>()) {
            std::wstring name = fe.Name().c_str();
            if (!name.empty()) out << L" #" << name;
            out << L" [" << fe.ActualWidth() << L"x" << fe.ActualHeight()
                << L" vis=" << static_cast<int>(fe.Visibility())
                << L" op=" << fe.Opacity();
            try {
                if (auto vis = xaml::Hosting::ElementCompositionPreview::GetElementVisual(fe)) {
                    auto off = vis.Offset();
                    out << L" vOff=(" << off.x << L"," << off.y << L"," << off.z << L") vOp=" << vis.Opacity();
                }
            } catch (...) {}
            out << L"]";
        }
        out << L"\n";

        int count = media::VisualTreeHelper::GetChildrenCount(node);
        for (int i = 0; i < count; ++i) {
            DumpElementRecursive(media::VisualTreeHelper::GetChild(node, i), depth + 1, out);
        }
    }

    std::mutex m_mutex;
    bool m_initialized{false};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_inClamp{false};
    std::atomic<bool> m_applyingRgn{false};
    std::atomic<int>  m_clampCount{0};
    std::atomic<bool> m_lastSwpOk{false};
    std::atomic<LONG> m_lastPostTop{0};

    bool m_appBarSaved{false};
    UINT m_origAppBarState{0};
    int m_normalTrayHeight{0};

    bool m_eventsCreated{false};
    HANDLE m_evtAlive{nullptr};
    HANDLE m_evtApply{nullptr};
    HANDLE m_evtRestore{nullptr};
    HANDLE m_evtRefresh{nullptr};
    HANDLE m_evtDump{nullptr};
    HANDLE m_waitApply{nullptr};
    HANDLE m_waitRestore{nullptr};
    HANDLE m_waitRefresh{nullptr};
    HANDLE m_waitDump{nullptr};
    HWINEVENTHOOK m_cloakHook{nullptr};
    HWINEVENTHOOK m_fgHook{nullptr};
    HHOOK m_mouseHook{nullptr};
    std::unordered_map<HWND, TrayAnimState> m_trayStates;

    winrt::com_ptr<IXamlDiagnostics> m_xamlDiag;
    winrt::com_ptr<IVisualTreeService3> m_visualTree;
    winrt::com_ptr<TreeCallback> m_callback;

    std::unordered_set<HWND> m_subclassedTrays;
    std::unordered_set<HWND> m_subclassedBridges;
    std::unordered_set<HWND> m_trayPillStyled;
    std::unordered_set<void*> m_subscribedContainers;
    std::unordered_map<HWND, std::vector<RECT>> m_lastGroups;
    std::unordered_map<HWND, RECT> m_appliedRgnBoxes;
    std::unordered_map<HWND, winrt::weak_ref<controls::Grid>> m_trayToCenterPill;
    std::unordered_map<HWND, std::pair<long, long>> m_lastCenterPillSig;
    std::vector<winrt::weak_ref<controls::Grid>> m_centerPillGrids;
    std::vector<winrt::weak_ref<controls::Panel>> m_trayPillPanels;
    std::vector<InstanceHandle> m_pendingRootHandles;
    std::vector<TrackedRoot> m_trackedRoots;
    std::vector<SavedElementState> m_savedElements;
};

// COM Object implementing IObjectWithSite (invoked by Windows.UI.Xaml.dll inside explorer.exe)
class TapSite : public IObjectWithSite {
public:
    TapSite() { g_refCount.fetch_add(1); }
    ~TapSite() { g_refCount.fetch_sub(1); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IObjectWithSite) {
            *ppv = static_cast<IObjectWithSite*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refs.fetch_add(1) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = m_refs.fetch_sub(1) - 1;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override {
        m_site.copy_from(pUnkSite);
        if (pUnkSite) {
            TaskbarController::Instance().InitializeFromSite(pUnkSite);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) override {
        if (!ppvSite) return E_POINTER;
        if (!m_site) {
            *ppvSite = nullptr;
            return E_FAIL;
        }
        return m_site->QueryInterface(riid, ppvSite);
    }

private:
    std::atomic<LONG> m_refs{1};
    winrt::com_ptr<IUnknown> m_site;
};

class TapClassFactory : public IClassFactory {
public:
    TapClassFactory() { g_refCount.fetch_add(1); }
    ~TapClassFactory() { g_refCount.fetch_sub(1); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_refs.fetch_add(1) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = m_refs.fetch_sub(1) - 1;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        auto* obj = new (std::nothrow) TapSite();
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppvObject);
        obj->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) g_refCount.fetch_add(1);
        else g_refCount.fetch_sub(1);
        return S_OK;
    }

private:
    std::atomic<LONG> m_refs{1};
};

} // namespace

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hInst;
        g_cmdMsg = RegisterWindowMessageW(kTaskbarCmdMsgName);
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_TransparentTaskbarTap) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) TapClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow(void) {
    return (g_refCount.load() == 0) ? S_OK : S_FALSE;
}
