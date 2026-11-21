
/* This file should be included after all headers, but before the definition of any suites
or data structures.
*/
#if defined(_MSC_VER) && !defined(__clang__)
    #pragma warning(push)
    #pragma warning(disable : 4103)
#elif defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wpragma-pack"
#endif
#include <adobesdk/config/PreConfig.h>
#if defined(_MSC_VER) && !defined(__clang__)
    #pragma warning(pop)
#elif defined(__clang__)
    #pragma clang diagnostic pop
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _AE_GENERAL_PLUG_PRE___
    #error "AE_GeneralPlugPre.h not balanced"
#endif

#define _AE_GENERAL_PLUG_PRE___