/*
   Copyright (c) 2012 Broadcom Corporation
   All Rights Reserved

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

#ifdef _CFE_ 
#include "lib_types.h"
#include "lib_printf.h"
#include "lib_string.h"
#include "bcm_map.h"  
#if 0//__MTS__, RaynorChung: Support 963268 nand flash, patch form SVN#3597 on http://svn.zyxel.com.tw/svn/CPE_SW1/BCM96368/trunk/P-870HA/branches/cht/fttb8/4.11 
#define  printk  printf
#endif
#else
#include <linux/autoconf.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/spi/spi.h>

#include <bcm_map_part.h>
#include <bcm_intr.h>
#endif
#include "bcmSpiRes.h"
#include "bcmSpi.h"

/* if HS_SPI is defined then the HS SPI controller is available, otherwise do not compile this code */
#ifdef HS_SPI
int BcmHsSpiRead(const unsigned char *pTxBuf, unsigned char *pRxBuf, int prependcnt,
                  int nbytes, int devId, int freqHz, int ctrlState );
int BcmHsSpiWrite(const unsigned char * msg_buf, int nbytes, int devId, int freqHz, int ctrlState);
int BcmHsSpiMultibitRead( struct spi_transfer *xfer, int devId, int ctrlState);
unsigned int BcmHsSpiGetMaxRWSize( int bAutoXfer );
int BcmHsSpiSetFlashCtrl( int opCode, int addrBytes, int dummyBytes, int busNum,
                          int devId, int clockHz, int multibitEn );
int BcmHsSpiSetup(int spiMode, int ctrlState);

#define HS_SPI_STATE_CLOCK_POLARITY   (1 << 31)
#define HS_SPI_STATE_GATE_CLOCK_SSOFF (1 << 30)
#define HS_SPI_STATE_LAUNCH_RISING    (1 << 29)
#define HS_SPI_STATE_LATCH_RISING     (1 << 28)
#define HS_SPI_STATE_ASYNC_CLOCK      (1 << 27)
#define HS_SPI_STATE_ONE_WIRE         (1 << 26)
#if defined(_BCM96816_) || defined(CONFIG_BCM96816)
#define HS_SPI_CONTROLLER_STATE_DEF    (HS_SPI_STATE_GATE_CLOCK_SSOFF)
#else
#define HS_SPI_CONTROLLER_STATE_DEF    (HS_SPI_STATE_GATE_CLOCK_SSOFF | HS_SPI_STATE_LATCH_RISING)
#endif

#define HS_SPI_PROFILE_MM     0
#define HS_SPI_PROFILE_PP0    1
#define HS_SPI_PROFILE_PP1    2

#ifndef _CFE_
/* current controller requires multibit mode to support 3-wire devices, define USE_MULTIBIT_FOR_3WIRE to enable 3-wire support */
//#define USE_MULTIBIT_FOR_3WIRE  
static struct bcmspi BcmHsSpi = { SPIN_LOCK_UNLOCKED, };
#else
#define udelay(X)                        \
    do { { int i; for( i = 0; i < (X) * 500; i++ ) ; } } while(0)
#endif

#if defined(_BCM96816_) || defined(CONFIG_BCM96816) || defined(_BCM96818_) || defined(CONFIG_BCM96818) || defined(_BCM96362_) || defined(CONFIG_BCM96362) || defined(_BCM963268_) || defined(CONFIG_BCM963268) || defined(_BCM96828_) || defined(CONFIG_BCM96828)
#define HS_SPI_PLL_FREQ     400000000
#elif defined(_BCM96318_) || defined(CONFIG_BCM96318)
#define HS_SPI_PLL_FREQ     (250*1000*1000)
#else
#define HS_SPI_PLL_FREQ     133333333
#endif

#define HS_SPI_BUFFER_LEN         512
#define HS_SPI_MSG_CTRL_LEN       2
#define HS_SPI_PREPEND_CNT_MAX    15
#define HS_SPI_MAX_TRANSFER_SIZE  0xFFFFFFFF /* no limit */
int hsSpiMaxRW = HS_SPI_BUFFER_LEN - HS_SPI_MSG_CTRL_LEN;

static int hsSpiReadP0( const unsigned char *pRxBuf, int prependcnt, int nbytes, int devId, int multiEn )
{
   uint16 msgCtrl;

   msgCtrl = (HS_SPI_OP_READ<<HS_SPI_OP_CODE) | nbytes;
   if (multiEn)
   {
      msgCtrl |= (1<<11);
   }
   HS_SPI_FIFO0[0] = (msgCtrl >> 8) & 0xFF;
   HS_SPI_FIFO0[1] = (msgCtrl >> 0) & 0xFF;
   
   if ( 0 != prependcnt )
   {
      memcpy((char *)(HS_SPI_FIFO0+2), (char *)pRxBuf, prependcnt);
   }

   HS_SPI_PINGPONG0->command = devId<<HS_SPI_SS_NUM | 
                               HS_SPI_PROFILE_PP0<<HS_SPI_PROFILE_NUM | 
                               0<<HS_SPI_TRIGGER_NUM | 
                               HS_SPI_COMMAND_START_NOW<<HS_SPI_COMMAND_VALUE;
   return SPI_STATUS_OK;

}

