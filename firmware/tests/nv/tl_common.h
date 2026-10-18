/* Host stub so the SDK's real drv_nv.c (copied to sdk/) compiles on Linux. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;
#define TRUE 1
#define FALSE 0
#define FLASH_CAP_SIZE_1M 1
#define BOOT_LOADER_MODE 0
#define NV_ENABLE 1
#define APP_IMAGE_ADDR 0
#define min2(a,b) ((a) < (b) ? (a) : (b))
#define OFFSETOF(s, m) ((unsigned int)(uintptr_t) &((s *)0)->m)
#define foreach(i, n) for(int i = 0; i < (n); ++i)
#define ZB_EXCEPTION_POST(x) do { fprintf(stderr, "exception %d\n", x); abort(); } while (0)
#define SYS_EXCEPTTION_NV_CHECK_TABLE_FULL 1
#include <stdio.h>
#include "sdk/drv_nv.h"
void flash_read(u32 addr, u32 len, u8 *buf);
void flash_write(u32 addr, u32 len, u8 *buf);
void flash_erase(u32 addr);
bool flash_writeWithCheck(u32 addr, u32 len, u8 *buf);
unsigned int xcrc32(const unsigned char *buf, int len, unsigned int init);
void *ev_buf_allocate(u16 size);
void ev_buf_free(void *p);
