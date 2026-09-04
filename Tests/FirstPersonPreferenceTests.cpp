#include "../ATLAS/Public/Hotkey.h"

#include <cassert>

int main()
{
    using namespace HotkeyPersist;

    // Existing settings files do not contain this newer preference and must
    // retain the safe, third-person default when they are first read.
    assert(!ParseUserPreferences(
        R"({"preferences":{"inGameFog":true}})").FirstPerson);

    assert(ParseUserPreferences(
        R"({"preferences":{"firstPerson":true}})").FirstPerson);
    assert(!ParseUserPreferences(
        R"({"preferences":{"firstPerson":false}})").FirstPerson);

    UserPreferences enabledByDefault{};
    enabledByDefault.FirstPerson = true;
    assert(ParseUserPreferences("{}", enabledByDefault).FirstPerson);
    return 0;
}
