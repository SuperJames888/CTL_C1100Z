/*
<:label-BRCM:2012:DUAL/GPL:standard

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

#ifndef BCMPCI_H
#define BCMPCI_H

/* BUS assignment */
#define BCM_BUS_PCI						0    /* bus 0, MPI. USB, etc */
#define BCM_BUS_PCIE_ROOT					1    /* bus 1, pcie root complex */
#define BCM_BUS_PCIE_DEVICE					2    /* bus 2, pcie devices */
/* 
 * For integrated onchip WLAN support
 */
#define WLAN_ONCHIP_DEV_SLOT					0      /* predefined pci slot number to sit in */
#define WLAN_ONCHIP_DEV_NUM					1      /* predefined instance of onchip wlan */
#define WLAN_ONCHIP_PCI_ID					0x435f14e4
#define WLAN_ONCHIP_RESOURCE_SIZE				0x2000
#define WLAN_ONCHIP_PCI_HDR_DW_LEN				64
#endif


#if defined (__BCM_MAP_PART_H)
/* chip specific is defined in PCIEH_MEMX_XXXX in bcm_map_part.h */
#if defined(PCIEH)
#ifdef PCIEH_MEM1_BASE
#ifdef PCIEH_PCIE_IS_DEFAULT_TARGET
/* for those are ubus default targets and can use a bigger MEM1 */
#define BCM_PCIE_MEM1_BASE					PCIEH_MEM1_BASE 
#define BCM_PCIE_MEM1_SIZE					PCIEH_MEM1_SIZE
#else
#define BCM_PCIE_MEM1_BASE					PCIEH_MEM2_BASE 
#define BCM_PCIE_MEM1_SIZE					PCIEH_MEM2_SIZE
#endif
#else	
#error "PCIEH_MEM1_BASE/PCIEH_MEM1_SIZE not defined in xxxx_map_part.h"
#endif

#ifdef PCIEH_MEM2_BASE
#ifdef PCIEH_PCIE_IS_DEFAULT_TARGET
#define BCM_PCIE_MEM2_BASE					PCIEH_MEM2_BASE 
#define BCM_PCIE_MEM2_SIZE					PCIEH_MEM2_SIZE
#else
#define BCM_PCIE_MEM2_BASE					PCIEH_MEM1_BASE 
#define BCM_PCIE_MEM2_SIZE					PCIEH_MEM1_SIZE
#endif
#else	
#error "PCIEH_MEM2_BASE/PCIEH_MEM2_SIZE not defined in xxxx_map_part.h"
#endif
#endif /* PCIEH */

/* PCI memory window in physical address space */
/* Not a true PCI memory allocated by the chip
   assume PCIE MEM2 is not used,
   stealing unused PCIE MEM2 space to get PCI scanning going
   all devices hanging on pci bus 0 must fix-up their base address accordingly   
*/
#if defined(MPI_BASE)
#define BCM_PCI_MEM_BASE						0x11000000
#define BCM_PCI_MEM_SIZE						0x01000000
#else
#define BCM_PCI_MEM_BASE						BCM_PCIE_MEM2_BASE
#define BCM_PCI_MEM_SIZE						0x00100000
#endif

/* Card bus memory window in physical address space */ 
#define BCM_CB_MEM_BASE    (BCM_PCI_MEM_BASE + BCM_PCI_MEM_SIZE)
#define BCM_CB_MEM_SIZE    0x01000000

/* IO window in physical address space */ 
#define BCM_PCI_IO_BASE    (BCM_CB_MEM_BASE + BCM_CB_MEM_SIZE)
#define BCM_PCI_IO_SIZE    0x00010000

#define BCM_PCI_ADDR_MASK       0x1fffffff

/* PCI Configuration and I/O space acesss */
#define BCM_PCI_CFG(d, f, o)    ( (d << 11) | (f << 8) | (o/4 << 2) )

/* fake USB PCI slot */
#define USB_HOST_SLOT           9
#define USB20_HOST_SLOT         10
#define USB_BAR0_MEM_SIZE       0x0100

#define BCM_HOST_MEM_SPACE1     0x10000000
#define BCM_HOST_MEM_SPACE2     0x00000000

/* 
 * EBI bus clock is 33MHz and share with PCI bus
 * each clock cycle is 30ns.
 */
/* attribute memory access wait cnt for 4306 */
#define PCMCIA_ATTR_CE_HOLD     3  // data hold time 70ns
#define PCMCIA_ATTR_CE_SETUP    3  // data setup time 50ns
#define PCMCIA_ATTR_INACTIVE    6  // time between read/write cycles 180ns. For the total cycle time 600ns (cnt1+cnt2+cnt3+cnt4)
#define PCMCIA_ATTR_ACTIVE      10 // OE/WE pulse width 300ns

/* common memory access wait cnt for 4306 */
#define PCMCIA_MEM_CE_HOLD      1  // data hold time 30ns
#define PCMCIA_MEM_CE_SETUP     1  // data setup time 30ns
#define PCMCIA_MEM_INACTIVE     2  // time between read/write cycles 40ns. For the total cycle time 250ns (cnt1+cnt2+cnt3+cnt4)
#define PCMCIA_MEM_ACTIVE       5  // OE/WE pulse width 150ns

#define PCCARD_VCC_MASK     0x00070000  // Mask Reset also
#define PCCARD_VCC_33V      0x00010000
#define PCCARD_VCC_50V      0x00020000

typedef enum {
    MPI_CARDTYPE_NONE,      // No Card in slot
    MPI_CARDTYPE_PCMCIA,    // 16-bit PCMCIA card in slot    
    MPI_CARDTYPE_CARDBUS,   // 32-bit CardBus card in slot
}   CardType;

#define CARDBUS_SLOT        0    // Slot 0 is default for CardBus

#define pcmciaAttrOffset    0x00200000
#define pcmciaMemOffset     0x00000000
// Needs to be right above PCI I/O space. Give 0x8000 (32K) to PCMCIA. 
#define pcmciaIoOffset      (BCM_PCI_IO_BASE + 0x80000)
// Base Address is that mapped into the MPI ChipSelect registers. 
// UBUS bridge MemoryWindow 0 outputs a 0x00 for the base.
#define pcmciaBase          0xbf000000
#define pcmciaAttr          (pcmciaAttrOffset | pcmciaBase)
#define pcmciaMem           (pcmciaMemOffset  | pcmciaBase)
#define pcmciaIo            (pcmciaIoOffset   | pcmciaBase)
#endif

