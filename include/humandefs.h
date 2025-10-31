/*
 * Copyright (c) 2023,2024,2025 Yuichi Nakamura (@yunkya2)
 *
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef _HUMANDEFS_H_
#define _HUMANDEFS_H_

#include <stdint.h>
#include <x68k/dos.h>

//****************************************************************************
// Human68k error code
//****************************************************************************

//****************************************************************************
// Human68k structures
//****************************************************************************

#define dos_fcb_mode(fcb)   (((uint8_t *)(fcb))[14])
#define dos_fcb_fpos(fcb)   (*(uint32_t *)(&((uint8_t *)(fcb))[6]))
#define dos_fcb_size(fcb)   (*(uint32_t *)(&((uint8_t *)(fcb))[64]))

struct dos_req_header {
  uint8_t magic;       // +0x00.b  Constant (26)
  uint8_t unit;        // +0x01.b  Unit number
  uint8_t command;     // +0x02.b  Command code
  uint8_t errl;        // +0x03.b  Error code low
  uint8_t errh;        // +0x04.b  Error code high
  uint8_t reserved[8]; // +0x05 .. +0x0c  not used
  uint8_t attr;        // +0x0d.b  Attribute / Seek mode
  void *addr;          // +0x0e.l  Buffer address
  uint32_t status;     // +0x12.l  Bytes / Buffer / Result status
  void *fcb;           // +0x16.l  FCB
} __attribute__((packed, aligned(2)));

struct dos_filesinfo {
  uint8_t dummy;
  uint8_t atr;
  uint16_t time;
  uint16_t date;
  uint32_t filelen;
  char name[23];
} __attribute__((packed, aligned(2)));  // part of dos_filbuf

typedef struct dos_namestbuf dos_namebuf;
#if 0
typedef struct {
  uint8_t flag;
  uint8_t drive;
  uint8_t path[65];
  uint8_t name1[8];
  uint8_t ext[3];
  uint8_t name2[10];
} dos_namebuf;    // == struct dos_namestbuf
#endif

#endif /* _HUMANDEFS_H_ */
