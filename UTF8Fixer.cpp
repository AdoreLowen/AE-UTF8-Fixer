#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectVers.h"
#include "AE_EffectSuites.h"
#include "AE_GeneralPlug.h"
#include "MinHook.h"

#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <string_view>
#include <unordered_set>
#include <mutex> 
#include <shared_mutex>
#include <vector> 
#include <algorithm>
#include <atomic>
#include <memory>
#include <cwctype>
#include <span>

// ==========================================
// 1. UTF-8 验证与处理
// ==========================================

inline bool IsValidUTF8(std::string_view sv) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(sv.data());
    const auto* end = bytes + sv.size();

    bool has_high_byte = false;      
    bool has_3byte_character = false; 

    while (bytes < end) {
        if (bytes[0] <= 0x7F) {
            bytes += 1;
        }
        else if ((bytes[0] & 0xE0) == 0xC0) {
            if (bytes + 1 >= end || (bytes[1] & 0xC0) != 0x80) return false;
            has_high_byte = true;
            bytes += 2;
        }
        else if ((bytes[0] & 0xF0) == 0xE0) {
            if (bytes + 2 >= end || (bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80) return false;
            has_high_byte = true;
            has_3byte_character = true;
            bytes += 3;
        }
        else if ((bytes[0] & 0xF8) == 0xF0) {
            if (bytes + 3 >= end || (bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 || (bytes[3] & 0xC0) != 0x80) return false;
            has_high_byte = true;
            bytes += 4;
        }
        else {
            return false;
        }
    }

    if (has_high_byte && !has_3byte_character) {
        return false; 
    }

    return true;
}

inline bool IsInvalidUTF8(const char* str) noexcept {
    if (!str || str[0] == '\0') return false;
    return !IsValidUTF8(std::string_view(str));
}

void TrimInvalidTailUTF8(std::span<char> s) noexcept {
    if (s.empty()) return;
    size_t i = s.size();
    while (i > 0) {
        auto c = static_cast<unsigned char>(s[i - 1]);
        if ((c & 0x80) == 0x00) {
            break;
        }
        else if ((c & 0xC0) == 0xC0) {
            size_t need_bytes = 0;
            if ((c & 0xE0) == 0xC0) need_bytes = 2;
            else if ((c & 0xF0) == 0xE0) need_bytes = 3;
            else if ((c & 0xF8) == 0xF0) need_bytes = 4;

            if (s.size() - (i - 1) < need_bytes) {
                s[i - 1] = '\0';
            }
            break;
        }
        else if ((c & 0xC0) == 0x80) {
            i--;
        }
        else {
            s[i - 1] = '\0';
            i--;
        }
    }
}

// ==========================================
// 2. GBKToUTF8 转换器
// ==========================================

template<size_t InlineSize = 128>
class GBKToUTF8Converter {
public:
    explicit GBKToUTF8Converter(const char* gbk_str) noexcept {
        if (!gbk_str || gbk_str[0] == '\0') return;

        constexpr UINT codePage = 936; 
        int wlen = MultiByteToWideChar(codePage, 0, gbk_str, -1, nullptr, 0);
        if (wlen <= 1) return;

        wchar_t* wbuf = m_inlineWBuf;
        std::unique_ptr<wchar_t[]> heapWBuf;
        if (static_cast<size_t>(wlen) > InlineSize) {
            heapWBuf = std::make_unique<wchar_t[]>(wlen);
            wbuf = heapWBuf.get();
        }

        MultiByteToWideChar(codePage, 0, gbk_str, -1, wbuf, wlen);

        int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
        if (u8len <= 1) return;

        if (static_cast<size_t>(u8len) > InlineSize * 2) {
            m_heapU8Buf.resize(u8len);
            WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, m_heapU8Buf.data(), u8len, nullptr, nullptr);
            m_heapU8Buf.resize(u8len - 1); 
            m_view = m_heapU8Buf;
        } else {
            WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, m_inlineU8Buf, u8len, nullptr, nullptr);
            m_view = std::string_view(m_inlineU8Buf, u8len - 1);
        }
    }

    [[nodiscard]] std::string_view view() const noexcept { return m_view; }
    [[nodiscard]] bool empty() const noexcept { return m_view.empty(); }
    
    std::string move_to_string() && noexcept {
        if (!m_heapU8Buf.empty()) {
            return std::move(m_heapU8Buf);
        }
        return std::string(m_view);
    }

