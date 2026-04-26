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
HWND g_hMainWnd = nullptr;  // 全局主窗口句柄

// 下载进度窗口相关
HWND g_hDownloadWnd = nullptr;
HWND g_hProgressBar = nullptr;
HWND g_hStatusText = nullptr;
HWND g_hCancelButton = nullptr;  // 新增：取消按钮
std::thread g_downloadThread;
std::atomic<bool> g_bDownloading(false);
std::atomic<bool> g_bDownloadCanceled(false);  // 新增：下载取消标志

// 成就通知队列
std::queue<Advancement> g_achievementQueue;
std::mutex g_queueMutex;
std::atomic<bool> g_showingNotification(false);
std::atomic<int> g_notificationCount(0);  // 通知窗口计数器

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK NotificationWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK DownloadWndProc(HWND, UINT, WPARAM, LPARAM);
void RestartApplication();

// 添加：下载成就列表函数
bool DownloadAdvancementJson(HWND hWnd);
void ShowDownloadWindow(HWND hParent);
void CloseDownloadWindow();
void UpdateDownloadProgress(int progress, const std::wstring& status);

// 新增：解析版本信息函数
std::wstring ExtractJSONVersion(const std::string& jsonContent);

// 新增：比较版本函数
bool IsNewerVersion(const std::wstring& currentVersion, const std::wstring& newVersion);

// 新增：取消下载函数
void CancelDownload();

// 新增：处理成就通知队列
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

// 新增：添加成就到队列
void AddAchievementToQueue(const Advancement& adv) {
    OutputDebugString(L"AddAchievementToQueue called\n");

    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_achievementQueue.push(adv);

    wchar_t debugMsg[256];
    swprintf_s(debugMsg, L"成就已添加到队列: %s\n", adv.title.c_str());
    OutputDebugString(debugMsg);

    // 如果当前没有正在显示通知，立即处理
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

// 新增：处理成就队列
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

        // 显示通知
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

// 新增：显示下一个成就
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

// 新增：解析版本信息函数
std::wstring ExtractJSONVersion(const std::string& jsonContent) {
    // 查找"version"字段
    std::string searchStr = "\"version\":";
    size_t pos = jsonContent.find(searchStr);
    if (pos == std::string::npos) {
        return L"";
    }

    pos += searchStr.length();

    // 跳过空格和制表符
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

// 新增：比较版本函数
bool IsNewerVersion(const std::wstring& currentVersion, const std::wstring& newVersion) {
    if (currentVersion.empty()) return true;  // 如果当前没有版本，则认为新版本是更新的

    // 解析版本字符串格式："yyyy/mm/dd hh:mm"
    // 转换为时间戳进行比较
    struct tm currentTm = {}, newTm = {};
    std::wistringstream currentStream(currentVersion);
    std::wistringstream newStream(newVersion);

    // 尝试解析日期和时间
    currentStream >> std::get_time(&currentTm, L"%Y/%m/%d %H:%M");
    newStream >> std::get_time(&newTm, L"%Y/%m/%d %H:%M");

    if (currentStream.fail() || newStream.fail()) {
        // 如果解析失败，使用字符串比较
        return newVersion > currentVersion;
    }

    // 转换为time_t进行比较
    time_t currentTime = mktime(&currentTm);
    time_t newTime = mktime(&newTm);

    return difftime(newTime, currentTime) > 0;
}

// 添加：检查JSON文件是否有效的函数
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

    // 检查是否包含必要的字段
    return (content.find("\"adv_list_info\"") != std::string::npos &&
        content.find("\"achievements\"") != std::string::npos &&
        content.find("\"id\"") != std::string::npos &&
        content.find("\"title\"") != std::string::npos);
}

// 新增：取消下载函数
void CancelDownload() {
    if (g_bDownloading) {
        g_bDownloadCanceled = true;

        // 通知下载线程停止
        if (g_downloadThread.joinable()) {
            // 等待下载线程结束（设置超时）
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
    version = L"";  // 初始化版本为空
}

AdvancementManager::~AdvancementManager() {
    StopMonitoring();
}

bool AdvancementManager::LoadAdvancementsFromJSON() {
    advancements.clear();
    version = L"";  // 清空版本信息

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

    // 首先解析版本信息
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

                // 使用ExtractJSONValue来解析num字段（因为它是带引号的字符串）
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

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, processName.c_str()) == 0) {
                CloseHandle(hSnapshot);
                return true;
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

            // 将成就添加到队列中，而不是立即显示
            AddAchievementToQueue(adv);
            OutputDebugString(L"成就触发: ");
            OutputDebugString(adv.title.c_str());
            OutputDebugString(L"\n");
            break;
        }
    }
}

