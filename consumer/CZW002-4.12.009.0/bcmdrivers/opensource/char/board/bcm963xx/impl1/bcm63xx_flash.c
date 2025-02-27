
/*
<:label-BRCM:2016:DUAL/GPL:standard 

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
 ***************************************************************************
 * File Name  : bcm63xx_flash.c
 *
 * Description: This file contains the flash device driver APIs for bcm63xx board. 
 *
 * Created on :  8/10/2002  seanl:  use cfiflash.c, cfliflash.h (AMD specific)
 *
 ***************************************************************************/


/* Includes. */
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/capability.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/preempt.h>
#include <asm/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/mtd/mtd.h>
#include <linux/kernel.h>
#include <linux/fcntl.h>
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/unistd.h>
#include <linux/jffs2.h>
#include <linux/mount.h>
#include <linux/crc32.h>
#include <linux/sched.h>
#include <linux/bcm_assert_locks.h>
#include <asm/uaccess.h>
#include <asm/delay.h>

#include <bcm_map_part.h>
#include <board.h>
#include <bcmTag.h>
#include "flash_api.h"
#include "boardparms.h"
#include "boardparms_voice.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
#include <linux/fs_struct.h>
#endif
//#define DEBUG_FLASH

extern int kerSysGetSequenceNumber(int);
extern PFILE_TAG kerSysUpdateTagSequenceNumber(int);

/*
 * inMemNvramData an in-memory copy of the nvram data that is in the flash.
 * This in-memory copy is used by NAND.  It is also used by NOR flash code
 * because it does not require a mutex or calls to memory allocation functions
 * which may sleep.  It is kept in sync with the flash copy by
 * updateInMemNvramData.
 */
static unsigned char *inMemNvramData_buf;
static NVRAM_DATA inMemNvramData;
static DEFINE_SPINLOCK(inMemNvramData_spinlock);
static void updateInMemNvramData(const unsigned char *data, int len, int offset);
#define UNINITIALIZED_FLASH_DATA_CHAR  0xff
static FLASH_ADDR_INFO fInfo;
static struct semaphore semflash;

// mutex is preferred over semaphore to provide simple mutual exclusion
// spMutex protects scratch pad writes
static DEFINE_MUTEX(spMutex);
extern struct mutex flashImageMutex;
static char bootCfeVersion[CFE_VERSION_MARK_SIZE+CFE_VERSION_SIZE];
static int bootFromNand = 0;

static int setScratchPad(char *buf, int len);
static char *getScratchPad(int len);
#if 1 //__MSTC__, JhenYang: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
#else
static int nandNvramSet(const char *nvramString );
#endif
#define ALLOC_TYPE_KMALLOC   0
#define ALLOC_TYPE_VMALLOC   1

static void *retriedKmalloc(size_t size)
{
    void *pBuf;
    unsigned char *bufp8 ;

    size += 4 ; /* 4 bytes are used to store the housekeeping information used for freeing */

    // Memory allocation changed from kmalloc() to vmalloc() as the latter is not susceptible to memory fragmentation under low memory conditions
    // We have modified Linux VM to search all pages by default, it is no longer necessary to retry here
    if (!in_interrupt() ) {
        pBuf = vmalloc(size);
        if (pBuf) {
            memset(pBuf, 0, size);
            bufp8 = (unsigned char *) pBuf ;
            *bufp8 = ALLOC_TYPE_VMALLOC ;
            pBuf = bufp8 + 4 ;
        }
    }
    else { // kmalloc is still needed if in interrupt
        printk("retriedKmalloc: someone calling from intrrupt context?!");
        BUG();
        pBuf = kmalloc(size, GFP_ATOMIC);
        if (pBuf) {
            memset(pBuf, 0, size);
            bufp8 = (unsigned char *) pBuf ;
            *bufp8 = ALLOC_TYPE_KMALLOC ;
            pBuf = bufp8 + 4 ;
        }
    }

    return pBuf;
}

static void retriedKfree(void *pBuf)
{
    unsigned char *bufp8  = (unsigned char *) pBuf ;
    bufp8 -= 4 ;

    if (*bufp8 == ALLOC_TYPE_KMALLOC)
        kfree(bufp8);
    else
        vfree(bufp8);
}

// get shared blks into *** pTempBuf *** which has to be released bye the caller!
// return: if pTempBuf != NULL, poits to the data with the dataSize of the buffer
// !NULL -- ok
// NULL  -- fail
static char *getSharedBlks(int start_blk, int num_blks)
{
    int i = 0;
    int usedBlkSize = 0;
    int sect_size = 0;
    char *pTempBuf = NULL;
    char *pBuf = NULL;

    down(&semflash);

    for (i = start_blk; i < (start_blk + num_blks); i++)
        usedBlkSize += flash_get_sector_size((unsigned short) i);

    if ((pTempBuf = (char *) retriedKmalloc(usedBlkSize)) == NULL)
    {
        printk("failed to allocate memory with size: %d\n", usedBlkSize);
        up(&semflash);
        return pTempBuf;
    }
    
    pBuf = pTempBuf;
    for (i = start_blk; i < (start_blk + num_blks); i++)
    {
        sect_size = flash_get_sector_size((unsigned short) i);

#if defined(DEBUG_FLASH)
        printk("getSharedBlks: blk=%d, sect_size=%d\n", i, sect_size);
#endif
        flash_read_buf((unsigned short)i, 0, pBuf, sect_size);
        pBuf += sect_size;
    }
    up(&semflash);
    
    return pTempBuf;
}

// Set the pTempBuf to flash from start_blk for num_blks
// return:
// 0 -- ok
// -1 -- fail
static int setSharedBlks(int start_blk, int num_blks, char *pTempBuf)
{
    int i = 0;
    int sect_size = 0;
    int sts = 0;
    char *pBuf = pTempBuf;

    down(&semflash);

    for (i = start_blk; i < (start_blk + num_blks); i++)
    {
        sect_size = flash_get_sector_size((unsigned short) i);
        flash_sector_erase_int(i);

        if (flash_write_buf(i, 0, pBuf, sect_size) != sect_size)
        {
            printk("Error writing flash sector %d.", i);
            sts = -1;
            break;
        }

#if defined(DEBUG_FLASH)
        printk("setShareBlks: blk=%d, sect_size=%d\n", i, sect_size);
#endif

        pBuf += sect_size;
    }

    up(&semflash);

    return sts;
}

#if !defined(CONFIG_BRCM_IKOS)
// Initialize the flash and fill out the fInfo structure
void kerSysEarlyFlashInit( void )
{
#ifdef CONFIG_BCM_ASSERTS
    // ASSERTS and bug() may be too unfriendly this early in the bootup
    // sequence, so just check manually
    if (sizeof(NVRAM_DATA) != NVRAM_LENGTH)
        printk("kerSysEarlyFlashInit: nvram size mismatch! "
               "NVRAM_LENGTH=%d sizeof(NVRAM_DATA)=%d\n",
               NVRAM_LENGTH, sizeof(NVRAM_DATA));
#endif
    inMemNvramData_buf = (unsigned char *) &inMemNvramData;
    memset(inMemNvramData_buf, UNINITIALIZED_FLASH_DATA_CHAR, NVRAM_LENGTH);

    flash_init();
    bootFromNand = 0;

#if defined(CONFIG_BCM96368)
    if( ((GPIO->StrapBus & MISC_STRAP_BUS_BOOT_SEL_MASK) >>
        MISC_STRAP_BUS_BOOT_SEL_SHIFT) == MISC_STRAP_BUS_BOOT_NAND)
        bootFromNand = 1;
#elif defined(CONFIG_BCM96362) || defined(CONFIG_BCM96328) || defined(CONFIG_BCM96816) || defined(CONFIG_BCM963268)
    if( ((MISC->miscStrapBus & MISC_STRAP_BUS_BOOT_SEL_MASK) >>
        MISC_STRAP_BUS_BOOT_SEL_SHIFT) == MISC_STRAP_BUS_BOOT_NAND )
        bootFromNand = 1;
#endif

#if defined(CONFIG_BCM96368) || defined(CONFIG_BCM96362) || defined(CONFIG_BCM96328) || defined(CONFIG_BCM96816) || defined(CONFIG_BCM963268)
    if( bootFromNand == 1 )
    {
        unsigned long bootCfgSave =  NAND->NandNandBootConfig;
        NAND->NandNandBootConfig = NBC_AUTO_DEV_ID_CFG | 0x101;
        NAND->NandCsNandXor = 1;

        memcpy((unsigned char *)&bootCfeVersion, (unsigned char *)
            FLASH_BASE + CFE_VERSION_OFFSET, sizeof(bootCfeVersion));
        memcpy(inMemNvramData_buf, (unsigned char *)
            FLASH_BASE + NVRAM_DATA_OFFSET, sizeof(NVRAM_DATA));

        NAND->NandNandBootConfig = bootCfgSave;
        NAND->NandCsNandXor = 0;
    }
    else
#endif
    {
        fInfo.flash_rootfs_start_offset = flash_get_sector_size(0);
        if( fInfo.flash_rootfs_start_offset < FLASH_LENGTH_BOOT_ROM )
            fInfo.flash_rootfs_start_offset = FLASH_LENGTH_BOOT_ROM;
     
        flash_read_buf (NVRAM_SECTOR, CFE_VERSION_OFFSET,
            (unsigned char *)&bootCfeVersion, sizeof(bootCfeVersion));

        /* Read the flash contents into NVRAM buffer */
        flash_read_buf (NVRAM_SECTOR, NVRAM_DATA_OFFSET,
                        inMemNvramData_buf, sizeof (NVRAM_DATA)) ;
    }

#if defined(DEBUG_FLASH)
    printk("reading nvram into inMemNvramData\n");
    printk("ulPsiSize 0x%x\n", (unsigned int)inMemNvramData.ulPsiSize);
    printk("backupPsi 0x%x\n", (unsigned int)inMemNvramData.backupPsi);
    printk("ulSyslogSize 0x%x\n", (unsigned int)inMemNvramData.ulSyslogSize);
#endif

    if ((BpSetBoardId(inMemNvramData.szBoardId) != BP_SUCCESS))
        printk("\n*** Board is not initialized properly ***\n\n");
#if defined (CONFIG_BCM_ENDPOINT_MODULE)
    if ((BpSetVoiceBoardId(inMemNvramData.szVoiceBoardId) != BP_SUCCESS))
        printk("\n*** Voice Board id is not initialized properly ***\n\n");
#endif
}

/***********************************************************************
 * Function Name: kerSysCfeVersionGet
 * Description  : Get CFE Version.
 * Returns      : 1 -- ok, 0 -- fail
 ***********************************************************************/
int kerSysCfeVersionGet(char *string, int stringLength)
{
    memcpy(string, (unsigned char *)&bootCfeVersion, stringLength);
    return(0);
}

/****************************************************************************
 * NVRAM functions
 * NVRAM data could share a sector with the CFE.  So a write to NVRAM
 * data is actually a read/modify/write operation on a sector.  Protected
 * by a higher level mutex, flashImageMutex.
 * Nvram data is cached in memory in a variable called inMemNvramData, so
 * writes will update this variable and reads just read from this variable.
 ****************************************************************************/


/** set nvram data
 * Must be called with flashImageMutex held
 *
 * @return 0 on success, -1 on failure.
 */
int kerSysNvRamSet(const char *string, int strLen, int offset)
{
    int sts = -1;  // initialize to failure
    char *pBuf = NULL;

    BCM_ASSERT_HAS_MUTEX_C(&flashImageMutex);
    BCM_ASSERT_R(offset+strLen <= NVRAM_LENGTH, sts);

#if 1 //__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
    {
        if ((pBuf = getSharedBlks(NVRAM_SECTOR, 1)) == NULL)
            return -1;

        // set string to the memory buffer
        memcpy((pBuf + NVRAM_DATA_OFFSET + offset), string, strLen);

        if ((sts = setSharedBlks(NVRAM_SECTOR, 1, pBuf)) != 0)
            sts = -1;

        retriedKfree(pBuf);

        if (0 == sts)
        {
            // write to flash was OK, now update in-memory copy
            updateInMemNvramData((unsigned char *) string, strLen, offset);
        }

    }
#else
    if (bootFromNand == 0)
    {
        if ((pBuf = getSharedBlks(NVRAM_SECTOR, 1)) == NULL)
            return sts;

        // set string to the memory buffer
        memcpy((pBuf + NVRAM_DATA_OFFSET + offset), string, strLen);

        sts = setSharedBlks(NVRAM_SECTOR, 1, pBuf);
    
        retriedKfree(pBuf);       
    }
    else
    {
        sts = nandNvramSet(string);
    }
    
    if (0 == sts)
    {
        // write to flash was OK, now update in-memory copy
        updateInMemNvramData((unsigned char *) string, strLen, offset);
    }

#endif
    return sts;
}


/** get nvram data
 *
 * since it reads from in-memory copy of the nvram data, always successful.
 */
