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
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <errno.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

#include <config.h>
#include <humandefs.h>
#include "smbfs.h"

typedef struct dos_namestbuf dos_namebuf;

struct smb2_context *path2smb2(const char *path, const char **shpath);

#include "fileop.h"

//****************************************************************************
// Global variables
//****************************************************************************

typedef char hostpath_t[256];

struct dos_req_header *reqheader;   // Human68kからのリクエストヘッダ
jmp_buf jenv;                       // タイムアウト時のジャンプ先
int unit_base = 0;                  // ユニット番号のベース値  (必要? TBD *****)

const char *rootpath[8];

#ifdef DEBUG
int debuglevel = 2;
#endif

uint32_t _vernum;

//****************************************************************************
// for debugging
//****************************************************************************

#ifdef DEBUG
char heap[1024 * 512];                // temporary heap for debug print
void *_HSTA = heap;
void *_HEND = heap + sizeof(heap);
void *_PSP;

void DPRINTF(int level, char *fmt, ...)
{
  char buf[256];
  va_list ap;

  if (debuglevel < level)
    return;

  va_start(ap, fmt);
  vsiprintf(buf, fmt, ap);
  va_end(ap);
  _iocs_b_print(buf);
}

void DNAMEPRINT(void *n, bool full, char *head)
{
  struct dos_namestbuf *b = (struct dos_namestbuf *)n;

  DPRINTF1("%s%c:", head, b->drive + 'A');
  for (int i = 0; i < 65 && b->path[i]; i++) {
      DPRINTF1("%c", (uint8_t)b->path[i] == 9 ? '\\' : (uint8_t)b->path[i]);
  }
  if (full)
    DPRINTF1("%.8s%.10s.%.3s", b->name1, b->name2, b->ext);
}
#endif

//****************************************************************************
// Utility routine
//****************************************************************************

#if 0
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

//////////////////////////////////////////////////////////////////////////////

struct smb2_context *path2smb2(const char *path, const char **shpath)
{}


//////////////////////////////////////////////////////////////////////////////

