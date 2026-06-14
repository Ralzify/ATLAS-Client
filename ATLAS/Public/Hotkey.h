#pragma once

#include <Windows.h>
#include <ShlObj.h>
#include <string>
#include <fstream>
#include <sstream>

namespace HotkeyPersist
{
    inline std::wstring GetSettingsPath()
    {
        wchar_t path[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path)))
        {
            std::wstring dir = std::wstring(path) + L"\\ATLAS";
            CreateDirectoryW(dir.c_str(), nullptr);
            return dir + L"\\console.json";
        }
        return L"atlasconsole.json";
    }

    inline int Load(int defaultVK = VK_F9)
    {
        auto path = GetSettingsPath();
        std::ifstream f(path);
        if (!f.is_open()) return defaultVK;

        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();

        auto pos = content.find("\"hotkey\"");

        if (pos == std::string::npos) 
            return defaultVK;

        pos = content.find(':', pos);

        if (pos == std::string::npos) 
            return defaultVK;

        pos++;

        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) 
            pos++;

        int vk = 0;
        while (pos < content.size() && std::isdigit((unsigned char)content[pos]))
        {
            vk = vk * 10 + (content[pos] - '0');
            pos++;
        }

        return (vk > 0 && vk <= 254) ? vk : defaultVK;
    }

    inline void Save(int vk)
    {
        auto path = GetSettingsPath();
        std::ofstream f(path, std::ios::trunc);

        if (!f.is_open()) 
            return;

        f << "{\n  \"hotkey\": " << vk << "\n}\n";
    }
}