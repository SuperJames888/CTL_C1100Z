/***********************************************************
 *
 * Copyright (c) 2009 Broadcom Corporation
 * All Rights Reserved
 *
 * <:label-BRCM:2009:DUAL/GPL:standard
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation (the "GPL").
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * 
 * A copy of the GPL is available at http://www.broadcom.com/licenses/GPLv2.php, or by
 * writing to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 * 
 * :>
 *
 ************************************************************/
#include <linux/module.h>
#include <asm/time.h>
#include <bcm_map_part.h>
#include "board.h"

#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE)
#define CLK_ALIGNMENT_REG   0xff410040
#define KEEPME_MASK         0x00007F00 // bit[14:8]

#if defined (CONFIG_BCM96318)
#define RATIO_ONE_ASYNC     0x0 /* 0b00 */
#define RATIO_ONE_HALF      0x1 /* 0b01 */
#define RATIO_ONE_QUARTER   0x2 /* 0b10 */
#define RATIO_ONE_EIGHTH    0x3 /* 0b11 */

#define MASK_ASCR_BITS 0x3
#define MASK_ASCR_SHFT 23
#define MASK_ASCR (MASK_ASCR_BITS << MASK_ASCR_SHFT)
#else
#define RATIO_ONE_SYNC      0x0 /* 0b000 */
#define RATIO_ONE_ASYNC     0x1 /* 0b001 */
#define RATIO_ONE_HALF      0x3 /* 0b011 */
#define RATIO_ONE_QUARTER   0x5 /* 0b101 */
#define RATIO_ONE_EIGHTH    0x7 /* 0b111 */

#define MASK_ASCR_BITS 0x7
#define MASK_ASCR_SHFT 28
#define MASK_ASCR (MASK_ASCR_BITS << MASK_ASCR_SHFT)
#endif

unsigned int originalMipsAscr = 0; // To keep track whether MIPS was in Async mode to start with at boot time
unsigned int originalMipsAscrChecked = 0;
unsigned int keepme;
#endif

#if defined(CONFIG_BCM_PWRMNGT_MODULE)
#if defined(CONFIG_BCM_DDR_SELF_REFRESH_PWRSAVE)
unsigned int self_refresh_enabled = 0; // Wait for the module to control if it is enabled or not
#endif
#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE)
unsigned int clock_divide_enabled = 0; // Wait for the module to control if it is enabled or not
#endif
#else
#if defined(CONFIG_BCM_DDR_SELF_REFRESH_PWRSAVE)
unsigned int self_refresh_enabled = 1;
#endif
#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE)
unsigned int clock_divide_enabled = 1;
#endif
#endif

unsigned int clock_divide_low_power0 = 0;
unsigned int clock_divide_active0 = 0;
unsigned int wait_count0 = 0;
#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE_TIMERS)
unsigned int TimerC0Snapshot0 = 0;
unsigned int prevTimerCnt0, newTimerCnt0, TimerAdjust0;
#endif

#if defined(CONFIG_SMP)
unsigned int clock_divide_low_power1 = 0;
unsigned int clock_divide_active1 = 0;
unsigned int wait_count1 = 0;
#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE_TIMERS)
unsigned int TimerC0Snapshot1 = 0;
unsigned int prevTimerCnt1, newTimerCnt1, TimerAdjust1;
#endif
#endif

#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE_TIMERS)
unsigned int C0divider, C0multiplier, C0ratio, C0adder;
#endif
extern volatile int isVoiceIdle;
 
spinlock_t pwrmgnt_clk_irqlock = SPIN_LOCK_UNLOCKED;
 
