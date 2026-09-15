/**********************************************************************************
* File Name:   main.c
*
* Description: This is the source code for Work Flash Sector Data Updating Example
*              for ModusToolbox.
*
* Related Document: See README.md
*
*
*******************************************************************************
* (c) 2026, Infineon Technologies AG, or an affiliate of Infineon
* Technologies AG. All rights reserved.
* This software, associated documentation and materials ("Software") is
* owned by Infineon Technologies AG or one of its affiliates ("Infineon")
* and is protected by and subject to worldwide patent protection, worldwide
* copyright laws, and international treaty provisions. Therefore, you may use
* this Software only as provided in the license agreement accompanying the
* software package from which you obtained this Software. If no license
* agreement applies, then any use, reproduction, modification, translation, or
* compilation of this Software is prohibited without the express written
* permission of Infineon.
*
* Disclaimer: UNLESS OTHERWISE EXPRESSLY AGREED WITH INFINEON, THIS SOFTWARE
* IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
* INCLUDING, BUT NOT LIMITED TO, ALL WARRANTIES OF NON-INFRINGEMENT OF
* THIRD-PARTY RIGHTS AND IMPLIED WARRANTIES SUCH AS WARRANTIES OF FITNESS FOR A
* SPECIFIC USE/PURPOSE OR MERCHANTABILITY.
* Infineon reserves the right to make changes to the Software without notice.
* You are responsible for properly designing, programming, and testing the
* functionality and safety of your intended application of the Software, as
* well as complying with any legal requirements related to its use. Infineon
* does not guarantee that the Software will be free from intrusion, data theft
* or loss, or other breaches ("Security Breaches"), and Infineon shall have
* no liability arising out of any Security Breaches. Unless otherwise
* explicitly approved by Infineon, the Software may not be used in any
* application where a failure of the Product or any consequences of the use
* thereof can reasonably be expected to result in personal injury.
*******************************************************************************/

#include "cybsp.h"
#include "cy_flash.h"
#include "cy_scb_uart.h"
#include "cy_syslib.h"
#include "mtb_hal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*******************************************************************************
* Macros
*******************************************************************************/

/* Select flash address to be tested. */
#define WORKFLASH_SECTOR_SIZE         (128UL)
#define WORKFLASH_PROGRAM_CHUNK_BYTES (32UL)
#define WORKFLASH_SECTOR_OFFSET       (2UL)
#define WORKFLASH_PREFERRED_TARGET_ADDR (0x1401FF00UL)
#define WORKFLASH_PREFERRED_BACKUP_ADDR (0x1401FF80UL)

/*******************************************************************************
* Function Prototypes
*******************************************************************************/


/*******************************************************************************
* Global Variables
*******************************************************************************/
/* For the Retarget -IO (Debug UART) usage */
static cy_stc_scb_uart_context_t g_uart_context;
static mtb_hal_uart_t g_uart_hal_obj;

static const mtb_hal_uart_configurator_t g_uart_mtb_config =
{
    .base = UART_HW,
    .clock = &UART_hal_clock,
#if defined(COMPONENT_MW_ASYNC_TRANSFER)
    .rts_enable = false,
#endif
};

static uint8_t g_target_image[WORKFLASH_SECTOR_SIZE] __attribute__((aligned(32)));
static uint8_t g_backup_image[WORKFLASH_SECTOR_SIZE] __attribute__((aligned(32)));
static uint8_t g_verify_image[WORKFLASH_SECTOR_SIZE] __attribute__((aligned(32)));
static uint8_t g_program_chunk[WORKFLASH_PROGRAM_CHUNK_BYTES] __attribute__((aligned(32)));


/*******************************************************************************
* Function Definitions
*******************************************************************************/
/*******************************************************************************
* Function Name: fail_fast
********************************************************************************
* Summary:
*
* Parameters:
*
* Return:
*
*
*******************************************************************************/
static __NO_RETURN void fail_fast(const char *step)
{
    (void)step;
    CY_ASSERT(0);

    for (;;)
    {
    }
}

/*******************************************************************************
* Function Name: uart_write
********************************************************************************
* Summary:
*
* Parameters:
*
* Return:
*
*
*******************************************************************************/
static void uart_write(const char *text)
{
    while (*text != '\0')
    {
        if (mtb_hal_uart_put(&g_uart_hal_obj, (uint32_t)(uint8_t)*text) != CY_RSLT_SUCCESS)
        {
            fail_fast("mtb_hal_uart_put");
        }
        text++;
    }
}

