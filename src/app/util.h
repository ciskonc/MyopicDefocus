#pragma once

#include <string>
#include <windows.h>

namespace md {

// UTF-8 → UTF-16
std::wstring Utf8ToWide(const std::string& s);

// UTF-16 → UTF-8
std::string WideToUtf8(const std::wstring& s);

// 应用数据目录：%APPDATA%\MyopicDefocus（不存在则创建）
std::wstring GetAppDataDir();

// 开机自启（HKCU Run 键）
bool SetAutoStart(bool enable);
bool IsAutoStartEnabled();

// 可执行文件所在目录
std::wstring GetExeDir();

} // namespace md