static int hsSpiWriteP0( const unsigned char *pTxBuf, int nbytes, int devId )
{
   uint16 msgCtrl;

   msgCtrl  = (HS_SPI_OP_WRITE<<HS_SPI_OP_CODE) | nbytes;
   HS_SPI_FIFO0[0] = (msgCtrl >> 8) & 0xFF;
   HS_SPI_FIFO0[1] = (msgCtrl >> 0) & 0xFF;

   memcpy((char *)(HS_SPI_FIFO0+2), (void *)pTxBuf, nbytes);

   HS_SPI_PINGPONG0->command = devId<<HS_SPI_SS_NUM | 
                               HS_SPI_PROFILE_PP0<<HS_SPI_PROFILE_NUM | 
                               0<<HS_SPI_TRIGGER_NUM | 
                               HS_SPI_COMMAND_START_NOW<<HS_SPI_COMMAND_VALUE;

   return SPI_STATUS_OK;

}

static int hsSpiTransEnd( unsigned char *rxBuf, int nbytes, 
                          int pingpong, int bitRedux )
{
#if defined(USE_MULTIBIT_FOR_3WIRE)
   unsigned short data;
   unsigned char  realData;
   unsigned char  rxData[HS_SPI_BUFFER_LEN];
   int            bit;
   int            i;
#endif

   if ( NULL != rxBuf )
   {
#if defined(USE_MULTIBIT_FOR_3WIRE)
      if ( bitRedux )
      {
         nbytes <<=1;
         if (0 == pingpong)
         {
            memcpy((char *)&rxData[0], (void *)HS_SPI_FIFO0, nbytes);
         }
         else
         {
            memcpy((char *)&rxData[0], (void *)HS_SPI_FIFO1, nbytes);
         }

         /* MOSI line is the single wire - even bits */
         for (i = 0; i < nbytes; i += 2 )
         {
            data     = rxData[i+1] | (rxData[i] << 8);
            realData = 0;
            for ( bit = 0; bit < 16; bit += 2)
            {
               realData |= ((data >> bit) & 0x1) << (bit >> 1);
            }
            rxBuf[i>>1] = realData;
         }
      }
      else
#endif
      {
         if (0 == pingpong)
         {
            memcpy((char *)rxBuf, (void *)HS_SPI_FIFO0, nbytes);
         }
         else
         {
            memcpy((char *)rxBuf, (void *)HS_SPI_FIFO1, nbytes);
         }
      }
   }
   
   return SPI_STATUS_OK;
    
}

static int hsSpiTransPoll(int pingpong)
{
   unsigned int wait;

   for (wait = (100*1000); wait>0; wait--)
   {
      if ( 0 == pingpong )
      {
         if (!(HS_SPI_PINGPONG0->status & (1<<HS_SPI_SOURCE_BUSY)))
         {
            break;
         }
      }
      else
      {
         if (!(HS_SPI_PINGPONG1->status & (1<<HS_SPI_SOURCE_BUSY)))
         {
            break;
         }
      }
      udelay(1);
   }

   if (wait == 0)
   {
      return SPI_STATUS_ERR;
   }
    
   return SPI_STATUS_OK;
}

static void hsSpiSetProfile(unsigned int ctrlState, unsigned int clockHz, int modeCtrl, unsigned int profile )
{
   unsigned int temp32;
   unsigned int clock;

   /* set clock for profile */
   clock = HS_SPI_PLL_FREQ/clockHz;
   if (HS_SPI_PLL_FREQ%clockHz)
   {
      clock++;
   }

   clock = 2048/clock;
   if (2048%(clock))
   {
      clock++;
   }

   HS_SPI_PROFILES[profile].clk_ctrl = 1<<HS_SPI_ACCUM_RST_ON_LOOP | 0<<HS_SPI_SPI_CLK_2X_SEL | clock<<HS_SPI_FREQ_CTRL_WORD;
   
   /* set signal control */
   temp32 = HS_SPI_PROFILES[profile].signal_ctrl;
   if ( 0 == (ctrlState & HS_SPI_STATE_LATCH_RISING) )
   {
       temp32 &= ~HS_SPI_LATCH_RISING;
   }
   else
   {
      temp32 |= HS_SPI_LATCH_RISING;      
   }
   if ( 0 == (ctrlState & HS_SPI_STATE_LAUNCH_RISING) )
   {
       temp32 &= ~HS_SPI_LAUNCH_RISING;
   }
   else
   {
      temp32 |= HS_SPI_LAUNCH_RISING;      
   }

#if defined(_BCM96328_) || defined(CONFIG_BCM96328) || defined(_BCM96362_) || defined(CONFIG_BCM96362) || defined(_BCM963268_) || defined(CONFIG_BCM963268) || defined(_BCM96828_) || defined(CONFIG_BCM96828) || defined(_BCM96818_) || defined(CONFIG_BCM96818) || defined(_BCM96318_) || defined(CONFIG_BCM96318)
   if ( 0 == (ctrlState & HS_SPI_STATE_ASYNC_CLOCK) )
   {
       temp32 &= ~HS_SPI_ASYNC_INPUT_PATH;
   }
   else
   {
      temp32 |= HS_SPI_ASYNC_INPUT_PATH;      
   }
#endif
   HS_SPI_PROFILES[profile].signal_ctrl = temp32;

   /* set mode control */
   
   temp32 = modeCtrl | 0xFF;
#ifndef USE_MULTIBIT_FOR_3WIRE 
   if ( 0 == (ctrlState & HS_SPI_STATE_ONE_WIRE) )
   {
       temp32 &= ~(1<<HS_SPI_MODE_ONE_WIRE);
   }
   else
   {
      temp32 |= (1<<HS_SPI_MODE_ONE_WIRE);
   }
#endif
   HS_SPI_PROFILES[profile].mode_ctrl = temp32;
}


