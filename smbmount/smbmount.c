/*
 * Copyright (c) 2025 Yuichi Nakamura (@yunkya2)
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

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <x68k/dos.h>

//****************************************************************************
// Macros and definitions
//****************************************************************************

//****************************************************************************
// Global variables
//****************************************************************************

//****************************************************************************
// Local variables
//****************************************************************************

//****************************************************************************
// Utility routine
//****************************************************************************

//****************************************************************************
// Main program
//****************************************************************************

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "Usage: smbmount <options> <func>\n");
    return 1;
  }

  int drive = toupper(argv[1][0]) - 'A' + 1;
  int func = atoi(argv[2]);

  char signature[8];

  if (_dos_ioctrlfdctl(drive, -1, signature) != 0 || 
      memcmp(signature, "SMBFSv1 ", 8) != 0) {
        printf("ドライブ %c: はSMBFSではありません\n", 'A' + drive - 1);
        return 1;
  }

  char buf[10];
  printf("drive=%d res=%d\n", drive, _dos_ioctrlfdctl(drive, func, buf));

  return 0;
}