private:
    wchar_t m_inlineWBuf[InlineSize]{};
    char m_inlineU8Buf[InlineSize * 2]{};
    std::string m_heapU8Buf;
    std::string_view m_view;
};

// ==========================================
// 3. 全局字符串缓存
// ==========================================

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
    std::shared_mutex mutex;
    std::unordered_set<std::string, StringHash, StringEqual> cache;
};

inline CacheContainer& GetGlobalCache() noexcept {
    static CacheContainer instance; 
    return instance;
}

const char* GetCachedUTF8String(GBKToUTF8Converter<>& converter) {
    auto& [mutex, cache] = GetGlobalCache(); 
    std::string_view sv = converter.view();
    
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
}

// ==========================================
// 4. 参数修复与 Hook 逻辑
// ==========================================

void FixStringPointer(const char*& ptr) noexcept {
    if (ptr && ptr[0] != '\0' && IsInvalidUTF8(ptr)) {
        GBKToUTF8Converter<> converter(ptr);
        if (!converter.empty()) {
            ptr = GetCachedUTF8String(converter);
        }
    }
}

template <size_t N>
void FixStringArray(char(&arr)[N]) noexcept {
    if (arr[0] != '\0' && IsInvalidUTF8(arr)) {
        GBKToUTF8Converter<> converter(arr);
        std::string_view u8name = converter.view();
        if (!u8name.empty()) {
            strncpy_s(arr, N, u8name.data(), _TRUNCATE);
            TrimInvalidTailUTF8(std::span<char>(arr, strlen(arr))); 
        }
    }
}

void ProcessAndFixParam(PF_ParamDef* def) noexcept {
    if (!def) return;

    FixStringArray(def->PF_DEF_NAME);

    switch (def->param_type) {
    case PF_Param_POPUP:
        FixStringPointer(def->u.pd.u.PF_DEF_NAMESPTR);
        break;

    case PF_Param_CHECKBOX:
        FixStringPointer(def->u.bd.u.PF_DEF_NAMEPTR);
        break;

    case PF_Param_BUTTON:
        FixStringPointer(def->u.button_d.u.PF_DEF_NAMESPTR);
        break;

    case PF_Param_SLIDER:
        FixStringArray(def->u.sd.value_desc);
        break;

    case PF_Param_FLOAT_SLIDER:
        FixStringArray(def->u.fs_d.value_desc);
        break;

    default:
        break;
    }
}

static std::atomic<void*> g_HostAddParamAddr{ nullptr };
static PF_Err(*g_OriginalHostAddParam)(PF_ProgPtr, PF_ParamIndex, PF_ParamDef*) = nullptr;
static std::mutex g_AddParamHookMutex;

PF_Err Hooked_HostAddParam(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_ParamDef* def) {
    if (def) {
        ProcessAndFixParam(def);
    }
    if (g_OriginalHostAddParam) {
        return g_OriginalHostAddParam(effect_ref, index, def);
    }
    return PF_Err_NONE;
}

void EnsureHostAddParamHooked(void* pTarget) noexcept {
    if (!pTarget) return;

    if (g_HostAddParamAddr.load(std::memory_order_acquire) == pTarget) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_AddParamHookMutex);
    if (g_HostAddParamAddr.load(std::memory_order_relaxed) == pTarget) {
        return;
    }

    if (g_HostAddParamAddr.load(std::memory_order_relaxed) == nullptr) {
        MH_STATUS status = MH_CreateHook(pTarget, reinterpret_cast<LPVOID>(&Hooked_HostAddParam), reinterpret_cast<LPVOID*>(&g_OriginalHostAddParam));
        if (status == MH_OK || status == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(pTarget);
            g_HostAddParamAddr.store(pTarget, std::memory_order_release);
        }
    }
}

static PF_Err(*g_Original_PF_UpdateParamUI)(PF_ProgPtr, PF_ParamIndex, const PF_ParamDef*) = nullptr;
static std::mutex g_SuiteHookMutex;

PF_Err Hooked_PF_UpdateParamUI(PF_ProgPtr effect_ref, PF_ParamIndex param_index, const PF_ParamDef* defP) {
    if (defP) {
        PF_ParamDef fixed_def = *defP; 
        ProcessAndFixParam(&fixed_def); 
        return g_Original_PF_UpdateParamUI(effect_ref, param_index, &fixed_def);
    }
    if (g_Original_PF_UpdateParamUI) {
        return g_Original_PF_UpdateParamUI(effect_ref, param_index, defP);
    }
    return PF_Err_NONE;
}

