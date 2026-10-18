/* Host model of the bits ensure_ota_scheme.c needs: NOR flash with power-cut injection. */
#ifndef HOST_FLASH_H
#define HOST_FLASH_H
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define _attribute_ram_code_sec_
#define FLASH_TYPE_8BIT_STATUS 0

extern jmp_buf g_reset;             /* SYSTEM_RESET and power cuts both end the "boot" */
#define SYSTEM_RESET() longjmp(g_reset, 1)

void ram_code_flash_read_page(unsigned long addr, unsigned long len, unsigned char *buf);
void ram_code_flash_write_page(unsigned long addr, unsigned long len, unsigned char *buf);
void ram_code_flash_erase_sector(unsigned long addr);
void ram_code_flash_write_status(int type, unsigned short data);
#endif