#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE)
/* To put CPU in ASYNC mode and change CPU clock speed */
void __BcmPwrMngtSetASCR(unsigned int freq_div)
{
   register unsigned int temp;
#if defined(CONFIG_BCM96358) || defined(CONFIG_BCM96368) || defined(CONFIG_BCM96328) || defined(CONFIG_BCM96816) || defined(CONFIG_BCM96362)
   register unsigned int cp0_ascr_asc;
   volatile register unsigned int * clk_alignment_reg = (unsigned int *) CLK_ALIGNMENT_REG;

   // A/ SYNC instruction // Step A, SYNC instruction
   asm("sync" : : );

   // B/ CMT mips : set cp0 reg 22 sel 5 bits [30:28] to 001 (RATIO_ONE_ASYNC)
   asm("mfc0 %0,$22,5" : "=d"(cp0_ascr_asc) :);
   if (!originalMipsAscrChecked) {
      originalMipsAscr = cp0_ascr_asc & MASK_ASCR;
      originalMipsAscrChecked = 1;
   }
   cp0_ascr_asc = ( cp0_ascr_asc & ~MASK_ASCR) | (RATIO_ONE_ASYNC << MASK_ASCR_SHFT);
   asm("mtc0 %0,$22,5" : : "d" (cp0_ascr_asc));

   // // These 3 steps (C,D and E) are needed to work around limitations on clock alignment logics [...]
   // C/ read from 0xff410040   ( make sure you set this to volatile first)
   temp = *clk_alignment_reg;    
    
   // D/ save bit[14:8] to some register, then zero out bit [14:8], write back to same address.
   keepme = temp | KEEPME_MASK;
#if defined(CONFIG_BCM96358) || defined(CONFIG_BCM96368)
   temp &= ~KEEPME_MASK;
   *clk_alignment_reg = temp;
#endif
   // E/ SYNC instruction   // Step E SYNC instruction  
   asm("sync" : : );

   // F/ change to 1/2, or 1/4, or 1/8 by setting cp0 sel 5 bits[30:28] (sel 4 bits[24:22] for single core mips)  to 011, 101, or 111 respectively
   // Step F change to 1/2, or 1/4, or 1/8 by setting cp0 bits[30:28]
   asm("mfc0 %0,$22,5" : "=d"(temp) :);
   temp = ( temp & ~MASK_ASCR) | (freq_div << MASK_ASCR_SHFT);
   asm("mtc0 %0,$22,5" : : "d" (temp));

   // Step G/ 16 nops // Was 32 nops
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : ); 
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );
#elif defined(CONFIG_BCM96318)
   asm("sync" : : );
   asm("mfc0 %0,$22,4" : "=d"(temp) :);
   temp = ( temp & ~MASK_ASCR) | (freq_div << MASK_ASCR_SHFT);
   asm("mtc0 %0,$22,4" : : "d" (temp));
#else
   if (freq_div == RATIO_ONE_ASYNC) {
      // Gradually bring the processor speed back to 1:1
      // If it is done in one step, CP0 timer interrupts are missed.

      // E/ SYNC instruction   // Step E SYNC instruction  
      asm("sync" : : );

      // Step F1 change to 1/4
      asm("mfc0 %0,$22,5" : "=d"(temp) :);
      temp = ( temp & ~MASK_ASCR) | (RATIO_ONE_QUARTER << MASK_ASCR_SHFT);
      asm("mtc0 %0,$22,5" : : "d" (temp));

      // Step F2 change to 1/2
      temp = ( temp & ~MASK_ASCR) | (RATIO_ONE_HALF << MASK_ASCR_SHFT);
      asm("mtc0 %0,$22,5" : : "d" (temp));

      // Step F3 change to 1/1, high performance memory access
      temp = ( temp & ~MASK_ASCR);
      asm("mtc0 %0,$22,5" : : "d" (temp));

   } else {
      // E/ SYNC instruction   // Step E SYNC instruction  
      asm("sync" : : );

      // F/ change to 1/2, or 1/4, or 1/8 by setting cp0 sel 5 bits[30:28] (sel 4 bits[24:22] for single core mips)  to 011, 101, or 111 respectively
      // Step F change to 1/2, or 1/4, or 1/8 by setting cp0 bits[30:28]
      asm("mfc0 %0,$22,5" : "=d"(temp) :);
      temp = ( temp & ~MASK_ASCR) | (freq_div << MASK_ASCR_SHFT);
      asm("mtc0 %0,$22,5" : : "d" (temp));
   }
#endif

   return;
} /* BcmPwrMngtSetASCR */

void BcmPwrMngtSetASCR(unsigned int freq_div)
{
   unsigned long flags;

   if (!freq_div) {
      // Can't use this function to set to SYNC mode
      return;
   }

   spin_lock_irqsave(&pwrmgnt_clk_irqlock, flags);
   __BcmPwrMngtSetASCR(freq_div);
   spin_unlock_irqrestore(&pwrmgnt_clk_irqlock, flags);
   return;
} /* BcmPwrMngtSetASCR */
EXPORT_SYMBOL(BcmPwrMngtSetASCR);