/*******************************************************************************
* Function Name: uart_logf
********************************************************************************
* Summary:
*
* Parameters:
*
* Return:
*
*
*******************************************************************************/
static void uart_logf(const char *fmt, ...)
{
    char buffer[256];
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len > 0)
    {
        uart_write(buffer);
    }
}

/*******************************************************************************
* Function Name: sync_flash_read_view
********************************************************************************
* Summary:
*
* Parameters:
*
* Return:
*
*
*******************************************************************************/
static void sync_flash_read_view(void)
{
#if defined(CY_IP_M7CPUSS) && defined(FLASHC_FLASH_CMD_INV_Msk)
    /* CAT1C (M7CPUSS): invalidate flash controller cache/buffer after erase/program. */
    FLASHC_FLASH_CMD = _VAL2FLD(FLASHC_FLASH_CMD_INV, 1UL);
    while (_FLD2VAL(FLASHC_FLASH_CMD_INV, FLASHC_FLASH_CMD) != 0UL)
    {
    }
#endif

#if defined(CY_IP_M7CPUSS)
    SCB_InvalidateDCache();
    SCB_InvalidateICache();
#endif
    __DSB();
    __ISB();
}

static bool program_workflash_sector(uint32_t sector_addr, const uint8_t *sector_data)
{
    uint32_t offset;

    for (offset = 0U; offset < WORKFLASH_SECTOR_SIZE; offset += WORKFLASH_PROGRAM_CHUNK_BYTES)
    {
        cy_stc_flash_programrow_config_t cfg;

        memcpy(g_program_chunk, &sector_data[offset], WORKFLASH_PROGRAM_CHUNK_BYTES);

        cfg.destAddr = (const uint32_t *)(sector_addr + offset);
        cfg.dataAddr = (const uint32_t *)g_program_chunk;
        cfg.blocking = CY_FLASH_PROGRAMROW_BLOCKING;
        cfg.skipBC = CY_FLASH_PROGRAMROW_BLANK_CHECK;
        cfg.dataSize = CY_FLASH_PROGRAMROW_DATA_SIZE_256BIT;
        cfg.dataLoc = CY_FLASH_PROGRAMROW_DATA_LOCATION_SRAM;
        cfg.intrMask = CY_FLASH_PROGRAMROW_NOT_SET_INTR_MASK;

        if (Cy_Flash_Program_WorkFlash(&cfg) != CY_FLASH_DRV_SUCCESS)
        {
            uart_logf("[FAIL ] Cy_Flash_Program_WorkFlash failed at 0x%08" PRIX32 "\r\n",
                      sector_addr + offset);
            return false;
        }
    }

    return true;
}

static bool erase_sector_and_verify(uint32_t sector_addr,
                                    bool verbose,
                                    cy_en_flashdrv_status_t *erase_status_out,
                                    uint32_t *verify_fail_addr_out)
{
    cy_en_flashdrv_status_t erase_status;
    cy_en_flashdrv_status_t blank_status;
    cy_stc_flash_blankcheck_config_t blank_cfg;

    if (erase_status_out != NULL)
    {
        *erase_status_out = CY_FLASH_DRV_SUCCESS;
    }
    if (verify_fail_addr_out != NULL)
    {
        *verify_fail_addr_out = 0UL;
    }

    erase_status = Cy_Flash_EraseSector(sector_addr);
    if (erase_status != CY_FLASH_DRV_SUCCESS)
    {
        if (erase_status_out != NULL)
        {
            *erase_status_out = erase_status;
        }

        if (verbose)
        {
            uart_logf("[FAIL ] Cy_Flash_EraseSector failed at 0x%08" PRIX32 " (st=0x%08" PRIX32 ")\r\n",
                      sector_addr,
                      (uint32_t)erase_status);
        }
        return false;
    }

    blank_cfg.addrToBeChecked = (const uint32_t *)sector_addr;
    blank_cfg.numOfWordsToBeChecked = WORKFLASH_SECTOR_SIZE / sizeof(uint32_t);
    blank_status = Cy_Flash_BlankCheck(&blank_cfg, CY_FLASH_DRIVER_BLOCKING);
    if (blank_status != CY_FLASH_DRV_SUCCESS)
    {
        if (erase_status_out != NULL)
        {
            *erase_status_out = blank_status;
        }
        if (verify_fail_addr_out != NULL)
        {
            *verify_fail_addr_out = sector_addr;
        }

        if (verbose)
        {
            uart_logf("[FAIL ] BlankCheck NG at 0x%08" PRIX32 " (st=0x%08" PRIX32 ")\r\n",
                      sector_addr,
                      (uint32_t)blank_status);
        }
        return false;
    }

    sync_flash_read_view();

    return true;
}

