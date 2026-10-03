#include "MCAdvancementsOnWin.h"
#include "resource.h"
#include <fstream>
#include <shellapi.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <sstream>
#include <algorithm>
#include <locale>
#include <chrono>
#include <wininet.h>
#include <iomanip>
#include <queue>
#include <mutex>
#include <fstream>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wininet.lib")

using namespace Gdiplus;

HINSTANCE hInst;
WCHAR szTitle[100] = L"MC Advancements on Windows";
WCHAR szWindowClass[100] = L"MCAdvancementsOnWin";
AdvancementManager* g_pAdvManager = nullptr;
ULONG_PTR g_gdiplusToken = 0;
SettingsManager* g_pSettingsManager = nullptr;
HWND g_hMainWnd = nullptr;

HWND g_hDownloadWnd = nullptr;
HWND g_hProgressBar = nullptr;
HWND g_hStatusText = nullptr;
HWND g_hCancelButton = nullptr;
std::thread g_downloadThread;
std::atomic<bool> g_bDownloading(false);
std::atomic<bool> g_bDownloadCanceled(false);
HINTERNET g_hInternet = NULL;
HINTERNET g_hUrl = NULL;

std::queue<Advancement> g_achievementQueue;
std::mutex g_queueMutex;
std::atomic<bool> g_showingNotification(false);
std::atomic<int> g_notificationCount(0);

// ===== DPI 适配 =====
// 程序若不声明 DPI 感知，Windows 会用位图拉伸把整个窗口放大，高分屏上文字就会发虚。
// 这里在进程启动时开启 Per-Monitor DPI 感知，并为所有硬编码像素值提供按 DPI 缩放的辅助函数。
// 所有 API 都通过 GetProcAddress 动态解析：Win10/11 走最新接口，Win8.1 与 Wine 走回退路径，
// 任何一步失败都退化成 96 DPI（即保持原本的像素尺寸），不会崩溃或变形。
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static void EnableDpiAwareness() {
    typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(INT_PTR);
    typedef HRESULT(WINAPI* PFN_SetProcessDpiAwareness)(int);
    typedef BOOL(WINAPI* PFN_SetProcessDPIAware)(void);

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        PFN_SetProcessDpiAwarenessContext pSetContext =
            (PFN_SetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 即 ((DPI_AWARENESS_CONTEXT)-4)
        // 不用 SDK 宏，避免在旧 SDK / Wine 头文件下编译不过
        if (pSetContext && pSetContext((INT_PTR)-4)) {
            return;   // Win10 1703 及以上
        }
    }

    static HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        PFN_SetProcessDpiAwareness pSetAwareness =
            (PFN_SetProcessDpiAwareness)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        // PROCESS_PER_MONITOR_DPI_AWARE == 2
        if (pSetAwareness && SUCCEEDED(pSetAwareness(2))) {
            return;   // Win8.1 及以上
        }
    }

    if (hUser32) {
        PFN_SetProcessDPIAware pSetAware = (PFN_SetProcessDPIAware)GetProcAddress(hUser32, "SetProcessDPIAware");
        if (pSetAware) pSetAware();   // Vista 起的系统级感知，Wine 也支持
    }
}

// 取窗口所在显示器的 DPI（物理像素 = 逻辑像素 * dpi / 96）
static int GetDpiForWindowSafe(HWND hWnd) {
    typedef UINT(WINAPI* PFN_GetDpiForWindow)(HWND);
    typedef HRESULT(WINAPI* PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);

    int dpi = 0;

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        PFN_GetDpiForWindow pGetDpiForWindow = (PFN_GetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
        if (pGetDpiForWindow) {
            dpi = (int)pGetDpiForWindow(hWnd);
        }
    }

    if (dpi <= 0 && hWnd) {
        static HMODULE hShcore = LoadLibraryW(L"shcore.dll");
        if (hShcore) {
            PFN_GetDpiForMonitor pGetDpiForMonitor = (PFN_GetDpiForMonitor)GetProcAddress(hShcore, "GetDpiForMonitor");
            if (pGetDpiForMonitor) {
                UINT dpiX = 0, dpiY = 0;
                // 第二个参数 MDT_EFFECTIVE_DPI == 0
                if (SUCCEEDED(pGetDpiForMonitor(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), 0, &dpiX, &dpiY))) {
                    dpi = (int)dpiX;
                }
            }
        }
    }

    if (dpi <= 0) {
        HDC hdc = GetDC(NULL);   // 兜底：系统 DPI，Win7 / Wine 上也能拿到合理值
        if (hdc) {
            dpi = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(NULL, hdc);
        }
    }

    return dpi > 0 ? dpi : 96;
}

// 把以 96 DPI 为基准书写的像素值换算到当前 DPI
static int ScaleDpi(int value, int dpi) {
    return MulDiv(value, dpi, 96);
}

static HFONT CreateUIFont(HWND hWnd, int logicalHeight, int weight, const wchar_t* faceName) {
    return CreateFont(ScaleDpi(logicalHeight, GetDpiForWindowSafe(hWnd)), 0, 0, 0, weight,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName);
}

// 主界面布局：全部以 96 DPI 的像素值书写，再按当前 DPI 缩放
struct MainLayout {
    int marginX;
    int listWidth;
    int list1Top;
    int listHeight;
    int list2Top;
    int labelRight;
    int versionBand;
};

static MainLayout CalcMainLayout(HWND hWnd) {
    RECT rc = { 0 };
    GetClientRect(hWnd, &rc);

    int dpi = GetDpiForWindowSafe(hWnd);
    MainLayout L;
    L.marginX = ScaleDpi(10, dpi);
    L.listWidth = rc.right - L.marginX * 2;
    if (L.listWidth < 0) L.listWidth = 0;
    L.list1Top = ScaleDpi(56, dpi);
    L.listHeight = (rc.bottom - ScaleDpi(130, dpi)) / 2;   // 等价于原来的 (rc.bottom - 110) / 2 - 10
    if (L.listHeight < ScaleDpi(20, dpi)) L.listHeight = ScaleDpi(20, dpi);
    L.list2Top = ScaleDpi(90, dpi) + L.listHeight;         // 等价于原来的 72 + listHeight + 18
    L.labelRight = ScaleDpi(360, dpi);
    L.versionBand = ScaleDpi(28, dpi);
    return L;
}

HFONT g_hListFont = NULL;   // 两个成就列表共用的字体，DPI 变化时需要重建

static void UpdateListFont(HWND hWnd) {
    HFONT hNew = CreateUIFont(hWnd, 22, FW_NORMAL, L"微软雅黑");
    if (!hNew) return;

    HWND hList1 = GetDlgItem(hWnd, ID_LIST_COMPLETED);
    HWND hList2 = GetDlgItem(hWnd, ID_LIST_UNCOMPLETED);
    if (hList1) SendMessage(hList1, WM_SETFONT, (WPARAM)hNew, TRUE);
    if (hList2) SendMessage(hList2, WM_SETFONT, (WPARAM)hNew, TRUE);

    if (g_hListFont) DeleteObject(g_hListFont);   // 旧的已不再被任何控件选中
    g_hListFont = hNew;
}

static void LayoutLists(HWND hWnd) {
    HWND hList1 = GetDlgItem(hWnd, ID_LIST_COMPLETED);
    HWND hList2 = GetDlgItem(hWnd, ID_LIST_UNCOMPLETED);
    if (!hList1 || !hList2) return;

    MainLayout L = CalcMainLayout(hWnd);
    MoveWindow(hList1, L.marginX, L.list1Top, L.listWidth, L.listHeight, TRUE);
    MoveWindow(hList2, L.marginX, L.list2Top, L.listWidth, L.listHeight, TRUE);
}

// 成就通知所在显示器的矩形与 DPI（物理像素）。有了它，通知在多显示器/高 DPI 下位置和大小才正确。
struct NotifyMetrics {
    int dpi;
    int left;
    int top;
    int right;
    int bottom;
    int windowWidth;
    int windowHeight;
};

static NotifyMetrics CalcNotifyMetrics() {
    NotifyMetrics m;
    MONITORINFO mi;
    mi.cbSize = sizeof(MONITORINFO);
    HMONITOR hMon = MonitorFromWindow(g_hMainWnd ? g_hMainWnd : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);

    if (!GetMonitorInfo(hMon, &mi)) {
        mi.rcMonitor.left = 0;
        mi.rcMonitor.top = 0;
        mi.rcMonitor.right = GetSystemMetrics(SM_CXSCREEN);
        mi.rcMonitor.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    m.left = mi.rcMonitor.left;
    m.top = mi.rcMonitor.top;
    m.right = mi.rcMonitor.right;
    m.bottom = mi.rcMonitor.bottom;
    m.dpi = GetDpiForWindowSafe(g_hMainWnd);

    int width = m.right - m.left;
    int height = m.bottom - m.top;
    int shortSide = (width < height) ? width : height;
    m.windowHeight = shortSide / 10;
    m.windowWidth = m.windowHeight * 5;
    return m;
}

// ===== 深色模式 =====
// 配色：窗口/对话框底 #0A0A0A，列表（表格）底 #262626，正文白，次要信息灰
// 菜单单独用一组贴合 Windows 10/11 系统深色菜单的配色
#define DARK_COLOR_BG        RGB(0x0A, 0x0A, 0x0A)
#define DARK_COLOR_SURFACE   RGB(0x26, 0x26, 0x26)
#define DARK_COLOR_TEXT      RGB(0xFF, 0xFF, 0xFF)
#define DARK_COLOR_TEXT_DIM  RGB(0x9A, 0x9A, 0x9A)
#define DARK_COLOR_MENUBAR   RGB(0x1C, 0x1C, 0x1C)   // 菜单栏背景，贴合 Win11
#define DARK_COLOR_MENUPOPUP RGB(0x2B, 0x2B, 0x2B)   // 弹出菜单背景
#define DARK_COLOR_MENUSELECT RGB(0x4C, 0x4C, 0x4C)  // 选中项

HBRUSH g_hDarkBgBrush = NULL;       // 窗口/对话框底色
HBRUSH g_hDarkSurfaceBrush = NULL;  // 列表底色
HBRUSH g_hDarkMenuBarBrush = NULL;  // 菜单栏背景
HBRUSH g_hDarkMenuPopupBrush = NULL; // 弹出菜单背景

static bool IsDarkModeEnabled() {
    return g_pSettingsManager != nullptr && g_pSettingsManager->IsDarkMode();
}

static void EnsureDarkBrushes() {
    if (!g_hDarkBgBrush) g_hDarkBgBrush = CreateSolidBrush(DARK_COLOR_BG);
    if (!g_hDarkSurfaceBrush) g_hDarkSurfaceBrush = CreateSolidBrush(DARK_COLOR_SURFACE);
    if (!g_hDarkMenuBarBrush) g_hDarkMenuBarBrush = CreateSolidBrush(DARK_COLOR_MENUBAR);
    if (!g_hDarkMenuPopupBrush) g_hDarkMenuPopupBrush = CreateSolidBrush(DARK_COLOR_MENUPOPUP);
}

static void ReleaseDarkBrushes() {
    if (g_hDarkBgBrush) { DeleteObject(g_hDarkBgBrush); g_hDarkBgBrush = NULL; }
    if (g_hDarkSurfaceBrush) { DeleteObject(g_hDarkSurfaceBrush); g_hDarkSurfaceBrush = NULL; }
    if (g_hDarkMenuBarBrush) { DeleteObject(g_hDarkMenuBarBrush); g_hDarkMenuBarBrush = NULL; }
    if (g_hDarkMenuPopupBrush) { DeleteObject(g_hDarkMenuPopupBrush); g_hDarkMenuPopupBrush = NULL; }
}

// 在进程启动时调用一次（必须在创建任何窗口之前）：
// 告诉系统"本进程允许使用深色模式"，这样后续 AllowDarkModeForWindow 才能生效。
static void EnableDarkModeInfrastructure() {
    HMODULE hUx = LoadLibraryW(L"uxtheme.dll");
    if (!hUx) {
        OutputDebugString(L"[Dark] uxtheme.dll 加载失败，菜单/标题栏无法走系统深色\n");
        return;
    }

    typedef BOOL(WINAPI* PFN_SetPreferredAppMode)(int);
    typedef void(WINAPI* PFN_RefreshImmersiveColorPolicyState)(void);

    PFN_SetPreferredAppMode pSetMode = (PFN_SetPreferredAppMode)GetProcAddress(hUx, (LPCSTR)(ULONG_PTR)135);
    if (pSetMode) {
        BOOL r = pSetMode(1);   // AllowDark
        wchar_t dbg[128];
        swprintf_s(dbg, L"[Dark] SetPreferredAppMode(AllowDark) = %d\n", r);
        OutputDebugString(dbg);
    }
    else {
        OutputDebugString(L"[Dark] SetPreferredAppMode(ord 135) 未找到\n");
    }

    PFN_RefreshImmersiveColorPolicyState pRefresh =
        (PFN_RefreshImmersiveColorPolicyState)GetProcAddress(hUx, (LPCSTR)(ULONG_PTR)132);
    if (pRefresh) pRefresh();
}

// 逐窗口启用/关闭深色标题栏与菜单主题（Win10 1903+ / Win11）。
static void ApplyDarkWindowFrame(HWND hWnd, bool enable) {
    BOOL useDark = enable ? TRUE : FALSE;

    HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");
    if (hUx) {
        // 135=SetPreferredAppMode：ForceDark(2) / ForceLight(3)
        // AllowDark(1) 只是"允许"，菜单栏不会变；必须 Force 才能强制系统菜单走深色
        typedef BOOL(WINAPI* PFN_SetPreferredAppMode)(int);
        typedef BOOL(WINAPI* PFN_AllowDarkModeForWindow)(HWND, BOOL);
        typedef void(WINAPI* PFN_FlushMenuThemes)(void);
        typedef void(WINAPI* PFN_RefreshImmersiveColorPolicyState)(void);

        PFN_SetPreferredAppMode pSetMode = (PFN_SetPreferredAppMode)GetProcAddress(hUx, (LPCSTR)(ULONG_PTR)135);
        if (pSetMode) {
            pSetMode(enable ? 2 : 3);   // 2=ForceDark, 3=ForceLight
        }

        PFN_RefreshImmersiveColorPolicyState pRefresh =
            (PFN_RefreshImmersiveColorPolicyState)GetProcAddress(hUx, (LPCSTR)(ULONG_PTR)132);
        if (pRefresh) pRefresh();

        PFN_AllowDarkModeForWindow pAllow = (PFN_AllowDarkModeForWindow)GetProcAddress(hUx, (LPCSTR)(ULONG_PTR)133);
        if (pAllow) pAllow(hWnd, useDark);

        PFN_FlushMenuThemes pFlush = (PFN_FlushMenuThemes)GetProcAddress(hUx, (LPCSTR)(ULONG_PTR)136);
        if (pFlush) pFlush();
    }

    // 标题栏/边框
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        typedef HRESULT(WINAPI* PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
        PFN_DwmSetWindowAttribute pSetAttr = (PFN_DwmSetWindowAttribute)GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (pSetAttr) {
            // DWMWA_USE_IMMERSIVE_DARK_MODE：Win10 1809 用 19，1903+ / Win11 用 20；两个都试
            HRESULT hr = pSetAttr(hWnd, 20, &useDark, sizeof(useDark));
            if (FAILED(hr)) pSetAttr(hWnd, 19, &useDark, sizeof(useDark));
        }
    }

    // 强制菜单栏重画，让主题立即生效
    if (hWnd) DrawMenuBar(hWnd);
}

// ===== 深色模式：菜单自绘 =====
// 系统未文档化 API 对菜单栏无效，只能自绘。
// 关键：用 SystemParametersInfo 取系统菜单字体（Segoe UI 9pt），
// 保证视觉上和系统菜单一模一样，只是颜色变深。

HFONT g_hMenuFont = NULL;

static HFONT GetMenuFont() {
    if (g_hMenuFont) return g_hMenuFont;

    NONCLIENTMETRICS ncm;
    ncm.cbSize = sizeof(NONCLIENTMETRICS);
    if (SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_hMenuFont = CreateFontIndirect(&ncm.lfMenuFont);
    }
    if (!g_hMenuFont) {
        g_hMenuFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return g_hMenuFont;
}

#ifndef MNS_OWNERDRAW
#define MNS_OWNERDRAW 0x40000000
#endif

struct MenuItemDrawInfo {
    WCHAR text[128];
    bool  separator;
    bool  hasSubmenu;
    bool  topLevel;
    int   mnemonicPos;
};

static void FillMenuItemDrawInfo(HMENU hMenu, int depth) {
    if (!hMenu) return;

    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFO mii;
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(MENUITEMINFO);
        mii.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU | MIIM_DATA;

        WCHAR buf[128] = { 0 };
        mii.dwTypeData = buf;
        mii.cch = 128;
        if (!GetMenuItemInfo(hMenu, i, TRUE, &mii)) continue;

        MenuItemDrawInfo* info = (MenuItemDrawInfo*)mii.dwItemData;
        if (!info) {
            info = new MenuItemDrawInfo();
            mii.dwItemData = (ULONG_PTR)info;
        }

        info->separator = (mii.fType & MFT_SEPARATOR) != 0;
        info->hasSubmenu = (mii.hSubMenu != NULL);
        info->topLevel = (depth == 0);
        info->mnemonicPos = -1;
        info->text[0] = L'\0';

        int outLen = 0;
        for (int c = 0; buf[c] != L'\0' && outLen < 127; ++c) {
            if (buf[c] == L'&') {
                if (info->mnemonicPos < 0) info->mnemonicPos = outLen;
                continue;
            }
            info->text[outLen++] = buf[c];
        }
        info->text[outLen] = L'\0';

        mii.fMask = MIIM_DATA;
        SetMenuItemInfo(hMenu, i, TRUE, &mii);

        if (mii.hSubMenu) FillMenuItemDrawInfo(mii.hSubMenu, depth + 1);
    }
}

static void FreeMenuItemDrawInfo(HMENU hMenu) {
    if (!hMenu) return;
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFO mii;
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(MENUITEMINFO);
        mii.fMask = MIIM_SUBMENU | MIIM_DATA;
        if (GetMenuItemInfo(hMenu, i, TRUE, &mii)) {
            delete (MenuItemDrawInfo*)mii.dwItemData;
            if (mii.hSubMenu) FreeMenuItemDrawInfo(mii.hSubMenu);
        }
    }
}

