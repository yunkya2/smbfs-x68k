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

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <x68k/dos.h>

#include <config.h>
#include <humandefs.h>
#include "smbfs.h"

//****************************************************************************
// Global variables
//****************************************************************************

struct dos_req_header *reqheader;   // Human68kからのリクエストヘッダ
jmp_buf jenv;                       // タイムアウト時のジャンプ先
int unit_base = 0;                  // ユニット番号のベース値

//****************************************************************************
// Utility routine
//****************************************************************************

#if 0
ssize_t send_read(uint32_t fcb, char *buf, uint32_t pos, size_t len)
{
  struct cmd_read *cmd = &comp->cmd_read;
  struct res_read *res = &comp->res_read;
  ssize_t total = 0;

  while (len > 0) {
    size_t size = len > sizeof(res->data) ? sizeof(res->data) : len;
    cmd->command = 0x4c; /* read */
    cmd->fcb = (uint32_t)fcb;
    cmd->pos = pos;
    cmd->len = size;

    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));

    DPRINTF1(" read: addr=0x%08x pos=%d len=%d size=%d\r\n", (uint32_t)buf, pos, len, res->len);
    if (res->len < 0)
      return res->len;
    if (res->len == 0)
      break;

    memcpy(buf, res->data, res->len);
    buf += res->len;
    total += res->len;
    pos += res->len;
    len -= res->len;
  }
  DPRINTF1(" read: total=%d\r\n", total);
  return total;
}

ssize_t send_write(uint32_t fcb, char *buf, uint32_t pos, size_t len)
{
  struct cmd_write *cmd = &comp->cmd_write;
  struct res_write *res = &comp->res_write;
  ssize_t total = 0;

  do {
    size_t size = len > sizeof(cmd->data) ? sizeof(cmd->data) : len;
    cmd->command = 0x4d; /* write */
    cmd->fcb = (uint32_t)fcb;
    cmd->pos = pos;
    cmd->len = size;
    memcpy(cmd->data, buf, size);

    com_cmdres(cmd, offsetof(struct cmd_write, data) + size, res, sizeof(*res));

    DPRINTF1(" write: addr=0x%08x pos=%d len=%d size=%d\r\n", (uint32_t)buf, pos, len, res->len);
    if (res->len < 0)
      return res->len;
    buf += res->len;
    total += res->len;
    pos += res->len;
    len -= res->len;
  } while (len > 0);
  DPRINTF1(" write: total=%d\r\n", total);
  return total;
}

struct dcache {
  uint32_t fcb;
  uint32_t pos;
  int16_t len;
  bool dirty;
  uint8_t cache[CONFIG_DATASIZE];
} dcache[CONFIG_NDCACHE];

struct dcache *dcache_alloc(uint32_t fcb)
{
  for (int i = 0; i < CONFIG_NDCACHE; i++) {
    if (dcache[i].fcb == fcb)
      return &dcache[i];
  }
  for (int i = 0; i < CONFIG_NDCACHE; i++) {
    if (dcache[i].fcb == 0)
      return &dcache[i];
  }
  return NULL;
}

int dcache_flash(uint32_t fcb, bool clean)
{
  int res = 0;
  for (int i = 0; i < CONFIG_NDCACHE; i++) {
    if (dcache[i].fcb == fcb) {
      if (dcache[i].dirty) {
        if (send_write(dcache[i].fcb, dcache[i].cache, dcache[i].pos, dcache[i].len) < 0)
          res = -1;
        dcache[i].dirty = false;
      }
      if (clean)
        dcache[i].fcb = 0;
    }
  }
  return res;
}

#if CONFIG_NFILEINFO > 1
struct fcache {
  uint32_t filep;
  int cnt;
  struct res_files files;
} fcache[CONFIG_NFCACHE];

struct fcache *fcache_alloc(uint32_t filep, bool new)
{
  for (int i = 0; i < CONFIG_NFCACHE; i++) {
    if (fcache[i].filep == filep) {
      return &fcache[i];
    }
  }
  if (!new)
    return NULL;
  for (int i = 0; i < CONFIG_NFCACHE; i++) {
    if (fcache[i].filep == 0) {
      return &fcache[i];
    }
  }
  return NULL;
}
#endif
#endif