void AdvancementManager::ShowAdvancementNotification(const Advancement& adv) {
    // 每次都重新注册窗口类，确保参数正确
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = NotificationWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = NULL;  // NULL背景刷，完全透明
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpszClassName = L"AdvancementNotification";

    // 尝试注销旧的窗口类，然后重新注册
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

    // 窗口高度为屏幕短边的1/10，保持5:1比例（宽:高）
    int windowHeight = shortSide / 10;
    int windowWidth = windowHeight * 5;

    // 计算垂直位置，每个窗口间隔10像素（从屏幕顶部开始）
    int verticalSpacing = 10;
    int currentCount = g_notificationCount.load();
    int yPos = (windowHeight + verticalSpacing) * currentCount;

    // 限制最大显示数量，避免超出屏幕
    if (yPos + windowHeight > GetSystemMetrics(SM_CYSCREEN)) {
        yPos = 0;
    }

    // 增加计数
    g_notificationCount++;

    // 加载自定义字体（mc_fonts.ttf）
    std::wstring fontPath;
    WCHAR exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);
    std::wstring exeDir = std::wstring(exePath).substr(0, std::wstring(exePath).find_last_of(L"\\/"));
    fontPath = exeDir + L"\\bin\\mc_fonts.ttf";

    // 检查字体文件是否存在
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

    // 恢复使用SetLayeredWindowAttributes
    SetLayeredWindowAttributes(hNotifWnd, 0, 230, LWA_ALPHA);

    NotificationData* pData = new NotificationData();
    pData->pAdv = new Advancement(adv);

    // 加载位图
    WCHAR bgPath[MAX_PATH];
    GetModuleFileName(NULL, bgPath, MAX_PATH);
    std::wstring bgExeDir = std::wstring(bgPath).substr(0, std::wstring(bgPath).find_last_of(L"\\/"));
    std::wstring bgFile = bgExeDir + L"\\bin\\adv_back.png";
    pData->pBitmap = Gdiplus::Bitmap::FromFile(bgFile.c_str());

    // 设置字体路径
    if (useCustomFont) {
        pData->pFontPath = new std::wstring(fontPath);
    } else {
        pData->pFontPath = nullptr;
    }

    SetWindowLongPtr(hNotifWnd, GWLP_USERDATA, (LONG_PTR)pData);

    ShowWindow(hNotifWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hNotifWnd);

    // 设置窗口透明度（使用UpdateLayeredWindow）
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
                OutputDebugString(L"音效文件不存在！\n");
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
    int listWidth = rc.right - 20;  // 宽度为窗口宽度减去边距
    int listHeight = (rc.bottom - 100) / 2 - 10;  // 高度为窗口高度的一半减去边距和标题区域

    // 已完成成就列表在上方
    hListCompleted = CreateWindowEx(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL | LBS_HASSTRINGS,
        10, 50, listWidth, listHeight,
        hMainWnd, (HMENU)ID_LIST_COMPLETED, hInst, NULL);

    // 未完成成就列表在下方
    hListUncompleted = CreateWindowEx(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL | LBS_HASSTRINGS,
        10, 60 + listHeight + 10, listWidth, listHeight,
        hMainWnd, (HMENU)ID_LIST_UNCOMPLETED, hInst, NULL);

    // 主窗口列表框使用还原的字体大小（16号）
    HFONT hFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,  // 恢复为16号
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

    for (const auto& adv : advancements) {
        // 构建列表项文本：编号. 标题 - 描述\n触发方式: 触发描述
        std::wstring item;
        if (!adv.num.empty()) {
            item = adv.num + L". " + adv.title + L" - " + adv.description;
        }
        else {
            item = adv.title + L" - " + adv.description;
        }

        // 添加触发方式信息
        std::wstring triggerInfo = L"\n触发方式: " + adv.triggerDescription;

        // 合并完整文本
        std::wstring fullItem = item + triggerInfo;

        if (adv.completed) {
            SendMessage(hListCompleted, LB_ADDSTRING, 0, (LPARAM)fullItem.c_str());
        }
        else {
            SendMessage(hListUncompleted, LB_ADDSTRING, 0, (LPARAM)fullItem.c_str());
        }
    }
}

