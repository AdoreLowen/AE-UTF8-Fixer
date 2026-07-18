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
#ifndef EXCEPTION_EXECUTE_FHANDLER
#define EXCEPTION_EXECUTE_FHANDLER 1
#endif
#include <string>
#include <string_view>
#include <cstring>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <span>

// 跟踪已 hook 的插件数量
#define WRAPPER_POOL_SIZE 2048
std::atomic<void*> g_OriginalEntryPoints[WRAPPER_POOL_SIZE] = {};
std::atomic<int> g_WrapperCount{ 0 };
std::mutex g_WrapperMutex;

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

template<size_t InlineSize = 256>
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

const char* GetCachedUTF8String(GBKToUTF8Converter<>& converter) {
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
}

// ==========================================
// 4. 参数修复与 Hook 逻辑
// ==========================================

inline bool IsBufferWritable(const void* p, size_t len) noexcept {
    if (!p || len == 0) return false;

    auto* base = static_cast<const unsigned char*>(p);
    auto* end = base + len;

    MEMORY_BASIC_INFORMATION mbi{};
    auto* cursor = base;
    while (cursor < end) {
        SIZE_T queried = VirtualQuery(cursor, &mbi, sizeof(mbi));
        if (queried == 0) return false;

        constexpr DWORD writable_mask = PAGE_READWRITE | PAGE_WRITECOPY
                                      | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & writable_mask) == 0) return false;

        auto* regionEnd = static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor) return false;

        cursor = regionEnd;
    }
    return true;
}

void FixStringPointer(const char*& ptr) noexcept {
    if (ptr && ptr[0] != '\0' && !IsValidUTF8(ptr)) [[unlikely]] {
        GBKToUTF8Converter<> converter(ptr);
        if (!converter.empty()) {
            ptr = GetCachedUTF8String(converter);
        }
    }
}

template <size_t N>
void FixStringArray(char(&arr)[N]) noexcept {
    if (arr[0] != '\0' && !IsValidUTF8(arr)) [[unlikely]] {
        if (!IsBufferWritable(arr, N)) {
            return;
        }
        GBKToUTF8Converter<> converter(arr);
        std::string_view u8name = converter.view();
        if (u8name.empty()) {
            return;
        }

        const size_t copyLen = (u8name.size() < N - 1) ? u8name.size() : (N - 1);
        memcpy_s(arr, N, u8name.data(), copyLen);
        arr[copyLen] = '\0';

        TrimInvalidTailUTF8(std::span<char>(arr, copyLen));
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

    case PF_Param_FIX_SLIDER:
        FixStringArray(def->u.fd.value_str);
        FixStringArray(def->u.fd.value_desc);
        break;

    case PF_Param_FLOAT_SLIDER:
        FixStringArray(def->u.fs_d.value_desc);
        break;

    default:

        break;
    }
}

// ==========================================
// 5. 后处理扫描，确保插件修改后参数名称仍然有效
// ==========================================

static int FixAllParamNamesAndCheck(PF_InData* in_data, PF_ParamDef* params[]) noexcept {
    if (!in_data || !params || in_data->num_params <= 0) return 0;

    PF_ParamIndex scan_limit = in_data->num_params;
    if (scan_limit > 10000) scan_limit = 10000;

    int fixed_count = 0;
    for (PF_ParamIndex i = 1; i < scan_limit; i++) {
        if (params[i] && !IsValidUTF8(params[i]->PF_DEF_NAME)) {
            ProcessAndFixParam(params[i]);
            fixed_count++;
        }
    }
    return fixed_count;
}

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

static std::unordered_set<void*> g_HookedAddParamAddrs;
static PF_Err(*g_OriginalHostAddParam)(PF_ProgPtr, PF_ParamIndex, PF_ParamDef*) = nullptr;
static std::mutex g_AddParamHookMutex;

PF_Err Hooked_HostAddParam(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_ParamDef* def) {
    if (def) ProcessAndFixParam(def);
    return g_OriginalHostAddParam(effect_ref, index, def);
}