//****************************************************************************
// Device driver interrupt rountine
//****************************************************************************

int interrupt(void)
{
  uint16_t err = 0;
  struct dos_req_header *req = reqheader;

  if (setjmp(jenv)) {
    return 0;
//    return com_timeout(req);
  }

  DPRINTF2("----Command: 0x%02x\r\n", req->command);

  req->command = (req->command & 0x1f) | (((req->unit + unit_base) & 7) << 5);

  switch (((req->command) & 0x1f) | 0x40) {
  case 0x40: /* init */
  {
    req->command = 0; /* for Human68k bug workaround */
//    int r = com_init(req);
    int r = 0;
    if (r >= 0) {
      req->attr = r; /* Number of units */
      extern char _end;
      req->addr = &_end;
      return 0;
    } else {
      return -r;
    }
    break;
  }

  case 0x41: /* chdir */
  {
#if 0
    struct cmd_dirop *cmd = &comp->cmd_dirop;
    struct res_dirop *res = &comp->res_dirop;
    cmd->command = req->command;
    memcpy(&cmd->path, req->addr, sizeof(struct dos_namestbuf));
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    DNAMEPRINT(req->addr, false, "CHDIR: ");
    DPRINTF1(" -> %d\r\n", res->res);
    req->status = res->res;
#endif
    break;
  }

  case 0x42: /* mkdir */
  {
#if 0
    struct cmd_dirop *cmd = &comp->cmd_dirop;
    struct res_dirop *res = &comp->res_dirop;
    cmd->command = req->command;
    memcpy(&cmd->path, req->addr, sizeof(struct dos_namestbuf));
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    DNAMEPRINT(req->addr, true, "MKDIR: ");
    DPRINTF1(" -> %d\r\n", res->res);
    req->status = res->res;
#endif
    break;
  }

  case 0x43: /* rmdir */
  {
#if 0
    struct cmd_dirop *cmd = &comp->cmd_dirop;
    struct res_dirop *res = &comp->res_dirop;
    cmd->command = req->command;
    memcpy(&cmd->path, req->addr, sizeof(struct dos_namestbuf));
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    DNAMEPRINT(req->addr, true, "RMDIR: ");
    DPRINTF1(" -> %d\r\n", res->res);
    req->status = res->res;
#endif
    break;
  }

  case 0x44: /* rename */
  {
#if 0
    struct cmd_rename *cmd = &comp->cmd_rename;
    struct res_rename *res = &comp->res_rename;
    cmd->command = req->command;
    memcpy(&cmd->path_old, req->addr, sizeof(struct dos_namestbuf));
    memcpy(&cmd->path_new, (void *)req->status, sizeof(struct dos_namestbuf));
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    DNAMEPRINT(req->addr, true, "RENAME: ");
    DNAMEPRINT((void *)req->status, true, " to ");
    DPRINTF1(" -> %d\r\n", res->res);
    req->status = res->res;
#endif
    break;
  }

  case 0x45: /* delete */
  {
#if 0
    struct cmd_dirop *cmd = &comp->cmd_dirop;
    struct res_dirop *res = &comp->res_dirop;
    cmd->command = req->command;
    memcpy(&cmd->path, req->addr, sizeof(struct dos_namestbuf));
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    DNAMEPRINT(req->addr, true, "DELETE: ");
    DPRINTF1(" -> %d\r\n", res->res);
    req->status = res->res;
#endif
    break;
  }

  case 0x46: /* chmod */
  {
#if 0
    struct cmd_chmod *cmd = &comp->cmd_chmod;
    struct res_chmod *res = &comp->res_chmod;
    cmd->command = req->command;
    cmd->attr = req->attr;
    memcpy(&cmd->path, req->addr, sizeof(struct dos_namestbuf));
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    DNAMEPRINT(req->addr, true, "CHMOD: ");
    DPRINTF1(" 0x%02x -> 0x%02x\r\n", req->attr, res->res);
    req->status = res->res;
#endif
    break;
  }

  case 0x47: /* files */
  {
#if 0
    struct cmd_files *cmd = &comp->cmd_files;
    struct res_files *res = &comp->res_files;
    cmd->command = req->command;
    cmd->attr = req->attr;
    cmd->filep = req->status;
    memcpy(&cmd->path, req->addr, sizeof(struct dos_namestbuf));
#if CONFIG_NFILEINFO > 1
    struct fcache *fc = fcache_alloc(cmd->filep, true);
    if (fc) {
      fc->filep = cmd->filep;
      fc->cnt = 0;
      cmd->num = CONFIG_NFILEINFO;
    } else {
      cmd->num = 1;
    }
#endif

    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));

    struct dos_filbuf *fb = (struct dos_filbuf *)req->status;
#if CONFIG_NFILEINFO > 1
    if (fc) {
      if (res->res == 0 && res->num > 1) {
        fc->files = *res;
        fc->cnt = 1;
      } else {
        fc->filep = 0;
      }
    }
#endif
    if (res->res == 0)
      memcpy(&fb->atr, &res->file[0].atr, sizeof(res->file[0]) - 1);
    DNAMEPRINT(req->addr, false, "FILES: ");
    DPRINTF1(" attr=0x%02x filep=0x%08x -> %d %s\r\n", req->attr, req->status, res->res, res->file[0].name);
    req->status = res->res;
#endif
    break;
  }

  case 0x48: /* nfiles */
  {
#if 0
    struct cmd_nfiles *cmd = &comp->cmd_nfiles;
    struct res_nfiles *res = &comp->res_nfiles;
    cmd->command = req->command;
    cmd->filep = req->status;

    struct dos_filbuf *fb = (struct dos_filbuf *)req->status;
#if CONFIG_NFILEINFO > 1
    struct fcache *fc;
    if (fc = fcache_alloc(cmd->filep, false)) {
      memcpy(&fb->atr, &fc->files.file[fc->cnt].atr, sizeof(fc->files.file[0]) - 1);
      fc->cnt++;
      res->res = 0;
      if (fc->cnt >= fc->files.num) {
        fc->filep = 0;
      }
      goto out_nfiles;
    }
    if (fc = fcache_alloc(cmd->filep, true)) {
      fc->filep = cmd->filep;
      fc->cnt = 0;
      cmd->num = CONFIG_NFILEINFO;
    } else {
      cmd->num = 1;
    }
#endif

    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));

