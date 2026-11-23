#include "windows_settings.hpp"
#include <windows.h>
#include <fstream>
#include "json.hpp"

namespace prefs {
namespace {
std::filesystem::path file;
nlohmann::json values;
std::string utf8(const std::wstring &s) {
    auto u = std::filesystem::path(s).u8string();
    return {u.begin(), u.end()};
}
} // namespace
void open(const std::filesystem::path &path) {
    file = path;
    values = nlohmann::json::object();
    std::ifstream in(file);
    if (in && !(in >> values))
        throw std::runtime_error("Could not read Kyujo2 settings.");
    if (!values.is_object())
        throw std::runtime_error("Invalid Kyujo2 settings.");
}
std::wstring get(const wchar_t *group, const wchar_t *key) {
    auto section = values.find(utf8(group));
    if (section == values.end() || !section->is_object())
        return {};
    auto item = section->find(utf8(key));
    if (item == section->end() || !item->is_string())
        return {};
    return std::filesystem::u8path(item->get<std::string>()).wstring();
}
int number(const wchar_t *group, const wchar_t *key, int fallback) {
    auto str = get(group, key);
    if (str.empty())
        return fallback;
    try {
        return std::stoi(str);
    } catch (const std::exception &) {
        return fallback;
    }
}
void set(const wchar_t *group, const wchar_t *key, const std::wstring &value) {
    if (get(group, key) == value)
        return;
    auto next = values;
    next[utf8(group)][utf8(key)] = utf8(value);
    auto temp = file;
    temp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream out(temp, std::ios::binary);
    out << next.dump(2);
    out.close();
    if (!out)
        throw std::filesystem::filesystem_error("Could not write Kyujo2 settings", temp,
                                                std::error_code(errno, std::generic_category()));
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::filesystem::filesystem_error("Could not install Kyujo2 settings", file,
                                                std::error_code(GetLastError(), std::system_category()));
    values = std::move(next);
}
} // namespace prefs