/* To put CPU in SYNC mode and change CPU clock speed to 1:1 ratio */
/* No SYNC mode in newer MIPS core, use the __BcmPwrMngtSetASCR with ratio 1:1 instead */
void __BcmPwrMngtSetSCR(void)
{
   register unsigned int cp0_ascr_asc;
#if defined(CONFIG_BCM96358) || defined(CONFIG_BCM96368) || defined(CONFIG_BCM96328) || defined(CONFIG_BCM96816) || defined(CONFIG_BCM96362)
   register unsigned int temp;
   volatile register unsigned int * clk_alignment_reg = (unsigned int *) CLK_ALIGNMENT_REG;
#endif
#if defined(CONFIG_BCM96358) || defined(CONFIG_BCM96368)
   unsigned int i;
#endif

   // It is important to go back to divide by 1 async mode first, don't jump directly from divided clock back to SYNC mode.
   // A/ set cp0 reg 22 sel 5 bits[30:28]  (sel 4 bits[24:22] for single core mips)  to 001
   asm("mfc0 %0,$22,5" : "=d"(cp0_ascr_asc) :);
   if (!originalMipsAscrChecked) {
      originalMipsAscr = cp0_ascr_asc & MASK_ASCR;
      originalMipsAscrChecked = 1;
   }
   if (originalMipsAscr)
      return;
   cp0_ascr_asc = ( cp0_ascr_asc & ~MASK_ASCR) | (RATIO_ONE_ASYNC << MASK_ASCR_SHFT);
   asm("mtc0 %0,$22,5" : : "d" (cp0_ascr_asc));

   // B/ 16 nops // Was 32 nops (wait a while to make sure clk is back to full speed)
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : ); 
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );
   asm("nop" : : ); asm("nop" : : );

   // C/ SYNC instruction
   asm("sync" : : );

#if defined(CONFIG_BCM96358) || defined(CONFIG_BCM96368) || defined(CONFIG_BCM96328) || defined(CONFIG_BCM96816) || defined(CONFIG_BCM96362)
   // This step is needed to work around limitations on clock alignment logics for chips from BCM3368 and before BCM6816.
#if defined(CONFIG_BCM96358) || defined(CONFIG_BCM96368)
   // D/ check for falling edge clock alignment. 
   //     - When bit[22:16] of 0xff410040 is sequence of 0's followed by 1's, then need to write "1" to bit[30],  eg [0011111]
   temp = (*clk_alignment_reg & 0x007F0000) >> 16;
   if (temp == 0x3F || temp == 0x1F || temp == 0x0F || temp == 0x07 || temp == 0x03 || temp == 0x01) {
      *clk_alignment_reg = (*clk_alignment_reg | 0x40000000);
   }
#endif  

   // E/ set bit[14:8] to be 6'b1000001
   *clk_alignment_reg = (*clk_alignment_reg & ~KEEPME_MASK) | 0x4100;

   // F/ repeat
   //  until caught rising edge
#if defined(CONFIG_BCM96358) || defined(CONFIG_BCM96368)
   i = 30;
#endif

   while (1) {
      //  a/ sread bit[22:16] to check rising edge:
      //   - When bit[22:16] of register 0xff410040 shows sequence of 1's (from bit[22]) followed by 0's (to bit[16]) means good alignment. 
      //    eg [1100000] or [1111000]
      temp = (*clk_alignment_reg & 0x007F0000) >> 16;
      if (temp == 0x40 || temp == 0x60 || temp == 0x70 || temp == 0x78 || temp == 0x7C || temp == 0x7E) {
        break;
      }

#if defined(CONFIG_BCM96358) || defined(CONFIG_BCM96368)
      //  b/ every 30 loops, check falling edge(same as step D above), write "1" to bit[30] if falling edge detected
      if (--i == 0) {
         i = 30;
         temp = (*clk_alignment_reg & 0x007F0000) >> 16;
         if (temp == 0x3F || temp == 0x1F || temp == 0x0F || temp == 0x07 || temp == 0x03 || temp == 0x01) {
            *clk_alignment_reg = (*clk_alignment_reg | 0x40000000);
         }
      }
#endif  
   }

   // G/ restore the saved value of bit[14:8] of 0xff410040 back to the register.
   *clk_alignment_reg = (*clk_alignment_reg & ~KEEPME_MASK) | (keepme & KEEPME_MASK);
