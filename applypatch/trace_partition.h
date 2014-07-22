/******************************************************************************/
/*                                                               Date:08/2012 */
/*                             PRESENTATION                                   */
/*                                                                            */
/*      Copyright 2012 TCL Communication Technology Holdings Limited.         */
/*                                                                            */
/* This material is company confidential, cannot be reproduced in any form    */
/* without the written permission of TCL Communication Technology Holdings    */
/* Limited.                                                                   */
/*                                                                            */
/* -------------------------------------------------------------------------- */
/* Author:  He Zhang                                                          */
/* E-Mail:  He.Zhang@tcl-mobile.com                                           */
/* Role  :  GLUE                                                              */
/* Reference documents :  05_[ADR-09-001]Framework Dev Specification.pdf      */
/* -------------------------------------------------------------------------- */
/* Comments:                                                                  */
/* File    : vender/tct/source/system/tct_diag/trace_partition.h              */
/* Labels  :                                                                  */
/* -------------------------------------------------------------------------- */
/* ========================================================================== */
/* Modifications on Features list / Changes Request / Problems Report         */
/* -------------------------------------------------------------------------- */
/* date    | author         | key                | comment (what, where, why) */
/* --------|----------------|--------------------|--------------------------- */
/* 12/09/21| He.Zhang       | FR-334604          | Adjust to fit new role     */
/* 12/09/21| Guobing.Miao   | FR-334604          | Create for nv backup       */
/* 12/12/09| Qianbo.Pan     | PR-370120          | adb device ID              */
/*---------|----------------|--------------------|--------------------------- */
/******************************************************************************/

#ifndef __TRACE_PARTITION_H__
#define __TRACE_PARTITION_H__

#define RETROFIT_MAGIC1 0xFACB10C1L
#define RETROFIT_MAGIC2 0xFACB10C2L

#define RETROFIT_DONE   0xa8
/*[BUGFIX]-Mod-BEGIN by TCTNB.(Qianbo Pan), 2012/12/03, for PR370120 adb device ID*/
#define MMCBLKP     "/dev/block/platform/msm_sdcc.1/by-name/traceability"
/*[BUGFIX]-Mod-END by TCTNB.(Qianbo Pan)*/
typedef struct {
    unsigned long magic1;
    unsigned long magic2;
    unsigned long total_length_bytes;
    unsigned long checksum;
    unsigned long nb_missing_items;
#if 0 //def NV_H
    e_rtrf_status status;
#else
    unsigned char status; /* e_rtrf_status is uint8 enum */
#endif
    unsigned char name[10];   /* ajayet 9/03/10 rollback to old size */
    /* total header size = 32bytes */
} rtrf_header;

#endif /* __TRACE_PARTITION_H__ */
