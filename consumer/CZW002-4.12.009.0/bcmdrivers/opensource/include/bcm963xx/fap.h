#ifndef __FAP_H_INCLUDED__
#define __FAP_H_INCLUDED__

/*
   Copyright (c) 2007-2012 Broadcom Corporation
   All Rights Reserved

<:label-BRCM:2007:DUAL/GPL:standard

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2, as published by
the Free Software Foundation (the "GPL").

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.


A copy of the GPL is available at http://www.broadcom.com/licenses/GPLv2.php, or by
writing to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.

:>
*/

/*
 *******************************************************************************
 * File Name  : fap.h
 *
 * Description: This file contains the specification of some common definitions
 *      and interfaces to other modules. This file may be included by both
 *      Kernel and userapp (C only).
 *
 *******************************************************************************
 */

#include <pktHdr.h>
#include <bcmHwAcc.h>

/*----- Defines -----*/

#define FAP_VERSION              "0.1"
#define FAP_VER_STR              "v" FAP_VERSION " " __DATE__ " " __TIME__
#define FAP_MODNAME              "Broadcom Forwarding Assist Processor (FAP)"

#define FAP_NAME                 "bcmfap"

#ifndef FAP_ERROR
#define FAP_ERROR                (-1)
#endif
#ifndef FAP_SUCCESS
#define FAP_SUCCESS              0
#endif

/* FAP Character Device */
#define FAPDRV_MAJOR             241
#define FAPDRV_NAME              FAP_NAME
#define FAPDRV_DEVICE_NAME       "/dev/" FAPDRV_NAME

/* FAP Control Utility Executable */
#define FAP_CTL_UTILITY_PATH     "/bin/fapctl"

/* FAP Proc FS Directory Path */
#define FAP_PROC_FS_DIR_PATH     FAP_NAME

/* Menuconfig: BRCM_DRIVER_PKTFLOW_DEBUG selection will cause -DPKTDBG C Flags*/
#ifdef PKTDBG
#define CC_FAP_DEBUG
#define CC_FAP_ASSERT
#endif

#if defined( __KERNEL__ )
#include <asm/system.h>             /* interrupt locking for MIPS */
#define KERNEL_LOCK(level)          local_irq_save(level)
#define KERNEL_UNLOCK(level)        local_irq_restore(level)
#endif

#define FAP_DONT_CARE        ~0
#define FAP_IS_DONT_CARE(_x) ( ((_x) == (typeof(_x))(FAP_DONT_CARE)) )

/*
 *------------------------------------------------------------------------------
 * Common defines for FAP layers.
 *------------------------------------------------------------------------------
 */
#undef FAP_DECL
#define FAP_DECL(x)                 x,  /* for enum declaration in H file */

/*
 *------------------------------------------------------------------------------
 *              Packet CFM character device driver IOCTL enums
 * A character device and the associated userspace utility for design debug.
 * Include fapParser.h for ACTIVATE/DEACTIVATE IOCTLs
 *------------------------------------------------------------------------------
 */
typedef enum {
    FAP_DECL(FAP_IOC_HW)
    FAP_DECL(FAP_IOC_STATUS)
    FAP_DECL(FAP_IOC_INIT)
    FAP_DECL(FAP_IOC_ENABLE)
    FAP_DECL(FAP_IOC_DISABLE)
    FAP_DECL(FAP_IOC_DEBUG)
    FAP_DECL(FAP_IOC_PRINT)
    FAP_DECL(FAP_IOC_CPU)
    FAP_DECL(FAP_IOC_DMA_DEBUG)
    FAP_DECL(FAP_IOC_MEM_DEBUG)
    FAP_DECL(FAP_IOC_MTU)
    FAP_DECL(FAP_IOC_TM)
    FAP_DECL(FAP_IOC_PERF)
    FAP_DECL(FAP_IOC_DM_DEBUG)
    FAP_DECL(FAP_IOC_FLOODING_MASK)
    FAP_DECL(FAP_IOC_ARL_FLUSH)
    FAP_DECL(FAP_IOC_ARL_SHOW)
    FAP_DECL(FAP_IOC_DO_4KE_TEST)
    FAP_DECL(FAP_IOC_MAX)
} fapIoctl_t;