#endif

   // H/ set cp0 reg 22 sel 5 bits[30:28]  (sel 4 bits[24:22] for single core mips)  to 000
   asm("mfc0 %0,$22,5" : "=d"(cp0_ascr_asc) :);
   cp0_ascr_asc = ( cp0_ascr_asc & ~MASK_ASCR);
   asm("mtc0 %0,$22,5" : : "d" (cp0_ascr_asc));

   // I/ SYNC instruction 
   asm("sync" : : );

   return;
} /* BcmPwrMngtSetSCR */

void BcmPwrMngtSetSCR(void)
{
   unsigned long flags;

   spin_lock_irqsave(&pwrmgnt_clk_irqlock, flags);
   __BcmPwrMngtSetSCR();
   spin_unlock_irqrestore(&pwrmgnt_clk_irqlock, flags);

   return;
} /* BcmPwrMngtSetSCR */
EXPORT_SYMBOL(BcmPwrMngtSetSCR);


void BcmPwrMngtSetAutoClkDivide(unsigned int enable)
{
   printk("Host MIPS Clock divider pwrsaving is %s\n", enable?"enabled":"disabled");
   clock_divide_enabled = enable;
}
EXPORT_SYMBOL(BcmPwrMngtSetAutoClkDivide);


int BcmPwrMngtGetAutoClkDivide(void)
{
   return (clock_divide_enabled);
}
EXPORT_SYMBOL(BcmPwrMngtGetAutoClkDivide);
#endif

#if defined(CONFIG_BCM_DDR_SELF_REFRESH_PWRSAVE)
void BcmPwrMngtSetDRAMSelfRefresh(unsigned int enable)
{
   printk("DDR Self Refresh pwrsaving is %s\n", enable?"enabled":"disabled");
   self_refresh_enabled = enable;

#if defined(CONFIG_BCM_DDR_SELF_REFRESH_PWRSAVE) && defined(CONFIG_USB) && defined(USBH_OHCI_MEM_REQ_DIS)
#if defined (CONFIG_BCM963268)
   if (0xD0 == (PERF->RevID & REV_ID_MASK)) {
#endif
      if (enable) {
         // Configure USB port to not access DDR if unused, to save power
         USBH->USBSimControl |= USBH_OHCI_MEM_REQ_DIS;
      } else {
         USBH->USBSimControl &= ~USBH_OHCI_MEM_REQ_DIS;
      }
#if defined (CONFIG_BCM963268)
   }
#endif
#endif
}
EXPORT_SYMBOL(BcmPwrMngtSetDRAMSelfRefresh);


int BcmPwrMngtGetDRAMSelfRefresh(void)
{
   return (self_refresh_enabled);
}
EXPORT_SYMBOL(BcmPwrMngtGetDRAMSelfRefresh);

#if defined(CONFIG_BCM_ADSL_MODULE) || defined(CONFIG_BCM_ADSL)
PWRMNGT_DDR_SR_CTRL *pDdrSrCtrl = NULL;
void BcmPwrMngtRegisterLmemAddr(PWRMNGT_DDR_SR_CTRL *pDdrSr)
{
    pDdrSrCtrl = pDdrSr;

    // Initialize tp0 to busy status and tp1 to idle
    // for cases where SMP is not compiled in.
    if(NULL != pDdrSrCtrl) {
        pDdrSrCtrl->tp0Busy = 1;
        pDdrSrCtrl->tp1Busy = 0;
    }
}
EXPORT_SYMBOL(BcmPwrMngtRegisterLmemAddr);
#else
PWRMNGT_DDR_SR_CTRL ddrSrCtl = {{.word=0}};
PWRMNGT_DDR_SR_CTRL *pDdrSrCtrl = &ddrSrCtl;
#endif
#endif