static void hsSpiSetGlobalState(unsigned int ctrlState)
{
   unsigned int temp32;

   temp32 = HS_SPI->hs_spiGlobalCtrl;
   if ( 0 == (ctrlState & HS_SPI_STATE_GATE_CLOCK_SSOFF) )
   {
       temp32 &= ~HS_SPI_CLK_GATE_SSOFF;
   }
   else
   {
      temp32 |= HS_SPI_CLK_GATE_SSOFF;
   }

#if defined(_BCM96328_) || defined(CONFIG_BCM96328) || defined(_BCM96362_) || defined(CONFIG_BCM96362) || defined(_BCM963268_) || defined(CONFIG_BCM963268) || defined(_BCM96828_) || defined(CONFIG_BCM96828) || defined(_BCM96818_) || defined(CONFIG_BCM96818) || defined(_BCM96318_) || defined(CONFIG_BCM96318)
   if ( 0 == (ctrlState & HS_SPI_STATE_CLOCK_POLARITY) )
   {
       temp32 &= ~HS_SPI_CLK_POLARITY;
   }
   else
   {
      temp32 |= HS_SPI_CLK_POLARITY;
   }
#endif
   /* write value if required */
   if ( temp32 != HS_SPI->hs_spiGlobalCtrl )
   {
      HS_SPI->hs_spiGlobalCtrl = temp32;
   }
}


/* BcmHsSpiMultibitRead, BcmHsSpiRead and BcmHsSpiWrite are availble for the 
   CFE and spi flash driver only the linux kernel spi framework must be used
   in all other cases */
int BcmHsSpiMultibitRead( struct spi_transfer *xfer, int devId, int ctrlState )
{
   unsigned int   modeCtrl;
#ifndef _CFE_
   struct bcmspi *pBcmSpi = &BcmHsSpi;

   spin_lock(&pBcmSpi->lock);
#endif
   hsSpiSetGlobalState(ctrlState);

   modeCtrl = ((xfer->prepend_cnt & 0xF)<<HS_SPI_PREPENDBYTE_CNT);
   if ( xfer->multi_bit_en )
   {
      modeCtrl |= (1<<HS_SPI_MULTIDATA_RD_SIZE);
      modeCtrl |= (xfer->multi_bit_start_offset << HS_SPI_MULTIDATA_RD_STRT);
   }
   
   hsSpiSetProfile(ctrlState, xfer->speed_hz, modeCtrl, 
                   HS_SPI_PROFILE_PP0);
   hsSpiReadP0(xfer->tx_buf, (xfer->prepend_cnt & 0xF), xfer->len, devId, xfer->multi_bit_en );
   hsSpiTransPoll(0);
   hsSpiTransEnd(xfer->rx_buf, xfer->len, 0, 0);
#ifndef _CFE_
    spin_unlock(&pBcmSpi->lock);
#endif

    return( SPI_STATUS_OK );
}

int BcmHsSpiRead( const unsigned char *pTxBuf, unsigned char *pRxBuf, int prependcnt,
                  int nbytes, int devId, int freqHz, int ctrlState )
{
   struct spi_transfer xfer;

   xfer.tx_buf                 = pTxBuf;
   xfer.rx_buf                 = pRxBuf;
   xfer.len                    = nbytes;
   xfer.prepend_cnt            = prependcnt;
   xfer.speed_hz               = freqHz;
   xfer.multi_bit_en           = 0;
   xfer.multi_bit_start_offset = 0;
   
   return(BcmHsSpiMultibitRead(&xfer, devId, ctrlState));
}

int BcmHsSpiWrite( const unsigned char *msg_buf, int nbytes, int devId, int freqHz, int ctrlState )
{
#ifndef _CFE_
    struct bcmspi *pBcmSpi = &BcmHsSpi;

    spin_lock(&pBcmSpi->lock);
#endif
    hsSpiSetGlobalState(ctrlState);
    hsSpiSetProfile(ctrlState, freqHz, 0, HS_SPI_PROFILE_PP0);
    hsSpiWriteP0(msg_buf, nbytes, devId);
    hsSpiTransPoll(0);
#ifndef _CFE_
    spin_unlock(&pBcmSpi->lock);
#endif

    return( SPI_STATUS_OK );
}

unsigned int BcmHsSpiGetMaxRWSize( int bAutoXfer )
{
   /* the transfer length is limited by contrtollers fifo size
      however, the controller driver is capable of breaking a transfer
      down into smaller chunks, appending a header to each chunk and
      increment an address in the header. If the device supports this,
      bAutoXfer will be set to 1 and this call will return an appropriate
      maximum transaction size. If the device does not support this then
      the maximum transaction size is limited by the fifo size */
   if ( bAutoXfer )
   {
      return hsSpiMaxRW;
   }
   else
   {
      return HS_SPI_BUFFER_LEN - HS_SPI_MSG_CTRL_LEN;
   }
}

int BcmHsSpiSetup(int spiMode, int ctrlState)
{
   int hsSpiCtrlState = 0;

   if ((ctrlState & SPI_CONTROLLER_STATE_MASK) != ctrlState)
      return SPI_STATUS_ERR;

   if (ctrlState & SPI_CONTROLLER_STATE_ASYNC_CLOCK )
      hsSpiCtrlState |= HS_SPI_STATE_ASYNC_CLOCK;

   if (ctrlState & SPI_CONTROLLER_STATE_GATE_CLK_SSOFF)
      hsSpiCtrlState |= HS_SPI_STATE_GATE_CLOCK_SSOFF;

   if ( SPI_CPOL == (spiMode & SPI_CPOL)  )
   {
      hsSpiCtrlState |= HS_SPI_STATE_CLOCK_POLARITY;
   }
  
   /* note that when CPOL is set the meaning of latch and launch changes */
   if( 0 == (ctrlState & SPI_CONTROLLER_STATE_CPHA_EXT) )
   {
      if ( 0 == (spiMode & SPI_CPHA) )
      {
         /* CPOL=0 -> latch data on rising edge, launch data on falling edge 
            CPOL=1 -> latch data on falling edge, launch data on rising edge */
         hsSpiCtrlState |= HS_SPI_STATE_LATCH_RISING;
         
      }
      else
      {
         /* CPOL=0 -> latch data on falling edge, launch data on rising edge
            CPOL=1 -> latch data on rising edge, launch data on falling edge */
         hsSpiCtrlState |= HS_SPI_STATE_LAUNCH_RISING;
      }
   }
   else
   {
      if ( 0 == (spiMode & SPI_CPHA) )
      {
         /* CPOL=0 -> latch data on rising edge, launch data on rising edge 
            CPOL=1 -> latch data on falling edge, launch data on falling edge */
         hsSpiCtrlState |= (HS_SPI_STATE_LATCH_RISING | 
                            HS_SPI_STATE_LAUNCH_RISING);
      }
      /* else - CPOL=0 -> latch data on rising edge, launch data on rising edge 
                CPOL=1 -> latch data on rising edge, launch data on rising edge */
   }

   return hsSpiCtrlState;
}