// 逐项设置/清除 MFT_OWNERDRAW
static void SetMenuItemOwnerDrawType(HMENU hMenu, bool enable) {
    if (!hMenu) return;
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFO mii;
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(MENUITEMINFO);
        mii.fMask = MIIM_FTYPE;
        if (GetMenuItemInfo(hMenu, i, TRUE, &mii)) {
            if (enable) mii.fType |= MFT_OWNERDRAW;
            else        mii.fType &= ~MFT_OWNERDRAW;
            SetMenuItemInfo(hMenu, i, TRUE, &mii);
        }
        HMENU hSub = GetSubMenu(hMenu, i);
        if (hSub) SetMenuItemOwnerDrawType(hSub, enable);
    }
}

// 设置/清除菜单背景画刷（MIM_BACKGROUND）
static void SetMenuDarkBackground(HMENU hMenu, HBRUSH hbr) {
    if (!hMenu) return;
    MENUINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(MENUINFO);
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = hbr;
    SetMenuInfo(hMenu, &mi);

    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; ++i) {
        HMENU hSub = GetSubMenu(hMenu, i);
        if (hSub) SetMenuDarkBackground(hSub, hbr);
    }
}

static void ApplyDarkMenus(bool enable) {
    if (!g_hMainWnd) return;

    if (enable) {
        // 深色：在当前菜单上设自绘
        EnsureDarkBrushes();
        HMENU hMainMenu = GetMenu(g_hMainWnd);
        FillMenuItemDrawInfo(hMainMenu, 0);
        SetMenuItemOwnerDrawType(hMainMenu, true);
        SetMenuDarkBackground(hMainMenu, g_hDarkMenuBarBrush);
    }
    else {
        // 浅色：直接重新加载菜单资源，彻底清除所有自绘标志和项数据，
        // 不留任何 MFT_OWNERDRAW / dwItemData / MIM_BACKGROUND 残留
        HMENU hMainMenu = GetMenu(g_hMainWnd);
        FreeMenuItemDrawInfo(hMainMenu);

        HMENU hNewMenu = LoadMenu(hInst, MAKEINTRESOURCE(IDC_MCADVANCEMENTSONWIN));
        if (hNewMenu) {
            SetMenu(g_hMainWnd, hNewMenu);   // SetMenu 会销毁旧菜单
            if (g_pSettingsManager) g_pSettingsManager->UpdateAllMenuItems(g_hMainWnd);
        }
    }

    DrawMenuBar(g_hMainWnd);
}

static bool MeasureDarkMenuItem(MEASUREITEMSTRUCT* pmis) {
    if (!pmis || pmis->CtlType != ODT_MENU) return false;

    MenuItemDrawInfo* info = (MenuItemDrawInfo*)pmis->itemData;
    HDC hdc = GetDC(NULL);
    HFONT hOld = (HFONT)SelectObject(hdc, GetMenuFont());
    SIZE sz = { 0 };
    GetTextExtentPoint32(hdc, L"Ag", 2, &sz);
    int textH = sz.cy;
    int textW = 0;
    if (info && !info->separator) {
        GetTextExtentPoint32(hdc, info->text, (int)wcslen(info->text), &sz);
        textW = sz.cx;
    }
    SelectObject(hdc, hOld);
    ReleaseDC(NULL, hdc);

    if (info && info->separator) {
        pmis->itemWidth = 0;
        pmis->itemHeight = GetSystemMetrics(SM_CYMENU) / 4;
    }
    else {
        int padX = (info && info->topLevel) ? 16 : 48;
        int padY = (info && info->topLevel) ? 4 : 6;
        pmis->itemWidth = textW + padX;
        pmis->itemHeight = textH + padY;
        if (info && info->topLevel) {
            int barH = GetSystemMetrics(SM_CYMENU);
            if (unsigned(barH) > pmis->itemHeight) pmis->itemHeight = barH;
        }
    }
    return true;
}

static bool DrawDarkMenuItem(DRAWITEMSTRUCT* pdis) {
    if (!pdis || pdis->CtlType != ODT_MENU) return false;

    MenuItemDrawInfo* info = (MenuItemDrawInfo*)pdis->itemData;
    HDC hdc = pdis->hDC;
    RECT rc = pdis->rcItem;

    bool topLevel = info && info->topLevel;
    bool selected = (pdis->itemState & ODS_SELECTED) != 0;
    bool disabled = (pdis->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    bool checked  = (pdis->itemState & ODS_CHECKED) != 0;

    COLORREF bg = topLevel
        ? (selected ? DARK_COLOR_MENUSELECT : DARK_COLOR_MENUBAR)
        : (selected ? DARK_COLOR_MENUSELECT : DARK_COLOR_MENUPOPUP);
    HBRUSH hbr = CreateSolidBrush(bg);
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);

    if (!info || info->separator) {
        int y = (rc.top + rc.bottom) / 2;
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0x45, 0x45, 0x45));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, rc.left + 8, y, NULL);
        LineTo(hdc, rc.right - 8, y);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
        return true;
    }

    int padX = topLevel ? 8 : 24;
    HFONT hOld = (HFONT)SelectObject(hdc, GetMenuFont());
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? RGB(0x70, 0x70, 0x70) : DARK_COLOR_TEXT);

    if (checked && !topLevel) {
        RECT rcMark = { rc.left + 4, rc.top, rc.left + padX, rc.bottom };
        DrawText(hdc, L"\u2713", -1, &rcMark, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    RECT rcText = { rc.left + padX, rc.top, rc.right - padX, rc.bottom };
    DrawText(hdc, info->text, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX);

    if (info->mnemonicPos > 0) {
        SIZE s1 = { 0 }, s2 = { 0 };
        GetTextExtentPoint32(hdc, info->text, info->mnemonicPos, &s1);
        GetTextExtentPoint32(hdc, info->text, info->mnemonicPos + 1, &s2);
        int y = (rc.top + rc.bottom) / 2 + 6;
        HPEN hPen = CreatePen(PS_SOLID, 1, DARK_COLOR_TEXT);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, rcText.left + s1.cx, y, NULL);
        LineTo(hdc, rcText.left + s2.cx, y);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    if (info->hasSubmenu && !topLevel) {
        RECT rcArrow = { rc.right - 22, rc.top, rc.right - 6, rc.bottom };
        DrawText(hdc, L"\u203A", -1, &rcArrow, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, hOld);
    return true;
}

// 菜单栏最后一个顶级项右边的留白：MIM_BACKGROUND 对菜单栏不一定生效，
// 在 WM_NCPAINT 里补画
static void PaintMenuBarLeftover(HWND hWnd) {
    MENUBARINFO mbi;
    ZeroMemory(&mbi, sizeof(mbi));
    mbi.cbSize = sizeof(MENUBARINFO);
    if (!GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi)) return;

    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;

    RECT rcUsed = mbi.rcBar;
    BOOL hasAny = FALSE;
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; ++i) {
        RECT rcItem;
        if (GetMenuItemRect(hWnd, hMenu, i, &rcItem)) {
            if (!hasAny) { rcUsed = rcItem; hasAny = TRUE; }
            else UnionRect(&rcUsed, &rcUsed, &rcItem);
        }
    }
    if (!hasAny || rcUsed.right >= mbi.rcBar.right) return;

    RECT rcWin;
    GetWindowRect(hWnd, &rcWin);

    RECT rcFill = { rcUsed.right, mbi.rcBar.top, mbi.rcBar.right, mbi.rcBar.bottom };
    OffsetRect(&rcFill, -rcWin.left, -rcWin.top);

    HDC hdc = GetWindowDC(hWnd);
    FillRect(hdc, &rcFill, g_hDarkMenuBarBrush);
    ReleaseDC(hWnd, hdc);
}

// ===== 深色模式：按钮自绘 =====
// 标准按钮不理 WM_CTLCOLORBTN，只能加 BS_OWNERDRAW 自己画
static void MakeDarkOwnerDrawButton(HWND hParent, int ctrlId) {
    if (!IsDarkModeEnabled()) return;
    HWND hBtn = GetDlgItem(hParent, ctrlId);
    if (!hBtn) return;

    LONG_PTR style = GetWindowLongPtr(hBtn, GWL_STYLE);
    SetWindowLongPtr(hBtn, GWL_STYLE, style | BS_OWNERDRAW);
    InvalidateRect(hBtn, NULL, TRUE);
}

static bool DrawDarkOwnerButton(DRAWITEMSTRUCT* pdis) {
    if (!pdis || pdis->CtlType != ODT_BUTTON) return false;

    HDC hdc = pdis->hDC;
    RECT rc = pdis->rcItem;
    bool pressed  = (pdis->itemState & ODS_SELECTED) != 0;
    bool disabled = (pdis->itemState & ODS_DISABLED) != 0;
    bool focused  = (pdis->itemState & ODS_FOCUS) != 0;

    COLORREF face = pressed ? RGB(0x45, 0x45, 0x45) : RGB(0x2A, 0x2A, 0x2A);
    HBRUSH hbr = CreateSolidBrush(face);
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);

    HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0x55, 0x55, 0x55));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBr);
    DeleteObject(hPen);

    WCHAR text[128] = { 0 };
    GetWindowText(pdis->hwndItem, text, 128);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? RGB(0x70, 0x70, 0x70) : DARK_COLOR_TEXT);
    DrawText(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX);

    if (focused) {
        RECT rcFocus = { rc.left + 3, rc.top + 3, rc.right - 3, rc.bottom - 3 };
        DrawFocusRect(hdc, &rcFocus);
    }
    return true;
}

// 主窗口、下载窗口、对话框共用的自绘分发
static bool HandleDarkOwnerDraw(UINT message, WPARAM wParam, LPARAM lParam, INT_PTR* pResult) {
    if (!IsDarkModeEnabled()) return false;

    if (message == WM_MEASUREITEM) {
        MEASUREITEMSTRUCT* pmis = (MEASUREITEMSTRUCT*)lParam;
        if (pmis && pmis->CtlType == ODT_MENU && MeasureDarkMenuItem(pmis)) {
            *pResult = TRUE;
            return true;
        }
        return false;
    }

    if (message == WM_DRAWITEM) {
        DRAWITEMSTRUCT* pdis = (DRAWITEMSTRUCT*)lParam;
        if (!pdis) return false;
        if (pdis->CtlType == ODT_MENU && DrawDarkMenuItem(pdis)) {
            *pResult = TRUE;
            return true;
        }
        if (pdis->CtlType == ODT_BUTTON && DrawDarkOwnerButton(pdis)) {
            *pResult = TRUE;
            return true;
        }
    }
    return false;
}

// ===== 通用消息对话框（替代 MessageBox）=====
// 深色/浅色主题下都用这一个窗口，布局和 About 一致，按钮直接放在底部，
// 没有 MessageBox 那块独立的按钮区；配色随主题变化。
struct MessageDialogData {
    std::wstring caption;
    std::wstring text;
    UINT type;
};

