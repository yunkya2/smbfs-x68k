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

#ifndef _SMBFSCMD_H_
#define _SMBFSCMD_H_

#include <stdint.h>
#include <stddef.h>

#define SMBFS_SIGNATURE     "SMBFSv1 "

#define SMBCMD_GETNAME      -1
#define SMBCMD_NOP          0
#define SMBCMD_MOUNT        1
#define SMBCMD_UNMOUNT      2
#define SMBCMD_UNMOUNTALL   3
#define SMBCMD_GETMOUNT     4

struct smbcmd_mount {
    size_t username_len;
    char *url;
    char *username;
    char *password;
    char **environ;
};

struct smbcmd_getmount {
    size_t server_len;
    size_t share_len;
    size_t rootpath_len;
    size_t username_len;
    char *server;
    char *share;
    char *rootpath;
    char *username;
};

#endif /* _SMBFSCMD_H_ */