// struct statのファイル情報を変換する
static void conv_statinfo(TYPE_STAT *st, void *v)
{
  struct dos_filesinfo *f = (struct dos_filesinfo *)v;

  f->atr = FUNC_FILEMODE_ATTR(st);
  f->filelen = htobe32(STAT_SIZE(st));
  struct tm *tm = localtime(&STAT_MTIME(st));
  f->time = htobe16(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec >> 1);
  f->date = htobe16((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday);
}

// namestsのパスをホストのパスに変換する
// (derived from HFS.java by Makoto Kamada)
static int conv_namebuf(int unit, dos_namebuf *ns, bool full, hostpath_t *path)
{
  uint8_t bb[88];   // SJISでのパス名
  int k = 0;

  if (rootpath[unit] == NULL) { // ホストパスが割り当てられていない
    return -1;
  }

  // パスの区切りを 0x09 -> '/' に変更
  for (int i = 0; i < 65; ) {
    for (; i < 65 && ns->path[i] == 0x09; i++)  //0x09の並びを読み飛ばす
      ;
    if (i >= 65 || ns->path[i] == 0x00)   //ディレクトリ名がなかった
      break;
    bb[k++] = 0x2f;  //ディレクトリ名の手前の'/'
    for (; i < 65 && ns->path[i] != 0x00 && ns->path[i] != 0x09; i++)
      bb[k++] = ns->path[i];  //ディレクトリ名
  }
  // 主ファイル名を展開する
  if (full) {
    bb[k++] = 0x2f;  //主ファイル名の手前の'/'
    memcpy(&bb[k], ns->name1, sizeof(ns->name1));   //主ファイル名1
    k += sizeof(ns->name1);
    memcpy(&bb[k], ns->name2, sizeof(ns->name2));   //主ファイル名2
    k += sizeof(ns->name2);
    for (; k > 0 && bb[k - 1] == 0x00; k--)   //主ファイル名2の末尾の0x00を切り捨てる
      ;
    for (; k > 0 && bb[k - 1] == 0x20; k--)   //主ファイル名1の末尾の0x20を切り捨てる
      ;
    bb[k++] = 0x2e;  //拡張子の手前の'.'
    memcpy(&bb[k], ns->ext, sizeof(ns->ext));   //拡張子
    k += sizeof(ns->ext);
    for (; k > 0 && bb[k - 1] == 0x20; k--)   //拡張子の末尾の0x20を切り捨てる
      ;
    for (; k > 0 && bb[k - 1] == 0x2e; k--)   //主ファイル名の末尾の0x2eを切り捨てる
      ;
  }

  char *dst_buf = (char *)path;
  strncpy(dst_buf, rootpath[unit], sizeof(*path) - 1);
  int len = strlen(rootpath[unit]);
  if (len >= 1 && rootpath[unit][len - 1] == '/' && bb[0] == '/') {
    len--;          //rootpathの末尾から'/'が連続しないようにする
  }
  dst_buf += len;   //マウント先パス名を前置

  // SJIS -> UTF-8に変換
  size_t dst_len = sizeof(*path) - 1 - len;  //パス名バッファ残りサイズ
  char *src_buf = bb;
  size_t src_len = k;
  if (len == 0 && bb[0] == '/') {
    src_buf++;      //変換後のパス名の先頭に'/'が来ないようにする
    src_len--;
  }
  if (FUNC_ICONV_S2U(&src_buf, &src_len, &dst_buf, &dst_len) < 0) {
    return -1;  //変換できなかった
  }
  *dst_buf = '\0';
  return 0;
}

// errnoをHuman68kのエラーコードに変換する
static int conv_errno(int err)
{
  switch (err) {
  case ENOENT:
    return _DOSE_NOENT;
  case ENOTDIR:
    return _DOSE_NODIR;
  case EMFILE:
    return _DOSE_MFILE;
  case EISDIR:
    return _DOSE_ISDIR;
  case EBADF:
    return _DOSE_BADF;
  case ENOMEM:
    return _DOSE_NOMEM;
  case EFAULT:
    return _DOSE_ILGMPTR;
  case ENOEXEC:
    return _DOSE_ILGFMT;
  /* case EINVAL:       // open
    return _DOSE_ILGARG; */
  case ENAMETOOLONG:
    return _DOSE_ILGFNAME;
  case EINVAL:
    return _DOSE_ILGPARM;
  case EXDEV:
    return _DOSE_ILGDRV;
  /* case EINVAL:       // rmdir
    return _DOSE_ISCURDIR; */
  case EACCES:
  case EPERM:
  case EROFS:
    return _DOSE_RDONLY;
  /* case EEXIST:       // mkdir
    return _DOSE_EXISTDIR; */
  case ENOTEMPTY:
    return _DOSE_NOTEMPTY;
  /* case ENOTEMPTY:    // rename
    return _DOSE_CANTREN; */
  case ENOSPC:
    return _DOSE_DISKFULL;
  /* case ENOSPC:       // create, open
    return _DOSE_DIRFULL; */
  case EOVERFLOW:
    return _DOSE_CANTSEEK;
  case EEXIST:
    return _DOSE_EXISTFILE;
  default:
    return _DOSE_ILGPARM;
  }
}

//****************************************************************************
// Device driver interrupt rountine
//****************************************************************************

static int my_atoi(char *p)
{
  int res = 0;
  while (*p >= '0' && *p <= '9') {
    res = res * 10 + *p++ - '0';
  }
  return res;
}

int com_init(struct dos_req_header *req)
{
  int units = 1;
  _dos_print
    ("\r\nX68000 Samba filesystem (version " GIT_REPO_VERSION ")\r\n");

  char *p = (char *)req->status;
  p += strlen(p) + 1;
  while (*p != '\0') {
    if (*p == '/' || *p =='-') {
      p++;
      switch (*p | 0x20) {
      case 'd':         // /D .. デバッグレベル増加
        debuglevel++;
        break;
      case 'u':         // /u<units> .. ユニット数設定
        p++;
        units = my_atoi(p);
        if (units < 1 || units > 7)
          units = 1;
        break;
      }
    }
    p += strlen(p) + 1;
  }

  _dos_print("ドライブ");
  _dos_putchar('A' + *(char *)&req->fcb);
  if (units > 1) {
    _dos_print(":-");
    _dos_putchar('A' + *(char *)&req->fcb + units - 1);
  }
  _dos_print(":でsmbfsが利用可能です\r\n");

  DPRINTF1("Debug level: %d\r\n", debuglevel);

  return units;
}

//****************************************************************************
// Filesystem operations
//****************************************************************************

int op_chdir(struct dos_req_header *req)
{
  hostpath_t path;

  DNAMEPRINT(req->addr, false, "CHDIR: ");

  if (conv_namebuf(req->unit, req->addr, false, &path) < 0) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  }

  TYPE_STAT st;
  if (FUNC_STAT(req->unit, NULL, path, &st) != 0 || !STAT_ISDIR(&st)) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  } else {
    DPRINTF1("-> 0\r\n");
    return 0;
  }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_mkdir(struct dos_req_header *req)
{
  hostpath_t path;

  DNAMEPRINT(req->addr, true, "MKDIR: ");

  if (conv_namebuf(req->unit, req->addr, true, &path) < 0) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  }

  int err;
  FUNC_MKDIR(req->unit, &err, path);
  switch (err) {
  case EEXIST:
    DPRINTF1("-> EXISTDIR\r\n");
    return _DOSE_EXISTDIR;
  default:
    err = conv_errno(err);
    DPRINTF1("-> %d\r\n", err);
    return err;
  }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_rmdir(struct dos_req_header *req)
{
  hostpath_t path;

  DNAMEPRINT(req->addr, true, "RMDIR: ");

  if (conv_namebuf(req->unit, req->addr, true, &path) < 0) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  }

  int err;
  FUNC_RMDIR(req->unit, &err, path);
  switch (err) {
  case EINVAL:
    DPRINTF1("-> ISCURDIR\r\n");
    return _DOSE_ISCURDIR;
  default:
    err = conv_errno(err);
    DPRINTF1("-> %d\r\n", err);
    return err;
  }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_rename(struct dos_req_header *req)
{
#if 0
  struct cmd_rename *cmd = (struct cmd_rename *)cbuf;
  struct res_rename *res = (struct res_rename *)rbuf;
  hostpath_t pathold;
  hostpath_t pathnew;

  res->res = 0;

  if (conv_namebuf(unit, &cmd->path_old, true, &pathold) < 0) {
    res->res = _DOSE_NODIR;
    goto errout;
  }
  if (conv_namebuf(unit, &cmd->path_new, true, &pathnew) < 0) {
    res->res = _DOSE_NODIR;
    goto errout;
  }

  int err;
  if (FUNC_RENAME(unit, &err, pathold, pathnew) < 0) {
    switch (err) {
    case ENOTEMPTY:
      res->res = _DOSE_CANTREN;
      break;
    default:
      res->res = conv_errno(err);
      break;
    }
  }
errout:
  DPRINTF1("RENAME: %s to %s  -> %d\n", pathold, pathnew, res->res);
  return sizeof(*res);
}