int BcmHsSpiSetFlashCtrl( int opCode, int addrBytes, int dummyBytes, int busNum,
                          int devId, int clockHz, int multibitEn )
{
   int clock;

   clock = HS_SPI_PLL_FREQ/clockHz;
   if (HS_SPI_PLL_FREQ%clockHz)
   {
      clock++;
   }

   clock = 2048/clock;
   if (2048%(clock))
   {
      clock++; 
   }

   HS_SPI_PROFILES[0].clk_ctrl = 1<<HS_SPI_ACCUM_RST_ON_LOOP | 
                                 0<<HS_SPI_SPI_CLK_2X_SEL | 
                                 clock<<HS_SPI_FREQ_CTRL_WORD;
   HS_SPI->hs_spiFlashCtrl     = devId<<HS_SPI_FCTRL_SS_NUM | 
                                 0<<HS_SPI_FCTRL_PROFILE_NUM | 
                                 dummyBytes<<HS_SPI_FCTRL_DUMMY_BYTES | 
                                 addrBytes<<HS_SPI_FCTRL_ADDR_BYTES | 
                                 opCode<<HS_SPI_FCTRL_READ_OPCODE | 
                                 multibitEn<<HS_SPI_FCTRL_MB_ENABLE;
   return SPI_STATUS_OK;
}

#ifndef _CFE_
static int hsSpiSetup(struct spi_device *spi)
{
   struct bcmspi *pBcmSpi;
   unsigned int   spiCtrlData;
   unsigned int   spiCtrlState = 0;

   pBcmSpi = spi_master_get_devdata(spi->master);

   if (pBcmSpi->stopping)
      return -ESHUTDOWN;

   spiCtrlData = (unsigned int)spi->controller_data;
   if ( 0 == spiCtrlData )
   {
      spiCtrlState = HS_SPI_CONTROLLER_STATE_DEF;
   }
   else
   {
        spiCtrlState = BcmHsSpiSetup(spi->mode, spiCtrlData);
   }

   spi_set_ctldata(spi, (void *)spiCtrlState);
 
   return 0;
}

