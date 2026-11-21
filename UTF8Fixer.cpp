#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectVers.h"
#include "AE_EffectSuites.h"
#include "AE_GeneralPlug.h"
#include "MinHook.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// ==========================================
// 0. 全局状态与运行模式
// ==========================================

#define WRAPPER_POOL_SIZE      2048
#define UPDATE_UI_POOL_SIZE    2

enum class FixerMode : int {
    Disabled = 0,
    LegacyToUTF8, // AE 26.2~26.x：宿主把本地 CJK 字节当 UTF-8 解析，本地 → UTF-8
    UTF8ToLegacy  // 其它版本(含 27.0+)：宿主按本地 CJK 码页解析，UTF-8 → 本地
};

static std::atomic<FixerMode> g_FixerMode{ FixerMode::Disabled };
static std::atomic<UINT>      g_LegacyCodePage{ 0 };
static HMODULE                g_SelfModule = nullptr;

// 每个被 hook 入口点的状态
static std::atomic<void*>     g_OriginalEntryPoints[WRAPPER_POOL_SIZE] = {};
static std::atomic<bool>      g_PluginOptInUTF8[WRAPPER_POOL_SIZE] = {};
static std::atomic<bool>      g_PluginPiPLOverridesFlags[WRAPPER_POOL_SIZE] = {};
static std::atomic<int>       g_WrapperCount{ 0 };
static std::unordered_set<void*> g_HookedTargets;
static std::mutex             g_WrapperMutex;

// 当前线程正在执行的插件 ID
static thread_local int tl_CurrentPluginID = -1;

struct ScopedPluginContext {
    int prev_id;
    explicit ScopedPluginContext(int id) noexcept : prev_id(tl_CurrentPluginID) {
        tl_CurrentPluginID = id;
    }
    ~ScopedPluginContext() noexcept {
        tl_CurrentPluginID = prev_id;
    }
    ScopedPluginContext(const ScopedPluginContext&) = delete;
    ScopedPluginContext& operator=(const ScopedPluginContext&) = delete;
};

inline bool IsCJKCodePage(UINT cp) noexcept {
    // 936 简体 / 950 繁体 / 932 日文 / 949 韩文
    return cp == 936 || cp == 950 || cp == 932 || cp == 949;
}

// 读取宿主(AfterFX.exe)版本与本地 CJK 码页
static FixerMode DetectHostFixerMode(UINT& outLegacyCP) noexcept {
    const UINT acp = GetACP();
    // 仅针对 CJK 系统语言，跳过非 CJK 与 UTF-8 Beta(65001)
    if (!IsCJKCodePage(acp)) {
        outLegacyCP = 0;
        return FixerMode::Disabled;
    }

    WORD major = 0;
    WORD minor = 0;
    bool gotVersion = false;

    HMODULE hVersion = LoadLibraryW(L"version.dll");
    if (hVersion) {
        using GetSize_t  = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
        using GetInfo_t  = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
        using VerQuery_t = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

        auto pGetSize  = reinterpret_cast<GetSize_t>(GetProcAddress(hVersion, "GetFileVersionInfoSizeW"));
        auto pGetInfo  = reinterpret_cast<GetInfo_t>(GetProcAddress(hVersion, "GetFileVersionInfoW"));
        auto pVerQuery = reinterpret_cast<VerQuery_t>(GetProcAddress(hVersion, "VerQueryValueW"));

        wchar_t exePath[MAX_PATH] = {};
        if (pGetSize && pGetInfo && pVerQuery && GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
            DWORD dummy = 0;
            DWORD size = pGetSize(exePath, &dummy);
            if (size > 0) {
                std::unique_ptr<BYTE[]> buf(new (std::nothrow) BYTE[size]);
                if (buf && pGetInfo(exePath, dummy, size, buf.get())) {
                    VS_FIXEDFILEINFO* pFileInfo = nullptr;
                    UINT len = 0;
                    if (pVerQuery(buf.get(), L"\\", reinterpret_cast<LPVOID*>(&pFileInfo), &len) && pFileInfo) {
                        major = HIWORD(pFileInfo->dwFileVersionMS);
                        minor = LOWORD(pFileInfo->dwFileVersionMS);
                        gotVersion = true;
                    }
                }
            }
        }
        FreeLibrary(hVersion);
    }

    if (!gotVersion) {
        outLegacyCP = 0;
        return FixerMode::Disabled;
    }

    outLegacyCP = acp;

    // AE 26.2~26.x：本地 → UTF-8
    if (major == 26 && minor >= 2) {
        return FixerMode::LegacyToUTF8;
    }

    // AE 27.0+ 与 ≤26.1：UTF-8 → 本地
    return FixerMode::UTF8ToLegacy;
}

// ==========================================
// 1. PiPL 二进制资源解析
// ==========================================

struct ParsedPiPLEffect {
    bool isEffect = false;
    bool optInUtf8 = false;
    bool piplOverridesFlags = false;
    char entryName[128]{};
};

