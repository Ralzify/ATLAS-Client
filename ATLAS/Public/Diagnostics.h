#pragma once

#include <Windows.h>
#include <ShlObj.h>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace AtlasDiagnostics
{
    inline std::wstring GetPath()
    {
        wchar_t appData[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        {
            std::wstring directory = std::wstring(appData) + L"\\ATLAS";
            CreateDirectoryW(directory.c_str(), nullptr);
            return directory + L"\\diagnostics.log";
        }

        return L"atlas-diagnostics.log";
    }

    inline void WriteLine(const char* format, ...)
    {
        char message[1024]{};
        va_list args;
        va_start(args, format);
        vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        SYSTEMTIME time{};
        GetLocalTime(&time);

        char line[1280]{};
        const int length = snprintf(line, sizeof(line),
            "%02u:%02u:%02u.%03u pid=%lu tid=%lu %s\r\n",
            time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
            GetCurrentProcessId(), GetCurrentThreadId(), message);
        if (length <= 0)
            return;

        const std::wstring path = GetPath();
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;

        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(min(length, (int)sizeof(line))), &written, nullptr);
        CloseHandle(file);
    }

    inline void BeginSession(const char* build)
    {
        WriteLine("session-start build=%s", build ? build : "unknown");
    }
}
