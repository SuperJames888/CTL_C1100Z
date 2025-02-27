/*
<:label-BRCM:2011:DUAL/GPL:standard

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
/**************************************************************************
 * File Name  : bcmxtmrtbond.h
 *
 * Description: This file contains constant definitions and structure
 *              definitions for the BCM63268 ATM/PTM network device driver.
 ***************************************************************************/

#if !defined(_BCMXTMRTBOND_H)
#define _BCMXTMRTBOND_H

#include "bcmxtmcfg.h"

#define PTM_FLOW_PRI_LOW                     0
#define PTM_FLOW_PRI_HIGH                    1

#define ETH_FCS_LEN                          4
#define XTMRT_PTM_CRC_SIZE                   2

#define XTMRT_PTM_BOND_FRAG_HDR_SIZE         2
#define XTMRT_PTM_BOND_FRAG_HDR_EOP          1
#define XTMRT_PTM_BOND_HDR_NON_EOP           0

#define XTMRT_PTM_BOND_FRAG_HDR_EOP_MASK     0x1000   /* bit 12 */
#define XTMRT_PTM_BOND_PORTSEL_SHIFT         15       /* bit 15 */

/* ATM Bonding Definitions */

#define XTMRT_ATM_BOND_ASM_VPI               0x0
#define XTMRT_ATM_BOND_ASM_VCI               0x14

/* PTM Tx Bonding Definitions */

#if 1
#define XTMRT_PTM_BOND_MAX_FRAG_PER_PKT      8
#else
/* This is only to test 63268 D0 additional capability, which is not used currently. */
#define XTMRT_PTM_BOND_MAX_FRAG_PER_PKT      24
#endif

#define XTMRT_PTM_BOND_TX_MAX_FRAGMENT_SIZE  508
#define XTMRT_PTM_BOND_TX_MIN_FRAGMENT_SIZE  96

#define XTMRT_PTM_BOND_TX_DEF_CREDIT         XTMRT_PTM_BOND_TX_MAX_FRAGMENT_SIZE+XTMRT_PTM_BOND_FRAG_HDR_SIZE+XTMRT_PTM_CRC_SIZE

typedef union _XtmRtPtmTxBondHeader {
   struct _sVal {
      uint16 portSel    : 1 ;
      uint16 Reserved   : 2 ;
      uint16 PktEop     : 1 ;
      uint16 FragSize   : 12 ;  /* Includes size of the fragment + 2bytes of frag hdr + 2bytes of CRC-16 */
   } sVal ;
   uint16  usVal ;
} XtmRtPtmTxBondHeader ;

#define MAX_WT_PORT_DIST      2

//#define BONDING_DEBUG        1

typedef struct _XtmRtPtmBondInfo {
#ifdef BONDING_DEBUG
   uint32   printEnable ;
#endif
   uint16   bonding;      /* 0=non-bonding, 1=bonding */
   uint16   portMask;
   uint16   totalWtPortDist ;
   uint16   ulCurrWtPortDistRunIndex ;
   uint16   u16ConfWtPortDist [MAX_WT_PORT_DIST] ;
   uint32   ulLinkUsWt [MAX_BOND_PORTS] ;
   int32    ilLinkUsCredit [MAX_BOND_PORTS] ;
   uint32   ulConfLinkUsWt [MAX_BOND_PORTS] ;
   int32    ilConfLinkUsCredit [MAX_BOND_PORTS] ;
} XtmRtPtmBondInfo;

/* Function Prototypes */
#ifdef FAP_4KE
int bcmxtmrt_ptmbond_addHdr_4ke(uint8 **packetTx_pp, uint16 *len_p, uint32 ptmPrioIdx, uint32 isNetPacket);
#else
int bcmxtmrt_ptmbond_calculate_link_weights(uint32 *pulLinkUSRates, uint32 portMask, UINT32 updateOnly); 
#endif   /* FAP_4KE */

#endif /* _BCMXTMRTBOND_H */
