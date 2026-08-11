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

std::queue<Advancement> g_achievementQueue;
std::mutex g_queueMutex;
std::atomic<bool> g_showingNotification(false);
std::atomic<int> g_notificationCount(0);

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

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hSubMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        pt.x, pt.y, 0, hWnd, NULL);
    PostMessage(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    bShowing = false;
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

void CancelDownload() {
    if (g_bDownloading) {
        g_bDownloadCanceled = true;

        if (g_downloadThread.joinable()) {
            auto startTime = std::chrono::steady_clock::now();
            while (g_bDownloading &&
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - startTime).count() < 5) {
                Sleep(100);
            }
        }

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
        MessageBox(hMainWnd, errorMsg.c_str(), L"错误", MB_ICONERROR | MB_OK);
        return false;
    }

    std::string jsonContent = ReadFileAsUTF8(jsonFilePath);
    if (jsonContent.empty()) {
        MessageBox(hMainWnd, L"JSON文件为空或读取失败！", L"错误", MB_ICONERROR | MB_OK);
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
        MessageBox(hMainWnd, L"JSON解析失败或没有找到成就配置！", L"错误", MB_ICONERROR | MB_OK);
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

void AdvancementManager::ShowAdvancementNotification(const Advancement& adv) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = NotificationWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
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

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int shortSide = min(screenWidth, screenHeight);

    int windowHeight = shortSide / 10;
    int windowWidth = windowHeight * 5;

    int verticalSpacing = 10;
    int currentCount = g_notificationCount.load();
    int yPos = (windowHeight + verticalSpacing) * currentCount;

    if (yPos + windowHeight > GetSystemMetrics(SM_CYSCREEN)) {
        yPos = 0;
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
        screenWidth, yPos, windowWidth, windowHeight,
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

    SetLayeredWindowAttributes(hNotifWnd, 0, 230, LWA_ALPHA);

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
            L"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAACXBIWXMAAA7EAAAOxAGVKw4bAAAE7mlUWHRYTUw6Y29tLmFkb2JlLnhtcAAAAAAAPD94cGFja2V0IGJlZ2luPSLvu78iIGlkPSJXNU0wTXBDZWhpSHpyZVN6TlRjemtjOWQiPz4gPHg6eG1wbWV0YSB4bWxuczp4PSJhZG9iZTpuczptZXRhLyIgeDp4bXB0az0iQWRvYmUgWE1QIENvcmUgOS4xLWMwMDIgNzkuNzhiNzYzOCwgMjAyNS8wMi8xMS0xOToxMDowOCAgICAgICAgIj4gPHJkZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4gPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIgeG1sbnM6eG1wPSJodHRwOi8vbnMuYWRvYmUuY29tL3hhcC8xLjAvIiB4bWxuczpkYz0iaHR0cDovL3B1cmwub3JnL2RjL2VsZW1lbnRzLzEuMS8iIHhtbG5zOnBob3Rvc2hvcD0iaHR0cDovL25zLmFkb2JlLmNvbS9waG90b3Nob3AvMS4wLyIgeG1sbnM6eG1wTU09Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC9tbS8iIHhtbG5zOnN0RXZ0PSJodHRwOi8vbnMuYWRvYmUuY29tL3hhcC8xLjAvc1R5cGUvUmVzb3VyY2VFdmVudCMiIHhtcDpDcmVhdG9yVG9vbD0iQWRvYmUgUGhvdG9zaG9wIDI2LjUgKFdpbmRvd3MpIiB4bXA6Q3JlYXRlRGF0ZT0iMjAyNi0wNi0xNFQxNDo0NDo1OSswODowMCIgeG1wOk1vZGlmeURhdGU9IjIwMjYtMDYtMTRUMTU6MDQ6MDArMDg6MDAiIHhtcDpNZXRhZGF0YURhdGU9IjIwMjYtMDYtMTRUMTU6MDQ6MDArMDg6MDAiIGRjOmZvcm1hdD0iaW1hZ2UvcG5nIiBwaG90b3Nob3A6Q29sb3JNb2RlPSIzIiB4bXBNTTpJbnN0YW5jZUlEPSJ4bXAuaWlkOjYyOWNhNTU4LTFmZjctODE0ZS04YTA1LWM2YTgwNzY3ZTE5MyIgeG1wTU06RG9jdW1lbnRJRD0ieG1wLmRpZDo2MjljYTU1OC0xZmY3LTgxNGUtOGEwNS1jNmE4MDc2N2UxOTMiIHhtcE1NOk9yaWdpbmFsRG9jdW1lbnRJRD0ieG1wLmRpZDo2MjljYTU1OC0xZmY3LTgxNGUtOGEwNS1jNmE4MDc2N2UxOTMiPiA8eG1wTU06SGlzdG9yeT4gPHJkZjpTZXE+IDxyZGY6bGkgc3RFdnQ6YWN0aW9uPSJjcmVhdGVkIiBzdEV2dDppbnN0YW5jZUlEPSJ4bXAuaWlkOjYyOWNhNTU4LTFmZjctODE0ZS04YTA1LWM2YTgwNzY3ZTE5MyIgc3RFdnQ6d2hlbj0iMjAyNi0wNi0xNFQxNDo0NDo1OSswODowMCIgc3RFdnQ6c29mdHdhcmVBZ2VudD0iQWRvYmUgUGhvdG9zaG9wIDI2LjUgKFdpbmRvd3MpIi8+IDwvcmRmOlNlcT4gPC94bXBNTTpIaXN0b3J5PiA8L3JkZjpEZXNjcmlwdGlvbj4gPC9yZGY6UkRGPiA8L3g6eG1wbWV0YT4gPD94cGFja2V0IGVuZD0iciI/PlHbY14AAAKmSURBVHic7ZtNboMwFISfQ1acoYtw/0M1i56gi66w6MpVQ8Ce92un6kiVUArPMPPZ/Dpt20bRyjnfiIimr+s7EVGe14WIaJqme/S+XKIb3B/87+Xyv0iFG0D0ePC13yIUagCScDQF4QTUku5BQZgBnGQjKQglAEk4moIQAySJRlEQRgAn2UgK3A2oJZnndSkXQdxtrRRCgCTRKApcDWilf7TMqWEhdwKq5/1puteu/yMocDMATb/2G1JLK1cCWukfLXNqWOjqURRJf79OSuktz+tydsA555vH7bKLAUT8q748r0tK6e1svVoX0ci8C6B9f2/Q/uEIt7ZULmMA2vf32rbtQ1JTI1MDuCO/ZF1rCswJkKaPrONBgZkBVukj21hSYEqANn1kXWsKTAywTh/Z1ooCMwKs0ke2saRAbYBX+kgNCwpMCLBOH9nWigKVAd7pI7W0FKgJ8EofqWFBgdiAqPSRmhoKVAR4p4/U0lIgMiA6faS2lAIxAVHpIzU1FLAN6JU+0oaEAhEB0ekjtaUUsAzonT7SFpcCNgG90kfakFAAGzBK+kibHApYBPROH2mLSwFkwGjpI22jFMAEWKe/33mukVYUNA3wSL/sfPk+QPOhpJYC6M2QR9+3ulM8O0j0bVKVgFH7/l4aCppdYJSRX7IPyFhwasCrpF8kpaBKwOjpF2koODTg1dIvklBwehaISP9opzRnFckZ4YmAsCe9v+YNlL9W+1BdJgWHXcA7/aNJE612EUnGggcDIvu+58dPHAqeCHiVkf9MXAp+DBhh5O/xNumBgMj0z+4Go98mXYji07e8G6wJoeDnOmCk5/xW9ZHrgssIfd9TLQquRO1TUo8Jjd4qFFQfiPSazBiptH1S/OThgdRl6uxI+jfgL4z0UuV5Xb4BpNcoyLX8aZgAAAAASUVORK5CYII="
        );   //没有有效的icon_base64就用它 ↑
    }

    if (useCustomFont) {
        pData->pFontPath = new std::wstring(fontPath);
    } else {
        pData->pFontPath = nullptr;
    }

    SetWindowLongPtr(hNotifWnd, GWLP_USERDATA, (LONG_PTR)pData);

    ShowWindow(hNotifWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hNotifWnd);

    SetLayeredWindowAttributes(hNotifWnd, 0, 230, LWA_ALPHA);

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
        MessageBox(hMainWnd, L"加载成就配置失败，程序将退出！", L"错误", MB_ICONERROR | MB_OK);
        PostQuitMessage(0);
        return;
    }

    LoadAdvancements();

    RECT rc;
    GetClientRect(hMainWnd, &rc);
    int listWidth = rc.right - 20;
    int listHeight = (rc.bottom - 100) / 2 - 10;

    hListCompleted = CreateWindowEx(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | LBS_HASSTRINGS,
        10, 50, listWidth, listHeight,
        hMainWnd, (HMENU)ID_LIST_COMPLETED, hInst, NULL);

    hListUncompleted = CreateWindowEx(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | LBS_HASSTRINGS,
        10, 60 + listHeight + 10, listWidth, listHeight,
        hMainWnd, (HMENU)ID_LIST_UNCOMPLETED, hInst, NULL);

    HFONT hFont = CreateFont(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");

    if (hFont) {
        SendMessage(hListCompleted, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(hListUncompleted, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

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

    RECT rcParent;
    GetWindowRect(hParent, &rcParent);
    int x = rcParent.left + (rcParent.right - rcParent.left) / 2 - 200;
    int y = rcParent.top + (rcParent.bottom - rcParent.top) / 2 - 100;

    g_hDownloadWnd = CreateWindowEx(
        WS_EX_DLGMODALFRAME,
        L"DownloadProgressWindow",
        L"下载成就列表",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, 400, 180,
        hParent, NULL, hInst, NULL
    );

    g_hProgressBar = CreateWindowEx(0, PROGRESS_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        20, 50, 360, 25,
        g_hDownloadWnd, NULL, hInst, NULL);

    g_hStatusText = CreateWindowEx(0, L"STATIC", L"正在连接到服务器...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 85, 360, 20,
        g_hDownloadWnd, NULL, hInst, NULL);

    g_hCancelButton = CreateWindowEx(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        300, 115, 80, 25,
        g_hDownloadWnd, (HMENU)IDCANCEL, hInst, NULL);

    SendMessage(g_hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_hProgressBar, PBM_SETPOS, 0, 0);

    HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
    if (hFont) {
        SendMessage(g_hStatusText, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(g_hCancelButton, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

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
        MessageBox(hWnd, L"当前正在下载，请稍候...", L"提示", MB_ICONINFORMATION | MB_OK);
        return false;
    }

    g_bDownloadCanceled = false;

    ShowDownloadWindow(hWnd);
    g_bDownloading = true;

    g_downloadThread = std::thread([hWnd]() {
        bool bSuccess = false;
        std::wstring errorMessage;

        HINTERNET hInternet = NULL;
        HINTERNET hUrl = NULL;
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

        hInternet = InternetOpen(L"MCAdvancementsOnWin", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
        if (!hInternet) {
            errorMessage = L"初始化网络连接失败！";
            goto cleanup;
        }

        UpdateDownloadProgress(30, L"正在连接到服务器...");
        hUrl = InternetOpenUrl(hInternet,
            L"https://raw.githubusercontent.com/MoyeeLZX/MCAdvancementsOnWin/refs/heads/main/repo/adv.json",
            NULL, 0, INTERNET_FLAG_RELOAD, 0);

        if (!hUrl) {
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

        if (HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_LENGTH, sizeBuffer, &sizeBufferLen, NULL)) {
            fileSize = atoi(sizeBuffer);
        }

        while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
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
        InternetCloseHandle(hUrl);
        hUrl = NULL;
        InternetCloseHandle(hInternet);
        hInternet = NULL;

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
        if (hUrl) {
            InternetCloseHandle(hUrl);
        }
        if (hInternet) {
            InternetCloseHandle(hInternet);
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

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc = { 20, 20, 380, 40 };
        HFONT hFont = CreateFont(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
        SetBkMode(hdc, TRANSPARENT);
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

    switch (message) {
    case WM_ERASEBKGND:
        return 1;

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
        if (pData) {
            if (pData->pAdv) {
                wchar_t debugMsg[512];
                swprintf_s(debugMsg, L"WM_CREATE: 成就标题=%s\n", pData->pAdv->title.c_str());
                OutputDebugString(debugMsg);
            }

            if (pData->pFontPath && GetFileAttributes(pData->pFontPath->c_str()) != INVALID_FILE_ATTRIBUTES) {
                int result = AddFontResourceEx(pData->pFontPath->c_str(), FR_PRIVATE, 0);
                if (result > 0) {
                    OutputDebugString(L"自定义字体加载成功\n");
                    wchar_t debugMsg[256];
                    swprintf_s(debugMsg, L"字体文件: %s, 加载数量: %d\n", pData->pFontPath->c_str(), result);
                    OutputDebugString(debugMsg);
                    SetWindowLongPtr(hWnd, GWLP_USERDATA + 2, 1);
                } else {
                    OutputDebugString(L"自定义字体加载失败，使用默认字体\n");
                }
            }
        }

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        int shortSide = min(screenWidth, screenHeight);

        int windowHeight2 = shortSide / 10;
        int windowWidth2 = windowHeight2 * 5;

        int verticalSpacing = 10;
        int currentCount = g_notificationCount.load() - 1;
        currentY = (windowHeight2 + verticalSpacing) * currentCount;

        SetWindowPos(hWnd, NULL, screenWidth, currentY, windowWidth2, windowHeight2, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        targetX = screenWidth - windowWidth2;
        startX = screenWidth;
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
            targetX = GetSystemMetrics(SM_CXSCREEN);
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
                    if (pData->pFontPath && GetFileAttributes(pData->pFontPath->c_str()) != INVALID_FILE_ATTRIBUTES) {
                        RemoveFontResourceEx(pData->pFontPath->c_str(), FR_PRIVATE, 0);
                        OutputDebugString(L"自定义字体已卸载\n");
                    }

                    if (pData->pIconBitmap) {
                        delete pData->pIconBitmap;
                    }
                    if (pData->pBitmap) {
                        delete pData->pBitmap;
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
        OutputDebugString(L"NotificationWndProc WM_PAINT called\n");
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetWindowRect(hWnd, &rc);
        int windowWidth = rc.right - rc.left;
        int windowHeight = rc.bottom - rc.top;

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, windowWidth, windowHeight);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

        NotificationData* pData = (NotificationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (pData && pData->pBitmap && pData->pBitmap->GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Graphics graphics(hdcMem);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
            graphics.DrawImage(pData->pBitmap, 0, 0, windowWidth, windowHeight);
        }
        else {
            HBRUSH hBrush = CreateSolidBrush(RGB(0, 100, 0));
            RECT rcFill = { 0, 0, windowWidth, windowHeight };
            FillRect(hdcMem, &rcFill, hBrush);
            DeleteObject(hBrush);

            HPEN hPen = CreatePen(PS_SOLID, 2, RGB(255, 215, 0));
            HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
            HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
            Rectangle(hdcMem, 1, 1, windowWidth - 1, windowHeight - 1);
            SelectObject(hdcMem, hOldPen);
            SelectObject(hdcMem, hOldBrush);
            DeleteObject(hPen);
        }

        if (pData && pData->pAdv) {
            SetBkMode(hdcMem, TRANSPARENT);

            Gdiplus::PrivateFontCollection privateFontCollection;
            std::wstring actualFontName = L"微软雅黑";

            if (pData->pFontPath && GetFileAttributes(pData->pFontPath->c_str()) != INVALID_FILE_ATTRIBUTES) {
                Gdiplus::Status status = privateFontCollection.AddFontFile(pData->pFontPath->c_str());
                if (status == Gdiplus::Ok) {
                    int numFound = 0;
                    Gdiplus::FontFamily fontFamily;
                    privateFontCollection.GetFamilies(1, &fontFamily, &numFound);
                    if (numFound > 0) {
                        WCHAR familyName[256] = {0};
                        fontFamily.GetFamilyName(familyName, LANG_NEUTRAL);
                        actualFontName = familyName;
                        wchar_t debugMsg[512];
                        swprintf_s(debugMsg, L"使用自定义字体: %s\n", actualFontName.c_str());
                        OutputDebugString(debugMsg);
                    }
                } else {
                    OutputDebugString(L"加载字体文件失败\n");
                }
            }

            int iconAreaWidth = 0;
            int iconPadding = windowWidth / 24;

            if (pData->pIconBitmap && pData->pIconBitmap->GetLastStatus() == Gdiplus::Ok) {
                int iconSize = windowHeight * 50 / 100;
                iconAreaWidth = iconSize + iconPadding;

                int iconX = iconPadding;
                int iconY = (windowHeight - iconSize) / 2;

                Gdiplus::Graphics iconGraphics(hdcMem);
                iconGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQuality);
                iconGraphics.DrawImage(pData->pIconBitmap, iconX, iconY, iconSize, iconSize);
            }

            int textLeft = iconAreaWidth + iconPadding;
            int padding = windowWidth / 24;

            int baseFontSize = windowHeight * 21 / 100;
            HFONT hBaseFont = CreateFont(baseFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, actualFontName.c_str());

            int advFontSize = windowHeight * 40 / 100;
            HFONT hAdvFont = CreateFont(advFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, actualFontName.c_str());

            RECT rcTitle = { textLeft, windowHeight * 8 / 100, windowWidth - padding, windowHeight * 35 / 100 };
            SetTextColor(hdcMem, RGB(255, 215, 0));
            HFONT hOldFont = (HFONT)SelectObject(hdcMem, hBaseFont);
            DrawText(hdcMem, L"获得成就", -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT rcAdv = { textLeft, windowHeight * 40 / 100, windowWidth - padding, windowHeight * 90 / 100 };
            std::wstring displayTitle = pData->pAdv->title;
            int maxTitleLength = (windowWidth - textLeft) / 13;
            if (maxTitleLength < 5) maxTitleLength = 5;
            if (displayTitle.length() > maxTitleLength) {
                displayTitle = displayTitle.substr(0, maxTitleLength) + L"...";
            }
            SelectObject(hdcMem, hAdvFont);
            DrawText(hdcMem, displayTitle.c_str(), -1, &rcAdv, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdcMem, hOldFont);
            DeleteObject(hBaseFont);
            DeleteObject(hAdvFont);
        }

        BitBlt(hdc, 0, 0, windowWidth, windowHeight, hdcMem, 0, 0, SRCCOPY);

        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hWnd, &ps);
        break;
    }

    case WM_DESTROY: {
        NotificationData* pData = (NotificationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (pData) {
            if (pData->pFontPath && GetFileAttributes(pData->pFontPath->c_str()) != INVALID_FILE_ATTRIBUTES) {
                RemoveFontResourceEx(pData->pFontPath->c_str(), FR_PRIVATE, 0);
                OutputDebugString(L"WM_DESTROY: 自定义字体已卸载\n");
            }

            if (pData->pIconBitmap) {
                delete pData->pIconBitmap;
            }
            if (pData->pBitmap) {
                delete pData->pBitmap;
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
            MessageBox(hWnd, L"无法初始化设置管理器！程序将退出。", L"错误", MB_ICONERROR | MB_OK);
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
        }
        else {
            MessageBox(hWnd, L"无法创建成就管理器！程序将退出。", L"错误", MB_ICONERROR | MB_OK);
            PostQuitMessage(1);
            break;
        }
        break;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
        pMMI->ptMinTrackSize.x = minWidth;
        pMMI->ptMinTrackSize.y = minHeight;
    }
    break;

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
        else if (wmId == IDM_SETTINGS_RELOAD) {
            if (g_pSettingsManager) {
                g_pSettingsManager->LoadSettings();
                g_pSettingsManager->UpdateAllMenuItems(hWnd);
            }
            if (g_pAdvManager) {
                g_pAdvManager->UpdateLists();
            }
            MessageBox(hWnd, L"设置已从 setting.config 重新加载。", L"重新加载设置", MB_OK | MB_ICONINFORMATION);
        }
        else if (wmId == IDM_SETTINGS_CLEAR_SAVE) {
            int result = MessageBox(hWnd,
                L"您确定要清空存档吗？\n这将删除所有已完成的成就记录，删除后无法恢复。",
                L"确认清空存档",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);

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
                        MessageBox(hWnd, L"删除存档文件失败！", L"错误", MB_ICONERROR | MB_OK);
                    }
                }
            }
        }
        else if (wmId == ID_TRAY_SHOW) {
            if (IsWindowVisible(hWnd)) {
                ShowWindow(hWnd, SW_HIDE);
            }
            else {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
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
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
        }
        break;
    }

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        if (g_pAdvManager) {
            RECT rc;
            GetClientRect(hWnd, &rc);

            if (rc.right < minWidth) rc.right = minWidth;
            if (rc.bottom < minHeight) rc.bottom = minHeight;

            int listWidth = rc.right - 20;
            int listHeight = (rc.bottom - 110) / 2 - 10;

            HWND hList1 = GetDlgItem(hWnd, ID_LIST_COMPLETED);
            HWND hList2 = GetDlgItem(hWnd, ID_LIST_UNCOMPLETED);

            if (hList1 && hList2) {
                MoveWindow(hList1, 10, 56, listWidth, listHeight, TRUE);
                MoveWindow(hList2, 10, 72 + listHeight + 18, listWidth, listHeight, TRUE);
            }
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

        int result = MessageBox(hWnd, message.c_str(), L"发现新版本", MB_YESNO | MB_ICONQUESTION | MB_APPLMODAL);

        if (result == IDYES) {
            bool hadOriginalFile = false;
            if (GetFileAttributes(jsonPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (MoveFile(jsonPath.c_str(), backupPath.c_str()) == FALSE) {
                    MessageBox(hWnd, L"备份旧文件失败！", L"错误", MB_ICONERROR | MB_OK | MB_APPLMODAL);
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

                int restartResult = MessageBox(hWnd, successMessage.c_str(), L"更新成功", MB_YESNO | MB_ICONINFORMATION | MB_APPLMODAL);

                if (restartResult == IDYES) {
                    RestartApplication();
                }
            }
            else {
                MessageBox(hWnd, L"更新文件失败！", L"错误", MB_ICONERROR | MB_OK | MB_APPLMODAL);
                if (hadOriginalFile && GetFileAttributes(backupPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    MoveFile(backupPath.c_str(), jsonPath.c_str());
                }
                DeleteFile(tempPath.c_str());
            }
        }
        else {
            DeleteFile(tempPath.c_str());
            MessageBox(hWnd, L"已取消更新。", L"取消更新", MB_ICONINFORMATION | MB_OK | MB_APPLMODAL);
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
        MessageBox(hWnd, pMessage->c_str(), L"已是最新版本", MB_ICONINFORMATION | MB_OK | MB_APPLMODAL);
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
        MessageBox(hWnd, pErrorMessage->c_str(), L"下载错误", MB_ICONERROR | MB_OK | MB_APPLMODAL);
        delete pErrorMessage;

        CloseDownloadWindow();
        g_bDownloading = false;
        if (g_downloadThread.joinable()) {
            g_downloadThread.join();
        }
        break;
    }

    case WM_USER + 104: {
        std::wstring* pMessage = (std::wstring*)lParam;
        MessageBox(hWnd, pMessage->c_str(), L"下载取消", MB_ICONINFORMATION | MB_OK | MB_APPLMODAL);
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

        HFONT hLabelFont = CreateFont(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hLabelFont ? hLabelFont : GetStockObject(DEFAULT_GUI_FONT));

        RECT rc;
        GetClientRect(hWnd, &rc);
        int listHeight = (rc.bottom - 110) / 2 - 10;

        RECT rc1 = { 10, 24, 360, 52 };
        DrawText(hdc, L"已完成成就:", -1, &rc1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        int uncompletedListTop = 72 + listHeight + 18;
        RECT rc2 = { 10, uncompletedListTop - 28, 360, uncompletedListTop - 4 };
        DrawText(hdc, L"未完成成就:", -1, &rc2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (g_pAdvManager) {
            if (!hVersionFont) {
                hVersionFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
            }
            if (hVersionFont) {
                HFONT hOldVersionFont = (HFONT)SelectObject(hdc, hVersionFont);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(100, 100, 100));

                std::wstring versionText = L"成就列表版本: " + g_pAdvManager->GetVersion();
                RECT versionRect = { 10, rc.bottom - 28, rc.right - 10, rc.bottom - 6 };
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
    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 800, 600, nullptr, nullptr, hInstance, nullptr);
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
    switch (message)
    {
    case WM_INITDIALOG:
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
    switch (message)
    {
    case WM_INITDIALOG:
    {
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