static bool find_usable_sector_pair(uint32_t workflash_region_start,
                                    uint32_t workflash_region_end,
                                    uint32_t *target_sector_addr,
                                    uint32_t *backup_sector_addr)
{
    uint32_t scan_start = workflash_region_start + (WORKFLASH_SECTOR_OFFSET * WORKFLASH_SECTOR_SIZE);
    uint32_t scan_end = workflash_region_end - (2UL * WORKFLASH_SECTOR_SIZE);
    uint32_t candidate;
    uint32_t scanned_pairs = 0UL;
    uint32_t total_pairs;

    if (scan_start > scan_end)
    {
        scan_start = workflash_region_start;
        scan_end = workflash_region_end - (2UL * WORKFLASH_SECTOR_SIZE);
    }

    total_pairs = ((scan_end - scan_start) / WORKFLASH_SECTOR_SIZE) + 1UL;

    uart_write("[SCAN] Searching erasable Work Flash sector pair...\r\n");
    uart_logf("[SCAN] Range start=0x%08" PRIX32 ", end=0x%08" PRIX32 ", pairs=%" PRIu32 "\r\n",
              scan_start, scan_end + WORKFLASH_SECTOR_SIZE, total_pairs);
    uart_write("[SCAN] Direction: high -> low\r\n");

    for (candidate = scan_end;; candidate -= WORKFLASH_SECTOR_SIZE)
    {
        uint32_t candidate_backup = candidate + WORKFLASH_SECTOR_SIZE;
        bool target_ok;
        bool backup_ok;
        cy_en_flashdrv_status_t target_st;
        cy_en_flashdrv_status_t backup_st;
        uint32_t target_fail_addr;
        uint32_t backup_fail_addr;
        bool should_log_probe;

        should_log_probe = ((scanned_pairs < 4UL) || ((scanned_pairs & 0x0FUL) == 0UL));
        if (should_log_probe)
        {
            uart_logf("[SCAN] Probe[%" PRIu32 "/%" PRIu32 "] target=0x%08" PRIX32 ", backup=0x%08" PRIX32 "\r\n",
                      scanned_pairs + 1UL, total_pairs, candidate, candidate_backup);
        }

        target_ok = erase_sector_and_verify(candidate, false, &target_st, &target_fail_addr);
        backup_ok = erase_sector_and_verify(candidate_backup, false, &backup_st, &backup_fail_addr);
        scanned_pairs++;

        if (should_log_probe && (!target_ok || !backup_ok))
        {
            uart_logf("[SCAN] Fail[%" PRIu32 "] t_ok=%u st=0x%08" PRIX32 " vfy=0x%08" PRIX32
                      " | b_ok=%u st=0x%08" PRIX32 " vfy=0x%08" PRIX32 "\r\n",
                      scanned_pairs,
                      target_ok ? 1U : 0U,
                      (uint32_t)target_st,
                      target_fail_addr,
                      backup_ok ? 1U : 0U,
                      (uint32_t)backup_st,
                      backup_fail_addr);
        }

        if (target_ok && backup_ok)
        {
            *target_sector_addr = candidate;
            *backup_sector_addr = candidate_backup;
            uart_logf("[SCAN] Selected target=0x%08" PRIX32 ", backup=0x%08" PRIX32 "\r\n\r\n",
                      *target_sector_addr, *backup_sector_addr);
            return true;
        }

        if (candidate <= scan_start)
        {
            break;
        }
    }

    uart_write("[SCAN] No erasable sector pair found in scan range.\r\n");
    return false;
}

static void fill_source_pattern(uint8_t *buffer, uint32_t size)
{
    uint32_t i;
    for (i = 0U; i < size; i++)
    {
        buffer[i] = (uint8_t)(i & 0xFFU);
    }
}