// Determine if cpu is busy by checking the number of times we entered the wait
// state in the last milisecond. If we entered the wait state only once or
// twice, then the processor is very likely not busy and we can afford to slow
// it down while on wait state. Otherwise, we don't slow down the processor
// while on wait state in order to avoid affecting the time it takes to
// process interrupts
void BcmPwrMngtCheckWaitCount (void)
{
    int cpu = smp_processor_id();

    if (cpu == 0) {
#if defined(CONFIG_SMP) && defined(CONFIG_BCM_HOSTMIPS_PWRSAVE_TIMERS)
        if (isVoiceIdle && TimerC0Snapshot1) {
#else
        if (isVoiceIdle) {
#endif
           if (wait_count0 > 0 && wait_count0 < 3) {
              clock_divide_low_power0 = 1;
           }
           else {
              clock_divide_low_power0 = 0;
           }
        }
        else {
           clock_divide_low_power0 = 0;
        }
        wait_count0 = 0;
    }
#if defined(CONFIG_SMP)
    else {
#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE_TIMERS)
        if (TimerC0Snapshot1) {
#else
        {
#endif
           if (wait_count1 > 0 && wait_count1 < 3) {
              clock_divide_low_power1 = 1;
           }
           else {
              clock_divide_low_power1 = 0;
           }
        }
        wait_count1 = 0;
    }
#endif
}

// When entering wait state, consider reducing the MIPS clock speed.
// Clock speed is reduced if it has been determined that the cpu was
// mostly idle in the previous milisecond. Clock speed is reduced only
// once per 1 milisecond interval.
void BcmPwrMngtReduceCpuSpeed (void)
{
    int cpu = smp_processor_id();
    unsigned long flags;

    spin_lock_irqsave(&pwrmgnt_clk_irqlock, flags);

    if (cpu == 0) {
        // Slow down the clock when entering wait instruction
        // only if the cpu is not busy
        if (clock_divide_low_power0) {
            if (wait_count0 < 2) {
                clock_divide_active0 = 1;
#if defined(CONFIG_BCM_DDR_SELF_REFRESH_PWRSAVE)
                if (pDdrSrCtrl && self_refresh_enabled) {
                    // Communicate TP status to PHY MIPS
                    pDdrSrCtrl->tp0Busy = 0;
                }
#endif
            }
        }
        wait_count0++;
    }
#if defined(CONFIG_SMP)
    else {
        // Slow down the clock when entering wait instruction
        // only if the cpu is not busy
        if (clock_divide_low_power1) {
            if (wait_count1 < 2) {
                clock_divide_active1 = 1;
#if defined(CONFIG_BCM_DDR_SELF_REFRESH_PWRSAVE)
                if (pDdrSrCtrl && self_refresh_enabled) {
                    // Communicate TP status to PHY MIPS
                    pDdrSrCtrl->tp1Busy = 0;
                }
#endif
            }
        }
        wait_count1++;
    }
#endif

#if defined(CONFIG_SMP)
    if (clock_divide_active0 && clock_divide_active1) {
#else
    if (clock_divide_active0) {
#endif
#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE)
        if (clock_divide_enabled) {
            __BcmPwrMngtSetASCR(RATIO_ONE_EIGHTH);
		}
#endif

#if defined(CONFIG_BCM_DDR_SELF_REFRESH_PWRSAVE)
        // Place DDR in self-refresh mode if enabled and other processors are OK with it
        if (pDdrSrCtrl && !pDdrSrCtrl->word && self_refresh_enabled) {
            // Below defines are CHIP Specific - refer to xxxx_map_part.h
#if defined(DMODE_1_DRAMSLEEP)
            DDR->DMODE_1 |= DMODE_1_DRAMSLEEP;
#elif defined(MEMC_SELF_REFRESH)
            MEMC->Control |= MEMC_SELF_REFRESH;
#elif defined(CFG_DRAMSLEEP)
            MEMC->DRAM_CFG |= CFG_DRAMSLEEP;
#else
            #error "DDR Self refresh definition missing in xxxx_map_part.h for this chip"
#endif
        }
#endif
    }
    spin_unlock_irqrestore(&pwrmgnt_clk_irqlock, flags);
}

// Full MIPS clock speed is resumed on the first interrupt following
// the wait instruction. If the clock speed was reduced, the MIPS
// C0 counter was also slowed down and its value needs to be readjusted.
// The adjustments are done based on a reliable timer from the peripheral
// block, timer2. The adjustments are such that C0 will never drift
// but will see minor jitter.
void BcmPwrMngtResumeFullSpeed (void)
{
#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE_TIMERS)
    unsigned int mult, rem, new;
#endif
    int cpu = smp_processor_id();
    unsigned long flags;

    spin_lock_irqsave(&pwrmgnt_clk_irqlock, flags);

#if defined(CONFIG_BCM_DDR_SELF_REFRESH_PWRSAVE)
    if (pDdrSrCtrl) {
        // Communicate TP status to PHY MIPS
        // Here I don't check if Self-Refresh is enabled because when it is,
        // I want PHY MIPS to think the Host MIPS is always busy so it won't assert SR
        if (cpu == 0) {
            pDdrSrCtrl->tp0Busy = 1;
        } else {
            pDdrSrCtrl->tp1Busy = 1;
        }
    }
#endif


#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE)

#if defined(CONFIG_SMP)
    if (clock_divide_enabled && clock_divide_active0 && clock_divide_active1) {
#else
    if (clock_divide_enabled && clock_divide_active0) {
#endif
#if defined(CONFIG_BCM96362) || defined(CONFIG_BCM96328) || defined(CONFIG_BCM96816) || defined(CONFIG_BCM96368)
        if (originalMipsAscr) {
            __BcmPwrMngtSetASCR(RATIO_ONE_ASYNC);
        } else {
            __BcmPwrMngtSetSCR();
        }
#else
        // In newer MIPS core, there is no SYNC mode, simply use 1:1 async
        __BcmPwrMngtSetASCR(RATIO_ONE_ASYNC);
#endif
    }
#endif

#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE_TIMERS)
    if (cpu == 0) {
        // Check for TimerCnt2 rollover
        newTimerCnt0 = TIMER->TimerCnt2 & 0x3fffffff;
        if (newTimerCnt0 < prevTimerCnt0) {
           TimerAdjust0 += C0adder;
        }

        // fix the C0 counter because it slowed down while on wait state
        if (clock_divide_active0) {
           mult = newTimerCnt0/C0divider;
           rem  = newTimerCnt0%C0divider;
           new  = mult*C0multiplier + ((rem*C0ratio)>>10);
           write_c0_count(TimerAdjust0 + TimerC0Snapshot0 + new);
           clock_divide_active0 = 0;
        }
        prevTimerCnt0 = newTimerCnt0;
    }
#if defined(CONFIG_SMP)
    else {
        // Check for TimerCnt2 rollover
        newTimerCnt1 = TIMER->TimerCnt2 & 0x3fffffff;
        if (newTimerCnt1 < prevTimerCnt1) {
           TimerAdjust1 += C0adder;
        }

        // fix the C0 counter because it slowed down while on wait state
        if (clock_divide_active1) {
           mult = newTimerCnt1/C0divider;
           rem  = newTimerCnt1%C0divider;
           new  = mult*C0multiplier + ((rem*C0ratio)>>10);
           write_c0_count(TimerAdjust1 + TimerC0Snapshot1 + new);
           clock_divide_active1 = 0;
        }
        prevTimerCnt1 = newTimerCnt1;
    }
#endif
#else
    // On chips not requiring the PERIPH Timers workaround,
    // only need to clear the active flags, no need to adjust timers
    if (cpu == 0) {
       clock_divide_active0 = 0;
    }
#if defined(CONFIG_SMP)
    else {
       clock_divide_active1 = 0;
    }
#endif
#endif
    spin_unlock_irqrestore(&pwrmgnt_clk_irqlock, flags);
}


#if defined(CONFIG_BCM_HOSTMIPS_PWRSAVE_TIMERS)
// These numbers can be precomputed. The values are chosen such that the
// calculations will never overflow as long as the MIPS frequency never
// exceeds 850 MHz (hence mips_hpt_frequency must not exceed 425 MHz)
void BcmPwrMngtInitC0Speed (void)
{
    unsigned int mult, rem;
    if (mips_hpt_frequency > 425000000) {
       printk("\n\nWarning!!! CPU frequency exceeds limits to support" \
          " Clock Divider feature for Power Management\n");
    }
    C0divider = 50000000/128;
    C0multiplier = mips_hpt_frequency/128;
    C0ratio = ((mips_hpt_frequency/1000000)<<10)/50;
    mult = 0x40000000/C0divider;
    rem = 0x40000000%C0divider;
    // Value below may overflow from 32 bits but that's ok
    C0adder = mult*C0multiplier + ((rem*C0ratio)>>10);
}
#endif