#if CONFIG_NFILEINFO > 1
    if (fc) {
      if (res->res == 0 && res->num > 1) {
        fc->files = *(struct res_files *)res;
        fc->cnt = 1;
      } else {
        fc->filep = 0;
      }
    }
#endif
    if (res->res == 0)
      memcpy(&fb->atr, &res->file[0].atr, sizeof(res->file[0]) - 1);
out_nfiles:
    DPRINTF1("NFILES: filep=0x%08x -> %d %s\r\n", req->status, res->res, fb->name);
    req->status = res->res;
#endif
    break;
  }

  case 0x49: /* create */
  {
#if 0
    struct cmd_create *cmd = &comp->cmd_create;
    struct res_create *res = &comp->res_create;
    cmd->command = req->command;
    cmd->attr = req->attr;
    cmd->mode = req->status;
    cmd->fcb = (uint32_t)req->fcb;
    memcpy(&cmd->path, req->addr, sizeof(struct dos_namestbuf));
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    dos_fcb_size(req->fcb) = 0;
    DNAMEPRINT(req->addr, true, "CREATE: ");
    DPRINTF1(" fcb=0x%08x attr=0x%02x mode=%d -> %d\r\n", (uint32_t)req->fcb, req->attr, req->status, res->res);
    req->status = res->res;
#endif
    break;
  }

  case 0x4a: /* open */
  {
#if 0
    struct cmd_open *cmd = &comp->cmd_open;
    struct res_open *res = &comp->res_open;
    int mode = dos_fcb_mode(req->fcb);
    cmd->command = req->command;
    cmd->mode = mode;
    cmd->fcb = (uint32_t)req->fcb;
    memcpy(&cmd->path, req->addr, sizeof(struct dos_namestbuf));
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    dos_fcb_size(req->fcb) = res->size;
    DNAMEPRINT(req->addr, true, "OPEN: ");
    DPRINTF1(" fcb=0x%08x mode=%d -> %d %d\r\n", (uint32_t)req->fcb, mode, res->res, res->size);
    req->status = res->res;
#endif
    break;
  }

  case 0x4b: /* close */
  {
#if 0
    dcache_flash((uint32_t)req->fcb, true);

    struct cmd_close *cmd = &comp->cmd_close;
    struct res_close *res = &comp->res_close;
    cmd->command = req->command;
    cmd->fcb = (uint32_t)req->fcb;
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    DPRINTF1("CLOSE: fcb=0x%08x\r\n", (uint32_t)req->fcb);
    req->status = res->res;
#endif
    break;
  }

  case 0x4c: /* read */
  {
#if 0
    dcache_flash((uint32_t)req->fcb, false);

    uint32_t *pp = &dos_fcb_fpos(req->fcb);
    char *buf = (char *)req->addr;
    size_t len = (size_t)req->status;
    ssize_t size = 0;
    struct dcache *d;
    
    if (d = dcache_alloc((uint32_t)req->fcb)) {
      // キャッシュが未使用または自分のデータが入っている場合
      do {
        if (d->fcb == (uint32_t)req->fcb &&
            *pp >= d->pos && *pp < d->pos + d->len) {
          // これから読むデータがキャッシュに入っている場合、キャッシュから読めるだけ読む
          size_t clen = d->pos + d->len - *pp;   // キャッシュから読めるサイズ
          clen = clen < len ? clen : len;

          memcpy(buf, d->cache + (*pp - d->pos), clen);
          buf += clen;
          len -= clen;
          size += clen;
          *pp += clen;    // FCBのファイルポインタを進める
        }
        if (len == 0 || len >= sizeof(d->cache))
          break;
        // キャッシュサイズ未満の読み込みならキャッシュを充填
        dcache_flash((uint32_t)req->fcb, true);
        d->len = send_read((uint32_t)req->fcb, d->cache, *pp, sizeof(d->cache));
        if (d->len < 0) {
          size = -1;
          goto errout_read;
        }
        d->fcb = (uint32_t)req->fcb;
        d->pos = *pp;
        d->dirty = false;
      } while (d->len > 0);
    }

    if (len > 0) {
      size_t rlen;
      rlen = send_read((uint32_t)req->fcb, buf, *pp, len);
      if (rlen < 0) {
        size = -1;
        goto errout_read;
      }
      size += rlen;
      *pp += rlen;    // FCBのファイルポインタを進める
    }

errout_read:
    DPRINTF1("READ: fcb=0x%08x %d -> %d\r\n", (uint32_t)req->fcb, req->status, size);
    req->status = size;
#endif
    break;
  }

  case 0x4d: /* write */
  {
#if 0
    uint32_t *pp = &dos_fcb_fpos(req->fcb);
    uint32_t *sp = &dos_fcb_size(req->fcb);
    size_t len = (uint32_t)req->status;
    struct dcache *d;

    if (len > 0 && len < sizeof(dcache[0].cache)) {  // 書き込みサイズがキャッシュサイズ未満
      if (d = dcache_alloc((uint32_t)req->fcb)) {
        // キャッシュが未使用または自分のデータが入っている場合
        if (d->fcb == (uint32_t)req->fcb) {         //キャッシュに自分のデータが入っている
          if ((*pp = d->pos + d->len) &&
              ((*pp + len) <= (d->pos + sizeof(d->cache)))) {
            // 書き込みデータがキャッシュに収まる場合はキャッシュに書く
            memcpy(d->cache + d->len, (char *)req->addr, len);
            d->len += len;
            d->dirty = true;
            goto okout_write;
          } else {    //キャッシュに収まらないのでフラッシュ
            dcache_flash((uint32_t)req->fcb, true);
          }
        }
        // 書き込みデータをキャッシュに書く
        d->fcb = (uint32_t)req->fcb;
        d->pos = *pp;
        memcpy(d->cache, (char *)req->addr, len);
        d->len = len;
        d->dirty = true;
        goto okout_write;
      }
    }

    dcache_flash((uint32_t)req->fcb, false);
    len = send_write((uint32_t)req->fcb, (char *)req->addr, *pp, (uint32_t)req->status);
    if (len == 0) {
      *sp = *pp;      //0バイト書き込み=truncateなのでFCBのファイルサイズをポインタ位置にする
    }

okout_write:
    if (len > 0) {
      *pp += len;     //FCBのファイルポインタを進める
      if (*pp > *sp)
        *sp = *pp;    //FCBのファイルサイズを増やす
    }
    DPRINTF1("WRITE: fcb=0x%08x %d -> %d\r\n", (uint32_t)req->fcb, req->status, len);
    req->status = len;
#endif
    break;
  }

  case 0x4e: /* seek */
  {
#if 0
    dcache_flash((uint32_t)req->fcb, false);

    int whence = req->attr;
    int32_t offset = req->status;
    uint32_t pos = dos_fcb_fpos(req->fcb);
    uint32_t size = dos_fcb_size(req->fcb);
    pos = (whence == 0 ? 0 : (whence == 1 ? pos : size)) + offset;
    if (pos > size) {   // ファイル末尾を越えてseekしようとした
      pos = _DOSE_CANTSEEK;
    } else {
      dos_fcb_fpos(req->fcb) = pos;
    }
    DPRINTF1("SEEK: fcb=0x%x offset=%d whence=%d -> %d\r\n", (uint32_t)req->fcb, offset, whence, pos);
    req->status = pos;
#endif
    break;
  }

  case 0x4f: /* filedate */
  {
#if 0
    struct cmd_filedate *cmd = &comp->cmd_filedate;
    struct res_filedate *res = &comp->res_filedate;
    cmd->command = req->command;
    cmd->fcb = (uint32_t)req->fcb;
    cmd->time = req->status & 0xffff;
    cmd->date = req->status >> 16;
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    DPRINTF1("FILEDATE: fcb=0x%08x 0x%04x 0x%04x -> 0x%04x 0x%04x\r\n", (uint32_t)req->fcb, req->status >> 16, req->status & 0xffff, res->date, res->time);
    req->status = res->time + (res->date << 16);
#endif
    break;
  }

  case 0x50: /* dskfre */
  {
#if 0
    struct cmd_dskfre *cmd = &comp->cmd_dskfre;
    struct res_dskfre *res = &comp->res_dskfre;
    cmd->command = req->command;
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));

    uint16_t *p = (uint16_t *)req->addr;
    p[0] = res->freeclu;
    p[1] = res->totalclu;
    p[2] = res->clusect;
    p[3] = res->sectsize;
    DPRINTF1("DSKFRE: free=%u total=%u clusect=%u sectsz=%u res=%d\r\n", res->freeclu, res->totalclu, res->clusect, res->sectsize, res->res);
    req->status = res->res;

