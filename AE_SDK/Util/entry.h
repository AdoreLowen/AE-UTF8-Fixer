/*******************************************************************/
/*                                                                 */
/* Copyright 2007 Adobe                                            */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  Adobe permits you to use, modify, and distribute this  */
/* file in accordance with the terms of the Adobe license          */
/* agreement accompanying it.                                      */
/*                                                                 */
/*******************************************************************/

/*
    Entry.h
    
    Part of the Adobe After Effects SDK.
*/

#include "AE_PluginData.h"

#ifdef AE_OS_WIN
    #define DllExport __declspec(dllexport)
#elif defined AE_OS_MAC
    #define DllExport __attribute__((visibility("default")))
#endif

#define AE_ENTRY_POINT "EffectMain"
#define AE_RESERVED_INFO 8

#define PF_REGISTER_EFFECT(INPTR, CBPTR, NAME, MATCHNAME, CATEGORY, RESERVEDINFO)                                      \
    result = (*(CBPTR))(                                                                                               \
        (INPTR),                                                                                                       \
        reinterpret_cast<const A_u_char*>(NAME),                                                                       \
        reinterpret_cast<const A_u_char*>(MATCHNAME),                                                                  \
        reinterpret_cast<const A_u_char*>(CATEGORY),                                                                   \
        reinterpret_cast<const A_u_char*>(AE_ENTRY_POINT),                                                             \
        'eFKT',                                                                                                        \
        PF_AE_PLUG_IN_VERSION,                                                                                         \
        PF_AE_PLUG_IN_SUBVERS,                                                                                         \
        RESERVEDINFO);                                                                                                 \
    if (result == A_Err_NONE)                                                                                          \
    {                                                                                                                  \
        result = PF_Err_NONE;                                                                                          \
    }

#define PF_REGISTER_EFFECT_EXT2(INPTR, CBPTR, NAME, MATCHNAME, CATEGORY, RESERVEDINFO, ENTRY_POINT, SUPPORT_URL)       \
    result = (*(CBPTR))(                                                                                               \
        (INPTR),                                                                                                       \
        reinterpret_cast<const A_u_char*>(NAME),                                                                       \
        reinterpret_cast<const A_u_char*>(MATCHNAME),                                                                  \
        reinterpret_cast<const A_u_char*>(CATEGORY),                                                                   \
        reinterpret_cast<const A_u_char*>(ENTRY_POINT),                                                                \
        'eFKT',                                                                                                        \
        PF_AE_PLUG_IN_VERSION,                                                                                         \
        PF_AE_PLUG_IN_SUBVERS,                                                                                         \
        RESERVEDINFO,                                                                                                  \
        reinterpret_cast<const A_u_char*>(SUPPORT_URL));                                                               \
    if (result == A_Err_NONE)                                                                                          \
    {                                                                                                                  \
        result = PF_Err_NONE;                                                                                          \
    }

#define PF_REGISTER_EFFECT_EXT3(                                                                                       \
    INPTR,                                                                                                             \
    CBPTR,                                                                                                             \
    NAME,                                                                                                              \
    MATCHNAME,                                                                                                         \
    CATEGORY,                                                                                                          \
    RESERVEDINFO,                                                                                                      \
    ENTRY_POINT,                                                                                                       \
    SUPPORT_URL,                                                                                                       \
    SEARCH_KEYWORDS,                                                                                                   \
    EFFECT_DESCRIPTION)                                                                                                \
    result = (*(CBPTR))(                                                                                               \
        (INPTR),                                                                                                       \
        reinterpret_cast<const A_u_char*>(NAME),                                                                       \
        reinterpret_cast<const A_u_char*>(MATCHNAME),                                                                  \
        reinterpret_cast<const A_u_char*>(CATEGORY),                                                                   \
        reinterpret_cast<const A_u_char*>(ENTRY_POINT),                                                                \
        'eFKT',                                                                                                        \
        PF_AE_PLUG_IN_VERSION,                                                                                         \
        PF_AE_PLUG_IN_SUBVERS,                                                                                         \
        RESERVEDINFO,                                                                                                  \
        reinterpret_cast<const A_u_char*>(SUPPORT_URL),                                                                \
        reinterpret_cast<const A_u_char*>(SEARCH_KEYWORDS),                                                            \
        reinterpret_cast<const A_u_char*>(EFFECT_DESCRIPTION));                                                        \
    if (result == A_Err_NONE)                                                                                          \
    {                                                                                                                  \
        result = PF_Err_NONE;                                                                                          \
    }