#if 1//__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
int kerSysNvRamGet(char *string, int strLen, int offset)
#else
void kerSysNvRamGet(char *string, int strLen, int offset)
#endif
{
    unsigned long flags;

    spin_lock_irqsave(&inMemNvramData_spinlock, flags);
    memcpy(string, inMemNvramData_buf + offset, strLen);
    spin_unlock_irqrestore(&inMemNvramData_spinlock, flags);

#if 1//__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
    return 0;
#else
    return;
#endif
}


/** Erase entire nvram area.
 *
 * Currently there are no callers of this function.  THe return value is
 * the opposite of kerSysNvramSet.  Kept this way for compatibility.
 *
 * @return 0 on failure, 1 on success.
 */
int kerSysEraseNvRam(void)
{
    int sts = 1;

    BCM_ASSERT_NOT_HAS_MUTEX_C(&flashImageMutex);

    if (bootFromNand == 0)
    {
        char *tempStorage;
        if (NULL == (tempStorage = kmalloc(NVRAM_LENGTH, GFP_KERNEL)))
        {
            sts = 0;
        }
        else
        {
            // just write the whole buf with '0xff' to the flash
            memset(tempStorage, UNINITIALIZED_FLASH_DATA_CHAR, NVRAM_LENGTH);
            mutex_lock(&flashImageMutex);
            if (kerSysNvRamSet(tempStorage, NVRAM_LENGTH, 0) != 0)
                sts = 0;
            mutex_unlock(&flashImageMutex);
            kfree(tempStorage);
        }
    }
#if 1 //__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
#else
    else
    {
        printk("kerSysEraseNvram: not supported when bootFromNand == 1\n");
        sts = 0;
    }
#endif
    return sts;
}

#if 1 //__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
#else
unsigned long kerSysReadFromFlash( void *toaddr, unsigned long fromaddr,
    unsigned long len )
{
    int sect = flash_get_blk((int) fromaddr);
    unsigned char *start = flash_get_memptr(sect);

    down(&semflash);
    flash_read_buf( sect, (int) fromaddr - (int) start, toaddr, len );
    up(&semflash);

    return( len );
}
#endif

#else // CONFIG_BRCM_IKOS
static NVRAM_DATA ikos_nvram_data =
    {
    NVRAM_VERSION_NUMBER,
    "",
    "ikos",
    0,
    DEFAULT_PSI_SIZE,
    11,
    {0x02, 0x10, 0x18, 0x01, 0x00, 0x01},
    0x00, 0x00,
    0x720c9f60
    };

void kerSysEarlyFlashInit( void )
{
    inMemNvramData_buf = (unsigned char *) &inMemNvramData;
    memset(inMemNvramData_buf, UNINITIALIZED_FLASH_DATA_CHAR, NVRAM_LENGTH);

    memcpy(inMemNvramData_buf, (unsigned char *)&ikos_nvram_data,
        sizeof (NVRAM_DATA));
    fInfo.flash_scratch_pad_length = 0;
    fInfo.flash_persistent_start_blk = 0;
}

int kerSysCfeVersionGet(char *string, int stringLength)
{
    *string = '\0';
    return(0);
}

int kerSysNvRamGet(char *string, int strLen, int offset)
{
    memcpy(string, (unsigned char *) &ikos_nvram_data, sizeof(NVRAM_DATA));
    return(0);
}

int kerSysNvRamSet(char *string, int strLen, int offset)
{
    return(0);
}

int kerSysEraseNvRam(void)
{
    return(0);
}

unsigned long kerSysReadFromFlash( void *toaddr, unsigned long fromaddr,
    unsigned long len )
{
    return(memcpy((unsigned char *) toaddr, (unsigned char *) fromaddr, len));
}
#endif  // CONFIG_BRCM_IKOS


/** Update the in-Memory copy of the nvram with the given data.
 *
 * @data: pointer to new nvram data
 * @len: number of valid bytes in nvram data
 * @offset: offset of the given data in the nvram data
 */
void updateInMemNvramData(const unsigned char *data, int len, int offset)
{
    unsigned long flags;

    spin_lock_irqsave(&inMemNvramData_spinlock, flags);
    memcpy(inMemNvramData_buf + offset, data, len);
    spin_unlock_irqrestore(&inMemNvramData_spinlock, flags);
}


/** Get the bootline string from the NVRAM data.
 * Assumes the caller has the inMemNvramData locked.
 * Special case: this is called from prom.c without acquiring the
 * spinlock.  It is too early in the bootup sequence for spinlocks.
 *
 * @param bootline (OUT) a buffer of NVRAM_BOOTLINE_LEN bytes for the result
 */
void kerSysNvRamGetBootlineLocked(char *bootline)
{
    memcpy(bootline, inMemNvramData.szBootline,
                     sizeof(inMemNvramData.szBootline));
}
EXPORT_SYMBOL(kerSysNvRamGetBootlineLocked);


/** Get the bootline string from the NVRAM data.
 *
 * @param bootline (OUT) a buffer of NVRAM_BOOTLINE_LEN bytes for the result
 */
void kerSysNvRamGetBootline(char *bootline)
{
    unsigned long flags;

    spin_lock_irqsave(&inMemNvramData_spinlock, flags);
    kerSysNvRamGetBootlineLocked(bootline);
    spin_unlock_irqrestore(&inMemNvramData_spinlock, flags);
}
EXPORT_SYMBOL(kerSysNvRamGetBootline);


/** Get the BoardId string from the NVRAM data.
 * Assumes the caller has the inMemNvramData locked.
 * Special case: this is called from prom_init without acquiring the
 * spinlock.  It is too early in the bootup sequence for spinlocks.
 *
 * @param boardId (OUT) a buffer of NVRAM_BOARD_ID_STRING_LEN
 */
void kerSysNvRamGetBoardIdLocked(char *boardId)
{
    memcpy(boardId, inMemNvramData.szBoardId,
                    sizeof(inMemNvramData.szBoardId));
}
EXPORT_SYMBOL(kerSysNvRamGetBoardIdLocked);


/** Get the BoardId string from the NVRAM data.
 *
 * @param boardId (OUT) a buffer of NVRAM_BOARD_ID_STRING_LEN
 */
void kerSysNvRamGetBoardId(char *boardId)
{
    unsigned long flags;

    spin_lock_irqsave(&inMemNvramData_spinlock, flags);
    kerSysNvRamGetBoardIdLocked(boardId);
    spin_unlock_irqrestore(&inMemNvramData_spinlock, flags);
}
EXPORT_SYMBOL(kerSysNvRamGetBoardId);


/** Get the base mac addr from the NVRAM data.  This is 6 bytes, not
 * a string.
 *
 * @param baseMacAddr (OUT) a buffer of NVRAM_MAC_ADDRESS_LEN
 */
void kerSysNvRamGetBaseMacAddr(unsigned char *baseMacAddr)
{
    unsigned long flags;

    spin_lock_irqsave(&inMemNvramData_spinlock, flags);
    memcpy(baseMacAddr, inMemNvramData.ucaBaseMacAddr,
                        sizeof(inMemNvramData.ucaBaseMacAddr));
    spin_unlock_irqrestore(&inMemNvramData_spinlock, flags);
}
EXPORT_SYMBOL(kerSysNvRamGetBaseMacAddr);


/** Get the nvram version from the NVRAM data.
 *
 * @return nvram version number.
 */
unsigned long kerSysNvRamGetVersion(void)
{
    return (inMemNvramData.ulVersion);
}
EXPORT_SYMBOL(kerSysNvRamGetVersion);


void kerSysFlashInit( void )
{
    sema_init(&semflash, 1);
#if 1 //__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
#ifdef CONFIG_MTD_BRCMNAND
    flash_init_nand_info(&inMemNvramData, &fInfo);
#else
    flash_init_info(&inMemNvramData, &fInfo);
#endif
#else
    // too early in bootup sequence to acquire spinlock, not needed anyways
    // only the kernel is running at this point
    flash_init_info(&inMemNvramData, &fInfo);
#endif
}

/***********************************************************************
 * Function Name: kerSysFlashAddrInfoGet
 * Description  : Fills in a structure with information about the NVRAM
 *                and persistent storage sections of flash memory.  
 *                Fro physmap.c to mount the fs vol.
 * Returns      : None.
 ***********************************************************************/
void kerSysFlashAddrInfoGet(PFLASH_ADDR_INFO pflash_addr_info)
{
    memcpy(pflash_addr_info, &fInfo, sizeof(FLASH_ADDR_INFO));
}
#if 1 /* __MSTC__, zongyue: to fix ImageDefault start postion and RootFS size if NAND FLASH have bad blocks */
void kerSysFlashBadBlockNumSet(int number)
{
    fInfo.flash_rootfs_bad_block_number = number;
}
#endif
#if 1 //__MSTC__, Dennis merge from Eleana
// get update configuration
// return:
//  0 - ok
//  -1 - fail
int kerSysUpdateConfigGet(char *string, int strLen, int offset)
{
    char *pBuf = NULL;

    if ((pBuf = getSharedBlks(fInfo.flash_persistent_start_blk,
        fInfo.flash_persistent_number_blk)) == NULL)
        return -1;

    memcpy(string, (pBuf + offset), strLen);

    retriedKfree(pBuf);

    return 0;
}
#endif
/*******************************************************************************
 * PSI functions
 * PSI is where we store the config file.  There is also a "backup" PSI
 * that stores an extra copy of the PSI.  THe idea is if the power goes out
 * while we are writing the primary PSI, the backup PSI will still have
 * a good copy from the last write.  No additional locking is required at
 * this level.
 *******************************************************************************/
#define PSI_FILE_NAME           "/data/psi"
#define PSI_BACKUP_FILE_NAME    "/data/psibackup"
#define SCRATCH_PAD_FILE_NAME   "/data/scratchpad"


// get psi data
// return:
//  0 - ok
//  -1 - fail
int kerSysPersistentGet(char *string, int strLen, int offset)
{
    char *pBuf = NULL;

    if( bootFromNand )
    {
        /* Root file system is on a writable NAND flash.  Read PSI from
         * a file on the NAND flash.
         */
        struct file *fp;
        mm_segment_t fs;
        int len;

        memset(string, 0x00, strLen);
        fp = filp_open(PSI_FILE_NAME, O_RDONLY, 0);
        if (!IS_ERR(fp) && fp->f_op && fp->f_op->read)
        {
            fs = get_fs();
            set_fs(get_ds());

            fp->f_pos = 0;

            if((len = (int) fp->f_op->read(fp, (void *) string, strLen,
               &fp->f_pos)) <= 0)
                printk("Failed to read psi from '%s'\n", PSI_FILE_NAME);

            filp_close(fp, NULL);
            set_fs(fs);
        }

        return 0;
    }

    if ((pBuf = getSharedBlks(fInfo.flash_persistent_start_blk,
        fInfo.flash_persistent_number_blk)) == NULL)
        return -1;

    // get string off the memory buffer
    memcpy(string, (pBuf + fInfo.flash_persistent_blk_offset + offset), strLen);

    retriedKfree(pBuf);

    return 0;
}

int kerSysBackupPsiGet(char *string, int strLen, int offset)
{
    char *pBuf = NULL;

    if( bootFromNand )
    {
        /* Root file system is on a writable NAND flash.  Read backup PSI from
         * a file on the NAND flash.
         */
        struct file *fp;
        mm_segment_t fs;
        int len;

        memset(string, 0x00, strLen);
        fp = filp_open(PSI_BACKUP_FILE_NAME, O_RDONLY, 0);
        if (!IS_ERR(fp) && fp->f_op && fp->f_op->read)
        {
            fs = get_fs();
            set_fs(get_ds());

            fp->f_pos = 0;

            if((len = (int) fp->f_op->read(fp, (void *) string, strLen,
               &fp->f_pos)) <= 0)
                printk("Failed to read psi from '%s'\n", PSI_BACKUP_FILE_NAME);

            filp_close(fp, NULL);
            set_fs(fs);
        }

        return 0;
    }

    if (fInfo.flash_backup_psi_number_blk <= 0)
    {
        printk("No backup psi blks allocated, change it in CFE\n");
        return -1;
    }

    if (fInfo.flash_persistent_start_blk == 0)
        return -1;

    if ((pBuf = getSharedBlks(fInfo.flash_backup_psi_start_blk,
                              fInfo.flash_backup_psi_number_blk)) == NULL)
        return -1;

    // get string off the memory buffer
    memcpy(string, (pBuf + offset), strLen);

    retriedKfree(pBuf);

    return 0;
}