#endif
    break;
  }

  case 0x51: /* drvctrl */
  {
    DPRINTF1("DRVCTRL:\r\n");
    req->attr = 2;
    req->status = 0;
    break;
  }

  case 0x52: /* getdbp */
  {
    DPRINTF1("GETDPB:\r\n");
    uint8_t *p = (uint8_t *)req->addr;
    memset(p, 0, 16);
    *(uint16_t *)&p[0] = 512;   // 一部のアプリがエラーになるので仮のセクタ長を設定しておく
    p[2] = 1;
    req->status = 0;
    break;
  }

  case 0x53: /* diskred */
    DPRINTF1("DISKRED:\r\n");
    req->status = 0;
    break;

  case 0x54: /* diskwrt */
    DPRINTF1("DISKWRT:\r\n");
    req->status = 0;
    break;

  case 0x55: /* ioctl */
    DPRINTF1("IOCTL:\r\n");
    req->status = 0;
    break;

  case 0x56: /* abort */
    DPRINTF1("ABORT:\r\n");
    req->status = 0;
    break;

  case 0x57: /* mediacheck */
    DPRINTF1("MEDIACHECK:\r\n");
    req->status = 0;
    break;

  case 0x58: /* lock */
    DPRINTF1("LOCK:\r\n");
    req->status = 0;
    break;

  default:
    break;
  }

  return err;
}

//****************************************************************************
// Dummy program entry
//****************************************************************************

void _start(void)
{}
