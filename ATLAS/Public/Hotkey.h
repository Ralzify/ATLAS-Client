#pragma once

#include <Windows.h>
#include <ShlObj.h>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <cctype>

namespace HotkeyPersist
{
    struct CommandBind
    {
        std::string Command;
        int VK = 0;
        int DefaultVK = 0;
    };

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

    inline std::string EscapeJson(const std::string& value)
    {
        std::string out;
        out.reserve(value.size());

        for (char ch : value)
        {
            switch (ch)
            {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += ch; break;
            }
        }

        return out;
    }

    inline std::string UnescapeJson(const std::string& value)
    {
        std::string out;
        out.reserve(value.size());

        for (size_t i = 0; i < value.size(); i++)
        {
            if (value[i] != '\\' || i + 1 >= value.size())
            {
                out += value[i];
                continue;
            }

            char escaped = value[++i];
            switch (escaped)
            {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default:  out += escaped; break;
            }
        }

        return out;
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

    inline int ParseIntField(const std::string& content, const char* key, int defaultValue)
    {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = content.find(needle);

        if (pos == std::string::npos)
            return defaultValue;

        pos = content.find(':', pos);

        if (pos == std::string::npos)
            return defaultValue;

        pos++;

        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t'))
            pos++;

        int value = 0;
        bool found = false;

        while (pos < content.size() && std::isdigit((unsigned char)content[pos]))
        {
            found = true;
            value = value * 10 + (content[pos] - '0');
            pos++;
        }

        return found ? value : defaultValue;
    }

    inline std::string ParseStringField(const std::string& content, const char* key)
    {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = content.find(needle);

        if (pos == std::string::npos)
            return std::string();

        pos = content.find(':', pos);
        if (pos == std::string::npos)
            return std::string();

        pos = content.find('"', pos);
        if (pos == std::string::npos)
            return std::string();

        const size_t start = ++pos;
        std::string raw;

        while (pos < content.size())
        {
            char ch = content[pos++];
            if (ch == '\\' && pos < content.size())
            {
                raw += ch;
                raw += content[pos++];
                continue;
            }
            if (ch == '"')
                break;
            raw += ch;
        }

        return UnescapeJson(raw);
    }

    inline size_t FindObjectEnd(const std::string& content, size_t start)
    {
        bool inString = false;
        bool escaped = false;
        int depth = 0;

        for (size_t i = start; i < content.size(); i++)
        {
            char ch = content[i];

            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (ch == '\\' && inString)
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                inString = !inString;
                continue;
            }

            if (inString)
                continue;

            if (ch == '{')
                depth++;
            else if (ch == '}')
            {
                depth--;
                if (depth == 0)
                    return i;
            }
        }

        return std::string::npos;
    }

    inline size_t FindArrayEnd(const std::string& content, size_t start)
    {
        bool inString = false;
        bool escaped = false;
        int depth = 0;

        for (size_t i = start; i < content.size(); i++)
        {
            char ch = content[i];

            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (ch == '\\' && inString)
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                inString = !inString;
                continue;
            }

            if (inString)
                continue;

            if (ch == '[')
                depth++;
            else if (ch == ']')
            {
                depth--;
                if (depth == 0)
                    return i;
            }
        }

        return std::string::npos;
    }

    inline std::vector<CommandBind> ParseCommands(const std::string& content)
    {
        std::vector<CommandBind> commands;

        auto pos = content.find("\"commands\"");
        if (pos == std::string::npos)
            return commands;

        auto arrayStart = content.find('[', pos);
        if (arrayStart == std::string::npos)
            return commands;

        auto arrayEnd = FindArrayEnd(content, arrayStart);
        if (arrayEnd == std::string::npos)
            return commands;

        pos = arrayStart + 1;

        while (pos < arrayEnd)
        {
            auto objStart = content.find('{', pos);
            if (objStart == std::string::npos || objStart >= arrayEnd)
                break;

            auto objEnd = FindObjectEnd(content, objStart);
            if (objEnd == std::string::npos || objEnd > arrayEnd)
                break;

            std::string obj = content.substr(objStart, objEnd - objStart + 1);
            CommandBind bind{};
            bind.Command = ParseStringField(obj, "command");
            bind.VK = ParseIntField(obj, "vk", 0);
            bind.DefaultVK = ParseIntField(obj, "defaultVK", bind.VK);

            if (!bind.Command.empty())
            {
                if (bind.VK < 0 || bind.VK > 254)
                    bind.VK = 0;
                if (bind.DefaultVK < 0 || bind.DefaultVK > 254)
                    bind.DefaultVK = 0;
                commands.push_back(bind);
            }

            pos = objEnd + 1;
        }

        return commands;
    }

    inline void SaveAll(int hotkey, int joinHotkey, const std::vector<CommandBind>& commands)
    {
        auto path = GetSettingsPath();
        std::ofstream f(path, std::ios::trunc);

        if (!f.is_open())
            return;

        f << "{\n";
        f << "  \"hotkey\": " << hotkey << ",\n";
        f << "  \"joinHotkey\": " << joinHotkey << ",\n";
        f << "  \"commands\": [\n";

        for (size_t i = 0; i < commands.size(); i++)
        {
            const auto& command = commands[i];
            f << "    { \"command\": \"" << EscapeJson(command.Command) << "\", \"vk\": " << command.VK
                << ", \"defaultVK\": " << command.DefaultVK << " }";
            if (i + 1 < commands.size())
                f << ",";
            f << "\n";
        }

        f << "  ]\n";
        f << "}\n";
    }

    inline int Load(int defaultVK = VK_F9, const char* key = "hotkey")
    {
        std::string content = ReadFileContents();

        if (content.empty()) 
            return defaultVK;

        return ParseKey(content, key, defaultVK);
    }

    inline std::vector<CommandBind> LoadCommands()
    {
        return ParseCommands(ReadFileContents());
    }

    inline void Save(int vk, const char* key = "hotkey")
    {
        std::string content = ReadFileContents();

        int hotkey = ParseKey(content, "hotkey", VK_F9);
        int joinHotkey = ParseKey(content, "joinHotkey", VK_F5);
        std::vector<CommandBind> commands = ParseCommands(content);

        if (strcmp(key, "hotkey") == 0)
            hotkey = vk;
        else if (strcmp(key, "joinHotkey") == 0)
            joinHotkey = vk;

        SaveAll(hotkey, joinHotkey, commands);
    }

    inline void SaveCommands(const std::vector<CommandBind>& commands)
    {
        std::string content = ReadFileContents();
        SaveAll(ParseKey(content, "hotkey", VK_F9), ParseKey(content, "joinHotkey", VK_F5), commands);
    }
}
