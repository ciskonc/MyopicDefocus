// ==========================================
// core.h — core 层内部头（实现细节，不导出公共接口）
// ==========================================
// 本头只服务于 src/core/ 下的两个实现单元：
//   - 引入跨模块契约 ../types.h
//   - 提供 core 层内部使用的字符串 / 路径 / 文件 / COM 辅助
//
// 公共接口（md::core::*）全部声明在 ../types.h，本文件不重复声明、不新增公共符号。
// 命名空间统一放在 md::core::detail，避免与 md:: 下的公共符号（如 md::Utf8ToWide，
// 由 app 层实现）发生名字查找冲突：detail 里的同名函数必须以 detail:: 限定调用。
#pragma once

#include "../types.h"

#include <windows.h>
#include <unknwn.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace md {
namespace core {
namespace detail {

// ==========================================
// 一、字符串辅助（core 内部）
// ==========================================

// UTF-8 → UTF-16。非法字节序列按 U+FFFD 处理。
std::wstring WidenUtf8(const std::string& s);

// UTF-16 → UTF-8。
std::string NarrowUtf8(const std::wstring& s);

// ASCII 字母转小写；非 ASCII 字节原样保留。
std::string ToLowerAscii(std::string s);

// 去掉尾部空白（DXGI 的描述字段是空格补齐的定长数组）。
std::string TrimTrailingWhitespace(std::string s);

// 忽略大小写比较（仅 ASCII 生效）。
bool EqualsIgnoreCase(const std::string& a, const std::string& b);

// ==========================================
// 二、路径辅助
// ==========================================

// %APPDATA%\MyopicDefocus（SHGetKnownFolderPath，失败回退环境变量 APPDATA）
std::filesystem::path AppDataDir();

// %APPDATA%\MyopicDefocus\config.json
std::filesystem::path NewConfigPath();

// 可执行文件所在目录。
std::filesystem::path ExeDir();

// exe 目录下的 config.json（旧版 Lerrenp 配置）。
std::filesystem::path LegacyConfigPath();

// ==========================================
// 三、文件辅助
// ==========================================

// 读整个文件为文本；自动剥掉 UTF-8 BOM。文件不存在或读失败返回 false。
bool ReadTextFile(const std::filesystem::path& path, std::string& out);

// 原子写：先写同目录 .tmp，写完再替换目标文件。
bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& content);

// ==========================================
// 四、最小 COM 智能指针（core 层自持，不与 GPU 层耦合）
// ==========================================

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : ptr_(p) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&)            = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); ptr_ = o.ptr_; o.ptr_ = nullptr; }
        return *this;
    }

    void reset(T* p = nullptr) {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }

    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

// 接口 UUID（避免依赖 IID_PPV_ARGS 所在头文件在不同 SDK 下的差异）
template <class T>
inline REFIID IidOf() noexcept { return __uuidof(T); }

// ==========================================
// 五、JSON 辅助
// ==========================================

// 解析新版（version 2）配置文本。
// 宽容策略：缺失字段保留默认值；"common" 子对象写法与顶层平铺写法都接受。
// 返回 true 表示"文本可解析且至少命中一个已知字段"。
bool ParseConfigJsonV2(const std::string& text, Config& out);

// ==========================================
// 六、显示器层说明
// ==========================================
// core::ScreenInfo 由 EnumerateScreens 产出「物理信息」部分：
//   id / deviceName / adapterName / x,y,width,height / refreshHz / bitsPerPixel /
//   isVirtual / isPrimary
// ScreenInfo::params 需要 Config 才能确定（通用值 + 单屏覆盖），
// EnumerateScreens 拿不到 Config，故 params 保持 DefocusParams 默认值；
// 由上层拿到 Config 后调用 Config::ResolveFor(screen.id) 填充。

} // namespace detail
} // namespace core
} // namespace md