static bool verify_sector(uint32_t sector_addr, const uint8_t *expected)
{
    sync_flash_read_view();

    memcpy(g_verify_image, (const void *)sector_addr, WORKFLASH_SECTOR_SIZE);
    if (memcmp(expected, g_verify_image, WORKFLASH_SECTOR_SIZE) != 0)
    {
        uart_logf("[FAIL ] Verify NG at sector 0x%08" PRIX32 "\r\n", sector_addr);
        return false;
    }

    return true;
}

/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
* Main function
*
* Parameters:
*  void
*
* Return:
*  int
*
*******************************************************************************/
int main(void)
{
    cy_rslt_t result;
    uint32_t target_sector_addr;
    uint32_t backup_sector_addr;
    uint32_t selected_region_start = 0UL;
    uint32_t selected_region_end = 0UL;
    bool pair_found = false;
    const uint32_t workflash_region_starts[] =
    {
        (uint32_t)CY_WFLASH_SM_DBM0_BASE,
        (uint32_t)CY_WFLASH_SM_SBM_BASE
    };
    const uint32_t workflash_region_ends[] =
    {
        (uint32_t)(CY_WFLASH_SM_DBM0_BASE + CY_WFLASH_SM_DBM0_SIZE),
        (uint32_t)(CY_WFLASH_SM_SBM_BASE + CY_WFLASH_SM_SBM_SIZE)
    };
    uint32_t region_idx;
    cy_en_flashdrv_status_t target_st = CY_FLASH_DRV_SUCCESS;
    cy_en_flashdrv_status_t backup_st = CY_FLASH_DRV_SUCCESS;
    uint32_t target_fail_addr = 0UL;
    uint32_t backup_fail_addr = 0UL;
    bool fixed_target_ok;
    bool fixed_backup_ok;

    result = cybsp_init();
    if (result != CY_RSLT_SUCCESS)
    {
        fail_fast("cybsp_init");
    }

#if defined(CY_IP_M7CPUSS)
    SCB_DisableDCache();
#endif

    result = (cy_rslt_t)Cy_SCB_UART_Init(UART_HW, &UART_config, &g_uart_context);
    if (result != CY_RSLT_SUCCESS)
    {
        fail_fast("Cy_SCB_UART_Init");
    }
    Cy_SCB_UART_Enable(UART_HW);

    result = mtb_hal_uart_setup(&g_uart_hal_obj, &g_uart_mtb_config, &g_uart_context, NULL);
    if (result != CY_RSLT_SUCCESS)
    {
        fail_fast("mtb_hal_uart_setup");
    }

    Cy_Flash_Init();

    Cy_Flashc_WorkWriteEnable();
    Cy_Flashc_MainWriteEnable();

    __enable_irq();

    uart_write("\x1b[2J\x1b[;H");
    uart_write("====================================\r\n");
    uart_write(" Work Flash Sector Data Updating \r\n");
    uart_write("====================================\r\n\r\n");

    uart_logf("[INFO] Trying fixed pair target=0x%08" PRIX32 ", backup=0x%08" PRIX32 "\r\n",
              (uint32_t)WORKFLASH_PREFERRED_TARGET_ADDR,
              (uint32_t)WORKFLASH_PREFERRED_BACKUP_ADDR);
    fixed_target_ok = erase_sector_and_verify((uint32_t)WORKFLASH_PREFERRED_TARGET_ADDR,
                                              false,
                                              &target_st,
                                              &target_fail_addr);
    fixed_backup_ok = erase_sector_and_verify((uint32_t)WORKFLASH_PREFERRED_BACKUP_ADDR,
                                              false,
                                              &backup_st,
                                              &backup_fail_addr);

    if (fixed_target_ok && fixed_backup_ok)
    {
        target_sector_addr = (uint32_t)WORKFLASH_PREFERRED_TARGET_ADDR;
        backup_sector_addr = (uint32_t)WORKFLASH_PREFERRED_BACKUP_ADDR;
        selected_region_start = (uint32_t)CY_WFLASH_SM_DBM0_BASE;
        selected_region_end = (uint32_t)(CY_WFLASH_SM_DBM0_BASE + CY_WFLASH_SM_DBM0_SIZE);
        pair_found = true;
        uart_logf("[INFO] Fixed pair accepted: target=0x%08" PRIX32 ", backup=0x%08" PRIX32 "\r\n\r\n",
                  target_sector_addr,
                  backup_sector_addr);
    }
    else
    {
        uart_logf("[INFO] Fixed pair rejected: t_st=0x%08" PRIX32 " t_vfy=0x%08" PRIX32
                  " | b_st=0x%08" PRIX32 " b_vfy=0x%08" PRIX32 "\r\n",
                  (uint32_t)target_st,
                  target_fail_addr,
                  (uint32_t)backup_st,
                  backup_fail_addr);
    }

    for (region_idx = 0UL;
         (!pair_found) && (region_idx < (sizeof(workflash_region_starts) / sizeof(workflash_region_starts[0])));
         region_idx++)
    {
        uint32_t workflash_region_start = workflash_region_starts[region_idx];
        uint32_t workflash_region_end = workflash_region_ends[region_idx];

        if ((workflash_region_end - workflash_region_start) < (WORKFLASH_SECTOR_SIZE * 2UL))
        {
            continue;
        }

        uart_logf("[INFO] Trying Work Flash region : 0x%08" PRIX32 " - 0x%08" PRIX32 "\r\n",
                  workflash_region_start,
                  workflash_region_end - 1UL);

        if (find_usable_sector_pair(workflash_region_start,
                                    workflash_region_end,
                                    &target_sector_addr,
                                    &backup_sector_addr))
        {
            selected_region_start = workflash_region_start;
            selected_region_end = workflash_region_end;
            pair_found = true;
            break;
        }
    }

    if (!pair_found)
    {
        fail_fast("find_usable_sector_pair");
    }

    uart_logf("[INFO] Selected Work Flash region: 0x%08" PRIX32 " - 0x%08" PRIX32 "\r\n",
              selected_region_start,
              selected_region_end - 1UL);
    uart_logf("[INFO] Target sector address   : 0x%08" PRIX32 "\r\n", target_sector_addr);
    uart_logf("[INFO] Backup sector address   : 0x%08" PRIX32 "\r\n", backup_sector_addr);
    uart_logf("[INFO] Sector size             : %" PRIu32 " bytes\r\n", (uint32_t)WORKFLASH_SECTOR_SIZE);
    uart_logf("[INFO] Program chunk size      : %" PRIu32 " bytes\r\n\r\n", (uint32_t)WORKFLASH_PROGRAM_CHUNK_BYTES);

    uart_write("[1] Initialize target sector data\r\n");
    if (!erase_sector_and_verify(target_sector_addr, true, NULL, NULL))
    {
        fail_fast("erase target for init");
    }

    fill_source_pattern(g_target_image, WORKFLASH_SECTOR_SIZE);
    if (!program_workflash_sector(target_sector_addr, g_target_image))
    {
        fail_fast("init program target");
    }
    if (!verify_sector(target_sector_addr, g_target_image))
    {
        fail_fast("init verify target");
    }
    uart_write("[1] Done\r\n\r\n");

    uart_write("[2] Copy target sector data to backup sector\r\n");
    if (!erase_sector_and_verify(backup_sector_addr, true, NULL, NULL))
    {
        fail_fast("erase backup");
    }

    sync_flash_read_view();
    memcpy(g_backup_image, (const void *)target_sector_addr, WORKFLASH_SECTOR_SIZE);
    if (!program_workflash_sector(backup_sector_addr, g_backup_image))
    {
        fail_fast("copy program backup");
    }
    if (!verify_sector(backup_sector_addr, g_backup_image))
    {
        fail_fast("copy verify backup");
    }
    uart_write("[2] Done\r\n\r\n");

    uart_write("[3] Erase target sector\r\n");
    if (!erase_sector_and_verify(target_sector_addr, true, NULL, NULL))
    {
        fail_fast("erase target");
    }
    uart_write("[3] Done\r\n\r\n");

    uart_write("[4] Process data and program back to target\r\n");
    sync_flash_read_view();
    memcpy(g_target_image, (const void *)backup_sector_addr, WORKFLASH_SECTOR_SIZE);
    g_target_image[0] = 0x55U;
    g_target_image[10] = 0xAAU;

    if (!program_workflash_sector(target_sector_addr, g_target_image))
    {
        fail_fast("process program target");
    }
    if (!verify_sector(target_sector_addr, g_target_image))
    {
        fail_fast("process verify target");
    }
    uart_write("[4] Done\r\n\r\n");

    uart_write("[PASS ] Work Flash sector data updating completed.\r\n");

    for (;;)
    {
        Cy_SysLib_Delay(1000UL);
    }
}
