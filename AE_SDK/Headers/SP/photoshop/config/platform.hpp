//
//  ADOBE CONFIDENTIAL
//  __________________
//
//  Copyright 2016 Adobe
//  All Rights Reserved.
//
//  NOTICE:  All information contained herein is, and remains
//  the property of Adobe and its suppliers, if any. The intellectual
//  and technical concepts contained herein are proprietary to Adobe
//  and its suppliers and are protected by all applicable intellectual
//  property laws, including trade secret and copyright laws.
//  Dissemination of this information or reproduction of this material
//  is strictly forbidden unless prior written permission is obtained
//  from Adobe.
//

#ifndef PHOTOSHOP_PLATFORM_HPP
#define PHOTOSHOP_PLATFORM_HPP

// Core platform detection macros from the standalone platform_config library.
// NOTE: Do not use relative includes here. Consumers should depend on
// //libs/platform_config:platform_config to get the include path.
#include <platform_config/platform.hpp>

/**************************************************************************************************/
// Legacy/deprecated macros kept here for backward compatibility.
// New code should use PHOTOSHOP_PLATFORM(), PHOTOSHOP_ARCH(), etc. from platform_config.
/**************************************************************************************************/

#ifndef RC_INVOKED // Windows Resource Compiler

// Poisoned 2026 Mar 01. Use PHOTOSHOP_PLATFORM() from platform_config.
#define PS_PLATFORM_ANDROID POISONED_MACRO_PS_PLATFORM_ANDROID()
#define PS_PLATFORM_APPLE POISONED_MACRO_PS_PLATFORM_APPLE()
#define PS_PLATFORM_IOS POISONED_MACRO_PS_PLATFORM_IOS()
#define PS_PLATFORM_LINUX POISONED_MACRO_PS_PLATFORM_LINUX()
#define PS_PLATFORM_MACOS POISONED_MACRO_PS_PLATFORM_MACOS()
#define PS_PLATFORM_MS POISONED_MACRO_PS_PLATFORM_MS() // -> PHOTOSHOP_PLATFORM(MICROSOFT)
#define PS_PLATFORM_POSIX POISONED_MACRO_PS_PLATFORM_POSIX()
#define PS_PLATFORM_WEB POISONED_MACRO_PS_PLATFORM_WEB()
// Poisoned 2026 Apr 15. Use PHOTOSHOP_PLATFORM(WIN32) from platform_config.
#define PS_PLATFORM_WIN32 POISONED_MACRO_PS_PLATFORM_WIN32()

// Poisoned PS_OS_* macros (2026 Mar 01, PS-188308). Use PHOTOSHOP_PLATFORM() instead.
// These expand to undefined function calls so the compiler will error if any old
// PS_OS_* macro is used. The error message includes the poisoned name, making it
// clear which macro to replace. See the inline comment for the correct replacement.
#define PS_OS_WIN POISONED_MACRO_PS_OS_WIN()         // Use PHOTOSHOP_PLATFORM(MICROSOFT).
#define PS_OS_IOS POISONED_MACRO_PS_OS_IOS()         // Use PHOTOSHOP_PLATFORM(IOS).
#define PS_OS_MAC POISONED_MACRO_PS_OS_MAC()         // Use PHOTOSHOP_PLATFORM(MACOS).
#define PS_OS_ANDROID POISONED_MACRO_PS_OS_ANDROID() // Use PHOTOSHOP_PLATFORM(ANDROID).
#define PS_OS_LINUX POISONED_MACRO_PS_OS_LINUX()     // Use PHOTOSHOP_PLATFORM(LINUX).
#define PS_OS_WEB POISONED_MACRO_PS_OS_WEB()         // Use PHOTOSHOP_PLATFORM(WEB).

/**************************************************************************************************/

#define qPSIsWin POISONED_MACRO_qPSIsWin() // Poisoned in PR #74383. Use PHOTOSHOP_PLATFORM(MICROSOFT).
#define qPSIsMac POISONED_MACRO_qPSIsMac() // Poisoned in PR #74383. Use PHOTOSHOP_PLATFORM(APPLE).

// Poisoned 2026 Mar 01. Use PHOTOSHOP_PLATFORM(MICROSOFT) instead.
#define MSWindows POISONED_MACRO_MSWindows()

// Deprecated 2026 Mar 01. Use PHOTOSHOP_PLATFORM(APPLE) instead.
// NOTE: We cannot use `#define` or `#pragma GCC poison` for Macintosh because
// Boost's predef/os/macos.h checks `defined(Macintosh)` — a #define would cause
// Boost to misdetect macOS on all platforms, and poison would error in Boost headers.
#ifdef Macintosh
#undef Macintosh
#endif

// Exposure LUT cache configuration
// Default is enabled on Android, disabled elsewhere
#ifndef PS_EXPOSURE_LUT_CACHE
#define PS_EXPOSURE_LUT_CACHE PHOTOSHOP_PLATFORM(ANDROID)
#endif // PS_EXPOSURE_LUT_CACHE

/**************************************************************************************************/
#endif // RC_INVOKED
#endif // PHOTOSHOP_PLATFORM_HPP