// set psi 
// return:
//  0 - ok
//  -1 - fail
int kerSysPersistentSet(char *string, int strLen, int offset)
{
    int sts = 0;
    char *pBuf = NULL;

    if( bootFromNand )
    {
        /* Root file system is on a writable NAND flash.  Write PSI to
         * a file on the NAND flash.
         */
        struct file *fp;
        mm_segment_t fs;

        fp = filp_open(PSI_FILE_NAME, O_RDWR | O_TRUNC | O_CREAT,
            S_IRUSR | S_IWUSR);

        if (!IS_ERR(fp) && fp->f_op && fp->f_op->write)
        {
            fs = get_fs();
            set_fs(get_ds());

            fp->f_pos = 0;

            if((int) fp->f_op->write(fp, (void *) string, strLen,
               &fp->f_pos) != strLen)
                printk("Failed to write psi to '%s'.\n", PSI_FILE_NAME);

            vfs_fsync(fp, fp->f_path.dentry, 0);
            filp_close(fp, NULL);
            set_fs(fs);
        }
        else
            printk("Unable to open '%s'.\n", PSI_FILE_NAME);

        return 0;
    }

    if ((pBuf = getSharedBlks(fInfo.flash_persistent_start_blk,
        fInfo.flash_persistent_number_blk)) == NULL)
        return -1;
#if 1 //__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
    memset((pBuf + fInfo.flash_persistent_blk_offset + offset), 0xFF, fInfo.flash_persistent_length );
#endif

    // set string to the memory buffer
    memcpy((pBuf + fInfo.flash_persistent_blk_offset + offset), string, strLen);

    if (setSharedBlks(fInfo.flash_persistent_start_blk, 
        fInfo.flash_persistent_number_blk, pBuf) != 0)
        sts = -1;
    
    retriedKfree(pBuf);

    return sts;
}

int kerSysBackupPsiSet(char *string, int strLen, int offset)
{
    int i;
    int sts = 0;
    int usedBlkSize = 0;
    char *pBuf = NULL;

    if( bootFromNand )
    {
        /* Root file system is on a writable NAND flash.  Write backup PSI to
         * a file on the NAND flash.
         */
        struct file *fp;
        mm_segment_t fs;

        fp = filp_open(PSI_BACKUP_FILE_NAME, O_RDWR | O_TRUNC | O_CREAT,
            S_IRUSR | S_IWUSR);

        if (!IS_ERR(fp) && fp->f_op && fp->f_op->write)
        {
            fs = get_fs();
            set_fs(get_ds());

            fp->f_pos = 0;

            if((int) fp->f_op->write(fp, (void *) string, strLen,
               &fp->f_pos) != strLen)
                printk("Failed to write psi to '%s'.\n", PSI_BACKUP_FILE_NAME);

            vfs_fsync(fp, fp->f_path.dentry, 0);
            filp_close(fp, NULL);
            set_fs(fs);
        }
        else
            printk("Unable to open '%s'.\n", PSI_BACKUP_FILE_NAME);


        return 0;
    }

    if (fInfo.flash_backup_psi_number_blk <= 0)
    {
        printk("No backup psi blks allocated, change it in CFE\n");
        return -1;
    }

    if (fInfo.flash_persistent_start_blk == 0)
        return -1;

    /*
     * The backup PSI does not share its blocks with anybody else, so I don't have
     * to read the flash first.  But now I have to make sure I allocate a buffer
     * big enough to cover all blocks that the backup PSI spans.
     */
    for (i=fInfo.flash_backup_psi_start_blk;
         i < (fInfo.flash_backup_psi_start_blk + fInfo.flash_backup_psi_number_blk); i++)
    {
       usedBlkSize += flash_get_sector_size((unsigned short) i);
    }

    if ((pBuf = (char *) retriedKmalloc(usedBlkSize)) == NULL)
    {
       printk("failed to allocate memory with size: %d\n", usedBlkSize);
       return -1;
    }

    memset(pBuf, 0, usedBlkSize);

    // set string to the memory buffer
    memcpy((pBuf + offset), string, strLen);

    if (setSharedBlks(fInfo.flash_backup_psi_start_blk, fInfo.flash_backup_psi_number_blk, 
                      pBuf) != 0)
        sts = -1;
    
    retriedKfree(pBuf);

    return sts;
}


/*******************************************************************************
 * "Kernel Syslog" is one or more sectors allocated in the flash
 * so that we can persist crash dump or other system diagnostics info
 * across reboots.  This feature is current not implemented.
 *******************************************************************************/

#define SYSLOG_FILE_NAME        "/etc/syslog"

int kerSysSyslogGet(char *string, int strLen, int offset)
{
    char *pBuf = NULL;

    if( bootFromNand )
    {
        /* Root file system is on a writable NAND flash.  Read syslog from
         * a file on the NAND flash.
         */
        struct file *fp;
        mm_segment_t fs;

        memset(string, 0x00, strLen);
        fp = filp_open(SYSLOG_FILE_NAME, O_RDONLY, 0);
        if (!IS_ERR(fp) && fp->f_op && fp->f_op->read)
        {
            fs = get_fs();
            set_fs(get_ds());

            fp->f_pos = 0;

            if((int) fp->f_op->read(fp, (void *) string, strLen,
               &fp->f_pos) <= 0)
                printk("Failed to read psi from '%s'\n", SYSLOG_FILE_NAME);

            filp_close(fp, NULL);
            set_fs(fs);
        }

        return 0;
    }

    if (fInfo.flash_syslog_number_blk <= 0)
    {
        printk("No syslog blks allocated, change it in CFE\n");
        return -1;
    }
    
    if (strLen > fInfo.flash_syslog_length)
        return -1;

    if ((pBuf = getSharedBlks(fInfo.flash_syslog_start_blk,
                              fInfo.flash_syslog_number_blk)) == NULL)
        return -1;

    // get string off the memory buffer
    memcpy(string, (pBuf + offset), strLen);

    retriedKfree(pBuf);

    return 0;
}

int kerSysSyslogSet(char *string, int strLen, int offset)
{
    int i;
    int sts = 0;
    int usedBlkSize = 0;
    char *pBuf = NULL;

    if( bootFromNand )
    {
        /* Root file system is on a writable NAND flash.  Write PSI to
         * a file on the NAND flash.
         */
        struct file *fp;
        mm_segment_t fs;

        fp = filp_open(PSI_FILE_NAME, O_RDWR | O_TRUNC | O_CREAT,
            S_IRUSR | S_IWUSR);

        if (!IS_ERR(fp) && fp->f_op && fp->f_op->write)
        {
            fs = get_fs();
            set_fs(get_ds());

            fp->f_pos = 0;

            if((int) fp->f_op->write(fp, (void *) string, strLen,
               &fp->f_pos) != strLen)
                printk("Failed to write psi to '%s'.\n", PSI_FILE_NAME);

            vfs_fsync(fp, fp->f_path.dentry, 0);
            filp_close(fp, NULL);
            set_fs(fs);
        }
        else
            printk("Unable to open '%s'.\n", PSI_FILE_NAME);

        return 0;
    }

    if (fInfo.flash_syslog_number_blk <= 0)
    {
        printk("No syslog blks allocated, change it in CFE\n");
        return -1;
    }
    
    if (strLen > fInfo.flash_syslog_length)
        return -1;

    /*
     * The syslog does not share its blocks with anybody else, so I don't have
     * to read the flash first.  But now I have to make sure I allocate a buffer
     * big enough to cover all blocks that the syslog spans.
     */
    for (i=fInfo.flash_syslog_start_blk;
         i < (fInfo.flash_syslog_start_blk + fInfo.flash_syslog_number_blk); i++)
    {
        usedBlkSize += flash_get_sector_size((unsigned short) i);
    }

    if ((pBuf = (char *) retriedKmalloc(usedBlkSize)) == NULL)
    {
       printk("failed to allocate memory with size: %d\n", usedBlkSize);
       return -1;
    }

    memset(pBuf, 0, usedBlkSize);

    // set string to the memory buffer
    memcpy((pBuf + offset), string, strLen);

    if (setSharedBlks(fInfo.flash_syslog_start_blk, fInfo.flash_syslog_number_blk, pBuf) != 0)
        sts = -1;

    retriedKfree(pBuf);

    return sts;
}


/*******************************************************************************
 * Writing software image to flash operations
 * This procedure should be serialized.  Look for flashImageMutex.
 *******************************************************************************/


#define je16_to_cpu(x) ((x).v16)
#define je32_to_cpu(x) ((x).v32)

/*
 * nandUpdateSeqNum
 * 
 * Read the sequence number from each rootfs partition.  The sequence number is
 * the extension on the cferam file.  Add one to the highest sequence number
 * and change the extenstion of the cferam in the image to be flashed to that
 * number.
 */
#if 0 //__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
static int nandUpdateSeqNum(unsigned char *imagePtr, int imageSize, int blkLen)
{
    char fname[] = NAND_CFE_RAM_NAME;
    int fname_actual_len = strlen(fname);
    int fname_cmp_len = strlen(fname) - 3; /* last three are digits */
    int seq = -1;
    int seq2 = -1;
    int ret = 1;

    seq = kerSysGetSequenceNumber(1);
    seq2 = kerSysGetSequenceNumber(2);
    seq = (seq >= seq2) ? seq : seq2;

    if( seq != -1 )
    {
        unsigned char *buf, *p;
        int len = blkLen;
        struct jffs2_raw_dirent *pdir;
        unsigned long version = 0;
        int done = 0;

        if( *(unsigned short *) imagePtr != JFFS2_MAGIC_BITMASK )
        {
            imagePtr += len;
            imageSize -= len;
        }

        /* Increment the new highest sequence number. Add it to the CFE RAM
         * file name.
         */
        seq++;

        /* Search the image and replace the last three characters of file
         * cferam.000 with the new sequence number.
         */
        for(buf = imagePtr; buf < imagePtr+imageSize && done == 0; buf += len)
        {
            p = buf;
            while( p < buf + len )
            {
                pdir = (struct jffs2_raw_dirent *) p;
                if( je16_to_cpu(pdir->magic) == JFFS2_MAGIC_BITMASK )
                {
                    if( je16_to_cpu(pdir->nodetype) == JFFS2_NODETYPE_DIRENT &&
                        fname_actual_len == pdir->nsize &&
                        !memcmp(fname, pdir->name, fname_cmp_len) &&
                        je32_to_cpu(pdir->version) > version &&
                        je32_to_cpu(pdir->ino) != 0 )
                     {
                        /* File cferam.000 found. Change the extension to the
                         * new sequence number and recalculate file name CRC.
                         */
                        p = pdir->name + fname_cmp_len;
                        p[0] = (seq / 100) + '0';
                        p[1] = ((seq % 100) / 10) + '0';
                        p[2] = ((seq % 100) % 10) + '0';
                        p[3] = '\0';

                        je32_to_cpu(pdir->name_crc) =
                            crc32(0, pdir->name, (unsigned int)
                            fname_actual_len);

                        version = je32_to_cpu(pdir->version);

                        /* Setting 'done = 1' assumes there is only one version
                         * of the directory entry.
                         */
                        done = 1;
                        ret = (buf - imagePtr) / len; /* block number */
                        break;
                    }

                    p += (je32_to_cpu(pdir->totlen) + 0x03) & ~0x03;
                }
                else
                {
                    done = 1;
                    break;
                }
            }
        }
    }

    return(ret);
}

/* Erase the specified NAND flash block but preserve the spare area. */
static int nandEraseBlkNotSpare( struct mtd_info *mtd, int blk )
{
    int sts = -1;

    /* block_is bad returns 0 if block is not bad */
    if( mtd->block_isbad(mtd, blk) == 0 )
    {
        unsigned char oobbuf[64]; /* expected to be a max size */
        struct mtd_oob_ops ops;

        memset(&ops, 0x00, sizeof(ops));
        ops.ooblen = mtd->oobsize;
        ops.ooboffs = 0;
        ops.datbuf = NULL;
        ops.oobbuf = oobbuf;
        ops.len = 0;
        ops.mode = MTD_OOB_PLACE;

        /* Read and save the spare area. */
        sts = mtd->read_oob(mtd, blk, &ops);
        if( sts == 0 )
        {
            struct erase_info erase;

            /* Erase the flash block. */
            memset(&erase, 0x00, sizeof(erase));
            erase.addr = blk;
            erase.len = mtd->erasesize;
            erase.mtd = mtd;

            sts = mtd->erase(mtd, &erase);
            if( sts == 0 )
            {
                int i;

                /* Function local_bh_disable has been called and this
                 * is the only operation that should be occurring.
                 * Therefore, spin waiting for erase to complete.
                 */
                for(i = 0; i < 10000 && erase.state != MTD_ERASE_DONE &&
                    erase.state != MTD_ERASE_FAILED; i++ )
                {
                    udelay(100);
                }

                if( erase.state != MTD_ERASE_DONE )
                    sts = -1;
            }

            if( sts == 0 )
            {
                memset(&ops, 0x00, sizeof(ops));
                ops.ooblen = mtd->oobsize;
                ops.ooboffs = 0;
                ops.datbuf = NULL;
                ops.oobbuf = oobbuf;
                ops.len = 0;
                ops.mode = MTD_OOB_PLACE;

                /* Restore the spare area. */
                if( (sts = mtd->write_oob(mtd, blk, &ops)) != 0 )
                    printk("nandImageSet - Block 0x%8.8x. Error writing spare "
                        "area.\n", blk);
            }
            else
                printk("nandImageSet - Block 0x%8.8x. Error erasing block.\n",blk);
        }
        else
            printk("nandImageSet - Block 0x%8.8x. Error read spare area.\n", blk);
    }

    return( sts );
}