typedef enum {
    FAP_IOCTL_TM_CMD_MASTER_CONFIG=0,
    FAP_IOCTL_TM_CMD_PORT_CONFIG,
    FAP_IOCTL_TM_CMD_GET_PORT_CONFIG,
    FAP_IOCTL_TM_CMD_PORT_MODE,
    FAP_IOCTL_TM_CMD_MODE_RESET,
    FAP_IOCTL_TM_CMD_PORT_TYPE,
    FAP_IOCTL_TM_CMD_PORT_ENABLE,
    FAP_IOCTL_TM_CMD_PORT_APPLY,
    FAP_IOCTL_TM_CMD_QUEUE_CONFIG,
    FAP_IOCTL_TM_CMD_QUEUE_WEIGHT,
    FAP_IOCTL_TM_CMD_ARBITER_CONFIG,
    FAP_IOCTL_TM_MAP_TMQUEUE_TO_SWQUEUE,
    FAP_IOCTL_TM_CMD_STATUS,
    FAP_IOCTL_TM_CMD_STATS,
    FAP_IOCTL_TM_CMD_DUMP_MAPS,
    FAP_IOCTL_TM_CMD_MAX
} fapIoctl_tmCmd_t;

/* This MUST be kept in sync with fapTm_mode_t */
typedef enum {
    FAP_IOCTL_TM_MODE_AUTO=0,
    FAP_IOCTL_TM_MODE_MANUAL,
    FAP_IOCTL_TM_MODE_MAX
} fapIoctl_tmMode_t;

/* This MUST be kept in sync with fapTm_portType_t */
typedef enum {
    FAP_IOCTL_TM_PORT_TYPE_LAN=0,
    FAP_IOCTL_TM_PORT_TYPE_WAN,
    FAP_IOCTL_TM_PORT_TYPE_MAX
} fapIoctl_tmPortType_t;

/* This MUST be kept in sync with fap4keTm_shaperType_t */
typedef enum {
    FAP_IOCTL_TM_SHAPER_TYPE_MIN=0,
    FAP_IOCTL_TM_SHAPER_TYPE_MAX,
    FAP_IOCTL_TM_SHAPER_TYPE_TOTAL
} fapIoctl_tmShaperType_t;

/* This MUST be kept in sync with fap4keTm_arbiterType_t */
typedef enum {
    FAP_IOCTL_TM_ARBITER_TYPE_SP=0,
    FAP_IOCTL_TM_ARBITER_TYPE_WRR,
    FAP_IOCTL_TM_ARBITER_TYPE_SP_WRR,
    FAP_IOCTL_TM_ARBITER_TYPE_WFQ,
    FAP_IOCTL_TM_ARBITER_TYPE_TOTAL
} fapIoctl_tmArbiterType_t;

/* This MUST be kept in sync with fapTm_shapingType_t */
typedef enum {
    FAP_IOCTL_TM_SHAPING_TYPE_DISABLED=0,
    FAP_IOCTL_TM_SHAPING_TYPE_RATE,
    FAP_IOCTL_TM_SHAPING_TYPE_RATIO,
    FAP_IOCTL_TM_SHAPING_TYPE_MAX
} fapIoctl_tmShapingType_t;

typedef struct {
    fapIoctl_tmCmd_t cmd;
    int enable;
    int port;
    fapIoctl_tmMode_t mode;
    int queue;
    int swQueue;
    fapIoctl_tmShaperType_t shaperType;
    int kbps;
    int mbs;
    int weight;
    fapIoctl_tmArbiterType_t arbiterType;
    int arbiterArg;
    fapIoctl_tmPortType_t portType;
    fapIoctl_tmShapingType_t shapingType;
} fapIoctl_tm_t;

