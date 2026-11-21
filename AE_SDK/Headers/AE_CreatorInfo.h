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

/*  If you'd like After Effects to open files created by your application
    IN your application, embed this structure in that file.
    
    Data must be in Motorola byte order.
    
    Your application must then respond to the appleEvent (mac) or the
    command line flags (win) that are specified in the structure.
*/

#define ADBE_CREATOR_ATOM_TYPE 'Cr8r'
#define CR8R_MAGIC 0xBEEFCAFE

#define ADBE_CREATOR_ATOM_VERS_MAJOR 1
#define ADBE_CREATOR_ATOM_VERS_MINOR 0

typedef struct adbe_creator_atom
{
    unsigned long magicLu; // set to CR8R_MAGIC

    long atom_sizeL;        // size of this structure (sizeof(CR8R_CreatorAtom))
    short atom_vers_majorS; // set to ADBE_CREATOR_ATOM_VERS_MAJOR
    short atom_vers_minorS; // set to ADBE_CREATOR_ATOM_VERS_MINOR

    // mac
    unsigned long creator_codeLu;  // application code on MacOS
    unsigned long creator_eventLu; // invocation appleEvent

    // windows
    char creator_extAC[16];  // extension allowing registry search to app
    char creator_flagAC[16]; // flag passed to app at invocation time

    char creator_nameAC[32]; // name of the creator application
} CR8R_CreatorAtom, **CR8R_CreatorAtomHandle;