// NAND flash bcm image 
// return: 
// 0 - ok
// !0 - the sector number fail to be flashed (should not be 0)
int kerSysBcmNandImageSet( char *rootfs_part, char *string, int img_size,
    int should_yield )
{
    /* Allow room to flash cferam sequence number at start of file system. */
    const int fs_start_blk_num = 8;

    int sts = -1;
    int blk = 0;
    int cferam_blk;
    int fs_start_blk;
    int old_img = 0;
    char *cferam_string;
    char *end_string = string + img_size;
    struct mtd_info *mtd0 = NULL;
    struct mtd_info *mtd1 = get_mtd_device_nm("nvram");
    WFI_TAG wt = {0};

    if( mtd1 )
    {
        unsigned long chip_id = kerSysGetChipId();
        int blksize = mtd1->erasesize / 1024;

        memcpy(&wt, end_string, sizeof(wt));
        if( (wt.wfiVersion & WFI_ANY_VERS_MASK) == WFI_ANY_VERS &&
            wt.wfiChipId != chip_id )
        {
            printk("Chip Id error.  Image Chip Id = %04lx, Board Chip Id = "
                "%04lx.\n", wt.wfiChipId, chip_id);
        }
        else if( (wt.wfiVersion & WFI_ANY_VERS_MASK) == WFI_ANY_VERS &&
            ((blksize == 16 && wt.wfiFlashType != WFI_NAND16_FLASH) ||
             (blksize < 128 && wt.wfiFlashType == WFI_NAND128_FLASH)) )
        {
            printk("\nERROR: NAND flash block size %dKB does not work with an "
                "image built with %dKB block size\n\n", blksize,
                (wt.wfiFlashType == WFI_NAND16_FLASH) ? 16 : 128);
        }
        else
        {
            mtd0 = get_mtd_device_nm(rootfs_part);

            /* If the image version indicates that is uses a 1MB data partition
             * size and the image is intended to be flashed to the second file
             * system partition, change to the flash to the first partition.
             * After new image is flashed, delete the second file system and
             * data partitions (at the bottom of this function).
             */
            if( wt.wfiVersion == WFI_VERSION_NAND_1MB_DATA )
            {
                unsigned long rootfs_ofs;
                kerSysBlParmsGetInt(NAND_RFS_OFS_NAME, (int *) &rootfs_ofs);
                
                if(rootfs_ofs == inMemNvramData.ulNandPartOfsKb[NP_ROOTFS_1] &&
                    mtd0)
                {
                    printk("Old image, flashing image to first partition.\n");
                    put_mtd_device(mtd0);
                    mtd0 = NULL;
                    old_img = 1;
                }
            }

            if( mtd0 == NULL || mtd0->size == 0LL )
            {
                /* Flash device is configured to use only one file system. */
                if( mtd0 )
                    put_mtd_device(mtd0);
                mtd0 = get_mtd_device_nm("rootfs");
            }
        }
    }

    if( mtd0 && mtd1 )
    {
        int ofs;
        unsigned long flags=0;
        int retlen = 0;

        if( *(unsigned short *) string == JFFS2_MAGIC_BITMASK )
            /* Image only contains file system. */
            ofs = 0; /* use entire string image to find sequence number */
        else
        {
            /* Image contains CFE ROM boot loader. */
            PNVRAM_DATA pnd = (PNVRAM_DATA) (string + NVRAM_DATA_OFFSET);

            /* skip block 0 to find sequence number */
            switch(wt.wfiFlashType)
            {
            case WFI_NAND128_FLASH:
                ofs = 128 * 1024;
                break;

            /* case WFI_NAND16_FLASH: */
            default:
                ofs = 16 * 1024;
                break;
            }

            /* Copy NVRAM data to block to be flashed so it is preserved. */
            spin_lock_irqsave(&inMemNvramData_spinlock, flags);
            memcpy((unsigned char *) pnd, inMemNvramData_buf,
                sizeof(NVRAM_DATA));
            spin_unlock_irqrestore(&inMemNvramData_spinlock, flags);

            /* Recalculate the nvramData CRC. */
            pnd->ulCheckSum = 0;
            pnd->ulCheckSum = crc32(CRC32_INIT_VALUE, pnd, sizeof(NVRAM_DATA));
        }

        /* Update the sequence number that replaces that extension in file
         * cferam.000
         */
        cferam_blk = nandUpdateSeqNum((unsigned char *) string + ofs,
            img_size - ofs, mtd0->erasesize) * mtd0->erasesize;
        cferam_string = string + ofs + cferam_blk;

        fs_start_blk = fs_start_blk_num * mtd0->erasesize;

        /*
         * write image to flash memory.
         * In theory, all calls to flash_write_buf() must be done with
         * semflash held, so I added it here.  However, in reality, all
         * flash image writes are protected by flashImageMutex at a higher
         * level.
         */
        down(&semflash);

        // Once we have acquired the flash semaphore, we can
        // disable activity on other processor and also on local processor.
        // Need to disable interrupts so that RCU stall checker will not complain.
        if (!should_yield)
        {
            stopOtherCpu();
            local_irq_save(flags);
        }

        local_bh_disable();

        if( *(unsigned short *) string != JFFS2_MAGIC_BITMASK )
        {
            /* Flash the CFE ROM boot loader. */
            nandEraseBlkNotSpare( mtd1, 0 );
            mtd1->write(mtd1, 0, mtd1->erasesize, &retlen, string);
            string += ofs;
        }

        /* Erase block with sequence number before flashing the image. */
        nandEraseBlkNotSpare( mtd0, cferam_blk );

        /* Flash the image except for the part with the sequence number. */
        for( blk = fs_start_blk; blk < mtd0->size; blk += mtd0->erasesize )
        {
            if( (sts = nandEraseBlkNotSpare( mtd0, blk )) == 0 )
            {
                /* Write a block of the image to flash. */
                if( string < end_string && string != cferam_string )
                {
                    int writelen = ((string + mtd0->erasesize) <= end_string)
                        ? mtd0->erasesize : (int) (end_string - string);

                    mtd0->write(mtd0, blk, writelen, &retlen, string);
                    if( retlen == writelen )
                    {
                        printk(".");
                        string += writelen;
                    }

                    if (should_yield)
                    {
                        local_bh_enable();
                        yield();
                        local_bh_disable();
                    }

                }
                else
                    string += mtd0->erasesize;
            }
        }

        /* Flash the image part with the sequence number. */
        for( blk = 0; blk < fs_start_blk; blk += mtd0->erasesize )
        {
            if( (sts = nandEraseBlkNotSpare( mtd0, blk )) == 0 )
            {
                /* Write a block of the image to flash. */
                if( cferam_string )
                {
                    mtd0->write(mtd0, blk, mtd0->erasesize,
                        &retlen, cferam_string);

                    if( retlen == mtd0->erasesize )
                    {
                        printk(".");
                        cferam_string = NULL;
                    }

                    if (should_yield)
                    {
                        local_bh_enable();
                        yield();
                        local_bh_disable();
                    }
                }
            }
        }

#if 1 // __MSTC__, Richard Huang , BRCM patch, csp842529_412L08_nandflash_oob
        if( sts && mtd0->block_isbad(mtd0, blk-mtd0->erasesize) )
        	sts = 0;
#endif
        up(&semflash);

        printk("\n\n");

        if (should_yield)
        {
            local_bh_enable();
        }

        if( sts )
        {
            /*
             * Even though we try to recover here, this is really bad because
             * we have stopped the other CPU and we cannot restart it.  So we
             * really should try hard to make sure flash writes will never fail.
             */
            printk(KERN_ERR "nandEraseBlkNotSpare: write failed at blk=%d\n", blk);
            sts = (blk > mtd0->erasesize) ? blk / mtd0->erasesize : 1;
            if (!should_yield)
            {
                local_irq_restore(flags);
                local_bh_enable();
            }
        }
    }

    if( mtd0 )
        put_mtd_device(mtd0);

    if( mtd1 )
        put_mtd_device(mtd1);

    if( sts == 0 && old_img == 1 )
    {
        printk("\nOld image, deleting data and secondary file system partitions\n");
        mtd0 = get_mtd_device_nm("data");
        for( blk = 0; blk < mtd0->size; blk += mtd0->erasesize )
            nandEraseBlkNotSpare( mtd0, blk );
        mtd0 = get_mtd_device_nm("rootfs_update");
        for( blk = 0; blk < mtd0->size; blk += mtd0->erasesize )
            nandEraseBlkNotSpare( mtd0, blk );
    }

    return sts;
}
#endif
    // NAND flash overwrite nvram block.    
    // return: 
    // 0 - ok
    // !0 - the sector number fail to be flashed (should not be 0)
#if 1 //__MSTC__, JhenYang: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
#else
static int nandNvramSet(const char *nvramString )
{
    /* Image contains CFE ROM boot loader. */
    struct mtd_info *mtd = get_mtd_device_nm("nvram"); 
    char *cferom = NULL;
    int retlen = 0;
        
    if ( (cferom = (char *)retriedKmalloc(mtd->erasesize)) == NULL )
    {
        printk("\n Failed to allocated memory in nandNvramSet();");
        return -1;
    }

    /* Read the whole cfe rom nand block 0 */
    mtd->read(mtd, 0, mtd->erasesize, &retlen, cferom);

    /* Copy the nvram string into place */
    memcpy(cferom+NVRAM_DATA_OFFSET, nvramString, sizeof(NVRAM_DATA));
    
    /* Flash the CFE ROM boot loader. */
    nandEraseBlkNotSpare( mtd, 0 );
    mtd->write(mtd, 0, mtd->erasesize, &retlen, cferom);
    
    retriedKfree(cferom);
    return 0;
}
#endif           

// flash bcm image 
// return: 
// 0 - ok
// !0 - the sector number fail to be flashed (should not be 0)
// Must be called with flashImageMutex held.
#if 1 //__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
#if 1 //__CTLK__, Thief
int kerSysBcmImageSet( int flash_start_addr, char *string, int size, char* data)
#else
int kerSysBcmImageSet( int flash_start_addr, char *string, int size)
#endif
{
    int sts;
    int sect_size;
    int blk_start;
    int savedSize = size;
    int whole_image = 0;
#if 1 //__CTLK__, Thief
	BOOL gLEDToogle = TRUE;
    char handle = (!strcmp(data, "ACS"))?'1':'0';
#endif
#if 1 //MitraStar, Elina
    //unsigned long flags=0;

    BCM_ASSERT_HAS_MUTEX_C(&flashImageMutex);
#endif

#if 1 // __VERIZON__, zongyue
#else
    if( bootFromNand )
        return( nandImageSet( flash_start_addr, string, size ) );
#endif

    if (flash_start_addr == FLASH_BASE)
        whole_image = 1;

#if defined(DEBUG_FLASH)
    printk("kerSysBcmImageSet: flash_start_addr=0x%x string=%p len=%d whole_image=%d\n",
           flash_start_addr, string, size, whole_image);
#endif

    blk_start = flash_get_blk(flash_start_addr);
    if( blk_start < 0 )
        return( -1 );

#if 1 //MitraStar, Elina

    /*
     * write image to flash memory.
     * In theory, all calls to flash_write_buf() must be done with
     * semflash held, so I added it here.  However, in reality, all
     * flash image writes are protected by flashImageMutex at a higher
     * level.
     */
    down(&semflash);

    // Once we have acquired the flash semaphore, we can
    // disable activity on other processor and also on local processor.
    // Need to disable interrupts so that RCU stall checker will not complain.
    //if (!is_cfe_write && !should_yield)
    //{
        stopOtherCpu();
        //local_irq_save(flags);
    //}

    local_bh_disable();
#else
    // Disable other tasks from this point on
#if defined(CONFIG_SMP)
    smp_send_stop();
    udelay(20);
#endif
    local_bh_disable();
#endif
    printk(KERN_WARNING ".");

    /* write image to flash memory */
    do 
    {
        sect_size = flash_get_sector_size(blk_start);

        flash_sector_erase_int(blk_start);     // erase blk before flash

        if (sect_size > size) 
        {
            if (size & 1) 
                size++;
            sect_size = size;
        }
#if 1 // __Verizon__, zongyue
        if (flash_write_buf(blk_start, 0, string, sect_size) != sect_size) {
            if( !bootFromNand ) {
                break;
            }
            else {
                printk("#");
            }
            blk_start++;
        }
        else { 
#if 1 //MitraStar, Elina
			// check if we just wrote into the sector where the NVRAM is.
			// update our in-memory copy
			if (NVRAM_SECTOR == blk_start)
			{
				updateInMemNvramData(string+NVRAM_DATA_OFFSET, NVRAM_LENGTH, 0);
			}
#endif
            printk(".");
            blk_start++;
            string += sect_size;
            size -= sect_size; 
#if 1 //__CTLK__, Thief
	        if ( blk_start%15 == 0 ) {
				if ( gLEDToogle ) {
					gLEDToogle = FALSE;
					if ( handle != '1' ) { //LAN
						kerSysLedCtrl(kLedPowerG, kLedStateOn);
						kerSysLedCtrl(kLedPowerR, kLedStateOn);
					}
					else {
						kerSysLedCtrl(kLedPowerR, kLedStateOff);
						kerSysLedCtrl(kLedPowerG, kLedStateOn);
					}
				}
				else {
					gLEDToogle = TRUE;
					if ( handle != '1' ) { //LAN
						kerSysLedCtrl(kLedPowerG, kLedStateOff);
						kerSysLedCtrl(kLedPowerR, kLedStateOff);
					}
					else {
						kerSysLedCtrl(kLedPowerG, kLedStateOn);
						kerSysLedCtrl(kLedPowerR, kLedStateOn);
					}
				}
			}
#endif
        }
#else
        if (flash_write_buf(blk_start, 0, string, sect_size) != sect_size) {
            break;
        }

        printk(".");
        blk_start++;
        string += sect_size;
        size -= sect_size;
#endif
    } while (size > 0);

    if (whole_image && savedSize > fInfo.flash_rootfs_start_offset)
    {
        // If flashing a whole image, erase to end of flash.
        int total_blks = flash_get_numsectors();
        while( blk_start < total_blks )
        {
            flash_sector_erase_int(blk_start);
            printk(".");
            blk_start++;
#if 1 //__CTLK__, Thief
			if ( blk_start%15 == 0 ) {
				if ( gLEDToogle ) {
					gLEDToogle = FALSE;
					if ( handle != '1' ) { //LAN
						kerSysLedCtrl(kLedPowerG, kLedStateOn);
						kerSysLedCtrl(kLedPowerR, kLedStateOn);
					}
					else {
						kerSysLedCtrl(kLedPowerR, kLedStateOff);
						kerSysLedCtrl(kLedPowerG, kLedStateOn);
					}
				}
				else {
					gLEDToogle = TRUE;
					if ( handle != '1' ) { //LAN
						kerSysLedCtrl(kLedPowerG, kLedStateOff);
						kerSysLedCtrl(kLedPowerR, kLedStateOff);
					}
					else {
						kerSysLedCtrl(kLedPowerG, kLedStateOn);
						kerSysLedCtrl(kLedPowerR, kLedStateOn);
					}
				}
			}
#endif
        }
    }
#if 1 //MitraStar, Elina
    up(&semflash);
#endif
    printk("\n\n");

    if( size == 0 ) 
    {
#if 1 //MitraStar, Elina
        sts = 0;  // ok
        local_bh_enable();
#else
        sts = 0;  // ok
#endif
    } 
    else
    {  
#if 1 //Mitrastar, Elina
        //local_irq_restore(flags);
        local_bh_enable();
        sts = blk_start;	// failed to flash this sector
#else
        sts = blk_start;    // failed to flash this sector
#endif
    }

#if 0 //MitraStar, Elina
    local_bh_enable();
#endif
    return sts;
}

