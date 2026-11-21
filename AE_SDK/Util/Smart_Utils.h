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

#include "AE_Effect.h"
#include "SPTypes.h"

#ifndef mmin
    #define mmin(a, b) ((a) < (b) ? (a) : (b))
    #define mmax(a, b) ((a) > (b) ? (a) : (b))
#endif

PF_Boolean IsEmptyRect(const PF_LRect* r);

void UnionLRect(const PF_LRect* src, PF_LRect* dst);

PF_Boolean IsEdgePixel(PF_LRect* rectP, A_long x, A_long y);