void hsSpiStartXfer(struct bcmspi *pBcmSpi)
{
   unsigned int            ctrlState;
   unsigned int            modeCtrl;
   struct bcmspi_xferInfo *pSpiXfer;
   unsigned short          msgCtrl;
   unsigned short          msgLen;
   int                     index;
   unsigned short          dataLen;
   volatile uint8         *pFifo;
   int i;

   while(1)
   {
      if (0 == pBcmSpi->pingProgNext)
      {
         /* need to wait for the transfer on pingpong 0 to finish */
         if ( 1 == pBcmSpi->ping0Started )
         {
            return;
         }
      }
      else
      {
         /* need to wait for the transfer on pingpong 1 to finish */
         if ( 1 == pBcmSpi->ping1Started )
         {
            return;
         }
      }

      ctrlState = (unsigned int)spi_get_ctldata(pBcmSpi->pCurMsg->spi);
      pSpiXfer  = &pBcmSpi->spiXfer[pBcmSpi->xferIdx];

      /* no transfers started or the last transfer finished */
      if (0 == pSpiXfer->remTxLen)
      {
         struct spi_transfer *pXfer;

         pXfer = pSpiXfer->pCurXfer;
         if (NULL == pXfer)
         {
            /* get the first transfer from the message */
            pXfer = list_entry(pBcmSpi->pCurMsg->transfers.next, struct spi_transfer, transfer_list);
         }
         else
         {
            /* get next transfer in the message */
            if (pBcmSpi->pCurMsg->transfers.prev == &pXfer->transfer_list)
            {
               /* no more transfers in the message */
               return;
            }
            pXfer = list_entry(pXfer->transfer_list.next, struct spi_transfer, transfer_list);
            pBcmSpi->xferIdx = (pBcmSpi->xferIdx) ? 0 : 1;;
            pSpiXfer = &pBcmSpi->spiXfer[pBcmSpi->xferIdx];
         }

         pSpiXfer->pCurXfer    = pXfer;
         pSpiXfer->rxBuf       = pXfer->rx_buf;
         pSpiXfer->txBuf       = pXfer->tx_buf;
         pSpiXfer->pHdr        = (unsigned char *)pXfer->tx_buf;
         pSpiXfer->delayUsecs  = pXfer->delay_usecs;
         if (pSpiXfer->rxBuf)
         {
            pSpiXfer->remRxLen = pXfer->len;
         }
         pSpiXfer->remTxLen    = pXfer->len;

         if (pXfer->hdr_len)
         {
            pSpiXfer->prependCnt = pXfer->hdr_len;
            pSpiXfer->addrLen    = pXfer->addr_len;
            pSpiXfer->addrOffset = pXfer->addr_offset;
            pSpiXfer->addr       = 0;

            /* if addrLen is non 0 then the header contains an address and 
               we need to increment after each transfer */
            if (0 != pSpiXfer->addrLen)
            {
               memcpy(&pSpiXfer->header[0], (char *)pXfer->tx_buf, pSpiXfer->prependCnt);
               pSpiXfer->pHdr = &pSpiXfer->header[0];
               for ( i=0; i<pSpiXfer->addrLen; i++ )
               {
                  if (i)
                  {
                     pSpiXfer->addr <<= 8;
                  }
                  pSpiXfer->addr  |= pSpiXfer->pHdr[pSpiXfer->addrOffset+i];
               }
            }
            pSpiXfer->txBuf += pSpiXfer->prependCnt;
         }
         else
         {
            pSpiXfer->prependCnt = pXfer->prepend_cnt;
         }

         if ( pXfer->prepend_cnt )
         {
            pSpiXfer->msgType  = (HS_SPI_OP_READ<<HS_SPI_OP_CODE);
         }
         else
         {
            if ( (NULL != pSpiXfer->rxBuf) && (NULL != pSpiXfer->txBuf) )
            {
               pSpiXfer->msgType = (HS_SPI_OP_READ_WRITE<<HS_SPI_OP_CODE);
            }
            else if ( NULL != pSpiXfer->rxBuf )
            {
               pSpiXfer->msgType  = (HS_SPI_OP_READ<<HS_SPI_OP_CODE);
            }
            else
            {
               pSpiXfer->msgType  = (HS_SPI_OP_WRITE<<HS_SPI_OP_CODE);
            }
         }

         /* determine how much data can be transfer each message */
         if ((HS_SPI_OP_READ<<HS_SPI_OP_CODE) == pSpiXfer->msgType)
         {
            /* can use the whole fifo for read data */
            pSpiXfer->maxLen = HS_SPI_BUFFER_LEN;
         }
         else
         {
            /* tx requires 2 bytes for message control data */
            pSpiXfer->maxLen  = (HS_SPI_BUFFER_LEN - HS_SPI_MSG_CTRL_LEN);
         }

         if (pXfer->unit_size)
         {
            if ( pSpiXfer->msgType != (HS_SPI_OP_READ<<HS_SPI_OP_CODE) )
            {
               pSpiXfer->maxLen -= pSpiXfer->prependCnt;
            }
            pSpiXfer->maxLen /= pXfer->unit_size;
            pSpiXfer->maxLen *= pXfer->unit_size;
         }

         pSpiXfer->bitRedux = 0;
#if defined(USE_MULTIBIT_FOR_3WIRE)
         if ( (ctrlState & HS_SPI_STATE_ONE_WIRE) &&
              (pSpiXfer->msgType == (HS_SPI_OP_READ<<HS_SPI_OP_CODE)) )
         {
            /* need to use multibit mode for 3-wire reads */
            pSpiXfer->bitRedux = 1;
         }
#endif
      }

      /* all information required for transfer is in pSpiXfer */

      //printk("len %d, pre %d, tx %p, rx %p, remTx %d, remRx %d, type %x, maxLen = %d, addr %x\n", pSpiXfer->pCurXfer->len,
      //        pSpiXfer->pCurXfer->prepend_cnt, pSpiXfer->pCurXfer->tx_buf, pSpiXfer->pCurXfer->rx_buf ,
      //        pSpiXfer->remTxLen, pSpiXfer->remRxLen, pSpiXfer->msgType, pSpiXfer->maxLen, pSpiXfer->addr);

      /* determine how much data we need to request/transmit
         this does not count the header tx */
      if ( pSpiXfer->remTxLen > pSpiXfer->maxLen )
      {
         dataLen = pSpiXfer->maxLen;
      }
      else
      {
         dataLen = pSpiXfer->remTxLen;
      }

      modeCtrl = 0;
      msgCtrl  = pSpiXfer->msgType;
      msgLen   = dataLen;
#if defined(USE_MULTIBIT_FOR_3WIRE)
      if ( 1 == pSpiXfer->bitRedux )
      {
         /* need to use multibit mode for 3-wire reads */
         msgCtrl  |= (1<<11);
         modeCtrl |= (1<<HS_SPI_MULTIDATA_RD_SIZE);
         modeCtrl |= (pSpiXfer->prependCnt << HS_SPI_MULTIDATA_RD_STRT);
         msgLen    = (dataLen << 1);
      }
#endif

      if (0 == pBcmSpi->pingProgNext)
      {
         pFifo = (volatile uint8 *)(HS_SPI_FIFO0);
         pBcmSpi->ping0Xfer = pBcmSpi->xferIdx;
      }
      else
      {
         pFifo = (volatile uint8 *)(HS_SPI_FIFO1);
         pBcmSpi->ping1Xfer = pBcmSpi->xferIdx;
      }

      if ( 1 == pSpiXfer->pCurXfer->multi_bit_en )
      {
         msgCtrl |= (1<<11);
         if (pSpiXfer->msgType == (HS_SPI_OP_READ<<HS_SPI_OP_CODE))
         {
            modeCtrl |= (1<<HS_SPI_MULTIDATA_RD_SIZE);
            modeCtrl |= (pSpiXfer->pCurXfer->multi_bit_start_offset << HS_SPI_MULTIDATA_RD_STRT);
         }
         else if (pSpiXfer->msgType == (HS_SPI_OP_WRITE<<HS_SPI_OP_CODE))
         {
            modeCtrl |= (1<<HS_SPI_MULTIDATA_WR_SIZE);
            modeCtrl |= (pSpiXfer->pCurXfer->multi_bit_start_offset << HS_SPI_MULTIDATA_WR_STRT);
         }
      }

      if (pSpiXfer->msgType == (HS_SPI_OP_READ<<HS_SPI_OP_CODE))
      {
         msgCtrl  |= msgLen;
         pFifo[0]  = (msgCtrl >> 8) & 0xFF;
         pFifo[1]  = (msgCtrl >> 0) & 0xFF;
         index     = 2;

         /* copy the prepend bytes to the FIFO */
         if ( pSpiXfer->prependCnt )
         {
            modeCtrl |= ((pSpiXfer->prependCnt & 0xF)<<HS_SPI_PREPENDBYTE_CNT);
            memcpy((char *)(pFifo + index), (char *)pSpiXfer->pHdr, pSpiXfer->prependCnt);
            index += pSpiXfer->prependCnt;
         }
      }
      else
      {
         msgCtrl  |= (msgLen+pSpiXfer->prependCnt);
         pFifo[0]  = (msgCtrl >> 8) & 0xFF;
         pFifo[1]  = (msgCtrl >> 0) & 0xFF;
         index     = 2;

         /* copy the header bytes to the FIFO */
         if ( pSpiXfer->prependCnt )
         {
            memcpy((char *)(pFifo + index), (char *)pSpiXfer->pHdr, pSpiXfer->prependCnt);
            index += pSpiXfer->prependCnt;
         }
         memcpy((char *)(pFifo + index), (char *)pSpiXfer->txBuf, dataLen);
         index += dataLen;
      }

      if (0 == pBcmSpi->pingProgNext)
      {
         /* inticate that a transfer has started on this pingpong
            and that the next transfer could be written to the next pingpong */
         pBcmSpi->ping0Started = 1;

         /* configure profile for this device */
         hsSpiSetProfile(ctrlState, pSpiXfer->pCurXfer->speed_hz, modeCtrl, HS_SPI_PROFILE_PP0);

         /* kick off the transfer */
         HS_SPI_PINGPONG0->command = (pBcmSpi->pCurMsg->spi->chip_select<<HS_SPI_SS_NUM) | 
                                     (HS_SPI_PROFILE_PP0<<HS_SPI_PROFILE_NUM) | 
                                     (0<<HS_SPI_TRIGGER_NUM) | 
                                     (HS_SPI_COMMAND_START_NOW<<HS_SPI_COMMAND_VALUE);
      }
      else
      {
         /* inticate that a transfer has started on this pingpong
            and that the next transfer could be written to the next pingpong */
         pBcmSpi->ping1Started = 1;

         /* configure profile for this device */
         hsSpiSetProfile(ctrlState, pSpiXfer->pCurXfer->speed_hz, modeCtrl, HS_SPI_PROFILE_PP1);

         /* kick off the transfer */
         HS_SPI_PINGPONG1->command = (pBcmSpi->pCurMsg->spi->chip_select<<HS_SPI_SS_NUM) | 
                                     (HS_SPI_PROFILE_PP1<<HS_SPI_PROFILE_NUM) | 
                                     (0<<HS_SPI_TRIGGER_NUM) | 
                                     (HS_SPI_COMMAND_START_NOW<<HS_SPI_COMMAND_VALUE);
      }

      pSpiXfer->txBuf    += dataLen;
      pSpiXfer->remTxLen -= dataLen;

      /* if all data was transmitted/requested and a delay is required
         do not program the next transfer */
      if ((0 == pSpiXfer->remTxLen) && (0 == pSpiXfer->delayUsecs))
      {
         pBcmSpi->pingProgNext = (0 == pBcmSpi->pingProgNext) ? 1 : 0;
      }

      if ( pSpiXfer->prependCnt )
      {
         /* increment address and update the header */
         if ( pSpiXfer->addrLen )
         {
            pSpiXfer->addr += dataLen;
            for (i=0; i < pSpiXfer->addrLen; i++)
            {
               pSpiXfer->pHdr[pSpiXfer->addrOffset+pSpiXfer->addrLen-i-1] = (pSpiXfer->addr >>(i*8)) & 0xFF;
            }
         }
      }
      // continue so that a second message can be started
   }

   return;
}