#else
int kerSysBcmImageSet( int flash_start_addr, char *string, int size,
    int should_yield)
{
    int sts;
    int sect_size;
    int blk_start;
    int savedSize = size;
    int whole_image = 0;
    unsigned long flags=0;
    int is_cfe_write=0;
    WFI_TAG wt = {0};

    BCM_ASSERT_HAS_MUTEX_C(&flashImageMutex);

    if (flash_start_addr == FLASH_BASE)
    {
        unsigned long chip_id = kerSysGetChipId();
        whole_image = 1;
        memcpy(&wt, string + size, sizeof(wt));
        if( (wt.wfiVersion & WFI_ANY_VERS_MASK) == WFI_ANY_VERS &&
            wt.wfiChipId != chip_id )
        {
            printk("Chip Id error.  Image Chip Id = %04lx, Board Chip Id = "
                "%04lx.\n", wt.wfiChipId, chip_id);
            return -1;
        }
    }

    if( whole_image && (wt.wfiVersion & WFI_ANY_VERS_MASK) == WFI_ANY_VERS &&
        wt.wfiFlashType != WFI_NOR_FLASH )
    {
        printk("ERROR: Image does not support a NOR flash device.\n");
        return -1;
    }


#if defined(DEBUG_FLASH)
    printk("kerSysBcmImageSet: flash_start_addr=0x%x string=%p len=%d whole_image=%d\n",
           flash_start_addr, string, size, whole_image);
#endif

    blk_start = flash_get_blk(flash_start_addr);
    if( blk_start < 0 )
        return( -1 );

    is_cfe_write = ((NVRAM_SECTOR == blk_start) &&
                    (size <= FLASH_LENGTH_BOOT_ROM));

    /*
     * write image to flash memory.
     * In theory, all calls to flash_write_buf() must be done with
     * semflash held, so I added it here.  However, in reality, all
     * flash image writes are protected by flashImageMutex at a higher
     * level.
     */
    down(&semflash);

    // Once we have acquired the flash semaphore, we can
    // disable activity on other processor and also on local processor.
    // Need to disable interrupts so that RCU stall checker will not complain.
    if (!is_cfe_write && !should_yield)
    {
        stopOtherCpu();
        local_irq_save(flags);
    }

    local_bh_disable();

    do 
    {
        sect_size = flash_get_sector_size(blk_start);

        flash_sector_erase_int(blk_start);     // erase blk before flash

        if (sect_size > size) 
        {
            if (size & 1) 
                size++;
            sect_size = size;
        }
        
        if (flash_write_buf(blk_start, 0, string, sect_size) != sect_size) {
            break;
        }

        // check if we just wrote into the sector where the NVRAM is.
        // update our in-memory copy
        if (NVRAM_SECTOR == blk_start)
        {
            updateInMemNvramData(string+NVRAM_DATA_OFFSET, NVRAM_LENGTH, 0);
        }

        printk(".");
        blk_start++;
        string += sect_size;
        size -= sect_size; 

        if (should_yield)
        {
            local_bh_enable();
            yield();
            local_bh_disable();
        }
    } while (size > 0);

    if (whole_image && savedSize > fInfo.flash_rootfs_start_offset)
    {
        // If flashing a whole image, erase to end of flash.
        int total_blks = flash_get_numsectors();
        while( blk_start < total_blks )
        {
            flash_sector_erase_int(blk_start);
            printk(".");
            blk_start++;

            if (should_yield)
            {
                local_bh_enable();
                yield();
                local_bh_disable();
            }
        }
    }

    up(&semflash);

    printk("\n\n");

    if (is_cfe_write || should_yield)
    {
        local_bh_enable();
    }

    if( size == 0 )
    {
        sts = 0;  // ok
    }
    else
    {
        /*
         * Even though we try to recover here, this is really bad because
         * we have stopped the other CPU and we cannot restart it.  So we
         * really should try hard to make sure flash writes will never fail.
         */
        printk(KERN_ERR "kerSysBcmImageSet: write failed at blk=%d\n",
                        blk_start);
        sts = blk_start;    // failed to flash this sector
        if (!is_cfe_write && !should_yield)
        {
            local_irq_restore(flags);
            local_bh_enable();
        }
    }

    return sts;
}
#endif

/*******************************************************************************
 * SP functions
 * SP = ScratchPad, one or more sectors in the flash which user apps can
 * store small bits of data referenced by a small tag at the beginning.
 * kerSysScratchPadSet() and kerSysScratchPadCLearAll() must be protected by
 * a mutex because they do read/modify/writes to the flash sector(s).
 * kerSysScratchPadGet() and KerSysScratchPadList() do not need to acquire
 * the mutex, however, I acquire the mutex anyways just to make this interface
 * symmetrical.  High performance and concurrency is not needed on this path.
 *
 *******************************************************************************/

// get scratch pad data into *** pTempBuf *** which has to be released by the
//      caller!
// return: if pTempBuf != NULL, points to the data with the dataSize of the
//      buffer
// !NULL -- ok
// NULL  -- fail
static char *getScratchPad(int len)
{
    /* Root file system is on a writable NAND flash.  Read scratch pad from
     * a file on the NAND flash.
     */
    char *ret = NULL;

    if( (ret = retriedKmalloc(len)) != NULL )
    {
        struct file *fp;
        mm_segment_t fs;

        memset(ret, 0x00, len);
        fp = filp_open(SCRATCH_PAD_FILE_NAME, O_RDONLY, 0);
        if (!IS_ERR(fp) && fp->f_op && fp->f_op->read)
        {
            fs = get_fs();
            set_fs(get_ds());

            fp->f_pos = 0;

            if((int) fp->f_op->read(fp, (void *) ret, len, &fp->f_pos) <= 0)
                printk("Failed to read scratch pad from '%s'\n",
                    SCRATCH_PAD_FILE_NAME);

            filp_close(fp, NULL);
            set_fs(fs);
        }
    }
    else
        printk("Could not allocate scratch pad memory.\n");

    return( ret );
}

// set scratch pad - write the scratch pad file
// return:
// 0 -- ok
// -1 -- fail
static int setScratchPad(char *buf, int len)
{
    /* Root file system is on a writable NAND flash.  Write PSI to
     * a file on the NAND flash.
     */
    int ret = -1;
    struct file *fp;
    mm_segment_t fs;

    fp = filp_open(SCRATCH_PAD_FILE_NAME, O_RDWR | O_TRUNC | O_CREAT,
        S_IRUSR | S_IWUSR);

    if (!IS_ERR(fp) && fp->f_op && fp->f_op->write)
    {
        fs = get_fs();
        set_fs(get_ds());

        fp->f_pos = 0;

        if((int) fp->f_op->write(fp, (void *) buf, len, &fp->f_pos) == len)
            ret = 0;
        else
            printk("Failed to write scratch pad to '%s'.\n", 
                SCRATCH_PAD_FILE_NAME);

        vfs_fsync(fp, fp->f_path.dentry, 0);
        filp_close(fp, NULL);
        set_fs(fs);
    }
    else
        printk("Unable to open '%s'.\n", SCRATCH_PAD_FILE_NAME);

    return( ret );
}

/*
 * get list of all keys/tokenID's in the scratch pad.
 * NOTE: memcpy work here -- not using copy_from/to_user
 *
 * return:
 *         greater than 0 means number of bytes copied to tokBuf,
 *         0 means fail,
 *         negative number means provided buffer is not big enough and the
 *         absolute value of the negative number is the number of bytes needed.
 */
int kerSysScratchPadList(char *tokBuf, int bufLen)
{
    PSP_TOKEN pToken = NULL;
    char *pBuf = NULL;
    char *pShareBuf = NULL;
    char *startPtr = NULL;
    int usedLen;
    int tokenNameLen=0;
    int copiedLen=0;
    int needLen=0;
    int sts = 0;

    BCM_ASSERT_NOT_HAS_MUTEX_R(&spMutex, 0);

    mutex_lock(&spMutex);

    if( bootFromNand )
    {
        if((pShareBuf = getScratchPad(fInfo.flash_scratch_pad_length)) == NULL) {
            mutex_unlock(&spMutex);
            return sts;
        }

        pBuf = pShareBuf;
    }
    else
    {
        if( (pShareBuf = getSharedBlks(fInfo.flash_scratch_pad_start_blk,
            fInfo.flash_scratch_pad_number_blk)) == NULL )
        {
            printk("could not getSharedBlks.\n");
            mutex_unlock(&spMutex);
            return sts;
        }

        // pBuf points to SP buf
        pBuf = pShareBuf + fInfo.flash_scratch_pad_blk_offset;  
    }

    if(memcmp(((PSP_HEADER)pBuf)->SPMagicNum, MAGIC_NUMBER, MAGIC_NUM_LEN) != 0) 
    {
        printk("Scratch pad is not initialized.\n");
        retriedKfree(pShareBuf);
        mutex_unlock(&spMutex);
        return sts;
    }

    // Walk through all the tokens
    usedLen = sizeof(SP_HEADER);
    startPtr = pBuf + sizeof(SP_HEADER);
    pToken = (PSP_TOKEN) startPtr;

    while( pToken->tokenName[0] != '\0' && pToken->tokenLen > 0 &&
           ((usedLen + pToken->tokenLen) <= fInfo.flash_scratch_pad_length))
    {
        tokenNameLen = strlen(pToken->tokenName);
        needLen += tokenNameLen + 1;
        if (needLen <= bufLen)
        {
            strcpy(&tokBuf[copiedLen], pToken->tokenName);
            copiedLen += tokenNameLen + 1;
        }

        usedLen += ((pToken->tokenLen + 0x03) & ~0x03);
        startPtr += sizeof(SP_TOKEN) + ((pToken->tokenLen + 0x03) & ~0x03);
        pToken = (PSP_TOKEN) startPtr;
    }

    if ( needLen > bufLen )
    {
        // User may purposely pass in a 0 length buffer just to get
        // the size, so don't log this as an error.
        sts = needLen * (-1);
    }
    else
    {
        sts = copiedLen;
    }

    retriedKfree(pShareBuf);

    mutex_unlock(&spMutex);

    return sts;
}

/*
 * get sp data.  NOTE: memcpy work here -- not using copy_from/to_user
 * return:
 *         greater than 0 means number of bytes copied to tokBuf,
 *         0 means fail,
 *         negative number means provided buffer is not big enough and the
 *         absolute value of the negative number is the number of bytes needed.
 */
