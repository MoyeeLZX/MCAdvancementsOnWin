#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mmsystem.h>
#include <psapi.h>
#include <fstream>
#include <gdiplus.h>
#include "resource.h"
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "gdiplus.lib")

#define WM_ADVANCEMENT_TRIGGERED (WM_USER + 100)

enum AdvancementTriggerType {
    TRIGGER_NONE = 0,
    TRIGGER_WINDOW_TITLE,
    TRIGGER_PROCESS_NAME
};

struct Advancement {
    std::wstring id;
    std::wstring num;  // 新增：成就编号
    std::wstring title;
    std::wstring description;
    std::wstring triggerDescription;
    std::wstring triggerValue;
    AdvancementTriggerType triggerType;
    bool completed = false;
};

// 通知窗口数据结构体
struct NotificationData {
    Advancement* pAdv;
    Gdiplus::Bitmap* pBitmap;
    std::wstring* pFontPath;  // 字体文件路径（可选）
};

// 设置管理器
class SettingsManager {
private:
    std::wstring configFilePath;
    bool soundEnabled;

public:
    SettingsManager() : soundEnabled(true) {
        WCHAR exePath[MAX_PATH];
        GetModuleFileName(NULL, exePath, MAX_PATH);
        std::wstring exeDir = std::wstring(exePath).substr(0, std::wstring(exePath).find_last_of(L"\\/"));
        configFilePath = exeDir + L"\\setting.config";
    }

    void LoadSettings() {
        soundEnabled = true;

        std::wifstream file(configFilePath);
        if (file.is_open()) {
            std::wstring line;
            while (std::getline(file, line)) {
                size_t pos = line.find(L'=');
                if (pos != std::wstring::npos) {
                    std::wstring key = line.substr(0, pos);
                    std::wstring value = line.substr(pos + 1);

                    if (key == L"sound") {
                        soundEnabled = (value == L"1" || value == L"true");
                    }
                }
            }
            file.close();
        }
    }

    void SaveSettings() {
        std::wofstream file(configFilePath);
        if (file.is_open()) {
            file << L"sound=" << (soundEnabled ? L"1" : L"0") << std::endl;
            file.close();
        }
    }

    bool IsSoundEnabled() const { return soundEnabled; }
    void SetSoundEnabled(bool enabled) { soundEnabled = enabled; }

    void UpdateSoundMenuItem(HWND hWnd, HMENU hMenu) {
        if (hMenu) {
            UINT checkState = soundEnabled ? MF_CHECKED : MF_UNCHECKED;
            CheckMenuItem(hMenu, IDM_SETTINGS_SOUND, checkState);
        }
    }

    void UpdateAllMenuItems(HWND hWnd) {  // 更新所有菜单项
        HMENU hMenu = GetMenu(hWnd);
        if (hMenu) {
            UpdateSoundMenuItem(hWnd, hMenu);
        }
    }
};

class AdvancementManager {
private:
    std::vector<Advancement> advancements;
    std::map<std::wstring, bool> completedAdvancements;
    std::wstring saveFilePath;
    std::wstring jsonFilePath;
    HWND hMainWnd;
    HWND hListCompleted;
    HWND hListUncompleted;
    std::wstring version;  // 新增：版本信息

    std::thread monitorThread;
    std::atomic<bool> monitoring;

    bool CheckWindowTitle(const std::wstring& targetTitle);
    bool CheckProcessExists(const std::wstring& processName);
    bool LoadAdvancementsFromJSON();
    void LoadAdvancements();
    void SaveAdvancements();
    void MonitoringThread();

public:
    AdvancementManager(HWND hWnd);
    ~AdvancementManager();

    void Initialize();
    void TriggerAdvancement(const std::wstring& id);
    void ShowAdvancementNotification(const Advancement& adv);
    void UpdateLists();
    void StartMonitoring();
    void StopMonitoring();
    void CheckAndTriggerAdvancements();
    const std::wstring& GetVersion() const { return version; }  // 新增：获取版本信息

    static void PlaySoundAsync(const std::wstring& soundPath);
};
