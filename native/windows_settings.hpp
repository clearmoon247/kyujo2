#pragma once
#include <filesystem>
#include <string>
namespace prefs {
void open(const std::filesystem::path &path);
std::wstring get(const wchar_t *group, const wchar_t *key);
int number(const wchar_t *group, const wchar_t *key, int fallback);
void set(const wchar_t *group, const wchar_t *key, const std::wstring &value);
} // namespace prefs
