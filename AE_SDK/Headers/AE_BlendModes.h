/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2025 Adobe                                            */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  All information contained herein is, and remains the   */
/* property of Adobe Systems Incorporated and its suppliers, if    */
/* any.  The intellectual and technical concepts contained         */
/* herein are proprietary to Adobe Systems Incorporated and its    */
/* suppliers and may be covered by U.S. and Foreign Patents,       */
/* patents in process, and are protected by trade secret or        */
/* copyright law.  Dissemination of this information or            */
/* reproduction of this material is strictly forbidden unless      */
/* prior written permission is obtained from Adobe Systems         */
/* Incorporated.                                                   */
/*                                                                 */
/*******************************************************************/

#ifndef _H_AE_BLENDMODES
#define _H_AE_BLENDMODES

#pragma once

#include "A.h"

typedef A_long PF_TransferMode;
typedef PF_TransferMode PF_XferMode;

enum {
    PF_Xfer_NONE = -1,
    PF_Xfer_COPY,
    PF_Xfer_BEHIND,
    PF_Xfer_IN_FRONT,
    PF_Xfer_DISSOLVE,
    PF_Xfer_ADD,
    PF_Xfer_MULTIPLY,
    PF_Xfer_SCREEN,
    PF_Xfer_OVERLAY,
    PF_Xfer_SOFT_LIGHT,
    PF_Xfer_HARD_LIGHT,
    PF_Xfer_DARKEN,
    PF_Xfer_LIGHTEN,
    PF_Xfer_DIFFERENCE,                            // original < PS5.5 Difference
    PF_Xfer_HUE,
    PF_Xfer_SATURATION,
    PF_Xfer_COLOR,
    PF_Xfer_LUMINOSITY,
    PF_Xfer_MULTIPLY_ALPHA,                        // dest alpha *= src alpha
    PF_Xfer_MULTIPLY_ALPHA_LUMA,                   // dest alpha *= src luminance
    PF_Xfer_MULTIPLY_NOT_ALPHA,                    // dest alpha *= ~(src alpha)
    PF_Xfer_MULTIPLY_NOT_ALPHA_LUMA,               // dest alpha *= ~(src luminance)
    PF_Xfer_ADDITIVE_PREMUL,
    PF_Xfer_ALPHA_ADD,
    PF_Xfer_COLOR_DODGE,                           // original < PS5.5 Color Dodge
    PF_Xfer_COLOR_BURN,                            // original < PS5.5 Color Burn
    PF_Xfer_EXCLUSION,

    PF_Xfer_DIFFERENCE2,                           // PS >= 6.0, PDF 1.4 Difference
    PF_Xfer_COLOR_DODGE2,                          // PS >= 6.0, PDF 1.4 Color Dodge
    PF_Xfer_COLOR_BURN2,                           // PS >= 6.0, PDF 1.4 Color Burn

    PF_Xfer_LINEAR_DODGE,
    PF_Xfer_LINEAR_BURN,
    PF_Xfer_LINEAR_LIGHT,
    PF_Xfer_VIVID_LIGHT,
    PF_Xfer_PIN_LIGHT,

    PF_Xfer_HARD_MIX,

    PF_Xfer_LIGHTER_COLOR,                          // new in AE8
    PF_Xfer_DARKER_COLOR,

    PF_Xfer_SUBTRACT,                               // new in AE10
    PF_Xfer_DIVIDE,                            

    PF_Xfer_RESERVED0,                              // private/useless
    PF_Xfer_RESERVED1,                              // ditto

    PF_Xfer_NUM_MODES
};

// obsolete xfer mode names
enum {
    PF_Xfer_TINT  = PF_Xfer_LINEAR_DODGE,
    PF_Xfer_SHADE = PF_Xfer_LINEAR_BURN,
    PF_Xfer_INTENSE_LIGHT = PF_Xfer_VIVID_LIGHT
};

// clang-format off
#define PF_TransferMode_ZERO_SRC_ALPHA_CLEARS_DST_ALPHA(TMODE) \
    ((TMODE) == PF_Xfer_MULTIPLY_ALPHA ||        \
     (TMODE) == PF_Xfer_MULTIPLY_ALPHA_LUMA)
// clang-format on


// PF_TransferMode_ZERO_ALPHA_NOP is deprecated because it was
// confusing -- you probably want PF_TransferMode_ZERO_SRC_ALPHA_CLEARS_DST_ALPHA
// instead

// WARNING: this macro is incorrect for PF_Xfer_COPY (returns true), but it's been like this for so long
//            that we are leaving it unchanged so as not to create bugs by changing it.
// clang-format off
#define PF_TransferMode_ZERO_SRC_ALPHA_LEAVES_DST_UNCHANGED(TMODE) \
    (((TMODE) == PF_Xfer_MULTIPLY_ALPHA ||        \
     (TMODE) == PF_Xfer_MULTIPLY_ALPHA_LUMA ||    \
     (TMODE) == PF_Xfer_ADDITIVE_PREMUL) == 0)
// clang-format on

#endif // _H_AE_BLENDMODES