// 对话框（关于 / 关闭确认 / 消息框）的深色处理：背景 + 静态文字 + 单选/复选
static bool HandleDialogDarkColor(HWND hDlg, UINT message, WPARAM wParam, INT_PTR* pResult) {
    if (!IsDarkModeEnabled()) return false;

    // 对话框底部等没有子控件盖住的区域由 WM_ERASEBKGND 负责，不处理就会露白
    if (message == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hDlg, &rc);
        FillRect((HDC)wParam, &rc, g_hDarkBgBrush);
        *pResult = 1;
        return true;
    }

    if (message != WM_CTLCOLORDLG && message != WM_CTLCOLORSTATIC) return false;

    HDC hdcDlg = (HDC)wParam;
    SetTextColor(hdcDlg, DARK_COLOR_TEXT);
    SetBkColor(hdcDlg, DARK_COLOR_BG);
    SetBkMode(hdcDlg, TRANSPARENT);
    *pResult = (INT_PTR)g_hDarkBgBrush;
    return true;
}

// 按"实际显示的像素大小"加载图标：先试 LoadIconWithScaleDown（Vista+，会挑最合适的尺寸再缩放），
// 不行再用 LoadImage 取同尺寸。这样才能和 Windows 10/11 自己显示的一样清晰，不会先取 32 再被拉伸发虚
static HICON LoadSizedIcon(LPCWSTR iconRes, int size) {
    HICON hIcon = NULL;

    HMODULE hComCtl = GetModuleHandleW(L"comctl32.dll");
    if (hComCtl) {
        typedef HRESULT(WINAPI* PFN_LoadIconWithScaleDown)(HINSTANCE, PCWSTR, int, int, HICON*);
        PFN_LoadIconWithScaleDown pLoadScale =
            (PFN_LoadIconWithScaleDown)GetProcAddress(hComCtl, "LoadIconWithScaleDown");
        if (pLoadScale && SUCCEEDED(pLoadScale(NULL, iconRes, size, size, &hIcon)) && hIcon) {
            return hIcon;
        }
    }

    return (HICON)LoadImage(NULL, iconRes, IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
}

// 对话框标题栏默认用的是系统通用图标，和主窗口不一致，这里统一设成程序图标
static void SetDialogAppIcon(HWND hDlg) {
    HICON hBig = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_MCADVANCEMENTSONWIN), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED);
    HICON hSmall = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_MCADVANCEMENTSONWIN), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED);
    if (hBig) SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hBig);
    if (hSmall) SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
}

INT_PTR CALLBACK MessageDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    INT_PTR darkResult = 0;
    if (HandleDialogDarkColor(hDlg, message, wParam, &darkResult)) {
        return darkResult;
    }
    if (message == WM_DRAWITEM &&
        HandleDarkOwnerDraw(message, wParam, lParam, &darkResult)) {
        return darkResult;
    }

    switch (message)
    {
    case WM_INITDIALOG: {
        MessageDialogData* data = (MessageDialogData*)lParam;
        if (!data) return (INT_PTR)TRUE;

        ApplyDarkWindowFrame(hDlg, IsDarkModeEnabled());
        MakeDarkOwnerDrawButton(hDlg, IDOK);
        MakeDarkOwnerDrawButton(hDlg, IDCANCEL);
        MakeDarkOwnerDrawButton(hDlg, IDYES);
        MakeDarkOwnerDrawButton(hDlg, IDNO);
        SetWindowText(hDlg, data->caption.c_str());

        HWND hText = GetDlgItem(hDlg, IDC_MSG_TEXT);
        HWND hIcon = GetDlgItem(hDlg, IDC_MSG_ICON);
        SetWindowText(hText, data->text.c_str());

        // 按钮文字在运行时设置，资源文件里保持纯 ASCII，避免中文编码问题
        SetDlgItemText(hDlg, IDOK, L"确定");
        SetDlgItemText(hDlg, IDCANCEL, L"取消");
        SetDlgItemText(hDlg, IDYES, L"是");
        SetDlgItemText(hDlg, IDNO, L"否");

        // 图标（MB_ICONERROR / MB_ICONWARNING / MB_ICONQUESTION / MB_ICONINFORMATION）
        // IDI_* 就是 Windows 自己那套消息图标（ Vista 起的扁平样式，Win10/11 沿用）
        int dpi = GetDpiForWindowSafe(hDlg);
        int iconSize = ScaleDpi(32, dpi);

        LPCWSTR iconRes = NULL;
        if (data->type & MB_ICONERROR) iconRes = IDI_ERROR;
        else if (data->type & MB_ICONWARNING) iconRes = IDI_WARNING;
        else if (data->type & MB_ICONQUESTION) iconRes = IDI_QUESTION;
        else if (data->type & MB_ICONINFORMATION) iconRes = IDI_INFORMATION;

        bool hasIcon = iconRes != NULL;
        if (hasIcon) {
            HICON hIco = LoadSizedIcon(iconRes, iconSize);
            if (hIco) {
                SendMessage(hIcon, STM_SETICON, (WPARAM)hIco, 0);
                SetProp(hDlg, L"MsgIcon", hIco);   // 对话框关闭时销毁
            }
            else {
                hasIcon = false;
                ShowWindow(hIcon, SW_HIDE);
            }
        }
        else {
            ShowWindow(hIcon, SW_HIDE);
        }

        // 标题栏图标也用程序图标
        SetDialogAppIcon(hDlg);

        // 量出文字需要多大（限宽按 DPI 缩放，超长自动换行）
        HDC hdc = GetDC(hDlg);
        HFONT hFont = (HFONT)SendMessage(hText, WM_GETFONT, 0, 0);
        HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
        RECT rcCalc = { 0, 0, ScaleDpi(320, dpi), 0 };
        DrawText(hdc, data->text.c_str(), -1, &rcCalc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        if (hOld) SelectObject(hdc, hOld);
        ReleaseDC(hDlg, hdc);

        int margin = ScaleDpi(14, dpi);
        int iconGap = ScaleDpi(12, dpi);
        int btnW = ScaleDpi(88, dpi);
        int btnH = ScaleDpi(26, dpi);
        int btnGap = ScaleDpi(8, dpi);

        int textW = rcCalc.right - rcCalc.left;
        int textH = rcCalc.bottom - rcCalc.top;
        int textLeft = margin + (hasIcon ? iconSize + iconGap : 0);

        bool yesNo = (data->type & MB_YESNO) != 0;
        bool okCancel = (data->type & MB_OKCANCEL) != 0;
        int btnCount = (yesNo || okCancel) ? 2 : 1;

        int clientW = textLeft + textW + margin;
        int minClientW = btnCount * btnW + (btnCount - 1) * btnGap + margin * 2;
        if (clientW < minClientW) clientW = minClientW;
        if (clientW > ScaleDpi(560, dpi)) clientW = ScaleDpi(560, dpi);
        int clientH = margin + textH + ScaleDpi(18, dpi) + btnH + margin;

        // 用当前窗口的边框厚度换算外框尺寸，避免 AdjustWindowRect 在高 DPI 下的偏差
        RECT rcWin, rcClient;
        GetWindowRect(hDlg, &rcWin);
        GetClientRect(hDlg, &rcClient);
        int frameW = (rcWin.right - rcWin.left) - (rcClient.right - rcClient.left);
        int frameH = (rcWin.bottom - rcWin.top) - (rcClient.bottom - rcClient.top);

        int posX = rcWin.left;
        int posY = rcWin.top;
        if (g_hMainWnd && IsWindow(g_hMainWnd)) {   // 居中于主窗口
            RECT rcOwner;
            if (GetWindowRect(g_hMainWnd, &rcOwner)) {
                posX = rcOwner.left + ((rcOwner.right - rcOwner.left) - (clientW + frameW)) / 2;
                posY = rcOwner.top + ((rcOwner.bottom - rcOwner.top) - (clientH + frameH)) / 2;
            }
        }
        SetWindowPos(hDlg, NULL, posX, posY, clientW + frameW, clientH + frameH,
            SWP_NOZORDER | SWP_NOACTIVATE);

        if (hasIcon) {
            MoveWindow(hIcon, margin, margin, iconSize, iconSize, TRUE);
        }
        MoveWindow(hText, textLeft, margin, textW, textH, TRUE);

        int btnY = clientH - margin - btnH;
        int rightX = clientW - margin - btnW;
        int ids[4] = { IDOK, IDCANCEL, IDYES, IDNO };
        for (int i = 0; i < 4; ++i) {
            ShowWindow(GetDlgItem(hDlg, ids[i]), SW_HIDE);
        }

        int defId;
        if (yesNo) {
            MoveWindow(GetDlgItem(hDlg, IDNO), rightX, btnY, btnW, btnH, TRUE);
            MoveWindow(GetDlgItem(hDlg, IDYES), rightX - btnW - btnGap, btnY, btnW, btnH, TRUE);
            ShowWindow(GetDlgItem(hDlg, IDNO), SW_SHOW);
            ShowWindow(GetDlgItem(hDlg, IDYES), SW_SHOW);
            defId = (data->type & MB_DEFBUTTON2) ? IDNO : IDYES;
        }
        else if (okCancel) {
            MoveWindow(GetDlgItem(hDlg, IDCANCEL), rightX, btnY, btnW, btnH, TRUE);
            MoveWindow(GetDlgItem(hDlg, IDOK), rightX - btnW - btnGap, btnY, btnW, btnH, TRUE);
            ShowWindow(GetDlgItem(hDlg, IDCANCEL), SW_SHOW);
            ShowWindow(GetDlgItem(hDlg, IDOK), SW_SHOW);
            defId = (data->type & MB_DEFBUTTON2) ? IDCANCEL : IDOK;
        }
        else {
            MoveWindow(GetDlgItem(hDlg, IDOK), rightX, btnY, btnW, btnH, TRUE);
            ShowWindow(GetDlgItem(hDlg, IDOK), SW_SHOW);
            defId = IDOK;
        }

        SendMessage(hDlg, DM_SETDEFID, defId, 0);
        SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, defId), TRUE);
        return (INT_PTR)FALSE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK || id == IDCANCEL || id == IDYES || id == IDNO) {
            EndDialog(hDlg, id);
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return (INT_PTR)TRUE;

    case WM_DESTROY: {
        HICON hIco = (HICON)GetProp(hDlg, L"MsgIcon");
        if (hIco) {
            DestroyIcon(hIco);
            RemoveProp(hDlg, L"MsgIcon");
        }
        break;
    }
    }
    return (INT_PTR)FALSE;
}

// 两种主题下都用它代替 MessageBox，返回值与 MessageBox 一致（IDOK/IDYES/IDNO/IDCANCEL）
static int ShowMessageDialog(HWND hOwner, const std::wstring& text, const std::wstring& caption, UINT type) {
    EnsureDarkBrushes();

    MessageDialogData data;
    data.caption = caption;
    data.text = text;
    data.type = type;

    INT_PTR result = DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_MESSAGE), hOwner,
        MessageDialogProc, (LPARAM)&data);
    if (result <= 0) {
        return MessageBox(hOwner, text.c_str(), caption.c_str(), type);   // 创建失败时兜底
    }
    return (int)result;
}

// 系统托盘（通知区域）相关
#define WM_TRAYICON (WM_USER + 200)

NOTIFYICONDATAW g_nid = { 0 };
UINT g_uTaskbarRestart = 0;

void AddTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_MCADVANCEMENTSONWIN),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MCADVANCEMENTSONWIN));
    }
    wcscpy_s(g_nid.szTip, _countof(g_nid.szTip), L"MC Advancements on Windows");
    BOOL bAdded = Shell_NotifyIcon(NIM_ADD, &g_nid);
    if (bAdded) {
        OutputDebugString(L"[Tray] NIM_ADD 成功，托盘图标已创建\n");
    }
    else {
        wchar_t buf[128];
        swprintf_s(buf, L"[Tray] NIM_ADD 失败, GetLastError=%lu\n", GetLastError());
        OutputDebugString(buf);
    }
}

void RemoveTrayIcon() {
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = NULL;
    }
}

void ShowTrayMenu(HWND hWnd) {
    static bool bShowing = false;
    if (bShowing) return;
    bShowing = true;

    HMENU hMenu = LoadMenu(hInst, MAKEINTRESOURCE(IDR_TRAY_MENU));
    if (!hMenu) {
        OutputDebugString(L"[Tray] LoadMenu 失败，菜单资源未找到\n");
        bShowing = false;
        return;
    }

    HMENU hSubMenu = GetSubMenu(hMenu, 0);
    if (!hSubMenu) {
        OutputDebugString(L"[Tray] GetSubMenu 失败，菜单结构错误\n");
        DestroyMenu(hMenu);
        bShowing = false;
        return;
    }

    if (g_pSettingsManager) {
        CheckMenuItem(hSubMenu, ID_TRAY_SOUND,
            g_pSettingsManager->IsSoundEnabled() ? MF_CHECKED : MF_UNCHECKED);
    }

    // 深色模式：给这个临时菜单也设上自绘（用 Win11 弹出菜单配色）
    if (IsDarkModeEnabled()) {
        EnsureDarkBrushes();
        FillMenuItemDrawInfo(hMenu, 0);
        SetMenuItemOwnerDrawType(hMenu, true);
        SetMenuDarkBackground(hMenu, g_hDarkMenuPopupBrush);
    }

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hSubMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        pt.x, pt.y, 0, hWnd, NULL);
    PostMessage(hWnd, WM_NULL, 0, 0);

    if (IsDarkModeEnabled()) {
        FreeMenuItemDrawInfo(hMenu);   // 释放 new 出来的项数据
    }
    DestroyMenu(hMenu);

    bShowing = false;
}