typedef enum {
    FAP_IOCTL_PERF_CMD_ENABLE,
    FAP_IOCTL_PERF_CMD_DISABLE,
    FAP_IOCTL_PERF_CMD_CONFIG_RX,
    FAP_IOCTL_PERF_CMD_CONFIG_TX,
    FAP_IOCTL_PERF_CMD_GET_RESULTS,
    FAP_IOCTL_PERF_CMD_MAX
} fapIoctl_perfCmd_t;

typedef struct {
    uint32_t packets;
    uint32_t bytes;
} fapIoctl_perfRxResults_t;

typedef struct {
    uint32_t dropped;
} fapIoctl_perfTxResults_t;

typedef struct {
    uint8_t running;
    fapIoctl_perfRxResults_t rx;
    fapIoctl_perfTxResults_t tx;
} fapIoctl_perfResults_t;

typedef struct {
    fapIoctl_perfCmd_t cmd;
    uint32_t ipSa;
    uint32_t ipDa;
    uint16_t sPort;  /* UDP source port */
    uint16_t dPort;  /* UDP dest port */
    fapIoctl_perfResults_t results;
} fapIoctl_perf_t;

//#define CC_FAP_ENET_STATS

#if defined(CC_FAP_ENET_STATS)
void fapEnetStats_contextFull(void);
void fapEnetStats_dqmRxFull(void);
void fapEnetStats_rxPackets(void);
void fapEnetStats_txPackets(uint32_t contextCount);
void fapEnetStats_interrupts(void);
void fapEnetStats_dump(void);
#else
#define fapEnetStats_contextFull()
#define fapEnetStats_dqmRxFull()
#define fapEnetStats_rxPackets()
#define fapEnetStats_txPackets(_contextCount)
#define fapEnetStats_interrupts()
#define fapEnetStats_dump()
#endif


//#define CC_FAP_EVENTS

#if defined(CC_FAP_EVENTS)
#undef FAP_DECL
#define FAP_DECL(x) #x,

#define FAP_EVENT_TYPE_NAME                     \
    {                                           \
        FAP_DECL(RX_BEGIN)            \
        FAP_DECL(RX_END)          \
        FAP_DECL(TX_SCHED)    \
        FAP_DECL(TX_BEGIN)    \
        FAP_DECL(TX_END)      \
    }

typedef enum {
    FAP_EVENT_RX_BEGIN,
    FAP_EVENT_RX_END,
    FAP_EVENT_TX_SCHED,
    FAP_EVENT_TX_BEGIN,
    FAP_EVENT_TX_END,
    FAP_EVENT_MAX
} fapEvent_type_t;

void fapEvent_record(fapEvent_type_t type, uint32_t arg);
void fapEvent_print(void);
uint32_t fapEnet_txQueueUsage(uint32 fapIdx);
#else
#define fapEvent_record(_type, _arg)
#define fapEvent_print()
#define fapEnet_txQueueUsage() 0
#endif

#if (defined(CONFIG_BCM_ARL) || defined(CONFIG_BCM_ARL_MODULE))
/*
 *------------------------------------------------------------------------------
 *  Invoked by ARL Protocol layer to clear HW association.
 *  Based on the scope of the request:
 *------------------------------------------------------------------------------
 */

typedef int ( *FAP_CLEAR_HOOK)(uint32_t mcast, uint32_t port);

/*
 *------------------------------------------------------------------------------
 * Flow cache binding to ARL to register ARL upcalls and downcalls
 * Upcalls from FAP to ARL: activate, deactivate and refresh functions.
 * Downcalls from ARL to FAP: clear hardware associations function.
 *------------------------------------------------------------------------------
 */
extern void fap_bind_arl(HOOKP activate_fn, HOOK4PARM deactivate_fn,
                        HOOK3PARM refresh_fn, HOOK16 reset_stats_fn, 
                        HOOK16 clear_fn, FAP_CLEAR_HOOK *fap_clear_fn);
#endif

#endif  /* defined(__FAP_H_INCLUDED__) */