void EnsureHostAddParamHooked(void* pTarget) noexcept {
    if (!pTarget) return;

    std::lock_guard<std::mutex> lock(g_AddParamHookMutex);

    if (g_HookedAddParamAddrs.count(pTarget) > 0) {
        return;
    }

    if (TryCreateAndEnableHook(pTarget,
                               reinterpret_cast<LPVOID>(&Hooked_HostAddParam),
                               reinterpret_cast<LPVOID*>(&g_OriginalHostAddParam))) {
        g_HookedAddParamAddrs.insert(pTarget);
    }
}

static PF_Err(*g_Original_PF_UpdateParamUI)(PF_ProgPtr, PF_ParamIndex, const PF_ParamDef*) = nullptr;

PF_Err Hooked_PF_UpdateParamUI(PF_ProgPtr effect_ref, PF_ParamIndex param_index, const PF_ParamDef* defP) {
    if (!defP) return g_Original_PF_UpdateParamUI(effect_ref, param_index, defP);
    
    PF_ParamDef fixed_def = *defP;
    ProcessAndFixParam(&fixed_def);
    return g_Original_PF_UpdateParamUI(effect_ref, param_index, &fixed_def);
}

static bool TryInstallPFUpdateParamUIHook(SPBasicSuite* basic) {
    if (!basic) return false;
    
    __try {
        PF_ParamUtilsSuite3* paramSuite = nullptr;
        A_Err err = basic->AcquireSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, (const void**)&paramSuite);
        if (err != A_Err_NONE || !paramSuite) {
            return false;
        }
        
        if (!paramSuite->PF_UpdateParamUI) {
            basic->ReleaseSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
            return false;
        }
        
        void* target = reinterpret_cast<void*>(paramSuite->PF_UpdateParamUI);
        TryCreateAndEnableHook(target,
                               reinterpret_cast<LPVOID>(&Hooked_PF_UpdateParamUI),
                               reinterpret_cast<LPVOID*>(&g_Original_PF_UpdateParamUI));
        
        basic->ReleaseSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
        return true;
    }
    __except(EXCEPTION_EXECUTE_FHANDLER) {
        return false;
    }
}

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
    if (!original) [[unlikely]] return PF_Err_NONE;

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

    typedef PF_Err(*EffectEntryPoint_t)(PF_Cmd, PF_InData*, PF_OutData*, PF_ParamDef* [], PF_LayerDef*, void*);
    auto effect_original = reinterpret_cast<EffectEntryPoint_t>(original);

    if (cmd == PF_Cmd_PARAMS_SETUP && in_data) {
        if (in_data->inter.add_param) {
            EnsureHostAddParamHooked(reinterpret_cast<void*>(in_data->inter.add_param));
        }
        if (in_data->pica_basicP && !g_Original_PF_UpdateParamUI) {
            TryInstallPFUpdateParamUIHook(in_data->pica_basicP);
        }
    }

    PF_Err result = effect_original(cmd, in_data, out_data, params, output, extra);

    if (cmd == PF_Cmd_UPDATE_PARAMS_UI || cmd == PF_Cmd_USER_CHANGED_PARAM) {
        int fixed_count = FixAllParamNamesAndCheck(in_data, params);
        if (fixed_count > 0 && out_data) {
            out_data->out_flags |= PF_OutFlag_REFRESH_UI;
        }
    }

    return result;
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

    CharLowerBuffW(lowerPath.data(), static_cast<DWORD>(lowerPath.size()));

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
            for (int j = 0; j < g_WrapperCount.load(std::memory_order_relaxed); j++) {
                if (g_OriginalEntryPoints[j].load(std::memory_order_relaxed) == pTarget) [[unlikely]] {
                    alreadyHooked = true;
                    break;
                }
            }
            if (alreadyHooked) continue;

            int currentID = g_WrapperCount.load(std::memory_order_relaxed);
            if (currentID >= WRAPPER_POOL_SIZE) break;
            void* trampolineBuf = nullptr;
            if (TryCreateAndEnableHook(pTarget,
                                       reinterpret_cast<LPVOID>(g_WrapperPool[currentID]),
                                       reinterpret_cast<LPVOID*>(&trampolineBuf))) {
                g_OriginalEntryPoints[currentID].store(trampolineBuf, std::memory_order_release);
                g_WrapperCount.fetch_add(1, std::memory_order_release);
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
        DisableThreadLibraryCalls(hinstDLL);
    } 
    else if (fdwReason == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}