#define WRAPPER_POOL_SIZE 2048
std::atomic<void*> g_OriginalEntryPoints[WRAPPER_POOL_SIZE] = {};
int g_WrapperCount = 0;
std::mutex g_WrapperMutex;

template<int ID>
PF_Err Trigger_EffectEntryPoint(
    ULONG_PTR arg0,
    ULONG_PTR arg1,
    ULONG_PTR arg2,
    ULONG_PTR arg3,
    ULONG_PTR arg4,
    ULONG_PTR arg5)
{
    void* original = g_OriginalEntryPoints[ID].load(std::memory_order_acquire);
    if (!original) return PF_Err_NONE;

    if (arg0 > 0xFFFF) {
        typedef PF_Err(*AEGP_EntryPoint_t)(struct SPBasicSuite*, A_long, A_long, AEGP_PluginID, AEGP_GlobalRefcon*);
        auto aegp_original = reinterpret_cast<AEGP_EntryPoint_t>(original);
        return aegp_original(
            reinterpret_cast<struct SPBasicSuite*>(arg0),
            static_cast<A_long>(arg1),
            static_cast<A_long>(arg2),
            static_cast<AEGP_PluginID>(arg3),
            reinterpret_cast<AEGP_GlobalRefcon*>(arg4)
        );
    }

    PF_Cmd cmd = static_cast<PF_Cmd>(arg0);
    auto* in_data = reinterpret_cast<PF_InData*>(arg1);
    auto* out_data = reinterpret_cast<PF_OutData*>(arg2);
    auto** params = reinterpret_cast<PF_ParamDef**>(arg3);
    auto* output = reinterpret_cast<PF_LayerDef*>(arg4);
    void* extra = reinterpret_cast<void*>(arg5);

    if (in_data && in_data->pica_basicP) {
        std::lock_guard<std::mutex> lock(g_SuiteHookMutex);
        if (!g_Original_PF_UpdateParamUI) {
            PF_ParamUtilsSuite3* paramSuite = nullptr;
            in_data->pica_basicP->AcquireSuite("PF ParamUtils Suite", 3, (const void**)&paramSuite);
            
            if (paramSuite && paramSuite->PF_UpdateParamUI) {
                MH_CreateHook(reinterpret_cast<void*>(paramSuite->PF_UpdateParamUI), 
                              reinterpret_cast<LPVOID>(&Hooked_PF_UpdateParamUI), 
                              reinterpret_cast<LPVOID*>(&g_Original_PF_UpdateParamUI));
                MH_EnableHook(reinterpret_cast<void*>(paramSuite->PF_UpdateParamUI));
            }
            
            if (paramSuite) {
                in_data->pica_basicP->ReleaseSuite("PF ParamUtils Suite", 3);
            }
        }
    }

    if (cmd == PF_Cmd_PARAMS_SETUP && in_data && in_data->inter.add_param) {
        EnsureHostAddParamHooked(reinterpret_cast<void*>(in_data->inter.add_param));
    }

    typedef PF_Err(*EffectEntryPoint_t)(PF_Cmd, PF_InData*, PF_OutData*, PF_ParamDef* [], PF_LayerDef*, void*);
    auto effect_original = reinterpret_cast<EffectEntryPoint_t>(original);

    PF_Err err = PF_Err_NONE;
    if (effect_original) {
        err = effect_original(cmd, in_data, out_data, params, output, extra);
    }

    return err;
}

#define P_ENTRY(n) &Trigger_EffectEntryPoint<n>
typedef PF_Err(*EntryPointFunc_t)(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);

#define P_ENTRY_8(n)   P_ENTRY(n), P_ENTRY(n+1), P_ENTRY(n+2), P_ENTRY(n+3), P_ENTRY(n+4), P_ENTRY(n+5), P_ENTRY(n+6), P_ENTRY(n+7)
#define P_ENTRY_32(n)  P_ENTRY_8(n), P_ENTRY_8(n+8), P_ENTRY_8(n+16), P_ENTRY_8(n+24)
#define P_ENTRY_128(n) P_ENTRY_32(n), P_ENTRY_32(n+32), P_ENTRY_32(n+64), P_ENTRY_32(n+96)
#define P_ENTRY_512(n) P_ENTRY_128(n), P_ENTRY_128(n+128), P_ENTRY_128(n+256), P_ENTRY_128(n+384)
#define P_ENTRY_2048(n) P_ENTRY_512(n), P_ENTRY_512(n+512), P_ENTRY_512(n+1024), P_ENTRY_512(n+1536)