void AdvancementManager::PlaySoundAsync(const std::wstring& soundPath) {
    if (g_pSettingsManager && !g_pSettingsManager->IsSoundEnabled()) {
        return;
    }

    std::thread([soundPath]() {
        PlayAudioFile(soundPath);
        }).detach();
}

// 重启应用程序
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

// 新增：显示下载窗口
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

    // 创建进度条
    g_hProgressBar = CreateWindowEx(0, PROGRESS_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        20, 50, 360, 25,
        g_hDownloadWnd, NULL, hInst, NULL);

    // 创建状态文本
    g_hStatusText = CreateWindowEx(0, L"STATIC", L"正在连接到服务器...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 85, 360, 20,
        g_hDownloadWnd, NULL, hInst, NULL);

    // 创建取消按钮
    g_hCancelButton = CreateWindowEx(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        300, 115, 80, 25,
        g_hDownloadWnd, (HMENU)IDCANCEL, hInst, NULL);

    // 设置进度条范围
    SendMessage(g_hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_hProgressBar, PBM_SETPOS, 0, 0);

    // 设置字体
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

// 新增：关闭下载窗口
void CloseDownloadWindow() {
    if (g_hDownloadWnd) {
        DestroyWindow(g_hDownloadWnd);
        g_hDownloadWnd = nullptr;
        g_hProgressBar = nullptr;
        g_hStatusText = nullptr;
        g_hCancelButton = nullptr;
    }
}

// 新增：更新下载进度
void UpdateDownloadProgress(int progress, const std::wstring& status) {
    if (g_hProgressBar) {
        SendMessage(g_hProgressBar, PBM_SETPOS, progress, 0);
    }
    if (g_hStatusText) {
        SetWindowText(g_hStatusText, status.c_str());
    }
}

