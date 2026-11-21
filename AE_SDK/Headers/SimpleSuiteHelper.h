/*******************************************************************/
/*                                                                 */
/* Copyright 2025 Adobe                                            */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  Adobe permits you to use, modify, and distribute this  */
/* file in accordance with the terms of the Adobe license          */
/* agreement accompanying it.                                      */
/*                                                                 */
/*******************************************************************/

#ifndef _H_SIMPLESUITEHELPER
#define _H_SIMPLESUITEHELPER

#include "A.h"
#include "SPBasic.h"
#include <cassert>

// Simple non-template suite helper class
template <typename SuiteType>
class SimpleSuiteHelper
{
public:
    SimpleSuiteHelper(const SPBasicSuite* basic_suiteP, const A_char* suite_name, int32_t suite_version);
    ~SimpleSuiteHelper();

    const SuiteType* operator->() const
    {
        return i_SuiteP;
    }
    SuiteType* get() const
    {
        return i_SuiteP;
    }

private:
    mutable SuiteType* i_SuiteP;
    const SPBasicSuite* const i_basic_suiteP;
    const A_char* i_suite_name;
    int32_t i_suite_version;
};

template <typename SuiteType>
SimpleSuiteHelper<SuiteType>::SimpleSuiteHelper(
    const SPBasicSuite* basic_suiteP, const A_char* suite_name, int32_t suite_version)
    : i_basic_suiteP(basic_suiteP),
      i_SuiteP(NULL),
      i_suite_name(suite_name),
      i_suite_version(suite_version)
{
    assert(basic_suiteP);
    assert(suite_name);

    const void* acquired_suite = NULL;
    A_Err err = i_basic_suiteP->AcquireSuite(i_suite_name, i_suite_version, &acquired_suite);
    if (err || !acquired_suite)
    {
        // Could throw or assert here, but for now just leave i_SuiteP as NULL
        assert(false);
    }
    else
    {
        i_SuiteP = reinterpret_cast<SuiteType*>(const_cast<void*>(acquired_suite));
    }
}

template <typename SuiteType>
SimpleSuiteHelper<SuiteType>::~SimpleSuiteHelper()
{
    if (i_SuiteP)
    {
#ifdef DEBUG
        A_Err err =
#endif
            i_basic_suiteP->ReleaseSuite(i_suite_name, i_suite_version);

#ifdef DEBUG
        assert(!err);
#endif
    }
}

#endif // _H_SIMPLESUITEHELPER