int hsSpiProcMsg(struct bcmspi *pBcmSpi )
{
   unsigned long           flags;
   unsigned int            ctrlState;
   struct bcmspi_xferInfo *pSpiXfer;
   int                     dataLen;

   /* the following code is written such that any thread can finish
      any transfer. This means the code does not need to disable
      preemption or interrupts while waiting for a transfer to complete. 
      The outer while loop feeds messages into the innner loop */
   while ( 1 )
   {
      spin_lock_irqsave(&pBcmSpi->lock, flags);
      if (NULL == pBcmSpi->pCurMsg)
      {
         /* if list is empty or stop flag is set just return */
         if (list_empty(&pBcmSpi->queue) || pBcmSpi->stopping)
         {
            /* nothing to process or we need to stop */
            spin_unlock_irqrestore(&pBcmSpi->lock, flags);
            break;
         }
         else
         {
            pBcmSpi->pCurMsg = list_entry(pBcmSpi->queue.next, 
                                          struct spi_message, queue);
            ctrlState = (unsigned int)spi_get_ctldata(pBcmSpi->pCurMsg->spi);
            hsSpiSetGlobalState(ctrlState);

            pBcmSpi->xferIdx      = 0;
            pBcmSpi->pingProgNext = 0;
            pBcmSpi->pingEndNext  = 0;
            memset(&pBcmSpi->spiXfer[0], 0, sizeof(struct bcmspi_xferInfo)*2);

            /* start the next transfer */
            hsSpiStartXfer(pBcmSpi);
         }
      }
      spin_unlock_irqrestore(&pBcmSpi->lock, flags);
     
      /* process all transfers in the message */
      while (1)
      {
         spin_lock_irqsave(&pBcmSpi->lock, flags);
         if ( NULL == pBcmSpi->pCurMsg )
         {
            spin_unlock_irqrestore(&pBcmSpi->lock, flags);
            /* break out of inner while loop */
            break;
         }
         
         /* wait for transfer to complete */
         if ( 0 == pBcmSpi->pingEndNext )
         {
            /* check to see if controller is busy */
            if ( (HS_SPI_PINGPONG0->status & (1<<HS_SPI_SOURCE_BUSY)) )
            {
               /* controller is busy - continue */
               spin_unlock_irqrestore(&pBcmSpi->lock, flags);
               continue;
            }
            else
            {
               pSpiXfer = &pBcmSpi->spiXfer[pBcmSpi->ping0Xfer];
               pBcmSpi->ping0Started = 0;

               /* copy data if required */
               if ( pSpiXfer->rxBuf )
               {
                  if ( pSpiXfer->remRxLen > pSpiXfer->maxLen )
                  {
                     dataLen = pSpiXfer->maxLen;
                  }
                  else
                  {
                     dataLen = pSpiXfer->remRxLen;
                  }

                  hsSpiTransEnd( pSpiXfer->rxBuf, dataLen, 0, pSpiXfer->bitRedux);
                  pSpiXfer->rxBuf    += dataLen;
                  pSpiXfer->remRxLen -= dataLen;
               }
            }
         }
         else
         {
            /* check to see if controller is busy */
            if ( (HS_SPI_PINGPONG1->status & (1<<HS_SPI_SOURCE_BUSY)) )
            {
               /* controller is busy - continue */
               spin_unlock_irqrestore(&pBcmSpi->lock, flags);
               continue;
            }
            else
            {
               pSpiXfer = &pBcmSpi->spiXfer[pBcmSpi->ping1Xfer];
               pBcmSpi->ping1Started = 0;

               /* copy data if required */              
               if ( pSpiXfer->rxBuf )
               {
                  if ( pSpiXfer->remRxLen > pSpiXfer->maxLen )
                  {
                     dataLen = pSpiXfer->maxLen;
                  }
                  else
                  {
                     dataLen = pSpiXfer->remRxLen;
                  }

                  hsSpiTransEnd( pSpiXfer->rxBuf, dataLen, 1, pSpiXfer->bitRedux);
                  pSpiXfer->rxBuf    += dataLen;
                  pSpiXfer->remRxLen -= dataLen;
               }
            }
         }

         /* if all data was transmitted/requested and a delay is required
            the next xfer will use the same pingpong 
            otherwise do not modify pingEndNext and execute the delay */
         if ((0 == pSpiXfer->remTxLen) && (0 == pSpiXfer->remTxLen))
         {
            if (0 == pSpiXfer->delayUsecs)
            {
               pBcmSpi->pingEndNext = (0 == pBcmSpi->pingEndNext) ? 1 : 0;
            }
            else
            {
               udelay(pSpiXfer->delayUsecs);
            }
         }

         if ((pBcmSpi->pCurMsg->transfers.prev == &pSpiXfer->pCurXfer->transfer_list) &&
             (0==pSpiXfer->remRxLen) && (0==pSpiXfer->remTxLen))
         { 
            /* processing the last transfer in the message and all data 
               for the transfer has been pushed to the controller. */
            if ((0 == pBcmSpi->ping0Started) && (0 == pBcmSpi->ping1Started))
            {
               /* all transfers for the current message have been processed */
               list_del(&(pBcmSpi->pCurMsg->queue));
               pBcmSpi->pCurMsg->status = SPI_STATUS_OK;
               pBcmSpi->pCurMsg  = NULL;
               spin_unlock_irqrestore(&pBcmSpi->lock, flags);

               /* the message may specify a callback
                  the callback will be called when hsSpiProcMsg returns
                  to ensure that it is called from the original
                  callers context */

               /* break out of inner while loop */
               break;
            }
            else
            {
               /* no more transfers to start, need to wait for last 
                  transfer to finish */
               spin_unlock_irqrestore(&pBcmSpi->lock, flags);
               continue;
            }
         }
         
         /* start next transfer */
         hsSpiStartXfer(pBcmSpi);
         spin_unlock_irqrestore(&pBcmSpi->lock, flags);
      }
   }

   return SPI_STATUS_OK;
}