{
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
    break;
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_delete(struct dos_req_header *req)
{
#if 0
  struct cmd_dirop *cmd = (struct cmd_dirop *)cbuf;
  struct res_dirop *res = (struct res_dirop *)rbuf;
  hostpath_t path;

  res->res = 0;

  if (conv_namebuf(unit, &cmd->path, true, &path) < 0) {
    res->res = _DOSE_NODIR;
    goto errout;
  }

  int err;
  if (FUNC_UNLINK(unit, &err, path) < 0) {
    res->res = conv_errno(err);
  }
errout:
  DPRINTF1("DELETE: %s -> %d\n", path, res->res);
  return sizeof(*res);
}


  case 0x45: /* delete */
  {
    struct cmd_dirop *cmd = &comp->cmd_dirop;
    struct res_dirop *res = &comp->res_dirop;
    cmd->command = req->command;
    memcpy(&cmd->path, req->addr, sizeof(struct dos_namestbuf));
    com_cmdres(cmd, sizeof(*cmd), res, sizeof(*res));
    DNAMEPRINT(req->addr, true, "DELETE: ");
    DPRINTF1(" -> %d\r\n", res->res);
    req->status = res->res;
    break;
  }


#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_chmod(struct dos_req_header *req)
{
#if 0
  struct cmd_chmod *cmd = (struct cmd_chmod *)cbuf;
  struct res_chmod *res = (struct res_chmod *)rbuf;
  hostpath_t path;
  TYPE_STAT st;

  res->res = 0;

  if (conv_namebuf(unit, &cmd->path, true, &path) < 0) {
    res->res = _DOSE_NODIR;
    goto errout;
  }

  int err;
  if (FUNC_STAT(unit, &err, path, &st) < 0) {
    res->res = conv_errno(err);
  } else {
    res->res = FUNC_FILEMODE_ATTR(&st);
  }
  if (cmd->attr != 0xff) {
    if (FUNC_CHMOD(unit, &err, path, FUNC_ATTR_FILEMODE(cmd->attr, &st)) < 0) {
      res->res = conv_errno(err);
    } else {
      res->res = 0;
    }
  }
errout:
  if (res->res < 0)
    DPRINTF1("CHMOD: %s 0x%02x -> %d\n", path, cmd->attr, res->res);
  else
    DPRINTF1("CHMOD: %s 0x%02x -> 0x%02x\n", path, cmd->attr, res->res);
  return sizeof(*res);
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
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if 0
static int dl_opendir(dirlist_t **dlp, int unit, struct cmd_files *cmd)
{
  dirlist_t *dl;
  int res = 0;
  *dlp = NULL;

  if ((dl = dl_alloc(cmd->filep, true)) == NULL) {
    return ENOMEM;
  }

  if (conv_namebuf(unit, &cmd->path, false, &dl->hostpath) < 0) {
    dl_free(dl);
    return ENOENT;
  }
  dl->unit = unit;  
  dl->isroot = strcmp(cmd->path.path, "\t") == 0;
  dl->isfirst = true;
  dl->attr = cmd->attr;

  // (derived from HFS.java by Makoto Kamada)
  //検索するファイル名の順序を入れ替える
  //  主ファイル名1の末尾が'?'で主ファイル名2の先頭が'\0'のときは主ファイル名2を'?'で充填する
  memset(dl->fname, 0, sizeof(dl->fname));
  memcpy(&dl->fname[0], cmd->path.name1, 8);    //主ファイル名1
  if (cmd->path.name1[7] == '?' && cmd->path.name2[0] == '\0') {  //主ファイル名1の末尾が'?'で主ファイル名2の先頭が'\0'
    memset(&dl->fname[8], '?', 10);           //主ファイル名2
  } else {
    memcpy(&dl->fname[8], cmd->path.name2, 10); //主ファイル名2
  }
  for (int i = 17; i >= 0 && (dl->fname[i] == '\0' || dl->fname[i] == ' '); i--) {  //主ファイル名1+主ファイル名2の空き
    dl->fname[i] = '\0';
  }
  memcpy(&dl->fname[18], cmd->path.ext, 3);     //拡張子
  for (int i = 20; i >= 18 && (dl->fname[i] == ' '); i--) { //拡張子の空き
    dl->fname[i] = '\0';
  }
  //検索するファイル名を小文字化する
  for (int i = 0; i < 21; i++) {
    int c = dl->fname[i];
    if (0x81 <= c && c <= 0x9f || 0xe0 <= c && c <= 0xef) {  //SJISの1バイト目
      i++;
    } else {
      dl->fname[i] = tolower(dl->fname[i]);
    }
  }

  DPRINTF2("dl_opendir: %02x ", dl->attr);
  for (int i = 0; i < 21; i++)
    DPRINTF2("%c", dl->fname[i] == 0 ? '_' : dl->fname[i]);
  DPRINTF2("\n");

  //ディレクトリを開いてディスクリプタを得る
  int err;
  if ((dl->dir = FUNC_OPENDIR(unit, &err, dl->hostpath)) == DIR_BADDIR) {
    return err;
  }

  *dlp = dl;
  return 0;
}

int dl_readdir(dirlist_t *dl, void *v)
{
  TYPE_DIRENT *d;
  struct dos_filesinfo *fi = (struct dos_filesinfo *)v;

  if (dl->isfirst && dl->isroot && (dl->attr & 0x08) != 0 &&
      dl->fname[0] == '?' && dl->fname[18] == '?') {    //検索するファイル名が*.*のとき
    //ボリューム名エントリを作る
    fi->atr = 0x08;   //ボリューム名
    fi->time = fi->date = 0;
    fi->filelen = 0;
    // ファイル名をSJISに変換する
    char *dst_buf = fi->name;
    size_t dst_len = sizeof(fi->name) - 2;
    char *src_buf = dl->hostpath;
    size_t src_len = strlen(dl->hostpath);
    FUNC_ICONV_U2S(&src_buf, &src_len, &dst_buf, &dst_len);
    *dst_buf = '\0';
    dl->isfirst = false;
    return 1;
  }

  dl->isfirst = false;
  //ディレクトリの一覧から属性とファイル名の条件に合うものを選ぶ
  while (d = FUNC_READDIR(dl->unit, NULL, dl->dir)) {
    char *childName = DIRENT_NAME(d);

    if (dl->isroot) {  //ルートディレクトリのとき
      if (strcmp(childName, ".") == 0 || strcmp(childName, "..") == 0) {  //.と..を除く
        continue;
      }
    }

    // ファイル名をSJISに変換する
    char *dst_buf = fi->name;
    size_t dst_len = sizeof(fi->name) - 1;
    char *src_buf = childName;
    size_t src_len = strlen(childName);
    if (FUNC_ICONV_U2S(&src_buf, &src_len, &dst_buf, &dst_len) < 0) {
      continue;
    }
    *dst_buf = '\0';
    uint8_t c;
    for (int i = 0; i < sizeof(fi->name); i++) {
      if (!(c = fi->name[i]))
        break;
      if (0x81 <= c && c <= 0x9f || 0xe0 <= c && c <= 0xef) {  //SJISの1バイト目
        i++;
        continue;
      }
      if (c <= 0x1f ||  //変換できない文字または制御コード
          (c == '-' && i == 0) ||  //ファイル名の先頭に使えない文字
          strchr("/\\,;<=>[]|", c) != NULL) {  //ファイル名に使えない文字
        break;
      }
    }
    if (c) {  //ファイル名に使えない文字がある
      continue;
    }

    //ファイル名を分解する
    char *b = fi->name;
    int k = strlen(b);
    int m = (b[k - 1] == '.' ? k :  //name.
             k >= 3 && b[k - 2] == '.' ? k - 2 :  //name.e
             k >= 4 && b[k - 3] == '.' ? k - 3 :  //name.ex
             k >= 5 && b[k - 4] == '.' ? k - 4 :  //name.ext
             k);  //主ファイル名の直後。拡張子があるときは'.'の位置、ないときはk
    if (m > 18) {  //主ファイル名が長すぎる
      continue;
    }
    uint8_t w2[21] = { 0 };
    memcpy(&w2[0], &b[0], m);         //主ファイル名
    if (b[m] == '.')
      strncpy(&w2[18], &b[m + 1], 3); //拡張子

    for (int i = 0; i < 21; i++)
      DPRINTF2("%c", w2[i] == 0 ? '_' : w2[i]);
    DPRINTF2("\n");

    //ファイル名を比較する
    {
      int f = 0x20;  //0x00=次のバイトはSJISの2バイト目,0x20=次のバイトはSJISの2バイト目ではない
      int i;
      for (i = 0; i < 21; i++) {
        int c = w2[i];
        int d = dl->fname[i];
        if (d != '?' && ('A' <= c && c <= 'Z' ? c | f : c) != d) {  //検索するファイル名の'?'以外の部分がマッチしない。SJISの2バイト目でなければ小文字化してから比較する
          break;
        }
        f = f != 0x00 && (0x81 <= c && c <= 0x9f || 0xe0 <= c && c <= 0xef) ? 0x00 : 0x20;  //このバイトがSJISの2バイト目ではなくてSJISの1バイト目ならば次のバイトはSJISの2バイト目
      }
      if (i < 21) { //ファイル名がマッチしなかった
        continue;
      }
    }

    //属性、時刻、日付、ファイルサイズを取得する
    hostpath_t fullpath;
    strcpy(fullpath, dl->hostpath);
    int len = strlen(fullpath);
    if (len > 0 && fullpath[len - 1] != '/') {
      strncat(fullpath, "/", sizeof(fullpath) - 1);
    }
    strncat(fullpath, childName, sizeof(fullpath) - 1);
    TYPE_STAT st;
    if (FUNC_STAT(dl->unit, NULL, fullpath, &st) < 0) {  // ファイル情報を取得できなかった
      continue;
    }
    if (0xffffffffL < STAT_SIZE(&st)) {  //4GB以上のファイルは検索できないことにする
      continue;
    }
    conv_statinfo(&st, fi);
    if ((fi->atr & dl->attr) == 0) {  //属性がマッチしない
      continue;
    }

    return 1;
  }

  dl_free(dl);
  return 0;   // もうファイルがない
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_files(int unit, uint8_t *cbuf, uint8_t *rbuf)
{
  struct cmd_files *cmd = (struct cmd_files *)cbuf;
  struct res_files *res = (struct res_files *)rbuf;
  dirlist_t *dl;

  res->res = _DOSE_NOMORE;
#if CONFIG_NFILEINFO > 1
  res->num = 0;
#endif

  int err = dl_opendir(&dl, unit, cmd);
  if (err) {
    switch (err) {
    case ENOENT:
      res->res = _DOSE_NODIR;    //ディレクトリが存在しない場合に_DOSE_NOENTを返すと正常動作しない
      break;
    default:
      res->res = conv_errno(err);
      break;
    }
    goto errout;
  }

  int n = CONFIG_NFILEINFO;
#if CONFIG_NFILEINFO > 1
  n = n > cmd->num ? cmd->num : n;
#endif
  
  for (int i = 0; i < n; i++) {
    if (dl_readdir(dl, &res->file[i]) == 0) {
      break;
    }
#if CONFIG_NFILEINFO > 1
    res->num++;
#endif
    res->res = 0;
    DPRINTF1("(%d/%d) %s\n", i, n, res->file[i].name);
  }

errout:
#if CONFIG_NFILEINFO > 1
  DPRINTF1("FILES: 0x%08x 0x%02x %d %s -> ", cmd->filep, cmd->attr, cmd->num, dl->hostpath);
#else
  DPRINTF1("FILES: 0x%08x 0x%02x %s -> ", cmd->filep, cmd->attr, dl->hostpath);
#endif
  DPRINTF1("%d\n", res->res);

  return sizeof(*res);
}



#if 0

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
#endif
#endif


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_nfiles(struct dos_req_header *req)
{
#if 0
  struct cmd_nfiles *cmd = (struct cmd_nfiles *)cbuf;
  struct res_nfiles *res = (struct res_nfiles *)rbuf;
  dirlist_t *dl;

  res->res = _DOSE_NOMORE;
#if CONFIG_NFILEINFO > 1
  res->num = 0;
#endif

  if (dl = dl_alloc(cmd->filep, false)) {
    int n = CONFIG_NFILEINFO;
#if CONFIG_NFILEINFO > 1
    n = n > cmd->num ? cmd->num : n;
#endif

    for (int i = 0; i < n; i++) {
      if (dl_readdir(dl, &res->file[i]) == 0) {
        break;
      }
#if CONFIG_NFILEINFO > 1
      res->num++;
#endif
      res->res = 0;
      DPRINTF1("(%d/%d) %s\n", i, n, res->file[i].name);
    }
  }

#if CONFIG_NFILEINFO > 1
  DPRINTF1("NFILES: 0x%08x %d -> ", cmd->filep, cmd->num);
#else
  DPRINTF1("NFILES: 0x%08x -> ", cmd->filep);
#endif
  DPRINTF1("%d\n", res->res);

  return sizeof(*res);
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
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_create(struct dos_req_header *req)
{
#if 0
  struct cmd_create *cmd = (struct cmd_create *)cbuf;
  struct res_create *res = (struct res_create *)rbuf;
  hostpath_t path;
  TYPE_FD filefd;

  res->res = 0;

  if (conv_namebuf(unit, &cmd->path, true, &path) < 0) {
    res->res = _DOSE_NODIR;
    goto errout;
  }

  int mode = O_CREAT|O_RDWR|O_TRUNC|O_BINARY;
  mode |= cmd->mode ? 0 : O_EXCL;
  int err;
  if ((filefd = FUNC_OPEN(unit, &err, path, mode)) == FD_BADFD) {
    switch (err) {
    case ENOSPC:
      res->res = _DOSE_DIRFULL;
      break;
    default:
      res->res = conv_errno(err);
      break;
    }
  } else {
    fdinfo_t *fi = fi_alloc(unit, cmd->fcb, true);
    fi->fd = filefd;
    fi->pos = 0;
  }
errout:
  DPRINTF1("CREATE: fcb=0x%08x attr=0x%02x mode=%d %s -> %d\n", cmd->fcb, cmd->attr, cmd->mode, path, res->res);
  return sizeof(*res);
}


case 0x49: /* create */
  {

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

    break;
  }
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_open(struct dos_req_header *req)
{
#if 0
  struct cmd_open *cmd = (struct cmd_open *)cbuf;
  struct res_open *res = (struct res_open *)rbuf;
  hostpath_t path;
  int mode;
  TYPE_FD filefd;

  res->res = 0;

  if (conv_namebuf(unit, &cmd->path, true, &path) < 0) {
    res->res = _DOSE_NODIR;
    goto errout;
  }

  switch (cmd->mode) {
  case 0:
    mode = O_RDONLY|O_BINARY;
    break;
  case 1:
    mode = O_WRONLY|O_BINARY;
    break;
  case 2:
    mode = O_RDWR|O_BINARY;
    break;
  default:
    res->res = _DOSE_ILGARG;
    goto errout;
  }

  int err;
  if ((filefd = FUNC_OPEN(unit, &err, path, mode)) == FD_BADFD) {
    switch (err) {
    case EINVAL:
      res->res = _DOSE_ILGARG;
      break;
    default:
      res->res = conv_errno(err);
      break;
    }
  } else {
    fdinfo_t *fi = fi_alloc(unit, cmd->fcb, true);
    fi->fd = filefd;
    fi->pos = 0;
    uint32_t len = FUNC_LSEEK(unit, NULL, filefd, 0, SEEK_END);
    FUNC_LSEEK(unit, NULL, filefd, 0, SEEK_SET);
    res->size = htobe32(len);
  }
errout:
  DPRINTF1("OPEN: fcb=0x%08x mode=%d %s -> %d %d\n", cmd->fcb, cmd->mode, path, res->res, be32toh(res->size));
  return sizeof(*res);
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
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_close(struct dos_req_header *req)
{
#if 0
  struct cmd_close *cmd = (struct cmd_close *)cbuf;
  struct res_close *res = (struct res_close *)rbuf;
  fdinfo_t *fi = fi_alloc(unit, cmd->fcb, false);
  res->res = 0;

  if (!fi) {
    res->res = _DOSE_BADF;
    goto errout;
  }

  int err;
  if (FUNC_CLOSE(unit, &err, fi->fd) < 0) {
    res->res = conv_errno(err);
  }

errout:
  fi_free(cmd->fcb);
  DPRINTF1("CLOSE: fcb=0x%08x\n", cmd->fcb);
  return sizeof(*res);
}

void op_closeall(int unit)
{
  fi_freeall(unit);
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
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_read(struct dos_req_header *req)
{
#if 0
  struct cmd_read *cmd = (struct cmd_read *)cbuf;
  struct res_read *res = (struct res_read *)rbuf;
  fdinfo_t *fi = fi_alloc(unit, cmd->fcb, false);
  uint32_t pos = be32toh(cmd->pos);
  size_t len = be16toh(cmd->len);
  ssize_t bytes = 0;

  if (!fi) {
    res->len = _DOSE_BADF;
    goto errout;
  }

  int err;
  if (fi->pos != pos) {
    if (FUNC_LSEEK(unit, &err, fi->fd, pos, SEEK_SET) < 0) {
      res->len = htobe32(conv_errno(err));
      goto errout;
    }
  }
  bytes = FUNC_READ(unit, &err, fi->fd, res->data, len);
  if (bytes < 0) {
    res->len = htobe16(conv_errno(err));
    bytes = 0;
  } else {
    res->len = htobe16(bytes);
    fi->pos += bytes;
  }

errout:
  DPRINTF1("READ: fcb=0x%08x %d %d -> %d\n", cmd->fcb, pos, len, bytes);
  return offsetof(struct res_read, data) + bytes;
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
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_write(struct dos_req_header *req)
{
#if 0
  struct cmd_write *cmd = (struct cmd_write *)cbuf;
  struct res_write *res = (struct res_write *)rbuf;
  fdinfo_t *fi = fi_alloc(unit, cmd->fcb, false);
  uint32_t pos = be32toh(cmd->pos);
  size_t len = be16toh(cmd->len);
  ssize_t bytes;

  if (!fi) {
    res->len = _DOSE_BADF;
    goto errout;
  }

  int err;
  if (len == 0) {     // 0バイトのwriteはファイル長を切り詰める
    if (FUNC_FTRUNCATE(unit, &err, fi->fd, pos) < 0) {
      res->len = htobe16(conv_errno(err));
    } else {
      res->len = 0;
    }
  } else {
    if (fi->pos != pos) {
      if (FUNC_LSEEK(unit, &err, fi->fd, pos, SEEK_SET) < 0) {
        res->len = htobe32(conv_errno(err));
        goto errout;
      }
    }
    bytes = FUNC_WRITE(unit, &err, fi->fd, cmd->data, len);
    if (bytes < 0) {
      res->len = htobe16(conv_errno(err));
    } else {
      res->len = htobe16(bytes);
      fi->pos += bytes;
    }
  }

errout:
  DPRINTF1("WRITE: fcb=0x%08x %d %d -> %d\n", cmd->fcb, pos, len, bytes);
  return sizeof(*res);
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
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_seek(struct dos_req_header *req)
{

  #if 0

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
#endif

  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_filedate(struct dos_req_header *req)
{
#if 0
  struct cmd_filedate *cmd = (struct cmd_filedate *)cbuf;
  struct res_filedate *res = (struct res_filedate *)rbuf;
  fdinfo_t *fi = fi_alloc(unit, cmd->fcb, false);

  if (!fi) {
    res->date = 0xffff;
    res->time = _DOSE_BADF;
    goto errout;
  }

  int err;
  if (cmd->time == 0 && cmd->date == 0) {   // 更新日時取得
    TYPE_STAT st;
    if (FUNC_FSTAT(unit, &err, fi->fd, &st) < 0) {
      res->date = 0xffff;
      res->time = htobe32(conv_errno(err));
    } else {
      struct dos_filesinfo fi;
      conv_statinfo(&st, &fi);
      res->time = fi.time;
      res->date = fi.date;
    }
  } else {                                  // 更新日時設定
    uint16_t time = be16toh(cmd->time);
    uint16_t date = be16toh(cmd->date);
    if (FUNC_FILEDATE(unit, &err, fi->fd, time, date) < 0) {
      res->date = 0xffff;
      res->time = htobe32(conv_errno(err));
    } else {
      res->date = 0;
      res->time = 0;
    }
  }

errout:
  DPRINTF1("FILEDATE: fcb=0x%08x 0x%04x 0x%04x -> 0x%04x 0x%04x\n", cmd->fcb, be16toh(cmd->date), be16toh(cmd->time), be16toh(res->date), be16toh(res->time));
  return sizeof(*res);
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
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_dskfre(struct dos_req_header *req)
{
#if 0
  struct cmd_dskfre *cmd = (struct cmd_dskfre *)cbuf;
  struct res_dskfre *res = (struct res_dskfre *)rbuf;
  uint64_t total;
  uint64_t free;

  res->freeclu = res->totalclu = res->clusect = res->sectsize = 0;
  res->res = 0;

  if (rootpath[unit] != NULL) {
    FUNC_STATFS(unit, NULL, rootpath[unit], &total, &free);
    total = total > 0x7fffffff ? 0x7fffffff : total;
    free = free > 0x7fffffff ? 0x7fffffff : free;
    res->freeclu = htobe16(free / 32768);
    res->totalclu = htobe16(total /32768);
    res->clusect = htobe16(128);
    res->sectsize = htobe16(1024);
    res->res = htobe32(free);
  }

  DPRINTF1("DSKFRE: free=%u total=%u clusect=%u sectsz=%u res=%d\n", be16toh(res->freeclu), be16toh(res->totalclu), be16toh(res->clusect), be16toh(res->sectsize), be32toh(res->res));
  return sizeof(*res);
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
#endif
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if 0

case 0x51: /* drvctrl */
  {
    DPRINTF1("DRVCTRL:\r\n");
    req->attr = 2;
    req->status = 0;
    break;
  }
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if 0

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
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if 0

case 0x53: /* diskred */
    DPRINTF1("DISKRED:\r\n");
    req->status = 0;
    break;

#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if 0

case 0x54: /* diskwrt */
    DPRINTF1("DISKWRT:\r\n");
    req->status = 0;
    break;
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if 0

case 0x55: /* ioctl */
    DPRINTF1("IOCTL:\r\n");
    req->status = 0;
    break;
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if 0

case 0x56: /* abort */
    DPRINTF1("ABORT:\r\n");
    req->status = 0;
    break;
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if 0

case 0x57: /* mediacheck */
    DPRINTF1("MEDIACHECK:\r\n");
    req->status = 0;
    break;
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#if 0

case 0x58: /* lock */
    DPRINTF1("LOCK:\r\n");
    req->status = 0;
    break;

  default:
    break;
  }
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

  switch (req->command) {
  case 0x40: /* init */
  {
    _vernum = _dos_vernum();

    req->command = 0; /* for Human68k bug workaround */
    int r = com_init(req);
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
    req->status = op_chdir(req);
    break;
  case 0x42: /* mkdir */
    req->status = op_mkdir(req);
    break;
  case 0x43: /* rmdir */
    req->status = op_rmdir(req);
    break;

  #if 0
  case 0x44: /* rename */
    rsize = op_rename(unit, cbuf, rbuf);
    break;
  case 0x45: /* remove */
    rsize = op_delete(unit, cbuf, rbuf);
    break;
  case 0x46: /* chmod */
    rsize = op_chmod(unit, cbuf, rbuf);
    break;
  case 0x47: /* files */
    rsize = op_files(unit, cbuf, rbuf);
    break;
  case 0x48: /* nfiles */
    rsize = op_nfiles(unit, cbuf, rbuf);
    break;
  case 0x49: /* create */
    rsize = op_create(unit, cbuf, rbuf);
    break;
  case 0x4a: /* open */
    rsize = op_open(unit, cbuf, rbuf);
    break;
  case 0x4b: /* close */
    rsize = op_close(unit, cbuf, rbuf);
    break;
  case 0x4c: /* read */
    rsize = op_read(unit, cbuf, rbuf);
    break;
  case 0x4d: /* write */
    rsize = op_write(unit, cbuf, rbuf);
    break;
  case 0x4e: /* seek */
    req->status = op_seek(req);
    break;
  case 0x4f: /* filedate */
    rsize = op_filedate(unit, cbuf, rbuf);
    break;
  case 0x50: /* dskfre */
    rsize = op_dskfre(unit, cbuf, rbuf);
    break;
#endif

  case 0x51: /* drvctrl */
  case 0x52: /* getdbp */
  case 0x53: /* diskred */
  case 0x54: /* diskwrt */
  case 0x55: /* ioctl */
  case 0x56: /* abort */
  case 0x57: /* mediacheck */
  case 0x58: /* lock */
    req->status = 0;
    break;

  default:
    req->status = 0;
    err = 0x1003;  // 不正なコマンドコード
  }


  return err;
}

//****************************************************************************
// Program entry
//****************************************************************************

extern struct dos_devheader devheader;

struct dos_dpb dummy_dpb = {
  0,
  0,
  &devheader,
  0,
  0
};

void _start()
{
  _dos_super(0);

  char *drvxtbl = (char *)0x1c7e;     // ドライブ交換テーブル
  struct dos_curdir *curdir_table = *(struct dos_curdir **)0x1c38;

  int drv;
  int realdrv;
  struct dos_dpb *dpb;
  struct dos_curdir *curdir;

  realdrv = -1;
  for (drv = 0; drv < 26; drv++) {
    realdrv = drvxtbl[drv];
    curdir = &curdir_table[realdrv];
    if (curdir->type == 0x40) {
      dpb = curdir->dpb;
      if (memcmp(dpb->devheader->name, CONFIG_DEVNAME, 8) == 0) {
        break;
      }
    }
  }

  if (drv < 26) {
    // 常駐解除
    // TODO: バッファフラッシュ
    // TODO: ディレクトリ移動、subst設定時の対応
    printf("free %c: %p\n", 'A' + drv, dpb->devheader);
    curdir->type = 0;
    struct dos_dpb *olddpb = (struct dos_dpb *)((char *)dpb->devheader + ((char *)&dummy_dpb - (char *)&devheader));
    for (int i = 0; i < 26; i++) {
      if (curdir_table[i].type == 0x40 &&
        curdir_table[i].dpb->next == olddpb) {
        curdir_table[i].dpb->next = olddpb->next;
      }
    }
    _dos_mfree((char *)dpb->devheader - 0xf0);
    _dos_exit();
  }

  realdrv = -1;
  for (drv = 0; drv < 26; drv++) {
    realdrv = drvxtbl[drv];
    if (curdir_table[realdrv].type == 0) {
      break;
    }
  }
  if (realdrv < 0 || realdrv > *(char *)0x1c73) {
    printf("割り当て可能なドライブがありません\n");
    _dos_exit();
  }

  printf("ドライブ %c: (real %c:)\n", 'A' + drv, 'A' + realdrv);

  dummy_dpb.drive = realdrv; 
  dummy_dpb.next = (void *)-1;

  struct dos_dpb *prev_dpb = NULL;
  for (int i = 0; i < realdrv; i++) {
    if (curdir_table[i].type == 0x40) {
      prev_dpb = curdir_table[i].dpb;
    }
  }
  if (prev_dpb != NULL) {
    dummy_dpb.next = prev_dpb->next;
    prev_dpb->next = &dummy_dpb;
  }

  curdir = &curdir_table[realdrv];
  curdir->drive = 'A' + realdrv;
  curdir->coron = ':';
  curdir->path[0] = '\t';
  curdir->path[1] = '\0';
  curdir->type = 0x40;
  curdir->dpb = &dummy_dpb;
  curdir->fatno = (int)-1;
  curdir->pathlen = 2;

  extern char _end;
  _dos_keeppr((int)&_end - (int)&devheader, 0);
}
