/*
 * savetool.h -- the Save Tool blob's control block, shared by rom/savetool.c
 * (on the GBA) and the board's applet and tests.
 *
 * Layout: the blob is linked at SVT_BASE. Its first bytes are this block;
 * svt_main (Thumb) is at the address in `entry`. The save buffer is
 * SVT_BUF, 128 KB, below the agent's protected stack.
 */
#ifndef SAVETOOL_H
#define SAVETOOL_H

#include <stdint.h>

#define SVT_BASE    0x02001000u
#define SVT_BUF     0x02010000u           /* 128 KB: 0x02010000-0x0202FFFF */
#define SVT_SECTOR  4096u
#define SVT_MAGIC   0x31545653u           /* "SVT1" */

enum { SVT_DETECT = 1, SVT_FLASH_ID = 2, SVT_READ = 3, SVT_WRITE = 4 };

enum { SVT_NONE = 0, SVT_SRAM = 1, SVT_FLASH64 = 2, SVT_FLASH128 = 3, SVT_EEPROM = 4 };

enum {
    SVT_ST_OK = 0,
    SVT_ST_ARGS = 1,
    SVT_ST_UNSUPPORTED = 2,
    SVT_ST_TIMEOUT = 3,                   /* a flash chip did not finish */
    SVT_ST_VERIFY = 4,                    /* written, but reads back different */
};

typedef struct {
    uint32_t magic;                       /* SVT_MAGIC                         */
    uint32_t version;                     /* 1                                  */
    uint32_t entry;                       /* svt_main | 1                       */
    uint32_t done;                        /* +1 per command run                 */
    uint32_t cmd, arg0, arg1;             /* set by the board                   */
    uint32_t status;
    uint32_t save_type, save_size;        /* from SVT_DETECT                    */
    uint32_t found_at;                    /* where the ID string is in the ROM  */
    uint32_t chip_id;                     /* from SVT_FLASH_ID: maker | device << 8 */
    uint32_t mismatches;                  /* bytes that differ after a write    */
    uint32_t buf;                         /* SVT_BUF                            */
    uint32_t chip_size;                   /* from SVT_FLASH_ID: what the chip holds,
                                           * 0 if it is not one we know          */
} svt_block_t;

#define SVT_BLOCK ((svt_block_t *)SVT_BASE)

#endif