static inline uint32_t ReadU32LE(const uint8_t* p) noexcept {
    uint32_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

// 纯 POD SEH 辅助(__try 内无 C++ 析构)
static bool SafeParsePiPLBlob(const uint8_t* data, size_t size, ParsedPiPLEffect* out) noexcept {
    if (!data || size < 10 || !out) return false;
    memset(out, 0, sizeof(*out));

    // FourCC 常量，对应 AE_General.r 与 SPPiPL.h
    constexpr uint32_t kVendor8BIM = 0x3842494Du; // '8BIM'
    constexpr uint32_t kVendorADBE = 0x41444245u; // 'ADBE'
    constexpr uint32_t kKeyKind    = 0x6B696E64u; // 'kind'
    constexpr uint32_t kKindEffect = 0x65464B54u; // 'eFKT'
    constexpr uint32_t kKeyOutFlg1 = 0x65474C4Fu; // 'eGLO'
    constexpr uint32_t kKeyOutFlg2 = 0x65474C32u; // 'eGL2'
#if defined(_M_ARM64)
    constexpr uint32_t kKeyCodeWin = 0x77613634u; // 'wa64'
#else
    constexpr uint32_t kKeyCodeWin = 0x38363634u; // '8664'
#endif

    __try {
        // PiPLtool 编译的 Windows 资源以 2 字节 0x0001 前缀开头，缺省回退到偏移 0
        const size_t candidateOffsets[2] = { 2, 0 };
        size_t pos = 0;
        uint32_t propCount = 0;
        bool foundHeader = false;

        for (size_t off : candidateOffsets) {
            if (off + 8 + 16 > size) continue;
            const uint32_t version = ReadU32LE(data + off);
            const uint32_t count   = ReadU32LE(data + off + 4);
            if (version != 0 || count == 0 || count > 256) continue;

            const uint32_t firstVendor = ReadU32LE(data + off + 8);
            if (firstVendor == kVendor8BIM || firstVendor == kVendorADBE) {
                pos = off + 8;
                propCount = count;
                foundHeader = true;
                break;
            }
        }

        if (!foundHeader) return false;

        for (uint32_t i = 0; i < propCount; ++i) {
            if (pos + 16 > size) break;

            uint32_t vendor = ReadU32LE(data + pos);
            uint32_t key    = ReadU32LE(data + pos + 4);
            uint32_t len    = ReadU32LE(data + pos + 12);

            if (vendor != kVendor8BIM && vendor != kVendorADBE) {
                return false;
            }

            const size_t dataPos   = pos + 16;
            const size_t paddedLen = (static_cast<size_t>(len) + 3u) & ~static_cast<size_t>(3u);
            if (paddedLen < len || dataPos + paddedLen > size) {
                return false;
            }

            const uint8_t* payload = data + dataPos;

            if (key == kKeyKind && len >= 4) {
                uint32_t kindVal = ReadU32LE(payload);
                if (kindVal == kKindEffect) {
                    out->isEffect = true;
                }
            }
            else if (key == kKeyCodeWin && len > 0) {
                const char* str = reinterpret_cast<const char*>(payload);
                const size_t maxRead = (len < sizeof(out->entryName) - 1) ? len : (sizeof(out->entryName) - 1);
                const size_t actualLen = strnlen(str, maxRead);
                if (actualLen > 0) {
                    memcpy(out->entryName, str, actualLen);
                    out->entryName[actualLen] = '\0';
                }
            }
            else if (key == kKeyOutFlg1 && len >= 4) {
                const uint32_t flags1 = ReadU32LE(payload);
                if ((flags1 & static_cast<uint32_t>(PF_OutFlag_PiPL_OVERRIDES_OUTDATA_OUTFLAGS)) != 0) {
                    out->piplOverridesFlags = true;
                }
            }
            else if (key == kKeyOutFlg2 && len >= 4) {
                const uint32_t flags2 = ReadU32LE(payload);
                if ((flags2 & static_cast<uint32_t>(PF_OutFlag2_STRINGS_ARE_UTF8)) != 0) {
                    out->optInUtf8 = true;
                }
            }

            pos = dataPos + paddedLen;
        }

        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct ModulePiPLSummary {
    bool hasAnyPiPL = false;
    std::vector<ParsedPiPLEffect> effects;
};

static BOOL CALLBACK EnumPiPLResCallback(HMODULE hMod, LPCWSTR lpType, LPWSTR lpName, LONG_PTR lParam) {
    auto* summary = reinterpret_cast<ModulePiPLSummary*>(lParam);
    if (!summary) return FALSE;

    HRSRC hRes = FindResourceW(hMod, lpName, lpType);
    if (!hRes) return TRUE;

    const DWORD resSize = SizeofResource(hMod, hRes);
    if (resSize < 10) return TRUE;

    HGLOBAL hMem = LoadResource(hMod, hRes);
    if (!hMem) return TRUE;

    const auto* raw = static_cast<const uint8_t*>(LockResource(hMem));
    if (!raw) return TRUE;

    ParsedPiPLEffect parsed{};
    if (SafeParsePiPLBlob(raw, static_cast<size_t>(resSize), &parsed)) {
        summary->hasAnyPiPL = true;
        if (parsed.isEffect) {
            try {
                summary->effects.push_back(parsed);
            } catch (...) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static ModulePiPLSummary InspectModulePiPLs(HMODULE hMod) noexcept {
    ModulePiPLSummary summary;
    if (!hMod || (reinterpret_cast<uintptr_t>(hMod) & 3u) != 0) {
        return summary;
    }
    EnumResourceNamesW(hMod, L"PiPL", EnumPiPLResCallback, reinterpret_cast<LONG_PTR>(&summary));
    return summary;
}

static bool ShouldBypassForCurrentPlugin() noexcept {
    const int id = tl_CurrentPluginID;
    if (id >= 0 && id < WRAPPER_POOL_SIZE) {
        return g_PluginOptInUTF8[id].load(std::memory_order_acquire);
    }
    // 入口点未被 hook(插件身份未知)：UTF8ToLegacy 模式下跳过，避免误伤正确输出 UTF-8 的插件(如 AE 内置效果)
    return g_FixerMode.load(std::memory_order_acquire) == FixerMode::UTF8ToLegacy;
}

// ==========================================
// 2. UTF-8 校验与 CJK 安全检测
// ==========================================

// 严格 UTF-8 校验；含高位字节时要求至少一个 3 字节字符(避免与 2 字节 CJK 混淆)
inline bool IsValidUTF8(std::string_view sv, bool* out_has_high = nullptr) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(sv.data());
    const auto* end = bytes + sv.size();

    bool has_high_byte = false;
    bool has_3byte_character = false;

    while (bytes < end) {
        if (bytes[0] <= 0x7F) {
            bytes += 1;
        }
        else if ((bytes[0] & 0xE0) == 0xC0) {
            if (bytes[0] < 0xC2 || bytes + 1 >= end || (bytes[1] & 0xC0) != 0x80) return false;
            has_high_byte = true;
            bytes += 2;
        }
        else if ((bytes[0] & 0xF0) == 0xE0) {
            if (bytes + 2 >= end || (bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80) return false;
            if (bytes[0] == 0xE0 && bytes[1] < 0xA0) return false; // 过长编码
            if (bytes[0] == 0xED && bytes[1] >= 0xA0) return false; // UTF-16 代理区
            has_high_byte = true;
            has_3byte_character = true;
            bytes += 3;
        }
        else if ((bytes[0] & 0xF8) == 0xF0) {
            if (bytes[0] > 0xF4 || bytes + 3 >= end ||
                (bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 || (bytes[3] & 0xC0) != 0x80) {
                return false;
            }
            if (bytes[0] == 0xF0 && bytes[1] < 0x90) return false;
            if (bytes[0] == 0xF4 && bytes[1] > 0x8F) return false;
            has_high_byte = true;
            bytes += 4;
        }
        else {
            return false;
        }
    }

    if (out_has_high) {
        *out_has_high = has_high_byte;
    }

    if (has_high_byte && !has_3byte_character) {
        return false;
    }

    return true;
}

inline bool IsLikelyUTF8WithCJK(std::string_view sv) noexcept {
    bool has_high = false;
    return IsValidUTF8(sv, &has_high) && has_high;
}

inline size_t CalcTrimmedUTF8Length(std::string_view sv) noexcept {
    size_t len = sv.size();
    if (len == 0) return 0;

    size_t i = len;
    while (i > 0) {
        const auto c = static_cast<unsigned char>(sv[i - 1]);
        if ((c & 0x80) == 0x00) {
            return len;
        }
        else if ((c & 0xC0) == 0xC0) {
            size_t need_bytes = 0;
            if ((c & 0xE0) == 0xC0)      need_bytes = 2;
            else if ((c & 0xF0) == 0xE0) need_bytes = 3;
            else if ((c & 0xF8) == 0xF0) need_bytes = 4;
            else                         return i - 1;

            if (len - (i - 1) < need_bytes) {
                return i - 1;
            }
            return len;
        }
        else {
            if (len - (i - 1) >= 4) return len;
            i--;
        }
    }
    return 0;
}

void TrimInvalidTailUTF8(std::span<char> s) noexcept {
    const size_t valid_len = CalcTrimmedUTF8Length(std::string_view(s.data(), s.size()));
    if (valid_len < s.size()) {
        s[valid_len] = '\0';
    }
}

// ==========================================
// 3. 双向编码转换器
// ==========================================

template<size_t InlineSize = 256>
class LegacyToUTF8Converter {
public:
    explicit LegacyToUTF8Converter(std::string_view src) noexcept {
        if (src.empty()) return;

        const UINT codePage = g_LegacyCodePage.load(std::memory_order_acquire);
        if (!IsCJKCodePage(codePage)) return;

        const int srcLen = static_cast<int>(src.size());
        int wlen = MultiByteToWideChar(codePage, 0, src.data(), srcLen, nullptr, 0);
        if (wlen <= 0) return;

        wchar_t* wbuf = m_inlineWBuf;
        std::unique_ptr<wchar_t[]> heapWBuf;
        if (static_cast<size_t>(wlen) > InlineSize) {
            heapWBuf.reset(new (std::nothrow) wchar_t[wlen]);
            if (!heapWBuf) return;
            wbuf = heapWBuf.get();
        }

        if (MultiByteToWideChar(codePage, 0, src.data(), srcLen, wbuf, wlen) <= 0) return;

        int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) return;

        if (static_cast<size_t>(u8len) + 1 > InlineSize * 2) {
            try {
                m_heapBuf.resize(static_cast<size_t>(u8len));
            } catch (...) {
                return;
            }
            if (WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, m_heapBuf.data(), u8len, nullptr, nullptr) <= 0) {
                m_heapBuf.clear();
                return;
            }
            m_view = m_heapBuf;
        } else {
            if (WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, m_inlineBuf, u8len, nullptr, nullptr) <= 0) {
                return;
            }
            m_inlineBuf[u8len] = '\0';
            m_view = std::string_view(m_inlineBuf, static_cast<size_t>(u8len));
        }
    }

    [[nodiscard]] std::string_view view() const noexcept { return m_view; }
    [[nodiscard]] bool empty() const noexcept { return m_view.empty(); }

    std::string move_to_string() && {
        if (!m_heapBuf.empty()) {
            return std::move(m_heapBuf);
        }
        return std::string(m_view);
    }

private:
    wchar_t m_inlineWBuf[InlineSize];
    char m_inlineBuf[InlineSize * 2];
    std::string m_heapBuf;
    std::string_view m_view;
};

template<size_t InlineSize = 256>
class UTF8ToLegacyConverter {
public:
    explicit UTF8ToLegacyConverter(std::string_view src) noexcept {
        if (src.empty()) return;

        const UINT targetCP = g_LegacyCodePage.load(std::memory_order_acquire);
        if (!IsCJKCodePage(targetCP)) return;

        const int srcLen = static_cast<int>(src.size());

        // 1. UTF-8 → UTF-16
        int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src.data(), srcLen, nullptr, 0);
        if (wlen <= 0) return;

        wchar_t* wbuf = m_inlineWBuf;
        std::unique_ptr<wchar_t[]> heapWBuf;
        if (static_cast<size_t>(wlen) > InlineSize) {
            heapWBuf.reset(new (std::nothrow) wchar_t[wlen]);
            if (!heapWBuf) return;
            wbuf = heapWBuf.get();
        }

        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src.data(), srcLen, wbuf, wlen) <= 0) {
            return;
        }

        // 2. 检查码点防止误判，并归一化隐形空格(NBSP/BOM/ZWSP)
        int cleanLen = 0;
        int cjkCount = 0;
        for (int i = 0; i < wlen; ++i) {
            wchar_t wc = wbuf[i];

            if (wc == 0x00A0) {
                wbuf[cleanLen++] = L' ';
                continue;
            }
            if (wc == 0xFEFF || wc == 0x200B) {
                continue;
            }

            // 拒绝 C1 控制符、私用区、替换符
            if ((wc >= 0x0080 && wc <= 0x009F) ||
                (wc >= 0xE000 && wc <= 0xF8FF) ||
                wc == 0xFFFD) {
                return;
            }

            // 区分 CJK / 常见符号 / 生僻文字
            const bool isAscii    = (wc <= 0x007F);
            const bool isCjk      = (wc >= 0x3000 && wc <= 0x30FF) // CJK 标点/假名
                                 || (wc >= 0x3100 && wc <= 0x318F) // 注音/谚文兼容
                                 || (wc >= 0x3400 && wc <= 0x4DBF) // CJK 扩展 A
                                 || (wc >= 0x4E00 && wc <= 0x9FFF) // 中日韩统一表意文字
                                 || (wc >= 0xAC00 && wc <= 0xD7AF) // 谚文音节
                                 || (wc >= 0xF900 && wc <= 0xFAFF) // CJK 兼容表意文字
                                 || (wc >= 0xFF00 && wc <= 0xFFEF); // 半角/全角形式
            const bool isPlausible = isAscii || isCjk
                                 || (wc >= 0x00A1 && wc <= 0x024F) // 拉丁扩展
                                 || (wc >= 0x0370 && wc <= 0x04FF) // 希腊/西里尔
                                 || (wc >= 0x2000 && wc <= 0x2BFF) // 标点/箭头/数学/符号
                                 || (wc >= 0xD800 && wc <= 0xDFFF); // 代理对(emoji / CJK 扩展 B+)

            if (!isPlausible) {
                // 本地 CJK 字节误解码常落在藏文/缅文等生僻区
                return;
            }

            if (isCjk) {
                ++cjkCount;
            }
            wbuf[cleanLen++] = wc;
        }

        if (cleanLen <= 0) return;

        // 3. UTF-16 → 本地 CJK 码页(先严格无损检查)
        DWORD flags = WC_NO_BEST_FIT_CHARS;
        BOOL usedDefault = FALSE;
        int mbLen = WideCharToMultiByte(targetCP, flags, wbuf, cleanLen, nullptr, 0, nullptr, &usedDefault);
        if (mbLen <= 0) return;

        if (usedDefault) {
            // 仅当确含真实 CJK 文本时才允许有损回退
            if (cjkCount < 2) {
                return;
            }
            flags = 0;
            mbLen = WideCharToMultiByte(targetCP, flags, wbuf, cleanLen, nullptr, 0, nullptr, nullptr);
            if (mbLen <= 0) return;
        }

        if (static_cast<size_t>(mbLen) + 1 > InlineSize * 2) {
            try {
                m_heapBuf.resize(static_cast<size_t>(mbLen));
            } catch (...) {
                return;
            }
            if (WideCharToMultiByte(targetCP, flags, wbuf, cleanLen, m_heapBuf.data(), mbLen, nullptr, nullptr) <= 0) {
                m_heapBuf.clear();
                return;
            }
            m_view = m_heapBuf;
        } else {
            if (WideCharToMultiByte(targetCP, flags, wbuf, cleanLen, m_inlineBuf, mbLen, nullptr, nullptr) <= 0) {
                return;
            }
            m_inlineBuf[mbLen] = '\0';
            m_view = std::string_view(m_inlineBuf, static_cast<size_t>(mbLen));
        }
    }

    [[nodiscard]] std::string_view view() const noexcept { return m_view; }
    [[nodiscard]] bool empty() const noexcept { return m_view.empty(); }

    std::string move_to_string() && {
        if (!m_heapBuf.empty()) {
            return std::move(m_heapBuf);
        }
        return std::string(m_view);
    }

private:
    wchar_t m_inlineWBuf[InlineSize];
    char m_inlineBuf[InlineSize * 2];
    std::string m_heapBuf;
    std::string_view m_view;
};

// ==========================================
// 4. 全局分片字符串缓存
// ==========================================

static constexpr size_t kCacheShards = 4;

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

struct StringEqual {
    using is_transparent = void;
    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

struct CacheContainer {
    mutable std::shared_mutex mutex;
    std::unordered_set<std::string, StringHash, StringEqual> cache;
};

inline CacheContainer& GetShardedCache(std::string_view sv) noexcept {
    static CacheContainer shards[kCacheShards];
    size_t hash = std::hash<std::string_view>{}(sv);
    return shards[hash % kCacheShards];
}

template<typename ConverterT>
const char* GetCachedString(ConverterT& converter) noexcept {
    try {
        std::string_view sv = converter.view();
        auto& cache_container = GetShardedCache(sv);
        auto& cache = cache_container.cache;
        auto& mutex = cache_container.mutex;

        {
            std::shared_lock read_lock(mutex);
            if (auto it = cache.find(sv); it != cache.end()) {
                return it->c_str();
            }
        }

        std::unique_lock write_lock(mutex);
        if (auto it = cache.find(sv); it != cache.end()) {
            return it->c_str();
        }

        auto [it, inserted] = cache.insert(std::move(converter).move_to_string());
        return it->c_str();
    } catch (...) {
        return nullptr;
    }
}

// ==========================================
// 5. 参数修复逻辑
// ==========================================

bool FixStringPointer(const char*& ptr, bool pluginNativeUtf8) noexcept {
    if (pluginNativeUtf8 || !ptr || ptr[0] == '\0') return false;

    const FixerMode mode = g_FixerMode.load(std::memory_order_acquire);
    const std::string_view sv(ptr);

    if (mode == FixerMode::LegacyToUTF8) {
        if (!IsValidUTF8(sv)) [[unlikely]] {
            LegacyToUTF8Converter<> converter(sv);
            if (!converter.empty()) {
                if (const char* cached = GetCachedString(converter)) {
                    ptr = cached;
                    return true;
                }
            }
        }
    }
    else if (mode == FixerMode::UTF8ToLegacy) {
        if (IsLikelyUTF8WithCJK(sv)) [[unlikely]] {
            UTF8ToLegacyConverter<> converter(sv);
            if (!converter.empty()) {
                if (const char* cached = GetCachedString(converter)) {
                    ptr = cached;
                    return true;
                }
            }
        }
    }
    return false;
}

template <size_t N>
bool FixStringArray(char(&arr)[N], bool pluginNativeUtf8) noexcept {
    if (pluginNativeUtf8 || arr[0] == '\0') return false;

    const FixerMode mode = g_FixerMode.load(std::memory_order_acquire);
    const size_t len = strnlen(arr, N);
    const std::string_view sv(arr, len);

    if (mode == FixerMode::LegacyToUTF8) {
        if (IsValidUTF8(sv)) [[likely]] return false;

        LegacyToUTF8Converter<> converter(sv);
        std::string_view out = converter.view();
        if (out.empty()) return false;

        const size_t copyLen = (out.size() < N - 1) ? out.size() : (N - 1);
        memcpy_s(arr, N, out.data(), copyLen);
        arr[copyLen] = '\0';
        TrimInvalidTailUTF8(std::span<char>(arr, copyLen));
        return true;
    }
    else if (mode == FixerMode::UTF8ToLegacy) {
        std::string_view target = sv;
        bool likelyUtf8 = IsLikelyUTF8WithCJK(target);

        // 恢复在缓冲区边界被截断的尾部 UTF-8 字符
        if (!likelyUtf8 && len >= N - 2) {
            const size_t trimmedLen = CalcTrimmedUTF8Length(sv);
            if (trimmedLen > 0 && trimmedLen < len) {
                std::string_view prefix(arr, trimmedLen);
                if (IsLikelyUTF8WithCJK(prefix)) {
                    target = prefix;
                    likelyUtf8 = true;
                }
            }
        }

        if (!likelyUtf8) [[likely]] return false;

        UTF8ToLegacyConverter<> converter(target);
        std::string_view out = converter.view();
        if (out.empty()) return false;

        const size_t copyLen = (out.size() < N - 1) ? out.size() : (N - 1);
        memcpy_s(arr, N, out.data(), copyLen);
        arr[copyLen] = '\0';
        return true;
    }

    return false;
}

bool ProcessAndFixParam(PF_ParamDef* def, bool pluginNativeUtf8, bool allowPointerFix) noexcept {
    if (!def || pluginNativeUtf8) return false;

    bool changed = FixStringArray(def->PF_DEF_NAME, pluginNativeUtf8);

    switch (def->param_type) {
    case PF_Param_POPUP:
        if (allowPointerFix) changed |= FixStringPointer(def->u.pd.u.PF_DEF_NAMESPTR, pluginNativeUtf8);
        break;

    case PF_Param_CHECKBOX:
        if (allowPointerFix) changed |= FixStringPointer(def->u.bd.u.PF_DEF_NAMEPTR, pluginNativeUtf8);
        break;

    case PF_Param_BUTTON:
        if (allowPointerFix) changed |= FixStringPointer(def->u.button_d.u.PF_DEF_NAMESPTR, pluginNativeUtf8);
        break;

    case PF_Param_SLIDER:
        changed |= FixStringArray(def->u.sd.value_str, pluginNativeUtf8);
        changed |= FixStringArray(def->u.sd.value_desc, pluginNativeUtf8);
        break;

    case PF_Param_FIX_SLIDER:
        changed |= FixStringArray(def->u.fd.value_str, pluginNativeUtf8);
        changed |= FixStringArray(def->u.fd.value_desc, pluginNativeUtf8);
        break;

    case PF_Param_FLOAT_SLIDER:
        changed |= FixStringArray(def->u.fs_d.value_desc, pluginNativeUtf8);
        break;

    default:
        break;
    }

    return changed;
}

// 后处理只改内联字符数组，不解引用宿主托管的指针
static int FixInlineParamArraysOnly(PF_InData* in_data, PF_ParamDef* params[], bool pluginNativeUtf8) noexcept {
    if (pluginNativeUtf8 || !in_data || !params || in_data->num_params <= 0) return 0;

    PF_ParamIndex scan_limit = in_data->num_params;
    if (scan_limit > 10000) scan_limit = 10000;

    int fixed_count = 0;
    for (PF_ParamIndex i = 1; i < scan_limit; i++) {
        if (params[i] && ProcessAndFixParam(params[i], pluginNativeUtf8, /*allowPointerFix=*/false)) {
            fixed_count++;
        }
    }
    return fixed_count;
}

// ==========================================
// 6. 宿主 API Hook
// ==========================================

inline bool TryCreateAndEnableHook(void* pTarget, LPVOID pDetour, LPVOID* ppOriginal) noexcept {
    if (MH_CreateHook(pTarget, pDetour, ppOriginal) != MH_OK) {
        return false;
    }
    if (MH_EnableHook(pTarget) != MH_OK) {
        MH_DisableHook(pTarget);
        MH_RemoveHook(pTarget);
        return false;
    }
    return true;
}

// --- 单槽 add_param hook(AE 只有一个 add_param 地址) ---
using HostAddParam_t = PF_Err(*)(PF_ProgPtr, PF_ParamIndex, PF_ParamDef*);
static std::atomic<HostAddParam_t> g_OriginalHostAddParam{ nullptr };
static bool                        g_AddParamHooked = false;
static std::mutex                  g_AddParamHookMutex;

PF_Err Trigger_HostAddParam(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_ParamDef* def) {
    HostAddParam_t original = g_OriginalHostAddParam.load(std::memory_order_acquire);
    if (!original) [[unlikely]] return PF_Err_INVALID_CALLBACK;
    if (!def) return original(effect_ref, index, def);

    const bool nativeUtf8 = ShouldBypassForCurrentPlugin();
    if (nativeUtf8) {
        return original(effect_ref, index, def);
    }

    // 写时复制，避免改动调用方存储
    PF_ParamDef fixed_def = *def;
    ProcessAndFixParam(&fixed_def, /*pluginNativeUtf8=*/false, /*allowPointerFix=*/true);
    return original(effect_ref, index, &fixed_def);
}

void EnsureHostAddParamHooked(void* pTarget) noexcept {
    if (!pTarget) return;

    std::lock_guard<std::mutex> lock(g_AddParamHookMutex);
    if (g_AddParamHooked) return;

    HostAddParam_t trampoline = nullptr;
    if (MH_CreateHook(pTarget,
                      reinterpret_cast<LPVOID>(&Trigger_HostAddParam),
                      reinterpret_cast<LPVOID*>(&trampoline)) == MH_OK) {
        g_OriginalHostAddParam.store(trampoline, std::memory_order_release);
        if (MH_EnableHook(pTarget) == MH_OK) {
            g_AddParamHooked = true;
        } else {
            g_OriginalHostAddParam.store(nullptr, std::memory_order_release);
            MH_RemoveHook(pTarget);
        }
    }
}

// --- 双槽 PF_UpdateParamUI hook(支持 Suite v1 & v3) ---
using PFUpdateParamUI_t = PF_Err(*)(PF_ProgPtr, PF_ParamIndex, const PF_ParamDef*);
static std::atomic<PFUpdateParamUI_t> g_OriginalUpdateParamUIs[UPDATE_UI_POOL_SIZE] = {};
static std::atomic<int>               g_UpdateUIHookCount{ 0 };
static std::unordered_set<void*>      g_HookedUpdateUITargets;
static std::mutex                     g_UpdateParamUIHookMutex;

template<int Slot>
PF_Err Trigger_PF_UpdateParamUI(PF_ProgPtr effect_ref, PF_ParamIndex param_index, const PF_ParamDef* defP) {
    PFUpdateParamUI_t original = g_OriginalUpdateParamUIs[Slot].load(std::memory_order_acquire);
    if (!original) [[unlikely]] return PF_Err_INVALID_CALLBACK;
    if (!defP) return original(effect_ref, param_index, defP);

    const bool nativeUtf8 = ShouldBypassForCurrentPlugin();
    if (nativeUtf8) {
        return original(effect_ref, param_index, defP);
    }

    PF_ParamDef fixed_def = *defP;
    ProcessAndFixParam(&fixed_def, /*pluginNativeUtf8=*/false, /*allowPointerFix=*/true);
    return original(effect_ref, param_index, &fixed_def);
}

static constinit PFUpdateParamUI_t g_UpdateUIWrapperPool[UPDATE_UI_POOL_SIZE] = {
    &Trigger_PF_UpdateParamUI<0>,
    &Trigger_PF_UpdateParamUI<1>
};

// 隔离 SEH 辅助(__try 内无 C++ 析构)
static void* SafeAcquireUpdateParamUIAddr(SPBasicSuite* basic, A_long suiteVersion) noexcept {
    if (!basic) return nullptr;

    __try {
        const void* suitePtr = nullptr;
        A_Err err = basic->AcquireSuite(kPFParamUtilsSuite, suiteVersion, &suitePtr);
        if (err != A_Err_NONE || !suitePtr) {
            return nullptr;
        }

        // PF_UpdateParamUI 是 v1/v3 两个 suite 的首个函数指针
        void* target = nullptr;
        if (suiteVersion == kPFParamUtilsSuiteVersion3) {
            target = reinterpret_cast<void*>(static_cast<const PF_ParamUtilsSuite3*>(suitePtr)->PF_UpdateParamUI);
        }
#ifdef kPFParamUtilsSuiteVersion1
        else if (suiteVersion == kPFParamUtilsSuiteVersion1) {
            target = reinterpret_cast<void*>(static_cast<const PF_ParamUtilsSuite1*>(suitePtr)->PF_UpdateParamUI);
        }
#endif

        basic->ReleaseSuite(kPFParamUtilsSuite, suiteVersion);
        return target;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void HookSingleUpdateParamUI(void* pTarget) noexcept {
    if (!pTarget) return;

    std::lock_guard<std::mutex> lock(g_UpdateParamUIHookMutex);
    if (g_HookedUpdateUITargets.count(pTarget) > 0) return;

    const int slot = g_UpdateUIHookCount.load(std::memory_order_relaxed);
    if (slot >= UPDATE_UI_POOL_SIZE) return;

    PFUpdateParamUI_t trampoline = nullptr;
    if (MH_CreateHook(pTarget,
                      reinterpret_cast<LPVOID>(g_UpdateUIWrapperPool[slot]),
                      reinterpret_cast<LPVOID*>(&trampoline)) == MH_OK) {
        g_OriginalUpdateParamUIs[slot].store(trampoline, std::memory_order_release);
        if (MH_EnableHook(pTarget) == MH_OK) {
            try { g_HookedUpdateUITargets.insert(pTarget); } catch (...) {}
            g_UpdateUIHookCount.fetch_add(1, std::memory_order_release);
        } else {
            g_OriginalUpdateParamUIs[slot].store(nullptr, std::memory_order_release);
            MH_RemoveHook(pTarget);
        }
    }
}

static void EnsurePFUpdateParamUIHooked(SPBasicSuite* basic) noexcept {
    if (!basic) return;
    if (void* pV3 = SafeAcquireUpdateParamUIAddr(basic, kPFParamUtilsSuiteVersion3)) {
        HookSingleUpdateParamUI(pV3);
    }
#ifdef kPFParamUtilsSuiteVersion1
    if (void* pV1 = SafeAcquireUpdateParamUIAddr(basic, kPFParamUtilsSuiteVersion1)) {
        HookSingleUpdateParamUI(pV1);
    }
#endif
}

// ==========================================
// 7. 插件入口点包装池
// ==========================================

// 共享实现，只实例化一次；下方 2048 个模板包装保持极小
static PF_Err EffectEntryPointCommon(
    int ID, ULONG_PTR arg0, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3, ULONG_PTR arg4, ULONG_PTR arg5)
{
    void* original = g_OriginalEntryPoints[ID].load(std::memory_order_acquire);
    if (!original) [[unlikely]] return PF_Err_NONE;

    using EffectEntryPoint_t = PF_Err(*)(PF_Cmd, PF_InData*, PF_OutData*, PF_ParamDef* [], PF_LayerDef*, void*);
    const auto effect_original = reinterpret_cast<EffectEntryPoint_t>(original);
    const PF_Cmd cmd = static_cast<PF_Cmd>(arg0);

    switch (cmd) {
    // 只有这些命令会调用 add_param / PF_UpdateParamUI，才需要线程上下文
    case PF_Cmd_PARAMS_SETUP: {
        ScopedPluginContext ctx(ID);
        auto* in_data = reinterpret_cast<PF_InData*>(arg1);
        if (in_data) {
            if (in_data->inter.add_param) {
                EnsureHostAddParamHooked(reinterpret_cast<void*>(in_data->inter.add_param));
            }
            if (in_data->pica_basicP && g_UpdateUIHookCount.load(std::memory_order_acquire) == 0) {
                EnsurePFUpdateParamUIHooked(in_data->pica_basicP);
            }
        }
        return effect_original(cmd, in_data, reinterpret_cast<PF_OutData*>(arg2),
                               reinterpret_cast<PF_ParamDef**>(arg3),
                               reinterpret_cast<PF_LayerDef*>(arg4),
                               reinterpret_cast<void*>(arg5));
    }

    case PF_Cmd_USER_CHANGED_PARAM:
    case PF_Cmd_UPDATE_PARAMS_UI: {
        ScopedPluginContext ctx(ID);
        auto* in_data  = reinterpret_cast<PF_InData*>(arg1);
        auto* out_data = reinterpret_cast<PF_OutData*>(arg2);
        auto** params  = reinterpret_cast<PF_ParamDef**>(arg3);
        void* extra    = reinterpret_cast<void*>(arg5);

        PF_Err result = effect_original(cmd, in_data, out_data, params,
                                        reinterpret_cast<PF_LayerDef*>(arg4), extra);

        // 全量扫描两个命令；仅 USER_CHANGED_PARAM 可请求 UI 刷新(避免死循环)
        const bool nativeUtf8 = g_PluginOptInUTF8[ID].load(std::memory_order_acquire);
        const int fixed_count = FixInlineParamArraysOnly(in_data, params, nativeUtf8);
        if (cmd == PF_Cmd_USER_CHANGED_PARAM && fixed_count > 0 && out_data) {
            out_data->out_flags |= PF_OutFlag_REFRESH_UI;
        }
        return result;
    }

    default: {
        // 渲染、GLOBAL_SETUP 及其它命令无需线程上下文
        auto* in_data  = reinterpret_cast<PF_InData*>(arg1);
        auto* out_data = reinterpret_cast<PF_OutData*>(arg2);

        PF_Err result = effect_original(cmd, in_data, out_data,
                                        reinterpret_cast<PF_ParamDef**>(arg3),
                                        reinterpret_cast<PF_LayerDef*>(arg4),
                                        reinterpret_cast<void*>(arg5));

        // 记录插件是否声明 STRINGS_ARE_UTF8
        if (cmd == PF_Cmd_GLOBAL_SETUP && out_data) {
            if (!g_PluginPiPLOverridesFlags[ID].load(std::memory_order_acquire)) {
                const bool optInUtf8 = (out_data->out_flags2 & PF_OutFlag2_STRINGS_ARE_UTF8) != 0;
                g_PluginOptInUTF8[ID].store(optInUtf8, std::memory_order_release);
            }
        }
        return result;
    }
    }
}

template<int ID>
PF_Err Trigger_EffectEntryPoint(
    ULONG_PTR arg0, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3, ULONG_PTR arg4, ULONG_PTR arg5)
{
    return EffectEntryPointCommon(ID, arg0, arg1, arg2, arg3, arg4, arg5);
}

#define P_ENTRY(n) &Trigger_EffectEntryPoint<n>
using EntryPointFunc_t = PF_Err(*)(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);

#define P_ENTRY_8(n)    P_ENTRY(n), P_ENTRY(n+1), P_ENTRY(n+2), P_ENTRY(n+3), P_ENTRY(n+4), P_ENTRY(n+5), P_ENTRY(n+6), P_ENTRY(n+7)
#define P_ENTRY_32(n)   P_ENTRY_8(n), P_ENTRY_8(n+8), P_ENTRY_8(n+16), P_ENTRY_8(n+24)
#define P_ENTRY_128(n)  P_ENTRY_32(n), P_ENTRY_32(n+32), P_ENTRY_32(n+64), P_ENTRY_32(n+96)
#define P_ENTRY_512(n)  P_ENTRY_128(n), P_ENTRY_128(n+128), P_ENTRY_128(n+256), P_ENTRY_128(n+384)
#define P_ENTRY_2048(n) P_ENTRY_512(n), P_ENTRY_512(n+512), P_ENTRY_512(n+1024), P_ENTRY_512(n+1536)

static constinit EntryPointFunc_t g_WrapperPool[WRAPPER_POOL_SIZE] = {
    P_ENTRY_2048(0)
};

// ==========================================
// 8. 模块扫描与 LoadLibrary Hook
// ==========================================

struct ExportDirInfo {
    IMAGE_EXPORT_DIRECTORY* exports;
    DWORD rva;
    DWORD size;
};

static bool SafeGetExportDirectory(HMODULE hMod, ExportDirInfo* out_info) noexcept {
    if (!hMod || !out_info) return false;
    if ((reinterpret_cast<uintptr_t>(hMod) & 3u) != 0) return false;

    __try {
        auto* base = reinterpret_cast<unsigned char*>(hMod);
        auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;
        if (dosHeader->e_lfanew < sizeof(IMAGE_DOS_HEADER) || dosHeader->e_lfanew > 0x10000) return false;

        auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;

        IMAGE_DATA_DIRECTORY exportDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (exportDir.Size == 0 || exportDir.VirtualAddress == 0) return false;

        DWORD imageSize = ntHeaders->OptionalHeader.SizeOfImage;
        if (exportDir.VirtualAddress >= imageSize) return false;

        out_info->exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + exportDir.VirtualAddress);
        out_info->rva     = exportDir.VirtualAddress;
        out_info->size    = exportDir.Size;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool IsAexModulePath(HMODULE hMod, const wchar_t* hintPath) noexcept {
    wchar_t fullPath[MAX_PATH] = {};
    const wchar_t* pathToCheck = nullptr;

    if (DWORD n = GetModuleFileNameW(hMod, fullPath, MAX_PATH); n > 0 && n < MAX_PATH) {
        pathToCheck = fullPath;
    } else if (hintPath && hintPath[0] != L'\0') {
        pathToCheck = hintPath;
    } else {
        return false;
    }

    const size_t len = wcslen(pathToCheck);
    if (len < 4) return false;
    return _wcsicmp(pathToCheck + len - 4, L".aex") == 0;
}

static void HookSingleExportedEntry(void* pTarget, bool piplOptInUtf8, bool piplOverridesFlags) noexcept {
    if (!pTarget) return;

    std::lock_guard<std::mutex> lock(g_WrapperMutex);
    if (g_HookedTargets.count(pTarget) > 0) [[unlikely]] {
        return;
    }

    const int currentID = g_WrapperCount.load(std::memory_order_relaxed);
    if (currentID >= WRAPPER_POOL_SIZE) return;

    void* trampolineBuf = nullptr;
    if (MH_CreateHook(pTarget,
                      reinterpret_cast<LPVOID>(g_WrapperPool[currentID]),
                      reinterpret_cast<LPVOID*>(&trampolineBuf)) == MH_OK) {
        g_PluginOptInUTF8[currentID].store(piplOptInUtf8, std::memory_order_release);
        g_PluginPiPLOverridesFlags[currentID].store(piplOverridesFlags, std::memory_order_release);
        g_OriginalEntryPoints[currentID].store(trampolineBuf, std::memory_order_release);

        if (MH_EnableHook(pTarget) == MH_OK) {
            try { g_HookedTargets.insert(pTarget); } catch (...) {}
            g_WrapperCount.fetch_add(1, std::memory_order_release);
        } else {
            g_OriginalEntryPoints[currentID].store(nullptr, std::memory_order_release);
            g_PluginOptInUTF8[currentID].store(false, std::memory_order_release);
            g_PluginPiPLOverridesFlags[currentID].store(false, std::memory_order_release);
            MH_RemoveHook(pTarget);
        }
    }
}

void HookModuleEntryPoints(HMODULE hMod, const wchar_t* hintPath) {
    if (!hMod || hMod == g_SelfModule) return;
    if ((reinterpret_cast<uintptr_t>(hMod) & 3u) != 0) return;

    // 只处理 .aex 插件二进制，忽略 Plug-ins 目录下的辅助 .dll
    if (!IsAexModulePath(hMod, hintPath)) {
        return;
    }

    ExportDirInfo expInfo{};
    if (!SafeGetExportDirectory(hMod, &expInfo)) {
        return;
    }

    // 解析内嵌 PiPL 资源以发现 Effect 入口与 27.0 UTF-8 标志
    ModulePiPLSummary pipl = InspectModulePiPLs(hMod);

    // 若该 .aex 只有 AEGP(无 Effect)则跳过
    if (pipl.hasAnyPiPL && pipl.effects.empty()) {
        return;
    }

    auto* base      = reinterpret_cast<unsigned char*>(hMod);
    auto* exports   = expInfo.exports;
    auto* names     = reinterpret_cast<DWORD*>(base + exports->AddressOfNames);
    auto* functions = reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);
    auto* ordinals  = reinterpret_cast<WORD*>(base + exports->AddressOfNameOrdinals);

    auto findExportAddr = [&](const char* targetName) -> void* {
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            const char* name = reinterpret_cast<const char*>(base + names[i]);
            if (strcmp(name, targetName) == 0) {
                const DWORD funcRva = functions[ordinals[i]];
                if (funcRva >= expInfo.rva && funcRva < expInfo.rva + expInfo.size) {
                    return nullptr; // 转发导出
                }
                return reinterpret_cast<void*>(base + funcRva);
            }
        }
        return nullptr;
    };

    bool hookedViaPiPL = false;
    for (const auto& eff : pipl.effects) {
        if (eff.entryName[0] != '\0') {
            if (void* pTarget = findExportAddr(eff.entryName)) {
                HookSingleExportedEntry(pTarget, eff.optInUtf8, eff.piplOverridesFlags);
                hookedViaPiPL = true;
            }
        }
    }

    // 无 PiPL 的现代插件兜底
    if (!hookedViaPiPL) {
        if (void* pTarget = findExportAddr("EffectMain")) {
            HookSingleExportedEntry(pTarget, false, false);
        }
    }
}

void EnumerateAndHookLoadedModules() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me;
        me.dwSize = sizeof(me);
        if (Module32FirstW(hSnapshot, &me)) {
            do {
                HookModuleEntryPoints(me.hModule, me.szExePath);
            } while (Module32NextW(hSnapshot, &me));
        }
        CloseHandle(hSnapshot);
    }
}

using LoadLibraryW_t   = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
static LoadLibraryW_t   Original_LoadLibraryW   = nullptr;
static LoadLibraryExW_t Original_LoadLibraryExW = nullptr;

HMODULE WINAPI Hooked_LoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE hMod = Original_LoadLibraryW(lpLibFileName);
    if (hMod) {
        HookModuleEntryPoints(hMod, lpLibFileName);
    }
    return hMod;
}

HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hMod = Original_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    constexpr DWORD kDataFileFlags = LOAD_LIBRARY_AS_DATAFILE
                                   | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE
                                   | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if (hMod && (dwFlags & kDataFileFlags) == 0) {
        HookModuleEntryPoints(hMod, lpLibFileName);
    }
    return hMod;
}

// ==========================================
// 9. AEGP 插件入口与 DllMain
// ==========================================

#if defined(_MSC_VER)
#define FIXER_EXPORT __declspec(dllexport)
#else
#define FIXER_EXPORT __attribute__((visibility("default")))
#endif

extern "C" FIXER_EXPORT PF_Err EntryPointFunc(
    struct SPBasicSuite* pica_basicP,
    A_long major_version,
    A_long minor_version,
    AEGP_PluginID aegp_plugin_id,
    AEGP_GlobalRefcon* global_refconP)
{
    UINT legacyCP = 0;
    const FixerMode mode = DetectHostFixerMode(legacyCP);
    g_LegacyCodePage.store(legacyCP, std::memory_order_release);
    g_FixerMode.store(mode, std::memory_order_release);

    if (mode == FixerMode::Disabled) {
        return PF_Err_NONE;
    }

    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED) {
        // 启动时立即安装 PF_UpdateParamUI hook
        if (pica_basicP) {
            EnsurePFUpdateParamUIHooked(pica_basicP);
        }

        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        if (hKernel32) {
            auto* pLLW = reinterpret_cast<void*>(GetProcAddress(hKernel32, "LoadLibraryW"));
            if (pLLW) {
                TryCreateAndEnableHook(pLLW,
                                       reinterpret_cast<LPVOID>(&Hooked_LoadLibraryW),
                                       reinterpret_cast<LPVOID*>(&Original_LoadLibraryW));
            }

            auto* pLLEW = reinterpret_cast<void*>(GetProcAddress(hKernel32, "LoadLibraryExW"));
            if (pLLEW) {
                TryCreateAndEnableHook(pLLEW,
                                       reinterpret_cast<LPVOID>(&Hooked_LoadLibraryExW),
                                       reinterpret_cast<LPVOID*>(&Original_LoadLibraryExW));
            }
        }

        EnumerateAndHookLoadedModules();
    }
    return PF_Err_NONE;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_SelfModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        // 仅在显式 FreeLibrary 时清理，避免进程退出时加载器锁死锁
        if (lpvReserved == nullptr) {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
        }
    }
    return TRUE;
}
