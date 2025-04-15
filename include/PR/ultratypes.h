#ifndef _ULTRATYPES_H_
#define _ULTRATYPES_H_

#include <psptypes.h>
#include <stddef.h> // for size_t
#include <stdint.h>

/**************************************************************************
 *                                                                        *
 *               Copyright (C) 1995, Silicon Graphics, Inc.               *
 *                                                                        *
 *  These coded instructions, statements, and computer programs  contain  *
 *  unpublished  proprietary  information of Silicon Graphics, Inc., and  *
 *  are protected by Federal copyright law.  They  may  not be disclosed  *
 *  to  third  parties  or copied or duplicated in any form, in whole or  *
 *  in part, without the prior written consent of Silicon Graphics, Inc.  *
 *                                                                        *
 **************************************************************************/

/*************************************************************************
 *
 *  File: ultratypes.h
 *
 *  This file contains various types used in Ultra64 interfaces.
 *
 *  $Revision: 1.6 $
 *  $Date: 1997/12/17 04:02:06 $
 *  $Source: /hosts/gate3/exdisk2/cvs/N64OS/Master/cvsmdev2/PR/include/ultratypes.h,v $
 *
 **************************************************************************/

/**********************************************************************
 * General data types for R4300
 */
#if defined(_LANGUAGE_C) || defined(_LANGUAGE_C_PLUS_PLUS)

typedef unsigned char           u8;  /* unsigned  8-bit */
typedef unsigned short int      u16; /* unsigned 16-bit */

#ifndef u32
#define u32 uint32_t
#endif

typedef unsigned long long int  u64; /* unsigned 64-bit */

typedef signed char             s8;  /* signed  8-bit */
typedef signed short int        s16; /* signed 16-bit */

#ifndef s32
#define s32 int32_t
#endif

#ifndef vu32
#define vu32 volatile uint32_t
#endif

typedef volatile u8             vu8;	/* volatile unsigned  8-bit */
typedef volatile u16            vu16;	/* volatile unsigned 16-bit */

#ifndef vs32
#define vs32 volatile int32_t
#endif

typedef volatile s8             vs8;	/* volatile signed  8-bit */
typedef volatile s16            vs16;	/* volatile signed 16-bit */
typedef volatile u64            vu64;	/* volatile unsigned 64-bit */
typedef volatile s64            vs64;	/* volatile signed 64-bit */

typedef float                   f32;	/* single prec floating point */
typedef double                  f64;	/* double prec floating point */

#endif  /* _LANGUAGE_C */

/*************************************************************************
 * Common definitions
 */
#ifndef TRUE
#define TRUE    1
#endif

#ifndef FALSE
#define FALSE   0
#endif

#ifndef NULL
#define NULL    0
#endif

#endif  /* _ULTRATYPES_H_ */
