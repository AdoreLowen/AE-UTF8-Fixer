#include "AEConfig.h"
#include "AE_EffectVers.h"
#ifndef AE_OS_WIN
#include <AE_General.r>
#endif

resource 'PiPL'(16000) {
    {
        Kind { AEGP },
        Name { "UTF8 Fixer" },
        Category { "AL's Toolkit" },
        Version { 65536 },
#ifdef AE_OS_WIN
        CodeWin64X86 { "EntryPointFunc" },
#else
        CodeMacIntel64 { "EntryPointFunc" },
        CodeMacARM64 { "EntryPointFunc" },
#endif
    }
};