// 显示/隐藏主窗口：可见且未最小化时隐藏，否则恢复并置前
static void ToggleMainWindow(HWND hWnd) {
    if (IsWindowVisible(hWnd) && !IsIconic(hWnd)) {
        ShowWindow(hWnd, SW_HIDE);
    }
    else {
        ShowWindow(hWnd, SW_RESTORE);
        SetForegroundWindow(hWnd);
    }
}

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK NotificationWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK DownloadWndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK about(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK CloseConfirmProc(HWND, UINT, WPARAM, LPARAM);
void RestartApplication();

bool DownloadAdvancementJson(HWND hWnd);
void ShowDownloadWindow(HWND hParent);
void CloseDownloadWindow();
void UpdateDownloadProgress(int progress, const std::wstring& status);

std::wstring ExtractJSONVersion(const std::string& jsonContent);

bool IsNewerVersion(const std::wstring& currentVersion, const std::wstring& newVersion);

void CancelDownload();

void ProcessAchievementQueue();
void AddAchievementToQueue(const Advancement& adv);
void ShowNextAchievement(HWND hMainWnd);

bool PlayAudioFile(const std::wstring& filePath) {
    if (g_pSettingsManager && !g_pSettingsManager->IsSoundEnabled()) {
        return false;
    }

    if (GetFileAttributes(filePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    static int audioCounter = 0;
    audioCounter++;
    std::wstring alias = L"myaudio" + std::to_wstring(audioCounter);

    if (audioCounter > 100) {
        audioCounter = 0;
    }

    std::wstring openCmd = L"open \"" + filePath + L"\" type mpegvideo alias " + alias;
    if (mciSendString(openCmd.c_str(), NULL, 0, NULL) != 0) {
        openCmd = L"open \"" + filePath + L"\" type waveaudio alias " + alias;
        if (mciSendString(openCmd.c_str(), NULL, 0, NULL) != 0) {
            return false;
        }
    }

    std::wstring playCmd = L"play " + alias;
    mciSendString(playCmd.c_str(), NULL, 0, NULL);

    std::thread([alias]() {
        std::wstring statusCmd = L"status " + alias + L" mode";
        wchar_t status[256] = { 0 };

        for (int i = 0; i < 50; i++) {
            if (mciSendString(statusCmd.c_str(), status, 256, NULL) == 0) {
                if (std::wstring(status) == L"stopped" || std::wstring(status) == L"not ready") {
                    break;
                }
            }
            Sleep(100);
        }

        std::wstring closeCmd = L"close " + alias;
        mciSendString(closeCmd.c_str(), NULL, 0, NULL);
        }).detach();

    return true;
}

void AddAchievementToQueue(const Advancement& adv) {
    OutputDebugString(L"AddAchievementToQueue called\n");

    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_achievementQueue.push(adv);

    wchar_t debugMsg[256];
    swprintf_s(debugMsg, L"成就已添加到队列: %s\n", adv.title.c_str());
    OutputDebugString(debugMsg);

    if (!g_showingNotification) {
        OutputDebugString(L"没有通知显示，立即处理\n");
        if (g_hMainWnd) {
            PostMessage(g_hMainWnd, WM_USER + 105, 0, 0);
        }
        else {
            OutputDebugString(L"错误: 主窗口句柄为空！\n");
        }
    }
    else {
        OutputDebugString(L"等待当前通知结束\n");
    }
}

void ProcessAchievementQueue() {
    OutputDebugString(L"ProcessAchievementQueue called\n");

    if (g_showingNotification) {
        OutputDebugString(L"已经有通知在显示，跳过\n");
        return;
    }

    std::lock_guard<std::mutex> lock(g_queueMutex);
    OutputDebugString(L"队列大小: 需要检查\n");

    if (!g_achievementQueue.empty()) {
        g_showingNotification = true;
        Advancement adv = g_achievementQueue.front();
        g_achievementQueue.pop();

        wchar_t debugMsg[256];
        swprintf_s(debugMsg, L"从队列取出成就: %s\n", adv.title.c_str());
        OutputDebugString(debugMsg);

        if (g_pAdvManager) {
            OutputDebugString(L"调用ShowAdvancementNotification\n");
            g_pAdvManager->ShowAdvancementNotification(adv);
        }
        else {
            OutputDebugString(L"错误: g_pAdvManager为空！\n");
        }
    }
    else {
        OutputDebugString(L"队列为空\n");
    }
}

void ShowNextAchievement(HWND hMainWnd) {
    ProcessAchievementQueue();
}

std::wstring Trim(const std::wstring& str) {
    size_t first = str.find_first_not_of(L" \t\n\r");
    if (std::wstring::npos == first) {
        return L"";
    }
    size_t last = str.find_last_not_of(L" \t\n\r");
    return str.substr(first, (last - first + 1));
}

std::wstring UTF8ToWString(const std::string& utf8) {
    if (utf8.empty()) return L"";

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::vector<BYTE> Base64Decode(const std::string& encoded) {
    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::vector<BYTE> decoded;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;

    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(static_cast<BYTE>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return decoded;
}

Gdiplus::Bitmap* BitmapFromBase64DataURI(const std::wstring& dataUri) {
    if (dataUri.empty()) return nullptr;

    const std::wstring base64Prefix = L"base64,";
    size_t base64Pos = dataUri.find(base64Prefix);
    std::string base64Data;
    if (base64Pos != std::wstring::npos) {
        std::wstring encoded = dataUri.substr(base64Pos + base64Prefix.length());
        base64Data.reserve(encoded.size());
        for (wchar_t wc : encoded) {
            base64Data.push_back(static_cast<char>(wc));
        }
    }
    else {
        base64Data.reserve(dataUri.size());
        for (wchar_t wc : dataUri) {
            base64Data.push_back(static_cast<char>(wc));
        }
    }

    base64Data.erase(std::remove_if(base64Data.begin(), base64Data.end(),
        [](char c) { return c == '\n' || c == '\r' || c == ' '; }), base64Data.end());

    if (base64Data.empty()) return nullptr;

    std::vector<BYTE> imageData = Base64Decode(base64Data);
    if (imageData.empty()) return nullptr;

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, imageData.size());
    if (!hGlobal) return nullptr;

    void* pBuffer = GlobalLock(hGlobal);
    if (!pBuffer) {
        GlobalFree(hGlobal);
        return nullptr;
    }
    memcpy(pBuffer, imageData.data(), imageData.size());
    GlobalUnlock(hGlobal);

    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hGlobal, TRUE, &pStream) != S_OK) {
        GlobalFree(hGlobal);
        return nullptr;
    }

    Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream);
    pStream->Release();

    if (pBitmap && pBitmap->GetLastStatus() != Gdiplus::Ok) {
        delete pBitmap;
        return nullptr;
    }

    return pBitmap;
}

std::string ReadFileAsUTF8(const std::wstring& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    char bom[3] = { 0 };
    file.read(bom, 3);

    bool hasBOM = (bom[0] == (char)0xEF && bom[1] == (char)0xBB && bom[2] == (char)0xBF);

    file.seekg(0, std::ios::beg);
    if (hasBOM) {
        file.seekg(3, std::ios::beg);
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    return content;
}

std::wstring ExtractJSONValue(const std::string& line, const std::string& key) {
    std::string searchStr = "\"" + key + "\":";
    size_t pos = line.find(searchStr);
    if (pos == std::string::npos) {
        return L"";
    }

    pos += searchStr.length();

    while (pos < line.length() && (line[pos] == ' ' || line[pos] == '\t')) {
        pos++;
    }

    if (pos >= line.length() || line[pos] != '\"') {
        return L"";
    }

    pos++;

    std::string value;
    while (pos < line.length() && line[pos] != '\"') {
        if (line[pos] == '\\' && pos + 1 < line.length()) {
            value += line[pos];
            pos++;
            value += line[pos];
        }
        else {
            value += line[pos];
        }
        pos++;
    }

    return UTF8ToWString(value);
}

std::wstring ExtractJSONVersion(const std::string& jsonContent) {
    std::string searchStr = "\"version\":";
    size_t pos = jsonContent.find(searchStr);
    if (pos == std::string::npos) {
        return L"";
    }

    pos += searchStr.length();

    while (pos < jsonContent.length() && (jsonContent[pos] == ' ' || jsonContent[pos] == '\t' || jsonContent[pos] == '\n' || jsonContent[pos] == '\r')) {
        pos++;
    }

    if (pos >= jsonContent.length() || jsonContent[pos] != '\"') {
        return L"";
    }

    pos++;

    std::string value;
    while (pos < jsonContent.length() && jsonContent[pos] != '\"') {
        if (jsonContent[pos] == '\\' && pos + 1 < jsonContent.length()) {
            value += jsonContent[pos];
            pos++;
            value += jsonContent[pos];
        }
        else {
            value += jsonContent[pos];
        }
        pos++;
    }

    return UTF8ToWString(value);
}

bool IsNewerVersion(const std::wstring& currentVersion, const std::wstring& newVersion) {
    if (currentVersion.empty()) return true;

    struct tm currentTm = {}, newTm = {};
    std::wistringstream currentStream(currentVersion);
    std::wistringstream newStream(newVersion);

    currentStream >> std::get_time(&currentTm, L"%Y/%m/%d %H:%M");
    newStream >> std::get_time(&newTm, L"%Y/%m/%d %H:%M");

    if (currentStream.fail() || newStream.fail()) {
        return newVersion > currentVersion;
    }

    time_t currentTime = mktime(&currentTm);
    time_t newTime = mktime(&newTm);

    return difftime(newTime, currentTime) > 0;
}

bool IsJSONFileValid(const std::wstring& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    char bom[3] = { 0 };
    file.read(bom, 3);
    bool hasBOM = (bom[0] == (char)0xEF && bom[1] == (char)0xBB && bom[2] == (char)0xBF);

    file.seekg(0, std::ios::beg);
    if (hasBOM) {
        file.seekg(3, std::ios::beg);
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    if (content.empty()) {
        return false;
    }

    return (content.find("\"adv_list_info\"") != std::string::npos &&
        content.find("\"achievements\"") != std::string::npos &&
        content.find("\"id\"") != std::string::npos &&
        content.find("\"title\"") != std::string::npos);
}

// 安全地关闭 WinINet 句柄：用原子交换保证只有一个线程真正执行关闭，
// 这样 UI 线程可以关掉句柄来打断工作线程里阻塞的 InternetReadFile
static void CloseInetHandle(HINTERNET& handle) {
    HINTERNET h = (HINTERNET)InterlockedExchangePointer((PVOID*)&handle, NULL);
    if (h) {
        InternetCloseHandle(h);
    }
}

void CancelDownload() {
    if (g_bDownloading) {
        g_bDownloadCanceled = true;

        // 不要在这里等待工作线程：g_bDownloading 由主线程的 WM_USER+104 清除，
        // 在消息处理里忙等会把它自己卡死，只能等到超时才关窗
        CloseInetHandle(g_hUrl);
        CloseInetHandle(g_hInternet);

        CloseDownloadWindow();
    }
}

AdvancementManager::AdvancementManager(HWND hWnd) : hMainWnd(hWnd), monitoring(false) {
    WCHAR path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);
    std::wstring exePath = path;
    size_t pos = exePath.find_last_of(L"\\/");

    std::wstring exeDir = exePath.substr(0, pos);
    saveFilePath = exeDir + L"\\adv_save.txt";
    jsonFilePath = exeDir + L"\\bin\\adv.json";

    advancements.clear();
    version = L"";
}

AdvancementManager::~AdvancementManager() {
    StopMonitoring();
}

bool AdvancementManager::LoadAdvancementsFromJSON() {
    advancements.clear();
    version = L"";

    if (GetFileAttributes(jsonFilePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring errorMsg = L"找不到成就配置文件！\n请确保以下文件存在：\n" + jsonFilePath;
        ShowMessageDialog(hMainWnd, errorMsg.c_str(), L"错误", MB_ICONERROR | MB_OK);
        return false;
    }

    std::string jsonContent = ReadFileAsUTF8(jsonFilePath);
    if (jsonContent.empty()) {
        ShowMessageDialog(hMainWnd, L"JSON文件为空或读取失败！", L"错误", MB_ICONERROR | MB_OK);
        return false;
    }

    version = ExtractJSONVersion(jsonContent);
    if (version.empty()) {
        version = L"未知版本";
    }

    std::istringstream jsonStream(jsonContent);
    std::string line;
    bool inAchievementsArray = false;
    bool inObject = false;
    Advancement currentAdv;
    int braceDepth = 0;

    while (std::getline(jsonStream, line)) {
        std::string trimmedLine = line;
        trimmedLine.erase(std::remove(trimmedLine.begin(), trimmedLine.end(), ' '), trimmedLine.end());
        trimmedLine.erase(std::remove(trimmedLine.begin(), trimmedLine.end(), '\t'), trimmedLine.end());

        if (!inAchievementsArray && trimmedLine.find("\"achievements\":[") != std::string::npos) {
            inAchievementsArray = true;
            continue;
        }

        if (inAchievementsArray) {
            if (!inObject && trimmedLine.find('{') != std::string::npos) {
                inObject = true;
                currentAdv = Advancement();
                continue;
            }

            if (inObject) {
                std::wstring value;

                if ((value = ExtractJSONValue(line, "num")) != L"") {
                    currentAdv.num = value;
                }
                else if ((value = ExtractJSONValue(line, "id")) != L"") {
                    currentAdv.id = value;
                }
                else if ((value = ExtractJSONValue(line, "title")) != L"") {
                    currentAdv.title = value;
                }
                else if ((value = ExtractJSONValue(line, "description")) != L"") {
                    currentAdv.description = value;
                }
                else if ((value = ExtractJSONValue(line, "trigger_description")) != L"") {
                    currentAdv.triggerDescription = value;
                }
                else if ((value = ExtractJSONValue(line, "trigger_value")) != L"") {
                    currentAdv.triggerValue = value;
                }
                else if ((value = ExtractJSONValue(line, "trigger_type")) != L"") {
                    if (value == L"window_title") {
                        currentAdv.triggerType = TRIGGER_WINDOW_TITLE;
                    }
                    else if (value == L"process_name") {
                        currentAdv.triggerType = TRIGGER_PROCESS_NAME;
                    }
                    else {
                        currentAdv.triggerType = TRIGGER_NONE;
                    }
                }
                else if ((value = ExtractJSONValue(line, "icon_base64")) != L"") {
                    currentAdv.iconBase64 = value;
                }

                if (trimmedLine.find('}') != std::string::npos) {
                    inObject = false;

                    if (!currentAdv.id.empty()) {
                        currentAdv.completed = false;
                        advancements.push_back(currentAdv);
                    }
                }
            }

            if (trimmedLine.find(']') != std::string::npos) {
                inAchievementsArray = false;
                break;
            }
        }
    }

    if (advancements.empty()) {
        ShowMessageDialog(hMainWnd, L"JSON解析失败或没有找到成就配置！", L"错误", MB_ICONERROR | MB_OK);
        return false;
    }

    return true;
}

bool AdvancementManager::CheckWindowTitle(const std::wstring& targetTitle) {
    if (targetTitle.empty()) return false;

    std::vector<std::wstring> keywords;
    size_t start = 0, end = 0;
    while ((end = targetTitle.find(L'|', start)) != std::wstring::npos) {
        std::wstring keyword = targetTitle.substr(start, end - start);
        if (!keyword.empty()) {
            keywords.push_back(keyword);
        }
        start = end + 1;
    }
    std::wstring lastKeyword = targetTitle.substr(start);
    if (!lastKeyword.empty()) {
        keywords.push_back(lastKeyword);
    }

    struct EnumData {
        const std::vector<std::wstring>* keywords;
        bool found;
    } enumData = { &keywords, false };

    auto enumProc = [](HWND hwnd, LPARAM lParam) -> BOOL {
        EnumData* data = reinterpret_cast<EnumData*>(lParam);

        if (!IsWindowVisible(hwnd)) {
            return TRUE;
        }

        wchar_t title[256];
        if (GetWindowTextW(hwnd, title, 256) > 0) {
            std::wstring windowTitle = title;

            for (const auto& keyword : *(data->keywords)) {
                if (windowTitle.find(keyword) != std::wstring::npos) {
                    data->found = true;
                    return FALSE;
                }
            }
        }

        return TRUE;
        };

    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&enumData));

    return enumData.found;
}

bool AdvancementManager::CheckProcessExists(const std::wstring& processName) {
    if (processName.empty()) return false;

    std::vector<std::wstring> names;
    size_t start = 0, end = 0;
    while ((end = processName.find(L'|', start)) != std::wstring::npos) {
        std::wstring name = processName.substr(start, end - start);
        if (!name.empty()) {
            names.push_back(name);
        }
        start = end + 1;
    }
    std::wstring lastName = processName.substr(start);
    if (!lastName.empty()) {
        names.push_back(lastName);
    }

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            for (const auto& name : names) {
                if (_wcsicmp(pe32.szExeFile, name.c_str()) == 0) {
                    CloseHandle(hSnapshot);
                    return true;
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return false;
}

void AdvancementManager::LoadAdvancements() {
    completedAdvancements.clear();

    if (GetFileAttributes(saveFilePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    std::wifstream file(saveFilePath);
    if (file.is_open()) {
        std::wstring line;
        while (std::getline(file, line)) {
            size_t pos = line.find(L'=');
            if (pos != std::wstring::npos) {
                std::wstring id = line.substr(0, pos);
                std::wstring value = line.substr(pos + 1);
                completedAdvancements[id] = (value == L"done");
            }
        }
        file.close();
    }

    for (auto& adv : advancements) {
        adv.completed = completedAdvancements[adv.id];
    }
}

void AdvancementManager::SaveAdvancements() {
    std::wofstream file(saveFilePath);
    if (file.is_open()) {
        for (const auto& adv : advancements) {
            if (adv.completed) {
                file << adv.id << L"=done" << std::endl;
            }
        }
        file.close();
    }
}

void AdvancementManager::TriggerAdvancement(const std::wstring& id) {
    for (auto& adv : advancements) {
        if (adv.id == id && !adv.completed) {
            adv.completed = true;
            completedAdvancements[id] = true;
            SaveAdvancements();
            UpdateLists();

            AddAchievementToQueue(adv);
            OutputDebugString(L"成就触发: ");
            OutputDebugString(adv.title.c_str());
            OutputDebugString(L"\n");
            break;
        }
    }
}

// ===== 成就通知：带 per-pixel alpha 的合成 =====
// 以前用 SetLayeredWindowAttributes(LWA_ALPHA)：整窗统一 90% 不透明，
// adv_back.png 四个角的透明像素也被一起涂成不透明，圆角等于白做。
// 改成自己合成一张 32 位 ARGB 位图再用 UpdateLayeredWindow 贴上去后，
// 每个像素保留自己的 Alpha，角上 alpha=0 的地方就真的透明了。

// GDI 的 CreateFont 传的是"字符单元格高度"，GDI+ 的 Font 传的是 em 高度，
// 二者差一个 (ascent+descent)/em，不换算的话换成 GDI+ 后字会明显变大
static Gdiplus::REAL EmSizeFromCellHeight(const Gdiplus::FontFamily& family, int style, int cellHeight) {
    UINT16 em = family.GetEmHeight(style);
    UINT16 ascent = family.GetCellAscent(style);
    UINT16 descent = family.GetCellDescent(style);
    if (em == 0 || ascent + descent == 0) return (Gdiplus::REAL)cellHeight;
    return (Gdiplus::REAL)cellHeight * (Gdiplus::REAL)em / (Gdiplus::REAL)(ascent + descent);
}

static HBITMAP ComposeNotification(NotificationData* pData, int width, int height, int dpi) {
    if (!pData || width <= 0 || height <= 0) return NULL;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;      // 负值=自上而下，与 GDI+ 的行序一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HBITMAP hDib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hDib || !pBits) {
        if (hDib) DeleteObject(hDib);
        return NULL;
    }
    memset(pBits, 0, (size_t)width * height * 4);   // 起始：全透明

    // PARGB：告诉 GDI+ 这块内存按"预乘 Alpha"存放，正是 UpdateLayeredWindow 要求的格式
    Gdiplus::Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB, (BYTE*)pBits);
    Gdiplus::Graphics g(&surface);

    // 底图（四个角的透明度来自 PNG 本身）
    if (pData->pBitmap && pData->pBitmap->GetLastStatus() == Gdiplus::Ok) {
        g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
        g.DrawImage(pData->pBitmap, 0, 0, width, height);
    }
    else {
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0, 100, 0));
        g.FillRectangle(&brush, 0, 0, width, height);
        Gdiplus::Pen pen(Gdiplus::Color(255, 255, 215, 0), 2.0f);
        g.DrawRectangle(&pen, 1, 1, width - 2, height - 2);
    }

    if (!pData->pAdv) return hDib;

    // 图标
    int iconAreaWidth = 0;
    int iconPadding = width / 24;
    if (pData->pIconBitmap && pData->pIconBitmap->GetLastStatus() == Gdiplus::Ok) {
        int iconSize = height * 50 / 100;
        iconAreaWidth = iconSize + iconPadding;
        int iconX = iconPadding;
        int iconY = (height - iconSize) / 2;

        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQuality);
        g.DrawImage(pData->pIconBitmap, iconX, iconY, iconSize, iconSize);
    }

    // 字体：优先 bin\mc_fonts.ttf。GDI+ 私有字体集即可，不再需要 AddFontResourceEx
    Gdiplus::FontFamily defaultFamily(L"微软雅黑");
    Gdiplus::PrivateFontCollection fontCollection;
    Gdiplus::FontFamily customFamily;
    Gdiplus::FontFamily* pFamily = &defaultFamily;

    if (pData->pFontPath && GetFileAttributes(pData->pFontPath->c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (fontCollection.AddFontFile(pData->pFontPath->c_str()) == Gdiplus::Ok) {
            int numFound = 0;
            fontCollection.GetFamilies(1, &customFamily, &numFound);
            if (numFound > 0) {
                pFamily = &customFamily;
                WCHAR familyName[LF_FACESIZE] = { 0 };
                customFamily.GetFamilyName(familyName, LANG_NEUTRAL);
                wchar_t debugMsg[512];
                swprintf_s(debugMsg, L"使用自定义字体: %s\n", familyName);
                OutputDebugString(debugMsg);
            }
        }
        else {
            OutputDebugString(L"加载字体文件失败\n");
        }
    }

    const int fontStyle = Gdiplus::FontStyleBold;
    Gdiplus::Font baseFont(pFamily, EmSizeFromCellHeight(*pFamily, fontStyle, height * 21 / 100), fontStyle, Gdiplus::UnitPixel);
    Gdiplus::Font advFont(pFamily, EmSizeFromCellHeight(*pFamily, fontStyle, height * 40 / 100), fontStyle, Gdiplus::UnitPixel);

    int textLeft = iconAreaWidth + iconPadding;
    int padding = width / 24;

    Gdiplus::StringFormat sf(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetAlignment(Gdiplus::StringAlignmentNear);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);   // 对应原来的 DT_VCENTER

    // 透明背景上不能用 ClearType（需要不透明底色），只能用灰度抗锯齿
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::SolidBrush goldBrush(Gdiplus::Color(255, 255, 215, 0));

    Gdiplus::RectF rcTitle((Gdiplus::REAL)textLeft, (Gdiplus::REAL)(height * 15 / 100),
        (Gdiplus::REAL)(width - padding - textLeft), (Gdiplus::REAL)(height * 27 / 100));
    g.DrawString(L"获得成就", -1, &baseFont, rcTitle, &sf, &goldBrush);

    std::wstring displayTitle = pData->pAdv->title;
    int charWidth = ScaleDpi(13, dpi);
    int maxTitleLength = (width - textLeft) / (charWidth > 0 ? charWidth : 13);
    if (maxTitleLength < 5) maxTitleLength = 5;
    if (displayTitle.length() > (size_t)maxTitleLength) {
        displayTitle = displayTitle.substr(0, maxTitleLength) + L"...";
    }

    Gdiplus::RectF rcAdv((Gdiplus::REAL)textLeft, (Gdiplus::REAL)(height * 40 / 100),
        (Gdiplus::REAL)(width - padding - textLeft), (Gdiplus::REAL)(height * 50 / 100));
    g.DrawString(displayTitle.c_str(), -1, &advFont, rcAdv, &sf, &goldBrush);       //成就名称

    return hDib;
}

// 把合成好的位图贴到分层窗口上（位置与尺寸都是物理像素）
static void UpdateNotificationLayer(HWND hWnd) {
    NotificationData* pData = (NotificationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (!pData || !pData->hDib) return;

    RECT rc;
    GetWindowRect(hWnd, &rc);

    BITMAP bm;
    ZeroMemory(&bm, sizeof(bm));
    GetObject(pData->hDib, sizeof(bm), &bm);

    POINT ptSrc = { 0, 0 };
    POINT ptDst = { rc.left, rc.top };
    SIZE sizeWnd = { bm.bmWidth, bm.bmHeight };

    BLENDFUNCTION bf;
    ZeroMemory(&bf, sizeof(bf));
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 230;    // 原 LWA_ALPHA 那档整窗淡化，现在只负责整体透明度
    bf.AlphaFormat = AC_SRC_ALPHA;   // 每个像素用自己的 Alpha

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, pData->hDib);

    if (!UpdateLayeredWindow(hWnd, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA)) {
        wchar_t debugMsg[256];
        swprintf_s(debugMsg, L"[通知] UpdateLayeredWindow 失败, GetLastError=%lu\n", GetLastError());
        OutputDebugString(debugMsg);
    }

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void AdvancementManager::ShowAdvancementNotification(const Advancement& adv) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = NotificationWndProc;
    wc.hInstance = hInst;
    // 允许点击唤出主界面时用“手型”光标提示可点击
    bool notifyClickable = (g_pSettingsManager != nullptr && g_pSettingsManager->IsClickNotifyToShow());
    wc.hCursor = LoadCursor(nullptr, notifyClickable ? IDC_HAND : IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpszClassName = L"AdvancementNotification";

    if (UnregisterClass(L"AdvancementNotification", hInst)) {
        OutputDebugString(L"旧窗口类已注销\n");
    }
    if (RegisterClassEx(&wc)) {
        OutputDebugString(L"窗口类注册成功\n");
    } else {
        DWORD error = GetLastError();
        wchar_t debugMsg[256];
        swprintf_s(debugMsg, L"窗口类注册失败，错误代码: %d\n", error);
        OutputDebugString(debugMsg);
    }

    // 用窗口所在显示器的矩形（物理像素）而不是 SM_CXSCREEN，
    // 这样多显示器不同 DPI 时，通知仍能贴着正确屏幕的右上角
    NotifyMetrics nm = CalcNotifyMetrics();
    int windowHeight = nm.windowHeight;
    int windowWidth = nm.windowWidth;

    int verticalSpacing = ScaleDpi(10, nm.dpi);
    int currentCount = g_notificationCount.load();
    int yPos = nm.top + (windowHeight + verticalSpacing) * currentCount;

    if (yPos + windowHeight > nm.bottom) {
        yPos = nm.top;
    }

    g_notificationCount++;

    std::wstring fontPath;
    WCHAR exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);
    std::wstring exeDir = std::wstring(exePath).substr(0, std::wstring(exePath).find_last_of(L"\\/"));
    fontPath = exeDir + L"\\bin\\mc_fonts.ttf";

    bool useCustomFont = (GetFileAttributes(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (useCustomFont) {
        wchar_t debugMsg[512];
        swprintf_s(debugMsg, L"找到字体文件: %s\n", fontPath.c_str());
        OutputDebugString(debugMsg);
    } else {
        OutputDebugString(L"未找到自定义字体文件，使用默认字体\n");
    }

    HWND hNotifWnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"AdvancementNotification",
        L"Achievement",
        WS_POPUP,
        nm.right, yPos, windowWidth, windowHeight,   // 从屏幕右侧外面滑入
        NULL, NULL, hInst, NULL
    );

    if (!hNotifWnd) {
        DWORD error = GetLastError();
        wchar_t errorMsg[256];
        swprintf_s(errorMsg, L"创建通知窗口失败，错误代码: %d\n", error);
        OutputDebugString(errorMsg);
        return;
    }

    OutputDebugString(L"通知窗口创建成功\n");

    NotificationData* pData = new NotificationData();
    pData->pAdv = new Advancement(adv);

    WCHAR bgPath[MAX_PATH];
    GetModuleFileName(NULL, bgPath, MAX_PATH);
    std::wstring bgExeDir = std::wstring(bgPath).substr(0, std::wstring(bgPath).find_last_of(L"\\/"));
    std::wstring bgFile = bgExeDir + L"\\bin\\adv_back.png";
    pData->pBitmap = Gdiplus::Bitmap::FromFile(bgFile.c_str());

    if (!adv.iconBase64.empty()) {
        pData->pIconBitmap = BitmapFromBase64DataURI(adv.iconBase64);
    }
    else {
        pData->pIconBitmap = BitmapFromBase64DataURI(
            L"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABAAgMAAADXB5lNAAAADFBMVEUAAAD/8gD///////8x82RJAAAABHRSTlMA/yA8t+AKUgAAATFJREFUeJyN0zFyxCAMBVCGztxDBQeSsuM0W+4xcgn6NJ5JXOSM0UdgJFdL4fF+kHgLuynZqK8Ux7Hfgibxc27yFQPmGBTm7xBszM8QVOa4LzE/IkNahDSJkNz2I0AyPyhACr9qgGz83AKk8vkXIMS/nwFyyPERIE0oQJRB4iFgiIcog8RD1EDiITpJUhxEy0myg+gGGjiIEkjc1WBOgwVBtQYLgv4aLAgEGiwIpjRYEBRrsCBoj+CCAKDBBekzCCak1yKYkN4dwYT0/RFMSJ9AMCG9tO7pgvTm5SddkHUO9uZOyiDuLA3iTtsg7j7s1d2YLbbCfF7trHXB0za0zWtfh/fBsEKsHgwL8BwM6p2w42DUfUIGA9923KH/jaNh+BdgyyanGwppHIYkvo03gnuPfzaToGvt4mQmAAAAAElFTkSuQmCC"
        );   //没有有效的icon_base64就用它 ↑
    }

    if (useCustomFont) {
        pData->pFontPath = new std::wstring(fontPath);
    } else {
        pData->pFontPath = nullptr;
    }

    SetWindowLongPtr(hNotifWnd, GWLP_USERDATA, (LONG_PTR)pData);

    // 合成一次带 Alpha 的内容并贴到分层窗口上。之后滑入/滑出只移动窗口，位图不需要重画
    pData->hDib = ComposeNotification(pData, windowWidth, windowHeight, nm.dpi);
    UpdateNotificationLayer(hNotifWnd);

    ShowWindow(hNotifWnd, SW_SHOWNOACTIVATE);

    SetTimer(hNotifWnd, TIMER_NOTIFICATION_AUTO_CLOSE, 5000, NULL);

    if (g_pSettingsManager && g_pSettingsManager->IsSoundEnabled()) {
        WCHAR exePath[MAX_PATH];
        GetModuleFileName(NULL, exePath, MAX_PATH);
        std::wstring exeDir = std::wstring(exePath).substr(0, std::wstring(exePath).find_last_of(L"\\/"));

        std::wstring soundFile = exeDir + L"\\bin\\adv_sound.wav";

        wchar_t debugMsg[512];
        swprintf_s(debugMsg, L"检查音效文件: %s\n", soundFile.c_str());
        OutputDebugString(debugMsg);

        if (GetFileAttributes(soundFile.c_str()) == INVALID_FILE_ATTRIBUTES) {
            soundFile = exeDir + L"\\bin\\adv_sound.mp3";
            swprintf_s(debugMsg, L"WAV文件不存在，检查MP3: %s\n", soundFile.c_str());
            OutputDebugString(debugMsg);
            if (GetFileAttributes(soundFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
                OutputDebugString(L"播放MP3音效\n");
                PlayAudioFile(soundFile);
            }
            else {
                soundFile = exeDir + L"\\bin\\adv_soud.mp3";
                if (GetFileAttributes(soundFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    OutputDebugString(L"播放MP3音效(旧文件名)\n");
                    PlayAudioFile(soundFile);
                }
                else {
                    OutputDebugString(L"音效文件不存在！\n");
                }
            }
        }
        else {
            OutputDebugString(L"播放WAV音效\n");
            PlaySound(soundFile.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        }
    }
    else {
        OutputDebugString(L"音效已禁用\n");
    }
}

void AdvancementManager::Initialize() {
    if (!LoadAdvancementsFromJSON()) {
        ShowMessageDialog(hMainWnd, L"加载成就配置失败，程序将退出！", L"错误", MB_ICONERROR | MB_OK);
        PostQuitMessage(0);
        return;
    }

    LoadAdvancements();

    MainLayout L = CalcMainLayout(hMainWnd);

    hListCompleted = CreateWindowEx(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | LBS_HASSTRINGS,
        L.marginX, L.list1Top, L.listWidth, L.listHeight,
        hMainWnd, (HMENU)ID_LIST_COMPLETED, hInst, NULL);

    hListUncompleted = CreateWindowEx(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | LBS_HASSTRINGS,
        L.marginX, L.list2Top, L.listWidth, L.listHeight,
        hMainWnd, (HMENU)ID_LIST_UNCOMPLETED, hInst, NULL);

    UpdateListFont(hMainWnd);   // 字体按当前 DPI 创建，并交给 g_hListFont 统一管理

    UpdateLists();
    StartMonitoring();
}

void AdvancementManager::CheckAndTriggerAdvancements() {
    for (const auto& adv : advancements) {
        if (!adv.completed) {
            bool triggered = false;

            switch (adv.triggerType) {
            case TRIGGER_WINDOW_TITLE:
                triggered = CheckWindowTitle(adv.triggerValue);
                break;

            case TRIGGER_PROCESS_NAME:
                triggered = CheckProcessExists(adv.triggerValue);
                break;

            default:
                break;
            }

            if (triggered) {
                TriggerAdvancement(adv.id);
            }
        }
    }
}

void AdvancementManager::MonitoringThread() {
    while (monitoring) {
        CheckAndTriggerAdvancements();
        Sleep(500);
    }
}

void AdvancementManager::StartMonitoring() {
    if (monitoring) return;
    monitoring = true;
    monitorThread = std::thread(&AdvancementManager::MonitoringThread, this);
}

void AdvancementManager::StopMonitoring() {
    monitoring = false;
    if (monitorThread.joinable()) {
        monitorThread.join();
    }
}

void AdvancementManager::UpdateLists() {
    SendMessage(hListCompleted, LB_RESETCONTENT, 0, 0);
    SendMessage(hListUncompleted, LB_RESETCONTENT, 0, 0);

    HDC hdc = GetDC(hMainWnd);
    HFONT hFont = (HFONT)SendMessage(hListCompleted, WM_GETFONT, 0, 0);
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    int maxCompletedWidth = 0;
    int maxUncompletedWidth = 0;

    bool showTrigger = g_pSettingsManager ? g_pSettingsManager->IsShowTriggerInfo() : true;

    for (const auto& adv : advancements) {
        std::wstring item;
        if (!adv.num.empty()) {
            item = adv.num + L". " + adv.title + L" - " + adv.description;
        }
        else {
            item = adv.title + L" - " + adv.description;
        }

        std::wstring triggerInfo = L"    触发方式: " + adv.triggerDescription;

        SIZE size = {};
        GetTextExtentPoint32(hdc, item.c_str(), (int)item.length(), &size);
        int itemWidth = size.cx + 20;

        GetTextExtentPoint32(hdc, triggerInfo.c_str(), (int)triggerInfo.length(), &size);
        int triggerWidth = size.cx + 20;

        int maxWidth = (itemWidth > triggerWidth) ? itemWidth : triggerWidth;

        if (adv.completed) {
            SendMessage(hListCompleted, LB_ADDSTRING, 0, (LPARAM)item.c_str());
            SendMessage(hListCompleted, LB_ADDSTRING, 0, (LPARAM)triggerInfo.c_str());
            if (maxWidth > maxCompletedWidth) maxCompletedWidth = maxWidth;
        }
        else {
            SendMessage(hListUncompleted, LB_ADDSTRING, 0, (LPARAM)item.c_str());
            if (showTrigger) {
                SendMessage(hListUncompleted, LB_ADDSTRING, 0, (LPARAM)triggerInfo.c_str());
                if (maxWidth > maxUncompletedWidth) maxUncompletedWidth = maxWidth;
            }
            else {
                if (itemWidth > maxUncompletedWidth) maxUncompletedWidth = itemWidth;
            }
        }
    }

    SelectObject(hdc, hOldFont);
    ReleaseDC(hMainWnd, hdc);

    SendMessage(hListCompleted, LB_SETHORIZONTALEXTENT, maxCompletedWidth, 0);
    SendMessage(hListUncompleted, LB_SETHORIZONTALEXTENT, maxUncompletedWidth, 0);
}

void AdvancementManager::PlaySoundAsync(const std::wstring& soundPath) {
    if (g_pSettingsManager && !g_pSettingsManager->IsSoundEnabled()) {
        return;
    }

    std::thread([soundPath]() {
        PlayAudioFile(soundPath);
        }).detach();
}

void RestartApplication() {
    WCHAR exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);

    STARTUPINFO si = { sizeof(STARTUPINFO) };
    PROCESS_INFORMATION pi;

    if (CreateProcess(exePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    PostQuitMessage(0);
}

void ShowDownloadWindow(HWND hParent) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = DownloadWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DownloadProgressWindow";

    static bool classRegistered = false;
    if (!classRegistered) {
        RegisterClassEx(&wc);
        classRegistered = true;
    }

    int dpi = GetDpiForWindowSafe(hParent);

    RECT rcParent;
    GetWindowRect(hParent, &rcParent);
    int x = rcParent.left + (rcParent.right - rcParent.left) / 2 - ScaleDpi(200, dpi);
    int y = rcParent.top + (rcParent.bottom - rcParent.top) / 2 - ScaleDpi(100, dpi);

    g_hDownloadWnd = CreateWindowEx(
        WS_EX_DLGMODALFRAME,
        L"DownloadProgressWindow",
        L"下载成就列表",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, ScaleDpi(413, dpi), ScaleDpi(180, dpi),
        hParent, NULL, hInst, NULL
    );

    g_hProgressBar = CreateWindowEx(0, PROGRESS_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        ScaleDpi(20, dpi), ScaleDpi(50, dpi), ScaleDpi(360, dpi), ScaleDpi(25, dpi),
        g_hDownloadWnd, NULL, hInst, NULL);

    g_hStatusText = CreateWindowEx(0, L"STATIC", L"正在连接到服务器...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        ScaleDpi(20, dpi), ScaleDpi(85, dpi), ScaleDpi(360, dpi), ScaleDpi(20, dpi),
        g_hDownloadWnd, NULL, hInst, NULL);

    g_hCancelButton = CreateWindowEx(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        ScaleDpi(309, dpi), ScaleDpi(108, dpi), ScaleDpi(80, dpi), ScaleDpi(25, dpi),
        g_hDownloadWnd, (HMENU)IDCANCEL, hInst, NULL);

    SendMessage(g_hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_hProgressBar, PBM_SETPOS, 0, 0);

    HFONT hFont = CreateUIFont(hParent, 14, FW_NORMAL, L"微软雅黑");
    if (hFont) {
        SendMessage(g_hStatusText, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(g_hCancelButton, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

    SetDialogAppIcon(g_hDownloadWnd);   // 和主窗口用同一个图标

    if (IsDarkModeEnabled()) {
        SendMessage(g_hProgressBar, PBM_SETBARCOLOR, 0, (LPARAM)RGB(0x3A, 0x7A, 0x3A));
        SendMessage(g_hProgressBar, PBM_SETBKCOLOR, 0, (LPARAM)DARK_COLOR_SURFACE);
        MakeDarkOwnerDrawButton(g_hDownloadWnd, IDCANCEL);
    }

    EnsureDarkBrushes();
    ApplyDarkWindowFrame(g_hDownloadWnd, IsDarkModeEnabled());

    ShowWindow(g_hDownloadWnd, SW_SHOW);
    UpdateWindow(g_hDownloadWnd);
}

void CloseDownloadWindow() {
    if (g_hDownloadWnd) {
        DestroyWindow(g_hDownloadWnd);
        g_hDownloadWnd = nullptr;
        g_hProgressBar = nullptr;
        g_hStatusText = nullptr;
        g_hCancelButton = nullptr;
    }
}

void UpdateDownloadProgress(int progress, const std::wstring& status) {
    if (g_hProgressBar) {
        SendMessage(g_hProgressBar, PBM_SETPOS, progress, 0);
    }
    if (g_hStatusText) {
        SetWindowText(g_hStatusText, status.c_str());
    }
}

bool DownloadAdvancementJson(HWND hWnd) {
    if (g_bDownloading) {
        ShowMessageDialog(hWnd, L"当前正在下载，请稍候...", L"提示", MB_ICONINFORMATION | MB_OK);
        return false;
    }

    g_bDownloadCanceled = false;

    ShowDownloadWindow(hWnd);
    g_bDownloading = true;

    g_downloadThread = std::thread([hWnd]() {
        bool bSuccess = false;
        std::wstring errorMessage;

        HANDLE hFile = INVALID_HANDLE_VALUE;
        BOOL success = TRUE;
        DWORD totalBytes = 0;
        DWORD fileSize = 0;
        DWORD bytesRead = 0;
        char sizeBuffer[64] = { 0 };
        DWORD sizeBufferLen = sizeof(sizeBuffer);
        BYTE buffer[4096];
        std::string downloadedContent;
        std::wstring downloadedVersion;

        WCHAR exePath[MAX_PATH];
        GetModuleFileName(NULL, exePath, MAX_PATH);
        std::wstring exeDir = std::wstring(exePath).substr(0, std::wstring(exePath).find_last_of(L"\\/"));
        std::wstring jsonPath = exeDir + L"\\bin\\adv.json";
        std::wstring backupPath = exeDir + L"\\bin\\adv.json.bak";
        std::wstring tempPath = exeDir + L"\\bin\\adv.json.tmp";

        std::wstring currentVersion = L"";
        if (g_pAdvManager) {
            currentVersion = g_pAdvManager->GetVersion();
        }

        if (GetFileAttributes(tempPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFile(tempPath.c_str());
        }

        std::wstring binDir = exeDir + L"\\bin";
        if (GetFileAttributes(binDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            CreateDirectory(binDir.c_str(), NULL);
        }

        UpdateDownloadProgress(10, L"正在初始化网络连接...");

        g_hInternet = InternetOpen(L"MCAdvancementsOnWin", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
        if (!g_hInternet) {
            errorMessage = L"初始化网络连接失败！";
            goto cleanup;
        }

        UpdateDownloadProgress(30, L"正在连接到服务器...");
        g_hUrl = InternetOpenUrl(g_hInternet,
            L"https://raw.githubusercontent.com/MoyeeLZX/MCAdvancementsOnWin/refs/heads/main/repo/adv.json",
            NULL, 0, INTERNET_FLAG_RELOAD, 0);

        if (!g_hUrl) {
            errorMessage = L"无法连接到服务器！";
            goto cleanup;
        }

        UpdateDownloadProgress(50, L"正在创建临时文件...");
        hFile = CreateFile(tempPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            errorMessage = L"创建临时文件失败！";
            goto cleanup;
        }

        UpdateDownloadProgress(70, L"正在下载数据...");
        success = TRUE;
        totalBytes = 0;
        fileSize = 0;

        if (HttpQueryInfoA(g_hUrl, HTTP_QUERY_CONTENT_LENGTH, sizeBuffer, &sizeBufferLen, NULL)) {
            fileSize = atoi(sizeBuffer);
        }

        while (InternetReadFile(g_hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            if (g_bDownloadCanceled) {
                success = FALSE;
                errorMessage = L"下载已被取消";
                break;
            }

            DWORD bytesWritten;
            if (!WriteFile(hFile, buffer, bytesRead, &bytesWritten, NULL)) {
                success = FALSE;
                break;
            }
            totalBytes += bytesWritten;

            if (fileSize > 0) {
                int progress = 70 + (int)((float)totalBytes / fileSize * 25.0f);
                UpdateDownloadProgress(progress, L"正在下载数据...");
            }
            else {
                UpdateDownloadProgress(85, L"正在下载数据...");
            }
        }

        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
        CloseInetHandle(g_hUrl);
        CloseInetHandle(g_hInternet);

        if (g_bDownloadCanceled) {
            DeleteFile(tempPath.c_str());
            std::wstring* pMessage = new std::wstring(L"下载已被取消");
            PostMessage(hWnd, WM_USER + 104, 0, (LPARAM)pMessage);
            return;
        }

        if (!success || totalBytes == 0) {
            DeleteFile(tempPath.c_str());
            errorMessage = L"下载失败！";
            goto cleanup;
        }

        UpdateDownloadProgress(95, L"正在验证下载的文件...");

        downloadedContent = ReadFileAsUTF8(tempPath);
        if (downloadedContent.empty()) {
            DeleteFile(tempPath.c_str());
            errorMessage = L"下载的文件为空！";
            goto cleanup;
        }

        downloadedVersion = ExtractJSONVersion(downloadedContent);
        if (downloadedVersion.empty()) {
            downloadedVersion = L"未知版本";
        }

        if (!IsJSONFileValid(tempPath)) {
            DeleteFile(tempPath.c_str());
            errorMessage = L"下载的文件格式不正确！";
            goto cleanup;
        }

        UpdateDownloadProgress(100, L"下载完成，正在处理...");

        if (IsNewerVersion(currentVersion, downloadedVersion)) {
            std::wstring* pData = new std::wstring[4];
            pData[0] = tempPath;
            pData[1] = jsonPath;
            pData[2] = backupPath;
            pData[3] = downloadedVersion;

            PostMessage(hWnd, WM_USER + 101, 0, (LPARAM)pData);
            return;
        }
        else {
            DeleteFile(tempPath.c_str());

            std::wstring* pMessage = new std::wstring(L"当前已是最新版本！\n\n");
            *pMessage += L"当前版本: " + (currentVersion.empty() ? L"未知版本" : currentVersion) + L"\n";
            if (currentVersion == downloadedVersion) {
                *pMessage += L"服务器版本: " + downloadedVersion + L" (与当前版本相同)\n";
            }
            else {
                *pMessage += L"服务器版本: " + downloadedVersion + L" (比当前版本旧)\n";
            }

            PostMessage(hWnd, WM_USER + 102, 0, (LPARAM)pMessage);
        }

        bSuccess = true;
        return;

    cleanup:
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
        }
        CloseInetHandle(g_hUrl);
        CloseInetHandle(g_hInternet);
        DeleteFile(tempPath.c_str());

        // 取消时关闭句柄会让 InternetOpen/InternetOpenUrl 直接失败，
        // 这里必须先判取消，否则会被误报成“无法连接到服务器”
        if (g_bDownloadCanceled) {
            std::wstring* pMessage = new std::wstring(L"下载已被取消");
            PostMessage(hWnd, WM_USER + 104, 0, (LPARAM)pMessage);
            return;
        }

        std::wstring* pErrorMessage = new std::wstring(errorMessage);
        PostMessage(hWnd, WM_USER + 103, 0, (LPARAM)pErrorMessage);
        });

    return true;
}

LRESULT CALLBACK DownloadWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDCANCEL) {
            CancelDownload();
        }
        break;

    case WM_DRAWITEM: {
        INT_PTR drawResult = 0;
        if (HandleDarkOwnerDraw(message, wParam, lParam, &drawResult)) {
            return (LRESULT)drawResult;   // 深色模式下"取消"按钮自绘
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    case WM_CLOSE:
        if (g_bDownloading) {
            CancelDownload();
            return 0;
        }
        DestroyWindow(hWnd);
        break;

    case WM_DESTROY:
        g_hDownloadWnd = nullptr;
        g_hProgressBar = nullptr;
        g_hStatusText = nullptr;
        g_hCancelButton = nullptr;
        break;

    case WM_ERASEBKGND: {
        if (!IsDarkModeEnabled()) {
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect((HDC)wParam, &rc, g_hDarkBgBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        if (!IsDarkModeEnabled()) {
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        HDC hdcCtl = (HDC)wParam;
        SetTextColor(hdcCtl, DARK_COLOR_TEXT);
        SetBkColor(hdcCtl, DARK_COLOR_BG);
        SetBkMode(hdcCtl, TRANSPARENT);
        return (LRESULT)g_hDarkBgBrush;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        int dpi = GetDpiForWindowSafe(hWnd);
        RECT rc = { ScaleDpi(20, dpi), ScaleDpi(20, dpi), ScaleDpi(380, dpi), ScaleDpi(40, dpi) };
        HFONT hFont = CreateUIFont(hWnd, 16, FW_BOLD, L"微软雅黑");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, IsDarkModeEnabled() ? DARK_COLOR_TEXT : GetSysColor(COLOR_WINDOWTEXT));
        DrawText(hdc, L"正在下载成就列表...", -1, &rc, DT_LEFT);
        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);

        EndPaint(hWnd, &ps);
        break;
    }

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK NotificationWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static int animationStep = 0;
    static int targetX = 0;
    static int startX = 0;
    static int currentY = 0;
    static int screenRight = 0;   // 通知所在显示器右边界，滑出时用它

    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONUP: {
        if (g_pSettingsManager && !g_pSettingsManager->IsClickNotifyToShow()) {
            break;
        }

        if (g_hMainWnd && IsWindow(g_hMainWnd)) {
            ShowWindow(g_hMainWnd, SW_RESTORE);
            SetForegroundWindow(g_hMainWnd);
        }
        return 0;
    }

    case WM_CREATE: {
        OutputDebugString(L"WM_CREATE called\n");

        LONG_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);
        if (style & WS_CAPTION) {
            SetWindowLongPtr(hWnd, GWL_STYLE, style & ~WS_CAPTION);
        }

        LONG_PTR exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
        if (!(exStyle & WS_EX_LAYERED)) {
            SetWindowLongPtr(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
        }

        NotificationData* pData = (NotificationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (pData && pData->pAdv) {
            wchar_t debugMsg[512];
            swprintf_s(debugMsg, L"WM_CREATE: 成就标题=%s\n", pData->pAdv->title.c_str());
            OutputDebugString(debugMsg);
        }

        NotifyMetrics nm = CalcNotifyMetrics();
        int windowHeight2 = nm.windowHeight;
        int windowWidth2 = nm.windowWidth;

        int verticalSpacing = ScaleDpi(10, nm.dpi);
        int currentCount = g_notificationCount.load() - 1;
        if (currentCount < 0) currentCount = 0;
        currentY = nm.top + (windowHeight2 + verticalSpacing) * currentCount;

        screenRight = nm.right;

        SetWindowPos(hWnd, NULL, nm.right, currentY, windowWidth2, windowHeight2, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        targetX = nm.right - windowWidth2;
        startX = nm.right;
        animationStep = 0;

        SetTimer(hWnd, ANIMATION_TIMER, ANIMATION_INTERVAL, NULL);
        break;
    }

    case WM_TIMER:
        if (wParam == ANIMATION_TIMER) {
            if (animationStep <= ANIMATION_STEPS) {
                float t = (float)animationStep / ANIMATION_STEPS;
                float easeT = 1 - (1 - t) * (1 - t) * (1 - t);

                int currentX = startX + (int)((targetX - startX) * easeT);

                SetWindowPos(hWnd, NULL, currentX, currentY, 0, 0,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOREDRAW | SWP_NOCOPYBITS);

                animationStep++;
            }
            else {
                KillTimer(hWnd, ANIMATION_TIMER);
                SetWindowPos(hWnd, NULL, targetX, currentY, 0, 0,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
            }
        }
        else if (wParam == TIMER_NOTIFICATION_AUTO_CLOSE) {
            animationStep = 0;
            startX = targetX;
            targetX = screenRight;
            SetTimer(hWnd, TIMER_NOTIFICATION_SLIDE_OUT, ANIMATION_INTERVAL, NULL);
        }
        else if (wParam == TIMER_NOTIFICATION_SLIDE_OUT) {
            if (animationStep <= ANIMATION_STEPS) {
                float t = (float)animationStep / ANIMATION_STEPS;
                float easeT = t * t * t;

                int currentX = startX + (int)((targetX - startX) * easeT);

                SetWindowPos(hWnd, NULL, currentX, currentY, 0, 0,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOREDRAW | SWP_NOCOPYBITS);

                animationStep++;
            }
            else {
                KillTimer(hWnd, 4);

                NotificationData* pData = (NotificationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
                if (pData) {
                    if (pData->pIconBitmap) {
                        delete pData->pIconBitmap;
                    }
                    if (pData->pBitmap) {
                        delete pData->pBitmap;
                    }
                    if (pData->hDib) {
                        DeleteObject(pData->hDib);
                        pData->hDib = NULL;
                    }
                    if (pData->pAdv) {
                        delete pData->pAdv;
                    }
                    if (pData->pFontPath) {
                        delete pData->pFontPath;
                    }
                    delete pData;
                    SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
                }

                g_notificationCount--;
                g_showingNotification = false;

                if (g_hMainWnd) {
                    PostMessage(g_hMainWnd, WM_USER + 105, 0, 0);
                }

                DestroyWindow(hWnd);
            }
        }
        break;

    case WM_PAINT: {
        // 内容一律由 UpdateLayeredWindow 提供。这里不能再自绘，
        // 否则不透明的自绘会把 adv_back.png 四个角的透明像素覆盖掉
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY: {
        NotificationData* pData = (NotificationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (pData) {
            if (pData->pIconBitmap) {
                delete pData->pIconBitmap;
            }
            if (pData->pBitmap) {
                delete pData->pBitmap;
            }
            if (pData->hDib) {
                DeleteObject(pData->hDib);
                pData->hDib = NULL;
            }
            if (pData->pAdv) {
                delete pData->pAdv;
            }
            if (pData->pFontPath) {
                delete pData->pFontPath;
            }
            delete pData;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
        }

        if (g_showingNotification) {
            g_notificationCount--;
            g_showingNotification = false;

            if (g_hMainWnd) {
                PostMessage(g_hMainWnd, WM_USER + 105, 0, 0);
            }
        }

        KillTimer(hWnd, ANIMATION_TIMER);
        KillTimer(hWnd, TIMER_NOTIFICATION_AUTO_CLOSE);
        KillTimer(hWnd, TIMER_NOTIFICATION_SLIDE_OUT);
        break;
    }

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static HFONT hVersionFont = NULL;
    static int minWidth = 600;
    static int minHeight = 400;

    switch (message) {
    if (message == g_uTaskbarRestart && g_uTaskbarRestart != 0) {
        AddTrayIcon(hWnd);
        break;
    }

    case WM_CREATE: {
        g_pSettingsManager = new (std::nothrow) SettingsManager();
        if (g_pSettingsManager) {
            g_pSettingsManager->LoadSettings();
        }
        else {
            ShowMessageDialog(hWnd, L"无法初始化设置管理器！程序将退出。", L"错误", MB_ICONERROR | MB_OK);
            PostQuitMessage(1);
            break;
        }

        g_hMainWnd = hWnd;

        g_uTaskbarRestart = RegisterWindowMessage(L"TaskbarCreated");

        g_pAdvManager = new (std::nothrow) AdvancementManager(hWnd);
        if (g_pAdvManager) {
            g_pAdvManager->Initialize();
            SetTimer(hWnd, TIMER_CHECK_WINDOWS, 2000, NULL);
            g_pSettingsManager->UpdateAllMenuItems(hWnd);

            EnsureDarkBrushes();
            ApplyDarkWindowFrame(hWnd, g_pSettingsManager->IsDarkMode());
            ApplyDarkMenus(g_pSettingsManager->IsDarkMode());
        }
        else {
            ShowMessageDialog(hWnd, L"无法创建成就管理器！程序将退出。", L"错误", MB_ICONERROR | MB_OK);
            PostQuitMessage(1);
            break;
        }
        break;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
        int dpi = GetDpiForWindowSafe(hWnd);
        pMMI->ptMinTrackSize.x = ScaleDpi(minWidth, dpi);
        pMMI->ptMinTrackSize.y = ScaleDpi(minHeight, dpi);
    }
    break;

    case WM_MEASUREITEM:
    case WM_DRAWITEM: {
        INT_PTR drawResult = 0;
        if (HandleDarkOwnerDraw(message, wParam, lParam, &drawResult)) {
            return (LRESULT)drawResult;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    case WM_NCPAINT: {
        LRESULT ncResult = DefWindowProc(hWnd, message, wParam, lParam);
        if (IsDarkModeEnabled() && g_hDarkMenuBarBrush) {
            PaintMenuBarLeftover(hWnd);
        }
        return ncResult;
    }

    case WM_ERASEBKGND: {
        if (!IsDarkModeEnabled()) {
            return DefWindowProc(hWnd, message, wParam, lParam);   // 浅色时照旧用窗口类的画刷
        }
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect((HDC)wParam, &rc, g_hDarkBgBrush);
        return 1;
    }

    case WM_CTLCOLORLISTBOX: {
        if (!IsDarkModeEnabled()) {
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        HDC hdcCtl = (HDC)wParam;
        SetTextColor(hdcCtl, DARK_COLOR_TEXT);
        SetBkColor(hdcCtl, DARK_COLOR_SURFACE);
        return (LRESULT)g_hDarkSurfaceBrush;
    }

    case WM_DPICHANGED: {
        // 跨显示器拖动或系统缩放变更时触发；lParam 给出系统建议的新窗口矩形（物理像素）
        if (lParam) {
            RECT* pSuggested = (RECT*)lParam;
            SetWindowPos(hWnd, NULL, pSuggested->left, pSuggested->top,
                pSuggested->right - pSuggested->left, pSuggested->bottom - pSuggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (hVersionFont) {          // 字体是按旧 DPI 的像素大小创建的，必须重建
            DeleteObject(hVersionFont);
            hVersionFont = NULL;
        }
        UpdateListFont(hWnd);
        LayoutLists(hWnd);
        if (g_pAdvManager) {
            g_pAdvManager->UpdateLists();   // 水平滚动范围依赖字体度量
        }
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDM_ABOUT) {
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, about);
        }
        else if (wmId == IDM_FILE_EXIT) {
            DestroyWindow(hWnd);
        }
        else if (wmId == IDM_FILE_UPDATE_JSON) {
            DownloadAdvancementJson(hWnd);
        }
        else if (wmId == IDM_SETTINGS_SOUND) {
            if (g_pSettingsManager) {
                bool currentState = g_pSettingsManager->IsSoundEnabled();
                g_pSettingsManager->SetSoundEnabled(!currentState);
                g_pSettingsManager->SaveSettings();

                g_pSettingsManager->UpdateAllMenuItems(hWnd);
            }
        }
        else if (wmId == IDM_SETTINGS_SHOW_TRIGGER) {
            if (g_pSettingsManager) {
                bool currentState = g_pSettingsManager->IsShowTriggerInfo();
                g_pSettingsManager->SetShowTriggerInfo(!currentState);
                g_pSettingsManager->SaveSettings();
                g_pSettingsManager->UpdateAllMenuItems(hWnd);

                if (g_pAdvManager) {
                    g_pAdvManager->UpdateLists();
                }
            }
        }
        else if (wmId == IDM_SETTINGS_CLICK_NOTIFY) {
            if (g_pSettingsManager) {
                bool currentState = g_pSettingsManager->IsClickNotifyToShow();
                g_pSettingsManager->SetClickNotifyToShow(!currentState);
                g_pSettingsManager->SaveSettings();
                g_pSettingsManager->UpdateAllMenuItems(hWnd);
            }
        }
        else if (wmId == IDM_SETTINGS_DARK_MODE) {
            if (g_pSettingsManager) {
                bool dark = !g_pSettingsManager->IsDarkMode();
                g_pSettingsManager->SetDarkMode(dark);
                g_pSettingsManager->SaveSettings();
                g_pSettingsManager->UpdateAllMenuItems(hWnd);

                EnsureDarkBrushes();
                ApplyDarkWindowFrame(hWnd, dark);
                ApplyDarkMenus(dark);

                // 连子窗口一起重画，列表才能立刻换上新配色
                RedrawWindow(hWnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
            }
        }
        else if (wmId == IDM_SETTINGS_RELOAD) {
            if (g_pSettingsManager) {
                g_pSettingsManager->LoadSettings();
                g_pSettingsManager->UpdateAllMenuItems(hWnd);

                EnsureDarkBrushes();
                ApplyDarkWindowFrame(hWnd, g_pSettingsManager->IsDarkMode());
                ApplyDarkMenus(g_pSettingsManager->IsDarkMode());
                RedrawWindow(hWnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
            }
            if (g_pAdvManager) {
                g_pAdvManager->UpdateLists();
            }
            ShowMessageDialog(hWnd, L"设置已从 setting.config 重新加载。", L"重新加载设置", MB_OK | MB_ICONINFORMATION);
        }
        else if (wmId == IDM_SETTINGS_CLEAR_SAVE) {
            int result = ShowMessageDialog(hWnd,
                L"您确定要清空存档吗？\n这将删除所有已完成的成就记录，删除后无法恢复。",
                L"确认清空存档",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);

            if (result == IDYES) {
                WCHAR exePath[MAX_PATH];
                GetModuleFileName(NULL, exePath, MAX_PATH);
                std::wstring exeDir = std::wstring(exePath).substr(0, std::wstring(exePath).find_last_of(L"\\/"));
                std::wstring saveFile = exeDir + L"\\adv_save.txt";

                if (DeleteFile(saveFile.c_str())) {
                    RestartApplication();
                }
                else {
                    DWORD error = GetLastError();
                    if (error == ERROR_FILE_NOT_FOUND) {
                        RestartApplication();
                    }
                    else {
                        ShowMessageDialog(hWnd, L"删除存档文件失败！", L"错误", MB_ICONERROR | MB_OK);
                    }
                }
            }
        }
        else if (wmId == ID_TRAY_SHOW) {
            ToggleMainWindow(hWnd);
        }
        else if (wmId == ID_TRAY_SOUND) {
            if (g_pSettingsManager) {
                bool currentState = g_pSettingsManager->IsSoundEnabled();
                g_pSettingsManager->SetSoundEnabled(!currentState);
                g_pSettingsManager->SaveSettings();
                g_pSettingsManager->UpdateAllMenuItems(hWnd);
            }
        }
        else if (wmId == ID_TRAY_ABOUT) {
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, about);
        }
        else if (wmId == ID_TRAY_EXIT) {
            DestroyWindow(hWnd);
        }
        break;
    }

    case WM_ADVANCEMENT_TRIGGERED: {
        Advancement* pAdv = (Advancement*)lParam;
        if (pAdv) {
            AddAchievementToQueue(*pAdv);
            delete pAdv;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == TIMER_CHECK_WINDOWS && g_pAdvManager) {
            g_pAdvManager->CheckAndTriggerAdvancements();
        }
        break;

    case WM_TRAYICON: {
        wchar_t dbg[128];
        swprintf_s(dbg, L"[Tray] WM_TRAYICON 收到, lParam=0x%X\n", (unsigned int)lParam);
        OutputDebugString(dbg);
        if (lParam == WM_RBUTTONUP || lParam == WM_RBUTTONDOWN || lParam == WM_CONTEXTMENU) {
            ShowTrayMenu(hWnd);
        }
        else if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            ToggleMainWindow(hWnd);
        }
        break;
    }

    case WM_SIZE:
        // 最小化（SIZE_MINIMIZED）交给系统处理：窗口照常缩到任务栏，不隐藏、不重排布局。
        // “隐藏窗口”是另一条独立路径，只在关闭窗口选择“滚到后台去！”或托盘/菜单里触发（SW_HIDE，任务栏按钮消失）
        if (wParam == SIZE_MINIMIZED) {
            break;
        }
        if (g_pAdvManager) {
            LayoutLists(hWnd);
        }
        break;

    case WM_USER + 101: {
        std::wstring* pData = (std::wstring*)lParam;
        std::wstring tempPath = pData[0];
        std::wstring jsonPath = pData[1];
        std::wstring backupPath = pData[2];
        std::wstring downloadedVersion = pData[3];

        std::wstring currentVersion = L"";
        if (g_pAdvManager) {
            currentVersion = g_pAdvManager->GetVersion();
        }

        std::wstring message = L"发现新版本的成就列表！\n\n";
        message += L"当前版本: " + (currentVersion.empty() ? L"未知版本" : currentVersion) + L"\n";
        message += L"最新版本: " + downloadedVersion + L"\n\n";
        message += L"是否更新到最新版本？";

        int result = ShowMessageDialog(hWnd, message.c_str(), L"发现新版本", MB_YESNO | MB_ICONQUESTION | MB_APPLMODAL);

        if (result == IDYES) {
            bool hadOriginalFile = false;
            if (GetFileAttributes(jsonPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (MoveFile(jsonPath.c_str(), backupPath.c_str()) == FALSE) {
                    ShowMessageDialog(hWnd, L"备份旧文件失败！", L"错误", MB_ICONERROR | MB_OK | MB_APPLMODAL);
                    DeleteFile(tempPath.c_str());
                }
                else {
                    hadOriginalFile = true;
                }
            }

            if (MoveFile(tempPath.c_str(), jsonPath.c_str())) {
                if (hadOriginalFile && GetFileAttributes(backupPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    DeleteFile(backupPath.c_str());
                }

                std::wstring successMessage = L"成就列表更新成功！\n\n";
                successMessage += L"新版本: " + downloadedVersion + L"\n\n";
                successMessage += L"需要重启程序以加载新的成就列表。\n是否立即重启？";

                int restartResult = ShowMessageDialog(hWnd, successMessage.c_str(), L"更新成功", MB_YESNO | MB_ICONINFORMATION | MB_APPLMODAL);

                if (restartResult == IDYES) {
                    RestartApplication();
                }
            }
            else {
                ShowMessageDialog(hWnd, L"更新文件失败！", L"错误", MB_ICONERROR | MB_OK | MB_APPLMODAL);
                if (hadOriginalFile && GetFileAttributes(backupPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    MoveFile(backupPath.c_str(), jsonPath.c_str());
                }
                DeleteFile(tempPath.c_str());
            }
        }
        else {
            DeleteFile(tempPath.c_str());
            ShowMessageDialog(hWnd, L"已取消更新。", L"取消更新", MB_ICONINFORMATION | MB_OK | MB_APPLMODAL);
        }

        delete[] pData;

        CloseDownloadWindow();
        g_bDownloading = false;
        if (g_downloadThread.joinable()) {
            g_downloadThread.join();
        }
        break;
    }

    case WM_USER + 102: {
        std::wstring* pMessage = (std::wstring*)lParam;
        ShowMessageDialog(hWnd, pMessage->c_str(), L"已是最新版本", MB_ICONINFORMATION | MB_OK | MB_APPLMODAL);
        delete pMessage;

        CloseDownloadWindow();
        g_bDownloading = false;
        if (g_downloadThread.joinable()) {
            g_downloadThread.join();
        }
        break;
    }

    case WM_USER + 103: {
        std::wstring* pErrorMessage = (std::wstring*)lParam;
        ShowMessageDialog(hWnd, pErrorMessage->c_str(), L"下载错误", MB_ICONERROR | MB_OK | MB_APPLMODAL);
        delete pErrorMessage;

        CloseDownloadWindow();
        g_bDownloading = false;
        if (g_downloadThread.joinable()) {
            g_downloadThread.join();
        }
        break;
    }

    case WM_USER + 104: {
        // 用户主动取消：窗口已经关掉了，不再弹提示，只做收尾（回收消息、复位状态、回收线程）
        std::wstring* pMessage = (std::wstring*)lParam;
        delete pMessage;

        CloseDownloadWindow();
        g_bDownloading = false;
        g_bDownloadCanceled = false;
        if (g_downloadThread.joinable()) {
            g_downloadThread.join();
        }
        break;
    }

    case WM_USER + 105: {
        OutputDebugString(L"收到WM_USER + 105消息\n");
        ShowNextAchievement(hWnd);
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        MainLayout L = CalcMainLayout(hWnd);
        int dpi = GetDpiForWindowSafe(hWnd);
        bool dark = IsDarkModeEnabled();

        HFONT hLabelFont = CreateUIFont(hWnd, 22, FW_BOLD, L"微软雅黑");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hLabelFont ? hLabelFont : GetStockObject(DEFAULT_GUI_FONT));

        RECT rc;
        GetClientRect(hWnd, &rc);

        // 不透明背景模式下文字会把底色刷成默认的白色块，所以必须透明
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, dark ? DARK_COLOR_TEXT : GetSysColor(COLOR_WINDOWTEXT));

        RECT rc1 = { L.marginX, ScaleDpi(24, dpi), L.labelRight, ScaleDpi(52, dpi) };
        DrawText(hdc, L"已完成成就:", -1, &rc1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        int uncompletedListTop = L.list2Top;
        RECT rc2 = { L.marginX, uncompletedListTop - ScaleDpi(28, dpi), L.labelRight, uncompletedListTop - ScaleDpi(4, dpi) };
        DrawText(hdc, L"未完成成就:", -1, &rc2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (g_pAdvManager) {
            if (!hVersionFont) {
                hVersionFont = CreateUIFont(hWnd, 16, FW_NORMAL, L"微软雅黑");
            }
            if (hVersionFont) {
                HFONT hOldVersionFont = (HFONT)SelectObject(hdc, hVersionFont);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, dark ? DARK_COLOR_TEXT_DIM : RGB(100, 100, 100));

                std::wstring versionText = L"成就列表版本: " + g_pAdvManager->GetVersion();
                RECT versionRect = { L.marginX, rc.bottom - L.versionBand, rc.right - L.marginX, rc.bottom - ScaleDpi(6, dpi) };
                DrawText(hdc, versionText.c_str(), -1, &versionRect, DT_LEFT);

                SelectObject(hdc, hOldVersionFont);
            }
        }

        SelectObject(hdc, hOldFont);
        if (hLabelFont) DeleteObject(hLabelFont);

        EndPaint(hWnd, &ps);
        break;
    }

    case WM_CLOSE:
        if (g_pSettingsManager && g_pSettingsManager->IsCloseNoPrompt()) {
            if (g_pSettingsManager->GetCloseAction() == CLOSE_ACTION_EXIT) {
                DestroyWindow(hWnd);
            }
            else {
                ShowWindow(hWnd, SW_HIDE);
            }
        }
        else {
            DialogBox(hInst, MAKEINTRESOURCE(IDD_CLOSE_CONFIRM), hWnd, CloseConfirmProc);
        }
        break;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (hVersionFont) {
            DeleteObject(hVersionFont);
            hVersionFont = NULL;
        }
        if (g_hListFont) {
            DeleteObject(g_hListFont);
            g_hListFont = NULL;
        }
        ReleaseDarkBrushes();

        FreeMenuItemDrawInfo(GetMenu(hWnd));
        if (g_hMenuFont) {
            DeleteObject(g_hMenuFont);
            g_hMenuFont = NULL;
        }

        if (g_pAdvManager) {
            g_pAdvManager->StopMonitoring();
            delete g_pAdvManager;
            g_pAdvManager = nullptr;
        }
        if (g_pSettingsManager) {
            delete g_pSettingsManager;
            g_pSettingsManager = nullptr;
        }

        KillTimer(hWnd, TIMER_CHECK_WINDOWS);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

ATOM MyRegisterClass(HINSTANCE hInstance) {
    WNDCLASSEXW wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MCADVANCEMENTSONWIN));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCE(IDC_MCADVANCEMENTSONWIN);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow) {
    hInst = hInstance;

    int dpi = GetDpiForWindowSafe(NULL);
    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, ScaleDpi(800, dpi), ScaleDpi(600, dpi), nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return FALSE;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    AddTrayIcon(hWnd);
    return TRUE;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPWSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 必须在创建任何窗口/HDC 之前调用，否则 DPI 感知设置不生效
    EnableDpiAwareness();

    // 同样必须在创建任何窗口之前：告诉系统本进程允许深色模式，
    // 这样后续 AllowDarkModeForWindow 才能让标题栏/菜单走深色主题
    EnableDarkModeInfrastructure();

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    MyRegisterClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow)) return FALSE;

    HACCEL hAccelTable = LoadAccelerators(hInstance,
        MAKEINTRESOURCE(IDC_MCADVANCEMENTSONWIN));

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}

INT_PTR CALLBACK CloseConfirmProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    INT_PTR darkResult = 0;
    if (HandleDialogDarkColor(hDlg, message, wParam, &darkResult)) {
        return darkResult;
    }
    if (message == WM_DRAWITEM && HandleDarkOwnerDraw(message, wParam, lParam, &darkResult)) {
        return darkResult;
    }

    switch (message)
    {
    case WM_INITDIALOG:
        ApplyDarkWindowFrame(hDlg, IsDarkModeEnabled());
        SetDialogAppIcon(hDlg);
        MakeDarkOwnerDrawButton(hDlg, IDOK);
        MakeDarkOwnerDrawButton(hDlg, IDCANCEL);
        CheckRadioButton(hDlg, ID_CLOSE_RADIO_EXIT, ID_CLOSE_RADIO_MIN, ID_CLOSE_RADIO_EXIT);
        CheckDlgButton(hDlg, ID_CLOSE_NO_PROMPT, BST_UNCHECKED);
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            int action = (IsDlgButtonChecked(hDlg, ID_CLOSE_RADIO_EXIT) == BST_CHECKED)
                ? CLOSE_ACTION_EXIT : CLOSE_ACTION_MIN;
            bool noPrompt = (IsDlgButtonChecked(hDlg, ID_CLOSE_NO_PROMPT) == BST_CHECKED);

            if (g_pSettingsManager) {
                g_pSettingsManager->SetCloseAction(action);
                g_pSettingsManager->SetCloseNoPrompt(noPrompt);
                g_pSettingsManager->SaveSettings();
            }

            HWND hMain = g_hMainWnd;
            EndDialog(hDlg, IDOK);

            if (action == CLOSE_ACTION_EXIT) {
                DestroyWindow(hMain);
            }
            else {
                ShowWindow(hMain, SW_HIDE);
            }
            return (INT_PTR)TRUE;
        }
        else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK about(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    INT_PTR darkResult = 0;
    if (HandleDialogDarkColor(hDlg, message, wParam, &darkResult)) {
        return darkResult;
    }
    if (message == WM_DRAWITEM && HandleDarkOwnerDraw(message, wParam, lParam, &darkResult)) {
        return darkResult;
    }

    switch (message)
    {
    case WM_INITDIALOG:
    {
        ApplyDarkWindowFrame(hDlg, IsDarkModeEnabled());
        SetDialogAppIcon(hDlg);
        MakeDarkOwnerDrawButton(hDlg, IDOK);
        HWND hLink = GetDlgItem(hDlg, IDC_ABOUT_LINK);
        if (hLink) {
            SetWindowText(hLink, L"https://github.com/MoyeeLZX/MCAdvancementsOnWin");
        }
        SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDOK), TRUE);
        return (INT_PTR)FALSE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
