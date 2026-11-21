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

#include "AEGP_SuiteHandler.h"

#ifndef DVA_STATIC_BUILD
void AEGP_SuiteHandler::MissingSuiteError() const
{
    //  Yes, we've read Scott Meyers, and know throwing
    //  a stack-based object can cause problems. Since
    //  the err is just a long, and since we aren't de-
    //  referencing it in any way, risk is mimimal.

    //  As always, we expect those of you who use
    //  exception-based code to do a little less rudi-
    //  mentary job of it than we are here.

    //  Also, excuse the Madagascar-inspired monkey
    //  joke; couldn't resist.
    //                              -bbb 10/10/05

    PF_Err poop = PF_Err_BAD_CALLBACK_PARAM;

    throw poop;
}
#endif
