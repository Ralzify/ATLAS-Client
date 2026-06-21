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

    inline std::string ReadFileContents()
    {
        auto path = GetSettingsPath();
        std::ifstream f(path);

        if (!f.is_open()) 
            return std::string();

        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        return content;
    }

    inline int ParseKey(const std::string& content, const char* key, int defaultVK)
    {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = content.find(needle);

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

    inline int Load(int defaultVK = VK_F9, const char* key = "hotkey")
    {
        std::string content = ReadFileContents();

        if (content.empty()) 
            return defaultVK;

        return ParseKey(content, key, defaultVK);
    }

    inline void Save(int vk, const char* key = "hotkey")
    {
        std::string content = ReadFileContents();

        int hotkey = ParseKey(content, "hotkey", VK_F9);
        int joinHotkey = ParseKey(content, "joinHotkey", VK_F5);

        if (strcmp(key, "hotkey") == 0)
            hotkey = vk;
        else if (strcmp(key, "joinHotkey") == 0)
            joinHotkey = vk;

        auto path = GetSettingsPath();
        std::ofstream f(path, std::ios::trunc);

        if (!f.is_open())
            return;

        f << "{\n  \"hotkey\": " << hotkey << ",\n  \"joinHotkey\": " << joinHotkey << "\n}\n";
    }
}