static int hsSpiTransfer(struct spi_device *spi, struct spi_message *msg)
{
   struct bcmspi          *pBcmSpi = &BcmHsSpi;
   struct spi_transfer    *xfer;
   int                     xferCnt;
   int                     bCsChange;
   int                     rejectMsg;
   unsigned long           flags;
   int                     retVal;

   if (unlikely(list_empty(&msg->transfers)))
   {
      return -EINVAL;
   }
  
   if (pBcmSpi->stopping)
   {
      return -ESHUTDOWN;  
   }
  
   xferCnt      = 0;
   bCsChange    = 0;
   rejectMsg    = 0;
   list_for_each_entry(xfer, &msg->transfers, transfer_list)
   {
      if ( rejectMsg )
      {
         return -EINVAL;
      }

      /* check transfer parameters */
      if (!(xfer->tx_buf || xfer->rx_buf))
      {
         return -EINVAL;
      }

      if ( ((xfer->len > (HS_SPI_BUFFER_LEN - HS_SPI_MSG_CTRL_LEN)) && 
            (0 == xfer->unit_size)) ||
           (xfer->prepend_cnt > HS_SPI_PREPEND_CNT_MAX) ||
           (xfer->hdr_len > HS_SPI_PREPEND_CNT_MAX)
#if defined(USE_MULTIBIT_FOR_3WIRE)
            || ((spi->mode & SPI_3WIRE) && 
                (xfer->len > ((HS_SPI_BUFFER_LEN - HS_SPI_MSG_CTRL_LEN)>>1)) &&
                (0 == xfer->unit_size))
#endif
         )
      {
         printk("Invalid length for transfer\n");
         return -EINVAL;
      }

      if ( (NULL == xfer->tx_buf) && (xfer->prepend_cnt > 0))
      {
         printk("Prepend count specified with NULL transmit buffer\n");
         return -EINVAL;
      }

      /* check the clock setting - if it is 0 then set to max clock of the device */
      if ( 0 == xfer->speed_hz )
      {
         if ( 0 == spi->max_speed_hz )
         {
             return -EINVAL;
         }
         xfer->speed_hz = spi->max_speed_hz;
      }
 
      xferCnt++;
      bCsChange |= xfer->cs_change;

      /* controller does not support keeping the chip select active
         across multiple transfers. if this is not the last transfer
         then reject message if cs_change is not set */
      if ( 0 == xfer->cs_change )
      {
         rejectMsg = 1;
      }
   }
   msg->status        = -EINPROGRESS;
   msg->actual_length = 0;

   /* add the message to the queue */
   spin_lock_irqsave(&pBcmSpi->lock, flags);
   list_add_tail(&msg->queue, &pBcmSpi->queue);
   spin_unlock_irqrestore(&pBcmSpi->lock, flags);

   retVal = hsSpiProcMsg(pBcmSpi);
   if (SPI_STATUS_OK == retVal)
   {
      /* the callback is made here to ensure that it is made from
         the callers context */   
      if ( msg->complete )
      {
         msg->complete(msg->context);
      }
   }

   return retVal;

}

