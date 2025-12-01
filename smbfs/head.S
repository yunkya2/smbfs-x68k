/*
 * Copyright (c) 2023,2025 Yuichi Nakamura (@yunkya2)
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

#include <config.h>

/* Human68k device driver header */

    .section    ".header","ax"

    .global devheader
    .extern reqheader
    .extern interrupt
    .extern smbfs_data

devheader:
    .long   -1                  // link pointer
    .word   0x2040              // device type (remote disk)
    .long   strategy            // strategy routine entry point
    .long   interrupt_asm       // interrupt routine entry point
    .ascii  CONFIG_DEVNAME      // device name

    .long   smbfs_data          // device data pointer

strategy:
    move.l  %a5,reqheader
    rts

interrupt_asm:
    movem.l %d0-%d2/%a0-%a2/%a5,%sp@-
    bsr     interrupt
    movea.l reqheader,%a5
    ror.w   #8,%d0
    move.b  %d0,%a5@(4)             // errh エラーコードを設定
    rol.w   #8,%d0
    move.b  %d0,%a5@(3)             // errl エラーコードを設定
    movem.l %sp@+,%d0-%d2/%a0-%a2/%a5
    rts

/* C startup routine */

    .global _start
    .extern _HSTA
    .extern _HEND
    .extern _heap_size
    .extern _stack_size
    .extern start

_start:
    move.l  %a1,_HSTA
    add.l   _heap_size,%a1
    move.l  %a1,_HEND               // setup heap
    add.l   _stack_size,%a1
    cmpa.l  %a0@(8),%a1             // memory block bottom
    bhi     out_of_memory

    move.l  %a1,%sp                 // setup stack
    move.l  %a2,%sp@-               // command line address
    jsr     start
    .short  0xff00                  // exit

out_of_memory:
    pea.l   %pc@(oom_msg)
    .short  0xff09                  // print
    addq.l  #4,%sp
    move.w  #0x007f,%sp@-
    .short  0xff4c                  // exit2

oom_msg:
    .asciz  "Out of memory. Program terminated.\r\n"

    .end