int kerSysScratchPadGet(char *tokenId, char *tokBuf, int bufLen)
{
    PSP_TOKEN pToken = NULL;
    char *pBuf = NULL;
    char *pShareBuf = NULL;
    char *startPtr = NULL;
    int usedLen;
    int sts = 0;

    mutex_lock(&spMutex);

    if( bootFromNand )
    {
        if((pShareBuf = getScratchPad(fInfo.flash_scratch_pad_length)) == NULL) {
            mutex_unlock(&spMutex);
            return sts;
        }

        pBuf = pShareBuf;
    }
    else
    {
        if( (pShareBuf = getSharedBlks(fInfo.flash_scratch_pad_start_blk,
            fInfo.flash_scratch_pad_number_blk)) == NULL )
        {
            printk("could not getSharedBlks.\n");
            mutex_unlock(&spMutex);
            return sts;
        }

        // pBuf points to SP buf
        pBuf = pShareBuf + fInfo.flash_scratch_pad_blk_offset;
    }

    if(memcmp(((PSP_HEADER)pBuf)->SPMagicNum, MAGIC_NUMBER, MAGIC_NUM_LEN) != 0) 
    {
        printk("Scratch pad is not initialized.\n");
        retriedKfree(pShareBuf);
        mutex_unlock(&spMutex);
        return sts;
    }

    // search for the token
    usedLen = sizeof(SP_HEADER);
    startPtr = pBuf + sizeof(SP_HEADER);
    pToken = (PSP_TOKEN) startPtr;
    while( pToken->tokenName[0] != '\0' && pToken->tokenLen > 0 &&
        pToken->tokenLen < fInfo.flash_scratch_pad_length &&
        usedLen < fInfo.flash_scratch_pad_length )
    {

        if (strncmp(pToken->tokenName, tokenId, TOKEN_NAME_LEN) == 0)
        {
            if ( pToken->tokenLen > bufLen )
            {
               // User may purposely pass in a 0 length buffer just to get
               // the size, so don't log this as an error.
               // printk("The length %d of token %s is greater than buffer len %d.\n", pToken->tokenLen, pToken->tokenName, bufLen);
                sts = pToken->tokenLen * (-1);
            }
            else
            {
                memcpy(tokBuf, startPtr + sizeof(SP_TOKEN), pToken->tokenLen);
                sts = pToken->tokenLen;
            }
            break;
        }

        usedLen += ((pToken->tokenLen + 0x03) & ~0x03);
        startPtr += sizeof(SP_TOKEN) + ((pToken->tokenLen + 0x03) & ~0x03);
        pToken = (PSP_TOKEN) startPtr;
    }

    retriedKfree(pShareBuf);

    mutex_unlock(&spMutex);

    return sts;
}

// set sp.  NOTE: memcpy work here -- not using copy_from/to_user
// return:
//  0 - ok
//  -1 - fail
int kerSysScratchPadSet(char *tokenId, char *tokBuf, int bufLen)
{
    PSP_TOKEN pToken = NULL;
    char *pShareBuf = NULL;
    char *pBuf = NULL;
    SP_HEADER SPHead;
    SP_TOKEN SPToken;
    char *curPtr;
    int sts = -1;

    if( bufLen >= fInfo.flash_scratch_pad_length - sizeof(SP_HEADER) -
        sizeof(SP_TOKEN) )
    {
        printk("Scratch pad overflow by %d bytes.  Information not saved.\n",
            bufLen  - fInfo.flash_scratch_pad_length - sizeof(SP_HEADER) -
            sizeof(SP_TOKEN));
        return sts;
    }

    mutex_lock(&spMutex);

    if( bootFromNand )
    {
        if((pShareBuf = getScratchPad(fInfo.flash_scratch_pad_length)) == NULL)
        {
            mutex_unlock(&spMutex);
            return sts;
        }

        pBuf = pShareBuf;
    }
    else
    {
        if( (pShareBuf = getSharedBlks( fInfo.flash_scratch_pad_start_blk,
            fInfo.flash_scratch_pad_number_blk)) == NULL )
        {
            mutex_unlock(&spMutex);
            return sts;
        }

        // pBuf points to SP buf
        pBuf = pShareBuf + fInfo.flash_scratch_pad_blk_offset;  
    }

    // form header info.
    memset((char *)&SPHead, 0, sizeof(SP_HEADER));
    memcpy(SPHead.SPMagicNum, MAGIC_NUMBER, MAGIC_NUM_LEN);
    SPHead.SPVersion = SP_VERSION;

    // form token info.
    memset((char*)&SPToken, 0, sizeof(SP_TOKEN));
    strncpy(SPToken.tokenName, tokenId, TOKEN_NAME_LEN - 1);
    SPToken.tokenLen = bufLen;

    if(memcmp(((PSP_HEADER)pBuf)->SPMagicNum, MAGIC_NUMBER, MAGIC_NUM_LEN) != 0)
    {
        // new sp, so just flash the token
        printk("No scratch pad found.  Initialize scratch pad...\n");
        memcpy(pBuf, (char *)&SPHead, sizeof(SP_HEADER));
        curPtr = pBuf + sizeof(SP_HEADER);
        memcpy(curPtr, (char *)&SPToken, sizeof(SP_TOKEN));
        curPtr += sizeof(SP_TOKEN);
        if( tokBuf )
            memcpy(curPtr, tokBuf, bufLen);
    }
    else  
    {
        int putAtEnd = 1;
        int curLen;
        int usedLen;
        int skipLen;

        /* Calculate the used length. */
        usedLen = sizeof(SP_HEADER);
        curPtr = pBuf + sizeof(SP_HEADER);
        pToken = (PSP_TOKEN) curPtr;
        skipLen = (pToken->tokenLen + 0x03) & ~0x03;
        while( pToken->tokenName[0] >= 'A' && pToken->tokenName[0] <= 'z' &&
            strlen(pToken->tokenName) < TOKEN_NAME_LEN &&
            pToken->tokenLen > 0 &&
            pToken->tokenLen < fInfo.flash_scratch_pad_length &&
            usedLen < fInfo.flash_scratch_pad_length )
        {
            usedLen += sizeof(SP_TOKEN) + skipLen;
            curPtr += sizeof(SP_TOKEN) + skipLen;
            pToken = (PSP_TOKEN) curPtr;
            skipLen = (pToken->tokenLen + 0x03) & ~0x03;
        }

        if( usedLen + SPToken.tokenLen + sizeof(SP_TOKEN) >
            fInfo.flash_scratch_pad_length )
        {
            printk("Scratch pad overflow by %d bytes.  Information not saved.\n",
                (usedLen + SPToken.tokenLen + sizeof(SP_TOKEN)) -
                fInfo.flash_scratch_pad_length);
            mutex_unlock(&spMutex);
            return sts;
        }

        curPtr = pBuf + sizeof(SP_HEADER);
        curLen = sizeof(SP_HEADER);
        while( curLen < usedLen )
        {
            pToken = (PSP_TOKEN) curPtr;
            skipLen = (pToken->tokenLen + 0x03) & ~0x03;
            if (strncmp(pToken->tokenName, tokenId, TOKEN_NAME_LEN) == 0)
            {
                // The token id already exists.
                if( tokBuf && pToken->tokenLen == bufLen )
                {
                    // The length of the new data and the existing data is the
                    // same.  Overwrite the existing data.
                    memcpy((curPtr+sizeof(SP_TOKEN)), tokBuf, bufLen);
                    putAtEnd = 0;
                }
                else
                {
                    // The length of the new data and the existing data is
                    // different.  Shift the rest of the scratch pad to this
                    // token's location and put this token's data at the end.
                    char *nextPtr = curPtr + sizeof(SP_TOKEN) + skipLen;
                    int copyLen = usedLen - (curLen+sizeof(SP_TOKEN) + skipLen);
                    memcpy( curPtr, nextPtr, copyLen );
                    memset( curPtr + copyLen, 0x00, 
                        fInfo.flash_scratch_pad_length - (curLen + copyLen) );
                    usedLen -= sizeof(SP_TOKEN) + skipLen;
                }
                break;
            }

            // get next token
            curPtr += sizeof(SP_TOKEN) + skipLen;
            curLen += sizeof(SP_TOKEN) + skipLen;
        } // end while

        if( putAtEnd )
        {
            if( tokBuf )
            {
                memcpy( pBuf + usedLen, &SPToken, sizeof(SP_TOKEN) );
                memcpy( pBuf + usedLen + sizeof(SP_TOKEN), tokBuf, bufLen );
            }
            memcpy( pBuf, &SPHead, sizeof(SP_HEADER) );
        }

    } // else if not new sp

    if( bootFromNand )
        sts = setScratchPad(pShareBuf, fInfo.flash_scratch_pad_length);
    else
        sts = setSharedBlks(fInfo.flash_scratch_pad_start_blk, 
            fInfo.flash_scratch_pad_number_blk, pShareBuf);
    
    retriedKfree(pShareBuf);

    mutex_unlock(&spMutex);

    return sts;

    
}

// wipe out the scratchPad
// return:
//  0 - ok
//  -1 - fail
int kerSysScratchPadClearAll(void)
{ 
    int sts = -1;
    char *pShareBuf = NULL;
   int j ;
   int usedBlkSize = 0;

   // printk ("kerSysScratchPadClearAll.... \n") ;
   mutex_lock(&spMutex);

    if( bootFromNand )
    {
        if((pShareBuf = getScratchPad(fInfo.flash_scratch_pad_length)) == NULL)
        {
            mutex_unlock(&spMutex);
            return sts;
        }

        memset(pShareBuf, 0x00, fInfo.flash_scratch_pad_length);

        setScratchPad(pShareBuf, fInfo.flash_scratch_pad_length);
    }
    else
    {
        if( (pShareBuf = getSharedBlks( fInfo.flash_scratch_pad_start_blk,
            fInfo.flash_scratch_pad_number_blk)) == NULL )
        {
            mutex_unlock(&spMutex);
            return sts;
        }

        if (fInfo.flash_scratch_pad_number_blk == 1)
            memset(pShareBuf + fInfo.flash_scratch_pad_blk_offset, 0x00, fInfo.flash_scratch_pad_length) ;
        else
        {
            for (j = fInfo.flash_scratch_pad_start_blk;
                j < (fInfo.flash_scratch_pad_start_blk + fInfo.flash_scratch_pad_number_blk);
                j++)
            {
                usedBlkSize += flash_get_sector_size((unsigned short) j);
            }

            memset(pShareBuf, 0x00, usedBlkSize) ;
        }

        sts = setSharedBlks(fInfo.flash_scratch_pad_start_blk,    
            fInfo.flash_scratch_pad_number_blk,  pShareBuf);
    }

   retriedKfree(pShareBuf);

   mutex_unlock(&spMutex);

   //printk ("kerSysScratchPadClearAll Done.... \n") ;
   return sts;
}

int kerSysFlashSizeGet(void)
{
    int ret = 0;

#if 1 //__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
#else
    if( bootFromNand )
    {
        struct mtd_info *mtd;

        if( (mtd = get_mtd_device_nm("rootfs")) != NULL )
        {
            ret = mtd->size;
            put_mtd_device(mtd);
        }
    }
    else
#endif
        ret = flash_get_total_size();

    return ret;
}

#if 1 //__VERIZON__, Allen
#if !defined(CONFIG_BRCM_IKOS)
int kerSysEraseFlash(unsigned long eraseaddr, unsigned long len)
{
    int blk;
    int bgnBlk = flash_get_blk(eraseaddr);
    int endBlk = flash_get_blk(eraseaddr + len);
    unsigned long bgnAddr = (unsigned long) flash_get_memptr(bgnBlk);
    unsigned long endAddr = (unsigned long) flash_get_memptr(endBlk);

#ifdef DEBUG_FLASH
    printk("kerSysEraseFlash blk[%d] eraseaddr[0x%08x] len[%lu]\n",
    bgnBlk, (int)eraseaddr, len);
#endif

    if ( bgnAddr != eraseaddr)
    {
       printk(KERN_ERR "ERROR: kerSysEraseFlash eraseaddr[0x%08x]"
              " != first block start[0x%08x]\n",
              (int)eraseaddr, (int)bgnAddr);
        return (len);
    }

    if ( (endAddr - bgnAddr) != len)
    {
        printk(KERN_ERR "ERROR: kerSysEraseFlash eraseaddr[0x%08x] + len[%lu]"
               " != last+1 block start[0x%08x]\n",
               (int)eraseaddr, len, (int) endAddr);
        return (len);
    }

    for (blk=bgnBlk; blk<endBlk; blk++)
        flash_sector_erase_int(blk);

    return 0;
}