// 下载成就列表函数
bool DownloadAdvancementJson(HWND hWnd) {
    // 检查是否已经在下载
    if (g_bDownloading) {
        MessageBox(hWnd, L"当前正在下载，请稍候...", L"提示", MB_ICONINFORMATION | MB_OK);
        return false;
    }

    // 重置取消标志
    g_bDownloadCanceled = false;

    // 显示下载窗口
    ShowDownloadWindow(hWnd);
    g_bDownloading = true;

    // 在单独的线程中执行下载
    g_downloadThread = std::thread([hWnd]() {
        bool bSuccess = false;
        std::wstring errorMessage;

        // 提前声明所有变量
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

        // 获取程序目录
        WCHAR exePath[MAX_PATH];
        GetModuleFileName(NULL, exePath, MAX_PATH);
        std::wstring exeDir = std::wstring(exePath).substr(0, std::wstring(exePath).find_last_of(L"\\/"));
        std::wstring jsonPath = exeDir + L"\\bin\\adv.json";
        std::wstring backupPath = exeDir + L"\\bin\\adv.json.bak";
        std::wstring tempPath = exeDir + L"\\bin\\adv.json.tmp";  // 临时文件路径

        // 获取当前版本信息
        std::wstring currentVersion = L"";
        if (g_pAdvManager) {
            currentVersion = g_pAdvManager->GetVersion();
        }

        // 删除可能存在的临时文件
        if (GetFileAttributes(tempPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFile(tempPath.c_str());
        }

        // 创建bin目录（如果不存在）
        std::wstring binDir = exeDir + L"\\bin";
        if (GetFileAttributes(binDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            CreateDirectory(binDir.c_str(), NULL);
        }

        // 使用WinINet下载文件到临时文件
        UpdateDownloadProgress(10, L"正在初始化网络连接...");

        hInternet = InternetOpen(L"MCAdvancementsOnWin", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
        if (!hInternet) {
            errorMessage = L"初始化网络连接失败！";
            goto cleanup;
        }

        // 打开URL（修改后的地址）
        UpdateDownloadProgress(30, L"正在连接到服务器...");
        hUrl = InternetOpenUrl(hInternet,
            L"https://png.2btb.top/repo/mc_adv_on_win/adv.json",
            NULL, 0, INTERNET_FLAG_RELOAD, 0);

        if (!hUrl) {
            errorMessage = L"无法连接到服务器！";
            goto cleanup;
        }

        // 创建临时文件
        UpdateDownloadProgress(50, L"正在创建临时文件...");
        hFile = CreateFile(tempPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            errorMessage = L"创建临时文件失败！";
            goto cleanup;
        }

        // 下载数据到临时文件
        UpdateDownloadProgress(70, L"正在下载数据...");
        success = TRUE;
        totalBytes = 0;
        fileSize = 0;

        // 尝试获取文件大小
        if (HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_LENGTH, sizeBuffer, &sizeBufferLen, NULL)) {
            fileSize = atoi(sizeBuffer);
        }

        while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            // 检查是否取消了下载
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

            // 更新进度
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

        // 检查是否取消了下载
        if (g_bDownloadCanceled) {
            // 删除临时文件
            DeleteFile(tempPath.c_str());
            // 在主线程中显示取消消息
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

        // 读取临时文件内容来获取版本信息
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

        // 检查下载的文件是否有效
        if (!IsJSONFileValid(tempPath)) {
            // 如果下载的文件无效，删除临时文件
            DeleteFile(tempPath.c_str());
            errorMessage = L"下载的文件格式不正确！";
            goto cleanup;
        }

        UpdateDownloadProgress(100, L"下载完成，正在处理...");

        // 检查版本是否更新
        if (IsNewerVersion(currentVersion, downloadedVersion)) {
            // 版本更新，准备文件替换数据
            std::wstring* pData = new std::wstring[4];
            pData[0] = tempPath;           // 临时文件路径
            pData[1] = jsonPath;           // 目标文件路径
            pData[2] = backupPath;         // 备份文件路径
            pData[3] = downloadedVersion;  // 新版本号

            // 在主线程中显示确认对话框
            PostMessage(hWnd, WM_USER + 101, 0, (LPARAM)pData);
            return;  // 这里返回，等待用户响应
        }
        else {
            // 版本不是更新的
            DeleteFile(tempPath.c_str());

            std::wstring* pMessage = new std::wstring(L"当前已是最新版本！\n\n");
            *pMessage += L"当前版本: " + (currentVersion.empty() ? L"未知版本" : currentVersion) + L"\n";
            if (currentVersion == downloadedVersion) {
                *pMessage += L"服务器版本: " + downloadedVersion + L" (与当前版本相同)\n";
            }
            else {
                *pMessage += L"服务器版本: " + downloadedVersion + L" (比当前版本旧)\n";
            }

            // 在主线程中显示消息框
            PostMessage(hWnd, WM_USER + 102, 0, (LPARAM)pMessage);
        }

        bSuccess = true;
        return;

    cleanup:
        // 清理资源
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
        }
        if (hUrl) {
            InternetCloseHandle(hUrl);
        }
        if (hInternet) {
            InternetCloseHandle(hInternet);
        }

        // 在主线程中显示错误消息
        std::wstring* pErrorMessage = new std::wstring(errorMessage);
        PostMessage(hWnd, WM_USER + 103, 0, (LPARAM)pErrorMessage);
        });

    return true;
}

LRESULT CALLBACK DownloadWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        // 设置窗口居中
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDCANCEL) {
            // 用户点击了取消按钮
            CancelDownload();
        }
        break;

    case WM_CLOSE:
        // 如果正在下载，调用取消函数
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

        // 绘制标题
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

// 修复后的通知窗口过程
LRESULT CALLBACK NotificationWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static int animationStep = 0;
    static int targetX = 0;
    static int startX = 0;
    static int currentY = 0;

    switch (message) {
    case WM_ERASEBKGND:
        // 防止Windows清除背景，避免白色边框
        return 1;

    case WM_CREATE: {
        OutputDebugString(L"WM_CREATE called\n");

        // 确保窗口为无边框窗口
        LONG_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);
        if (style & WS_CAPTION) {
            SetWindowLongPtr(hWnd, GWL_STYLE, style & ~WS_CAPTION);
        }

        // 确保分层窗口标志
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

            // 加载自定义字体到系统（用于WM_PAINT中的文字绘制）
            if (pData->pFontPath && GetFileAttributes(pData->pFontPath->c_str()) != INVALID_FILE_ATTRIBUTES) {
                int result = AddFontResourceEx(pData->pFontPath->c_str(), FR_PRIVATE, 0);
                if (result > 0) {
                    OutputDebugString(L"自定义字体加载成功\n");
                    wchar_t debugMsg[256];
                    swprintf_s(debugMsg, L"字体文件: %s, 加载数量: %d\n", pData->pFontPath->c_str(), result);
                    OutputDebugString(debugMsg);
                    SetWindowLongPtr(hWnd, GWLP_USERDATA + 2, 1);  // 标记字体已加载
                } else {
                    OutputDebugString(L"自定义字体加载失败，使用默认字体\n");
                }
            }
        }

        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        int shortSide = min(screenWidth, screenHeight);

        // 窗口高度为屏幕短边的1/10，保持5:1比例（宽:高）
        int windowHeight2 = shortSide / 10;
        int windowWidth2 = windowHeight2 * 5;

        // 窗口从屏幕顶部开始（正右上方）
        // 计算Y坐标以支持多个通知窗口堆叠
        int verticalSpacing = 10;
        int currentCount = g_notificationCount.load() - 1;  // 减1是因为这个窗口已经被创建
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

                // 清理资源
                NotificationData* pData = (NotificationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
                if (pData) {
                    // 卸载自定义字体
                    if (pData->pFontPath && GetFileAttributes(pData->pFontPath->c_str()) != INVALID_FILE_ATTRIBUTES) {
                        RemoveFontResourceEx(pData->pFontPath->c_str(), FR_PRIVATE, 0);
                        OutputDebugString(L"自定义字体已卸载\n");
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

                // 减少通知计数
                g_notificationCount--;
                g_showingNotification = false;

                // 显示下一个成就
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

        // 获取窗口大小（动态计算）
        RECT rc;
        GetWindowRect(hWnd, &rc);
        int windowWidth = rc.right - rc.left;
        int windowHeight = rc.bottom - rc.top;

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, windowWidth, windowHeight);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

        // 不清除背景，直接绘制PNG
        NotificationData* pData = (NotificationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (pData && pData->pBitmap && pData->pBitmap->GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Graphics graphics(hdcMem);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
            graphics.DrawImage(pData->pBitmap, 0, 0, windowWidth, windowHeight);
        }
        else {
            // 如果位图不存在，使用默认背景
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

            // 使用GDI+ PrivateFontCollection加载自定义字体获取真实字体名
            Gdiplus::PrivateFontCollection privateFontCollection;
            std::wstring actualFontName = L"微软雅黑";  // 默认字体

            if (pData->pFontPath && GetFileAttributes(pData->pFontPath->c_str()) != INVALID_FILE_ATTRIBUTES) {
                Gdiplus::Status status = privateFontCollection.AddFontFile(pData->pFontPath->c_str());
                if (status == Gdiplus::Ok) {
                    // 获取字体家族名称
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

            // "获得成就"字体大小为窗口高度的约21%
            int baseFontSize = windowHeight * 21 / 100;
            HFONT hBaseFont = CreateFont(baseFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, actualFontName.c_str());

            // 成就名称字体大小为窗口高度的40%
            int advFontSize = windowHeight * 40 / 100;
            HFONT hAdvFont = CreateFont(advFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, actualFontName.c_str());

            // 绘制"获得成就"（黄色）
            int padding = windowWidth / 24;
            RECT rcTitle = { padding, windowHeight * 8 / 100, windowWidth - padding, windowHeight * 35 / 100 };
            SetTextColor(hdcMem, RGB(255, 215, 0));
            HFONT hOldFont = (HFONT)SelectObject(hdcMem, hBaseFont);
            DrawText(hdcMem, L"获得成就", -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // 绘制成就名称（黄色）
            RECT rcAdv = { padding, windowHeight * 40 / 100, windowWidth - padding, windowHeight * 90 / 100 };
            std::wstring displayTitle = pData->pAdv->title;
            int maxTitleLength = windowWidth / 13;
            if (displayTitle.length() > maxTitleLength) {
                displayTitle = displayTitle.substr(0, maxTitleLength) + L"...";
            }
            SelectObject(hdcMem, hAdvFont);
            DrawText(hdcMem, displayTitle.c_str(), -1, &rcAdv, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // 恢复并清理字体
            SelectObject(hdcMem, hOldFont);
            DeleteObject(hBaseFont);
            DeleteObject(hAdvFont);
        }

        BitBlt(hdc, 0, 0, windowWidth, windowHeight, hdcMem, 0, 0, SRCCOPY);

        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        // 使用UpdateLayeredWindow来正确处理透明度（避免白色边框）
        // 注意：我们不使用它，因为BeginPaint/EndPaint已经有正确的处理
        // 真正的问题可能是GDI+在绘制PNG时没有正确处理Alpha通道

        EndPaint(hWnd, &ps);
        break;
    }

    case WM_DESTROY: {
        // 确保清理所有资源（如果定时器未触发完成）
        // 清理资源
        NotificationData* pData = (NotificationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (pData) {
            // 卸载自定义字体
            if (pData->pFontPath && GetFileAttributes(pData->pFontPath->c_str()) != INVALID_FILE_ATTRIBUTES) {
                RemoveFontResourceEx(pData->pFontPath->c_str(), FR_PRIVATE, 0);
                OutputDebugString(L"WM_DESTROY: 自定义字体已卸载\n");
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

        // 减少通知计数（如果还没有减少）
        if (g_showingNotification) {
            g_notificationCount--;
            g_showingNotification = false;

            // 显示下一个成就
            if (g_hMainWnd) {
                PostMessage(g_hMainWnd, WM_USER + 105, 0, 0);
            }
        }

        // 杀掉所有定时器
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
    static HFONT hVersionFont = NULL;  // 静态变量用于版本信息字体
    static int minWidth = 600;  // 最小宽度
    static int minHeight = 400;  // 最小高度

    switch (message) {
    case WM_CREATE: {
        // 创建设置管理器
        g_pSettingsManager = new (std::nothrow) SettingsManager();
        if (g_pSettingsManager) {
            g_pSettingsManager->LoadSettings();
        }
        else {
            MessageBox(hWnd, L"无法初始化设置管理器！程序将退出。", L"错误", MB_ICONERROR | MB_OK);
            PostQuitMessage(1);
            break;
        }

        // 保存主窗口句柄
        g_hMainWnd = hWnd;

        // 创建成就管理器
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

        // 创建版本信息字体
        hVersionFont = CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
        break;
    }

    case WM_GETMINMAXINFO:
        // 限制窗口最小大小
    {
        MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
        pMMI->ptMinTrackSize.x = minWidth;
        pMMI->ptMinTrackSize.y = minHeight;
    }
    break;

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDM_HELP_ABOUT) {
            MessageBox(hWnd, L"MC Advancements on Windows \nversion : beta-2026.4.20 \n检测Windows操作并解锁成就！",
                L"关于", MB_OK);
        }
        else if (wmId == IDM_FILE_EXIT) {
            DestroyWindow(hWnd);
        }
        else if (wmId == IDM_FILE_UPDATE_JSON) {  // 新增：更新成就列表
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
        else if (wmId == IDM_SETTINGS_CLEAR_SAVE) {
            // 只有一个确认对话框，"是"在左侧（中文Windows默认就是"是"在左侧）
            int result = MessageBox(hWnd,
                L"您确定要清空存档吗？\n这将删除所有已完成的成就记录，删除后无法恢复。",
                L"确认清空存档",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2); // 默认焦点在"否"上

            // 用户点击"是"才能继续（"是"是第一个按钮，在左侧）
            if (result == IDYES) {
                WCHAR exePath[MAX_PATH];
                GetModuleFileName(NULL, exePath, MAX_PATH);
                std::wstring exeDir = std::wstring(exePath).substr(0, std::wstring(exePath).find_last_of(L"\\/"));
                std::wstring saveFile = exeDir + L"\\adv_save.txt";

                if (DeleteFile(saveFile.c_str())) {
                    // 重新启动程序
                    RestartApplication();
                }
                else {
                    // 如果文件不存在，也认为是成功（可能已经被删除）
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
        break;
    }

    case WM_ADVANCEMENT_TRIGGERED: {
        // 这个处理已经被成就队列替代，但保留以防万一
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

    case WM_SIZE:
        if (g_pAdvManager) {
            RECT rc;
            GetClientRect(hWnd, &rc);

            // 确保窗口不会小于最小值
            if (rc.right < minWidth) rc.right = minWidth;
            if (rc.bottom < minHeight) rc.bottom = minHeight;

            int listWidth = rc.right - 20;  // 宽度为窗口宽度减去边距
            int listHeight = (rc.bottom - 100) / 2 - 10;  // 高度为窗口高度的一半减去边距和标题区域

            HWND hList1 = GetDlgItem(hWnd, ID_LIST_COMPLETED);
            HWND hList2 = GetDlgItem(hWnd, ID_LIST_UNCOMPLETED);

            if (hList1 && hList2) {
                // 已完成成就列表在上方
                MoveWindow(hList1, 10, 50, listWidth, listHeight, TRUE);
                // 未完成成就列表在下方 - 修改为60 + listHeight + 10
                MoveWindow(hList2, 10, 60 + listHeight + 10, listWidth, listHeight, TRUE);
            }
        }
        break;

    case WM_USER + 101: {  // 发现新版本，询问用户
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

        // 确保在主窗口中央显示消息框
        int result = MessageBox(hWnd, message.c_str(), L"发现新版本", MB_YESNO | MB_ICONQUESTION | MB_APPLMODAL);

        if (result == IDYES) {
            // 用户选择更新，备份原文件并替换
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

            // 将临时文件重命名为正式文件
            if (MoveFile(tempPath.c_str(), jsonPath.c_str())) {
                // 删除备份文件（如果有）
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
                // 重命名失败，尝试恢复备份文件
                MessageBox(hWnd, L"更新文件失败！", L"错误", MB_ICONERROR | MB_OK | MB_APPLMODAL);
                if (hadOriginalFile && GetFileAttributes(backupPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    MoveFile(backupPath.c_str(), jsonPath.c_str());
                }
                DeleteFile(tempPath.c_str());
            }
        }
        else {
            // 用户选择不更新，删除临时文件
            DeleteFile(tempPath.c_str());
            MessageBox(hWnd, L"已取消更新。", L"取消更新", MB_ICONINFORMATION | MB_OK | MB_APPLMODAL);
        }

        // 清理内存
        delete[] pData;

        // 关闭下载窗口
        CloseDownloadWindow();
        g_bDownloading = false;
        if (g_downloadThread.joinable()) {
            g_downloadThread.join();
        }
        break;
    }

    case WM_USER + 102: {  // 已是最新版本
        std::wstring* pMessage = (std::wstring*)lParam;
        MessageBox(hWnd, pMessage->c_str(), L"已是最新版本", MB_ICONINFORMATION | MB_OK | MB_APPLMODAL);
        delete pMessage;

        // 关闭下载窗口
        CloseDownloadWindow();
        g_bDownloading = false;
        if (g_downloadThread.joinable()) {
            g_downloadThread.join();
        }
        break;
    }

    case WM_USER + 103: {  // 下载错误
        std::wstring* pErrorMessage = (std::wstring*)lParam;
        MessageBox(hWnd, pErrorMessage->c_str(), L"下载错误", MB_ICONERROR | MB_OK | MB_APPLMODAL);
        delete pErrorMessage;

        // 关闭下载窗口
        CloseDownloadWindow();
        g_bDownloading = false;
        if (g_downloadThread.joinable()) {
            g_downloadThread.join();
        }
        break;
    }

    case WM_USER + 104: {  // 下载取消
        std::wstring* pMessage = (std::wstring*)lParam;
        MessageBox(hWnd, pMessage->c_str(), L"下载取消", MB_ICONINFORMATION | MB_OK | MB_APPLMODAL);
        delete pMessage;

        // 关闭下载窗口
        CloseDownloadWindow();
        g_bDownloading = false;
        g_bDownloadCanceled = false;
        if (g_downloadThread.joinable()) {
            g_downloadThread.join();
        }
        break;
    }

    case WM_USER + 105: {  // 处理成就队列
        OutputDebugString(L"收到WM_USER + 105消息\n");
        ShowNextAchievement(hWnd);
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        HFONT hOldFont = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));

        RECT rc;
        GetClientRect(hWnd, &rc);
        int listHeight = (rc.bottom - 100) / 2 - 10;

        // 已完成成就标题在上方
        RECT rc1 = { 10, 25, 200, 45 };
        DrawText(hdc, L"已完成成就:", -1, &rc1, DT_LEFT);

        // 未完成成就标题在下方列表上方（调整位置，使其在列表上方显示）
        // 列表位置是60 + listHeight + 10，标题应该在这个位置上方
        int uncompletedListTop = 60 + listHeight + 10;
        RECT rc2 = { 10, uncompletedListTop - 20, 200, uncompletedListTop - 5 };
        DrawText(hdc, L"未完成成就:", -1, &rc2, DT_LEFT);

        // 绘制版本信息在左下角
        if (g_pAdvManager && hVersionFont) {
            HFONT hOldVersionFont = (HFONT)SelectObject(hdc, hVersionFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(100, 100, 100));  // 灰色文字

            std::wstring versionText = L"成就列表版本: " + g_pAdvManager->GetVersion();
            RECT versionRect = { 10, rc.bottom - 25, rc.right - 10, rc.bottom - 5 };
            DrawText(hdc, versionText.c_str(), -1, &versionRect, DT_LEFT);

            SelectObject(hdc, hOldVersionFont);
        }

        SelectObject(hdc, hOldFont);

        EndPaint(hWnd, &ps);
        break;
    }

    case WM_CLOSE:
        // 直接销毁窗口
        DestroyWindow(hWnd);
        break;

    case WM_DESTROY:
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
    return TRUE;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPWSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 初始化GDI+
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    // 初始化公共控件（确保包含进度条控件）
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