EntryPointFunc_t g_WrapperPool[WRAPPER_POOL_SIZE] = {
    P_ENTRY_2048(0)
};

bool SafeGetExportDirectory(HMODULE hMod, IMAGE_EXPORT_DIRECTORY** out_exports) noexcept {
    if (!hMod || !out_exports) return false;

    __try {
        auto* base = reinterpret_cast<unsigned char*>(hMod);
        auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;
        if (dosHeader->e_lfanew < sizeof(IMAGE_DOS_HEADER) || dosHeader->e_lfanew > 0x10000) return false;
        auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;
        IMAGE_DATA_DIRECTORY exportDirInfo = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (exportDirInfo.Size == 0 || exportDirInfo.VirtualAddress == 0) return false;
        DWORD imageSize = ntHeaders->OptionalHeader.SizeOfImage;
        if (exportDirInfo.VirtualAddress >= imageSize) return false;

        *out_exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + exportDirInfo.VirtualAddress);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void HookModuleEntryPoints(HMODULE hMod, const std::wstring& modPath) {
    if (!hMod) return;

    std::wstring lowerPath = modPath;
    if (lowerPath.empty()) return;
    
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::towlower);

    if (lowerPath.find(L".aex") == std::wstring::npos &&
        lowerPath.find(L"\\plug-ins\\") == std::wstring::npos) {
        return;
    }

    IMAGE_EXPORT_DIRECTORY* exports = nullptr;
    if (!SafeGetExportDirectory(hMod, &exports)) {
        return;
    }

    auto* base = reinterpret_cast<unsigned char*>(hMod);
    auto* names = reinterpret_cast<DWORD*>(base + exports->AddressOfNames);
    auto* functions = reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);
    auto* ordinals = reinterpret_cast<WORD*>(base + exports->AddressOfNameOrdinals);

    std::lock_guard<std::mutex> lock(g_WrapperMutex);

    for (DWORD i = 0; i < exports->NumberOfNames; i++) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (strcmp(name, "EntryPointFunc") == 0 || strcmp(name, "EffectMain") == 0) {
            void* pTarget = reinterpret_cast<void*>(base + functions[ordinals[i]]);

            bool alreadyHooked = false;
            for (int j = 0; j < g_WrapperCount; j++) {
                if (g_OriginalEntryPoints[j].load(std::memory_order_relaxed) == pTarget) {
                    alreadyHooked = true;
                    break;
                }
            }
            if (alreadyHooked) continue;

            if (g_WrapperCount >= WRAPPER_POOL_SIZE) break;

            int currentID = g_WrapperCount;
            void* originalTrampoline = nullptr;
            MH_STATUS status = MH_CreateHook(pTarget, reinterpret_cast<LPVOID>(g_WrapperPool[currentID]), &originalTrampoline);
            if (status == MH_OK) {
                g_OriginalEntryPoints[currentID].store(originalTrampoline, std::memory_order_release);
                MH_EnableHook(pTarget);
                g_WrapperCount++;
            }
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

typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
LoadLibraryW_t Original_LoadLibraryW = nullptr;
LoadLibraryExW_t Original_LoadLibraryExW = nullptr;

HMODULE WINAPI Hooked_LoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE hMod = Original_LoadLibraryW(lpLibFileName);
    if (hMod && lpLibFileName) {
        HookModuleEntryPoints(hMod, lpLibFileName);
    }
    return hMod;
}

HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hMod = Original_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (hMod && lpLibFileName) {
        HookModuleEntryPoints(hMod, lpLibFileName);
    }
    return hMod;
}

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
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED) {
        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        if (hKernel32) {
            auto* pLLW = reinterpret_cast<void*>(GetProcAddress(hKernel32, "LoadLibraryW"));
            if (pLLW) {
                MH_CreateHook(pLLW, reinterpret_cast<LPVOID>(&Hooked_LoadLibraryW), reinterpret_cast<LPVOID*>(&Original_LoadLibraryW));
                MH_EnableHook(pLLW);
            }

            auto* pLLEW = reinterpret_cast<void*>(GetProcAddress(hKernel32, "LoadLibraryExW"));
            if (pLLEW) {
                MH_CreateHook(pLLEW, reinterpret_cast<LPVOID>(&Hooked_LoadLibraryExW), reinterpret_cast<LPVOID*>(&Original_LoadLibraryExW));
                MH_EnableHook(pLLEW);
            }
        }

        EnumerateAndHookLoadedModules();
    }
    return PF_Err_NONE;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_DETACH:
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        break;
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}