unsigned long kerSysReadFromFlash( void *toaddr, unsigned long fromaddr,
    unsigned long len )
{
    int blk, offset, bytesRead;
    unsigned long blk_start;
    char * trailbyte = (char*) NULL;
    char val[2];

    blk = flash_get_blk((int)fromaddr);	/* sector in which fromaddr falls */
    blk_start = (unsigned long)flash_get_memptr(blk); /* sector start address */
    offset = (int)(fromaddr - blk_start); /* offset into sector */

#ifdef DEBUG_FLASH
    printk("kerSysReadFromFlash blk[%d] fromaddr[0x%08x]\n",
           blk, (int)fromaddr);
#endif

    bytesRead = 0;

        /* cfiflash : hardcoded for bankwidths of 2 bytes. */
    if ( offset & 1 )   /* toaddr is not 2 byte aligned */
    {
        flash_read_buf(blk, offset-1, val, 2);
        *((char*)toaddr) = val[1];

        toaddr = (void*)((char*)toaddr+1);
        fromaddr += 1;
        len -= 1;
        bytesRead = 1;

        /* if len is 0 we could return here, avoid this if */

        /* recompute blk and offset, using new fromaddr */
        blk = flash_get_blk(fromaddr);
        blk_start = (unsigned long)flash_get_memptr(blk);
        offset = (int)(fromaddr - blk_start);
    }

        /* cfiflash : hardcoded for len of bankwidths multiples. */
    if ( len & 1 )
    {
        len -= 1;
        trailbyte = (char *)toaddr + len;
    }

        /* Both len and toaddr will be 2byte aligned */
    if ( len )
    {
       flash_read_buf(blk, offset, toaddr, len);
       bytesRead += len;
    }

        /* write trailing byte */
    if ( trailbyte != (char*) NULL )
    {
        fromaddr += len;
        blk = flash_get_blk(fromaddr);
        blk_start = (unsigned long)flash_get_memptr(blk);
        offset = (int)(fromaddr - blk_start);
        flash_read_buf(blk, offset, val, 2 );
        *trailbyte = val[0];
        bytesRead += 1;
    }

    return( bytesRead );
}

/*
 * Function: kerSysWriteToFlash
 *
 * Description:
 * This function assumes that the area of flash to be written was
 * previously erased. An explicit erase is therfore NOT needed 
 * prior to a write. This function ensures that the offset and len are
 * two byte multiple. [cfiflash hardcoded for bankwidth of 2 byte].
 *
 * Parameters:
 *      toaddr : destination flash memory address
 *      fromaddr: RAM memory address containing data to be written
 *      len : non zero bytes to be written
 * Return:
 *      FAILURE: number of bytes remaining to be written
 *      SUCCESS: 0 (all requested bytes were written)
 */
int kerSysWriteToFlash( unsigned long toaddr,
                        void * fromaddr, unsigned long len)
{
    int blk, offset, size, blk_size, bytesWritten;
    unsigned long blk_start;
    char * trailbyte = (char*) NULL;
    unsigned char val[2];

#ifdef DEBUG_FLASH
    printk("kerSysWriteToFlash flashAddr[0x%08x] fromaddr[0x%08x] len[%lu]\n",
    (int)toaddr, (int)fromaddr, len);
#endif

    blk = flash_get_blk(toaddr);	/* sector in which toaddr falls */
    blk_start = (unsigned long)flash_get_memptr(blk); /* sector start address */
    offset = (int)(toaddr - blk_start);	/* offset into sector */

	/* cfiflash : hardcoded for bankwidths of 2 bytes. */
    if ( offset & 1 )	/* toaddr is not 2 byte aligned */
    {
        val[0] = 0xFF; // ignored
        val[1] = *((char *)fromaddr); /* write the first byte */
        bytesWritten = flash_write_buf(blk, offset-1, val, 2);
        if ( bytesWritten != 2 )
        {
#ifdef DEBUG_FLASH
           printk("ERROR kerSysWriteToFlash ... remaining<%lu>\n", len); 
#endif
           return len;
        }

	toaddr += 1;
        fromaddr = (void*)((char*)fromaddr+1);
        len -= 1;

	/* if len is 0 we could return bytesWritten, avoid this if */

	/* recompute blk and offset, using new toaddr */
        blk = flash_get_blk(toaddr);
        blk_start = (unsigned long)flash_get_memptr(blk);
        offset = (int)(toaddr - blk_start);
    }

	/* cfiflash : hardcoded for len of bankwidths multiples. */
    if ( len & 1 )
    {
	/* need to handle trailing byte seperately */
        len -= 1;
        trailbyte = (char *)fromaddr + len;
        toaddr += len;
    }

	/* Both len and toaddr will be 2byte aligned */
    while ( len > 0 )
    {
        blk_size = flash_get_sector_size(blk);
        size = blk_size - offset; /* space available in sector from offset */
        if ( size > len )
            size = len;

        bytesWritten = flash_write_buf(blk, offset, fromaddr, size); 
        if ( bytesWritten !=  size )
        {
#ifdef DEBUG_FLASH
           printk("ERROR kerSysWriteToFlash ... remaining<%lu>\n", 
               (len - bytesWritten + ((trailbyte == (char*)NULL)? 0 : 1)));
#endif
           return (len - bytesWritten + ((trailbyte == (char*)NULL)? 0 : 1));
        }

        fromaddr += size;
        len -= size;

        blk++;	/* Move to the next block */
        offset = 0; /* All further blocks will be written at offset 0 */
    }

	/* write trailing byte */
    if ( trailbyte != (char*) NULL )
    {
        blk = flash_get_blk(toaddr);
        blk_start = (unsigned long)flash_get_memptr(blk);
        offset = (int)(toaddr - blk_start);
        val[0] = *trailbyte; /* trailing byte */
        val[1] = 0xFF; // ignored
        bytesWritten = flash_write_buf(blk, offset, val, 2 );
        if ( bytesWritten != 2 )
        {
#ifdef DEBUG_FLASH
           printk("ERROR kerSysWriteToFlash ... remaining<%d>\n",1);
#endif
           return 1;
        }
    } 

    return len;
}
/*
 * Function: kerSysWriteToFlashREW
 * 
 * Description:
 * This function does not assume that the area of flash to be written was erased.
 * An explicit erase is therfore needed prior to a write.  
 * kerSysWriteToFlashREW uses a sector copy  algorithm. The first and last sectors
 * may need to be first read if they are not fully written. This is needed to
 * avoid the situation that there may be some valid data in the sector that does
 * not get overwritten, and would be erased.
 *
 * Due to run time costs for flash read, optimizations to read only that data
 * that will not be overwritten is introduced.
 *
 * Parameters:
 *	toaddr : destination flash memory address
 *	fromaddr: RAM memory address containing data to be written
 *	len : non zero bytes to be written
 * Return:
 *	FAILURE: number of bytes remaining to be written 
 *	SUCCESS: 0 (all requested bytes were written)
 *
 */
int kerSysWriteToFlashREW( unsigned long toaddr,
                        void * fromaddr, unsigned long len)
{
    int blk, offset, size, blk_size, bytesWritten;
    unsigned long sect_start;
    int mem_sz = 0;
    char * mem_p = (char*)NULL;

#ifdef DEBUG_FLASH
    printk("kerSysWriteToFlashREW flashAddr[0x%08x] fromaddr[0x%08x] len[%lu]\n",
    (int)toaddr, (int)fromaddr, len);
#endif

    blk = flash_get_blk( toaddr );
    sect_start = (unsigned long) flash_get_memptr(blk);
    offset = toaddr - sect_start;

    while ( len > 0 )
    {
        blk_size = flash_get_sector_size(blk);
        size = blk_size - offset; /* space available in sector from offset */

		/* bound size to remaining len in final block */
        if ( size > len )
            size = len;

		/* Entire blk written, no dirty data to read */
        if ( size == blk_size )
        {
            flash_sector_erase_int(blk);

            bytesWritten = flash_write_buf(blk, 0, fromaddr, blk_size);

            if ( bytesWritten != blk_size )
            {
                if ( mem_p != NULL )
                    retriedKfree(mem_p);
                return (len - bytesWritten);	/* FAILURE */
            }
        }
        else
        {
                /* Support for variable sized blocks, paranoia */
            if ( (mem_p != NULL) && (mem_sz < blk_size) )
            {
                retriedKfree(mem_p);	/* free previous temp buffer */
                mem_p = (char*)NULL;
            }

            if ( (mem_p == (char*)NULL)
              && ((mem_p = (char*)retriedKmalloc(blk_size)) == (char*)NULL) )
            {
                printk(KERN_ERR "\tERROR kerSysWriteToFlashREW fail to allocate memory\n");
                return len;
            }
            else
                mem_sz = blk_size;

            if ( offset ) /* First block */
            {
                if ( (offset + size) == blk_size)
                {
                   flash_read_buf(blk, 0, mem_p, offset);
                }
                else
                {  /*
		    *	 Potential for future optimization:
		    * Should have read the begining and trailing portions
		    * of the block. If the len written is smaller than some
		    * break even point.
		    * For now read the entire block ... move on ...
		    */
                   flash_read_buf(blk, 0, mem_p, blk_size);
                }
            }
            else
            {
                /* Read the tail of the block which may contain dirty data*/
                flash_read_buf(blk, len, mem_p+len, blk_size-len );
            }

            flash_sector_erase_int(blk);

            memcpy(mem_p+offset, fromaddr, size); /* Rebuild block contents */

            bytesWritten = flash_write_buf(blk, 0, mem_p, blk_size);

            if ( bytesWritten != blk_size )
            {
                if ( mem_p != (char*)NULL )
                    retriedKfree(mem_p);
                return (len + (blk_size - size) - bytesWritten );
            }
        }

		/* take into consideration that size bytes were copied */
        fromaddr += size;
        toaddr += size;
        len -= size;

        blk++;		/* Move to the next block */
        offset = 0;     /* All further blocks will be written at offset 0 */

    }

    if ( mem_p != (char*)NULL )
        retriedKfree(mem_p);

    return ( len );
}

#endif
#endif
#if 0  //__MSTC__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11
/***********************************************************************
 * Function Name: writeBootImageState
 * Description  : Persistently sets the state of an image update.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
static int writeBootImageState( int currState, int newState )
{
    int ret = -1;

    if(bootFromNand == 0)
    {
        /* NOR flash */
        char *pShareBuf = NULL;

        if( (pShareBuf = getSharedBlks( fInfo.flash_scratch_pad_start_blk,
            fInfo.flash_scratch_pad_number_blk)) != NULL )
        {
            PSP_HEADER pHdr = (PSP_HEADER) pShareBuf;
            unsigned long *pBootImgState=(unsigned long *)&pHdr->NvramData2[0];

            /* The boot image state is stored as a word in flash memory where
             * the most significant three bytes are a "magic number" and the
             * least significant byte is the state constant.
             */
            if( (*pBootImgState & 0xffffff00) == (BLPARMS_MAGIC & 0xffffff00) )
            {
                *pBootImgState &= ~0x000000ff;
                *pBootImgState |= newState;

                ret = setSharedBlks(fInfo.flash_scratch_pad_start_blk,    
                    fInfo.flash_scratch_pad_number_blk,  pShareBuf);
            }

            retriedKfree(pShareBuf);
        }
    }
    else
    {
        mm_segment_t fs;

        /* NAND flash */
        char state_name_old[] = "/data/" NAND_BOOT_STATE_FILE_NAME;
        char state_name_new[] = "/data/" NAND_BOOT_STATE_FILE_NAME;

        fs = get_fs();
        set_fs(get_ds());

        if( currState == -1 )
        {
            /* Create new state file name. */
            struct file *fp;

            state_name_new[strlen(state_name_new) - 1] = newState;
            fp = filp_open(state_name_new, O_RDWR | O_TRUNC | O_CREAT,
                S_IRUSR | S_IWUSR);

            if (!IS_ERR(fp))
            {
                fp->f_pos = 0;
                fp->f_op->write(fp, (void *) "boot state\n",
                    strlen("boot state\n"), &fp->f_pos);
                vfs_fsync(fp, fp->f_path.dentry, 0);
                filp_close(fp, NULL);
            }
            else
                printk("Unable to open '%s'.\n", PSI_FILE_NAME);
        }
        else
        {
            if( currState != newState )
            {
                /* Rename old state file name to new state file name. */
                state_name_old[strlen(state_name_old) - 1] = currState;
                state_name_new[strlen(state_name_new) - 1] = newState;
                sys_rename(state_name_old, state_name_new);
            }
        }

        set_fs(fs);
        ret = 0;
    }

    return( ret );
}

/***********************************************************************
 * Function Name: readBootImageState
 * Description  : Reads the current boot image state from flash memory.
 * Returns      : state constant or -1 for failure
 ***********************************************************************/
static int readBootImageState( void )
{
    int ret = -1;

    if(bootFromNand == 0)
    {
        /* NOR flash */
        char *pShareBuf = NULL;

        if( (pShareBuf = getSharedBlks( fInfo.flash_scratch_pad_start_blk,
            fInfo.flash_scratch_pad_number_blk)) != NULL )
        {
            PSP_HEADER pHdr = (PSP_HEADER) pShareBuf;
            unsigned long *pBootImgState=(unsigned long *)&pHdr->NvramData2[0];

            /* The boot image state is stored as a word in flash memory where
             * the most significant three bytes are a "magic number" and the
             * least significant byte is the state constant.
             */
            if( (*pBootImgState & 0xffffff00) == (BLPARMS_MAGIC & 0xffffff00) )
            {
                ret = *pBootImgState & 0x000000ff;
            }

            retriedKfree(pShareBuf);
        }
    }
    else
    {
        /* NAND flash */
        int i;
        char states[] = {BOOT_SET_NEW_IMAGE, BOOT_SET_OLD_IMAGE,
            BOOT_SET_NEW_IMAGE_ONCE};
        char boot_state_name[] = "/data/" NAND_BOOT_STATE_FILE_NAME;

        /* The boot state is stored as the last character of a file name on
         * the data partition, /data/boot_state_X, where X is
         * BOOT_SET_NEW_IMAGE, BOOT_SET_OLD_IMAGE or BOOT_SET_NEW_IMAGE_ONCE.
         */
        for( i = 0; i < sizeof(states); i++ )
        {
            struct file *fp;
            boot_state_name[strlen(boot_state_name) - 1] = states[i];
            fp = filp_open(boot_state_name, O_RDONLY, 0);
            if (!IS_ERR(fp) )
            {
                filp_close(fp, NULL);

                ret = (int) states[i];
                break;
            }
        }

        if( ret == -1 && writeBootImageState( -1, BOOT_SET_NEW_IMAGE ) == 0 )
            ret = BOOT_SET_NEW_IMAGE;
    }

    return( ret );
}