static void hsSpiCleanup(struct spi_device *spi)
{
    /* would free spi_controller memory here if any was allocated */

}

static int __init hsSpiProbe(struct platform_device *pdev)
{
    int                ret;
    struct spi_master *master;
    struct bcmspi     *pBcmSpi;    

    ret = -ENOMEM;
    master = spi_alloc_master(&pdev->dev, 0);
    if (!master)
    {
        goto out_free;
    }

    master->bus_num        = pdev->id;
    master->num_chipselect = 8;
    master->setup          = hsSpiSetup;
    master->transfer       = hsSpiTransfer;
    master->cleanup        = hsSpiCleanup;
    platform_set_drvdata(pdev, master);

    spi_master_set_devdata(master, (void *)&BcmHsSpi);
    pBcmSpi = spi_master_get_devdata(master);

    INIT_LIST_HEAD(&pBcmSpi->queue);
    pBcmSpi->pdev           = pdev;
    pBcmSpi->bus_num        = HS_SPI_BUS_NUM;
    pBcmSpi->num_chipselect = 8;

    /* Initialize the hardware */
    HS_SPI->hs_spiIntMask   = 0;
    HS_SPI->hs_spiIntStatus = HS_SPI->hs_spiIntStatus;

    /* register and we are done */
    ret = spi_register_master(master);
    if (ret)
    {
        goto out_free;
    }

    return 0;

out_free:  
    spi_master_put(master);  
    
    return ret;
}


static int __exit hsSpiRemove(struct platform_device *pdev)
{
    struct spi_master   *master  = platform_get_drvdata(pdev);
    struct bcmspi       *pBcmSpi = spi_master_get_devdata(master);
    struct spi_message  *msg;

    /* reset the hardware and block queue progress */
    spin_lock_bh(&pBcmSpi->lock);
    pBcmSpi->stopping = 1;

    /* HW shutdown */
    
    spin_unlock_bh(&pBcmSpi->lock);

    /* Terminate remaining queued transfers */
    list_for_each_entry(msg, &pBcmSpi->queue, queue)
    {
        list_del(&msg->queue);
        msg->status = -ESHUTDOWN;
        if ( msg->complete )
           msg->complete(msg->context);
    } 

    spi_unregister_master(master);

    return 0;
}

//#ifdef CONFIG_PM
#if 0
static int hsSpiSuspend(struct platform_device *pdev, pm_message_t mesg)
{
    struct spi_master *master = platform_get_drvdata(pdev);
    struct bcmspi     *pBcmSpi     = spi_master_get_devdata(master);

    return 0;
}

static int hsSpiResume(struct platform_device *pdev)
{
    struct spi_master *master = platform_get_drvdata(pdev);
    struct bcmspi     *pBcmSpi     = spi_master_get_devdata(master);

    return 0;
}
#else
#define   hsSpiSuspend   NULL
#define   hsSpiResume    NULL
#endif

static struct platform_device bcm_hsspi_device = {
    .name        = "bcmhs_spi",
    .id          = HS_SPI_BUS_NUM,
};

static struct platform_driver bcm_hsspi_driver = {
    .driver      =
    {
        .name    = "bcmhs_spi",
        .owner   = THIS_MODULE,
    },
    .suspend    = hsSpiSuspend,
    .resume     = hsSpiResume,
    .remove     = __exit_p(hsSpiRemove),
};

int __init hsSpiModInit( void )
{
   int retVal;

   platform_device_register(&bcm_hsspi_device);
   retVal = platform_driver_probe(&bcm_hsspi_driver, hsSpiProbe);

   hsSpiMaxRW = HS_SPI_MAX_TRANSFER_SIZE;

   return retVal;   

}
subsys_initcall(hsSpiModInit);
#endif /* _CFE_ */

#endif /* HS_SPI */

