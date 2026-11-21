
#ifdef __cplusplus
} // end extern "C"
#endif

#if defined(_MSC_VER) && !defined(__clang__)
    #pragma warning(push)
    #pragma warning(disable : 4103)
#elif defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wpragma-pack"
#endif
#include <adobesdk/config/PostConfig.h>
#if defined(_MSC_VER) && !defined(__clang__)
    #pragma warning(pop)
#elif defined(__clang__)
    #pragma clang diagnostic pop
#endif

#ifndef _AE_GENERAL_PLUG_PRE___
    #error "AE_GeneralPlugPost.h not balanced"
#else
    #undef _AE_GENERAL_PLUG_PRE___
#endif