/***********************************************************************
 * Function Name: dirent_rename
 * Description  : Renames file oldname to newname by parsing NAND flash
 *                blocks with JFFS2 file system entries.  When the JFFS2
 *                directory entry inode for oldname is found, it is modified
 *                for newname.  This differs from a file system rename
 *                operation that creates a new directory entry with the same
 *                inode value and greater version number.  The benefit of
 *                this method is that by having only one directory entry
 *                inode, the CFE ROM can stop at the first occurance which
 *                speeds up the boot by not having to search the entire file
 *                system partition.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
static int dirent_rename(char *oldname, char *newname)
{
    int ret = -1;
    struct mtd_info *mtd = get_mtd_device_nm("rootfs_update"); 
    unsigned char *buf = (mtd) ? retriedKmalloc(mtd->erasesize) : NULL;

    if( mtd && buf && strlen(oldname) == strlen(newname) )
    {
        int namelen = strlen(oldname);
        int blk, done, retlen;

        /* Read NAND flash blocks in the rootfs_update JFFS2 file system
         * partition to search for a JFFS2 directory entry for the oldname
         * file.
         */
        for(blk = 0, done = 0; blk < mtd->size && !done; blk += mtd->erasesize)
        {
            if(mtd->read(mtd, blk, mtd->erasesize, &retlen, buf) == 0)
            {
                struct jffs2_raw_dirent *pdir;
                unsigned char *p = buf;

                while( p < buf + mtd->erasesize )
                {
                    pdir = (struct jffs2_raw_dirent *) p;
                    if( je16_to_cpu(pdir->magic) == JFFS2_MAGIC_BITMASK )
                    {
                        if( je16_to_cpu(pdir->nodetype) ==
                                JFFS2_NODETYPE_DIRENT &&
                            !memcmp(oldname, pdir->name, namelen) &&
                            je32_to_cpu(pdir->ino) != 0 )
                        {
                            /* Found the oldname directory entry.  Change the
                             * name to newname.
                             */
                            memcpy(pdir->name, newname, namelen);
                            je32_to_cpu(pdir->name_crc) = crc32(0, pdir->name,
                                (unsigned int) namelen);

                            /* Write the modified block back to NAND flash. */
                            if(nandEraseBlkNotSpare( mtd, blk ) == 0)
                            {
                                if( mtd->write(mtd, blk, mtd->erasesize,
                                    &retlen, buf) == 0 )
                                {
                                    ret = 0;
                                }
                            }

                            done = 1;
                            break;
                        }

                        p += (je32_to_cpu(pdir->totlen) + 0x03) & ~0x03;
                    }
                    else
                        break;
                }
            }
        }
    }
    else
        printk("%s: Error renaming file\n", __FUNCTION__);

    if( mtd )
        put_mtd_device(mtd);

    if( buf )
        retriedKfree(buf);

    return( ret );
}

/***********************************************************************
 * Function Name: updateSequenceNumber
 * Description  : updates the sequence number on the specified partition
 *                to be the highest.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
static int updateSequenceNumber(int incSeqNumPart, int seqPart1, int seqPart2)
{
    int ret = -1;

    mutex_lock(&flashImageMutex);
    if(bootFromNand == 0)
    {
        /* NOR flash */
        char *pShareBuf = NULL;
        PFILE_TAG pTag;
        int blk;

        pTag = kerSysUpdateTagSequenceNumber(incSeqNumPart);
        blk = *(int *) (pTag + 1);

        if ((pShareBuf = getSharedBlks(blk, 1)) != NULL)
        {
            memcpy(pShareBuf, pTag, sizeof(FILE_TAG));
            setSharedBlks(blk, 1, pShareBuf);
            retriedKfree(pShareBuf);
        }
    }
    else
    {
        /* NAND flash */

        /* The sequence number on NAND flash is updated by renaming file
         * cferam.XXX where XXX is the sequence number. The rootfs partition
         * is usually read-only. Therefore, only the cferam.XXX file on the
         * rootfs_update partiton is modified. Increase or decrase the
         * sequence number on the rootfs_update partition so the desired
         * partition boots.
         */
        int seq = -1;
        unsigned long rootfs_ofs = 0xff;

        kerSysBlParmsGetInt(NAND_RFS_OFS_NAME, (int *) &rootfs_ofs);

        if(rootfs_ofs == inMemNvramData.ulNandPartOfsKb[NP_ROOTFS_1] )
        {
            /* Partition 1 is the booted partition. */
            if( incSeqNumPart == 2 )
            {
                /* Increase sequence number in rootfs_update partition. */
                if( seqPart1 >= seqPart2 )
                    seq = seqPart1 + 1;
            }
            else
            {
                /* Decrease sequence number in rootfs_update partition. */
                if( seqPart2 >= seqPart1 && seqPart1 != 0 )
                    seq = seqPart1 - 1;
            }
        }
        else
        {
            /* Partition 2 is the booted partition. */
            if( incSeqNumPart == 1 )
            {
                /* Increase sequence number in rootfs_update partition. */
                if( seqPart2 >= seqPart1 )
                    seq = seqPart2 + 1;
            }
            else
            {
                /* Decrease sequence number in rootfs_update partition. */
                if( seqPart1 >= seqPart2 && seqPart2 != 0 )
                    seq = seqPart2 - 1;
            }
        }

        if( seq != -1 )
        {
            /* Find the sequence number of the non-booted partition. */
            mm_segment_t fs;

            fs = get_fs();
            set_fs(get_ds());
            if( sys_mount("mtd:rootfs_update", "/mnt", "jffs2", 0, NULL) == 0 )
            {
                char fname[] = NAND_CFE_RAM_NAME;
                char cferam_old[32], cferam_new[32], cferam_fmt[32]; 
                int i;

                fname[strlen(fname) - 3] = '\0'; /* remove last three chars */
                strcpy(cferam_fmt, "/mnt/");
                strcat(cferam_fmt, fname);
                strcat(cferam_fmt, "%3.3d");

                for( i = 0; i < 999; i++ )
                {
                    struct file *fp;
                    sprintf(cferam_old, cferam_fmt, i);
                    fp = filp_open(cferam_old, O_RDONLY, 0);
                    if (!IS_ERR(fp) )
                    {
                        filp_close(fp, NULL);

                        /* Change the boot sequence number in the rootfs_update
                         * partition by renaming file cferam.XXX where XXX is
                         * a sequence number.
                         */
                        sprintf(cferam_new, cferam_fmt, seq);
                        if( NAND_FULL_PARTITION_SEARCH )
                        {
                            sys_rename(cferam_old, cferam_new);
                            sys_umount("/mnt", 0);
                        }
                        else
                        {
                            sys_umount("/mnt", 0);
                            dirent_rename(cferam_old + strlen("/mnt/"),
                                cferam_new + strlen("/mnt/"));
                        }
                        break;
                    }
                }
            }
            set_fs(fs);
        }
    }
    mutex_unlock(&flashImageMutex);

    return( ret );
}

/***********************************************************************
 * Function Name: kerSysSetBootImageState
 * Description  : Persistently sets the state of an image update.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
int kerSysSetBootImageState( int newState )
{
    int ret = -1;
    int incSeqNumPart = -1;
    int writeImageState = 0;
    int currState = readBootImageState();
    int seq1 = kerSysGetSequenceNumber(1);
    int seq2 = kerSysGetSequenceNumber(2);

    /* Update the image state persistently using "new image" and "old image"
     * states.  Convert "partition" states to "new image" state for
     * compatibility with the non-OMCI image update.
     */
    mutex_lock(&spMutex);
    switch(newState)
    {
    case BOOT_SET_PART1_IMAGE:
        if( seq1 != -1 )
        {
            if( seq1 < seq2 )
                incSeqNumPart = 1;
            newState = BOOT_SET_NEW_IMAGE;
            writeImageState = 1;
        }
        break;

    case BOOT_SET_PART2_IMAGE:
        if( seq2 != -1 )
        {
            if( seq2 < seq1 )
                incSeqNumPart = 2;
            newState = BOOT_SET_NEW_IMAGE;
            writeImageState = 1;
        }
        break;

    case BOOT_SET_PART1_IMAGE_ONCE:
        if( seq1 != -1 )
        {
            if( seq1 < seq2 )
                incSeqNumPart = 1;
            newState = BOOT_SET_NEW_IMAGE_ONCE;
            writeImageState = 1;
        }
        break;

    case BOOT_SET_PART2_IMAGE_ONCE:
        if( seq2 != -1 )
        {
            if( seq2 < seq1 )
                incSeqNumPart = 2;
            newState = BOOT_SET_NEW_IMAGE_ONCE;
            writeImageState = 1;
        }
        break;

    case BOOT_SET_OLD_IMAGE:
    case BOOT_SET_NEW_IMAGE:
    case BOOT_SET_NEW_IMAGE_ONCE:
        /* The boot image state is stored as a word in flash memory where
         * the most significant three bytes are a "magic number" and the
         * least significant byte is the state constant.
         */
        if( currState == newState )
        {
            ret = 0;
        }
        else
        {
            writeImageState = 1;

            if(newState==BOOT_SET_NEW_IMAGE && currState==BOOT_SET_OLD_IMAGE)
            {
                /* The old (previous) image is being set as the new
                 * (current) image. Make sequence number of the old
                 * image the highest sequence number in order for it
                 * to become the new image.
                 */
                incSeqNumPart = 0;
            }
        }
        break;

    default:
        break;
    }

    if( writeImageState )
        ret = writeBootImageState(currState, newState);

    mutex_unlock(&spMutex);

    if( incSeqNumPart != -1 )
        updateSequenceNumber(incSeqNumPart, seq1, seq2);

    return( ret );
}

/***********************************************************************
 * Function Name: kerSysGetBootImageState
 * Description  : Gets the state of an image update from flash.
 * Returns      : state constant or -1 for failure
 ***********************************************************************/
int kerSysGetBootImageState( void )
{
    int ret = readBootImageState();

    if( ret != -1 )
    {
        int seq1 = kerSysGetSequenceNumber(1);
        int seq2 = kerSysGetSequenceNumber(2);

        switch(ret)
        {
        case BOOT_SET_NEW_IMAGE:
            if( seq1 == -1 || seq1 < seq2 )
                ret = BOOT_SET_PART2_IMAGE;
            else
                ret = BOOT_SET_PART1_IMAGE;
            break;

        case BOOT_SET_NEW_IMAGE_ONCE:
            if( seq1 == -1 || seq1 < seq2 )
                ret = BOOT_SET_PART2_IMAGE_ONCE;
            else
                ret = BOOT_SET_PART1_IMAGE_ONCE;
            break;

        case BOOT_SET_OLD_IMAGE:
            if( seq1 == -1 || seq1 > seq2 )
                ret = BOOT_SET_PART2_IMAGE;
            else
                ret = BOOT_SET_PART1_IMAGE;
            break;

        default:
            ret = -1;
            break;
        }
    }

    return( ret );
}
#endif

/***********************************************************************
 * Function Name: kerSysSetOpticalPowerValues
 * Description  : Saves optical power values to flash that are obtained
 *                during the  manufacturing process. These values are
 *                stored in NVRAM_DATA which should not be erased.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
int kerSysSetOpticalPowerValues(UINT16 rxReading, UINT16 rxOffset, 
    UINT16 txReading)
{
    NVRAM_DATA nd;

    kerSysNvRamGet((char *) &nd, sizeof(nd), 0);

    nd.opticRxPwrReading = rxReading;
    nd.opticRxPwrOffset  = rxOffset;
    nd.opticTxPwrReading = txReading;
    
    nd.ulCheckSum = 0;
    nd.ulCheckSum = crc32(CRC32_INIT_VALUE, &nd, sizeof(NVRAM_DATA));

    return(kerSysNvRamSet((char *) &nd, sizeof(nd), 0));
}

/***********************************************************************
 * Function Name: kerSysGetOpticalPowerValues
 * Description  : Retrieves optical power values from flash that were
 *                saved during the manufacturing process.
 * Returns      : 0 - success, -1 - failure
 ***********************************************************************/
int kerSysGetOpticalPowerValues(UINT16 *prxReading, UINT16 *prxOffset, 
    UINT16 *ptxReading)
{
    NVRAM_DATA nd;

    kerSysNvRamGet((char *) &nd, sizeof(nd), 0);

    *prxReading = nd.opticRxPwrReading;
    *prxOffset  = nd.opticRxPwrOffset;
    *ptxReading = nd.opticTxPwrReading;

    return(0);
}

