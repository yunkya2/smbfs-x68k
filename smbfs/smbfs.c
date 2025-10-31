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
#include <ctype.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/socket.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

#include <smb2.h>
#include <libsmb2.h>
#include <libsmb2-raw.h>
#include <libsmb2-private.h>

#include <config.h>
#include <humandefs.h>
#include <smbfscmd.h>

#include "smbfs.h"
#include "fileop.h"

//****************************************************************************
// Macros and definitions
//****************************************************************************

#define DEBUG

#define MAXUNIT   8

typedef char hostpath_t[256];

//****************************************************************************
// Global variables
//****************************************************************************

extern struct dos_devheader devheader;  // Human68kのデバイスヘッダ
struct dos_req_header *reqheader;       // Human68kからのリクエストヘッダ

char *rootpath[MAXUNIT];                // 各ユニットのホストパス
struct smb2_context *rootsmb2[MAXUNIT]; // 各ユニットのsmb2_context

#ifdef DEBUG
int debuglevel = 1;
#endif

uint32_t _vernum;

char ** const environ_none = { NULL };
char **environ;

// TBD stack, heap
uint16_t stack[32 * 1024];
char heap[1024 * 512];                // temporary heap for debug print
void *_HSTA = heap;
void *_HEND = heap + sizeof(heap);
void *_PSP;

//****************************************************************************
// for debugging
//****************************************************************************

#ifdef DEBUG
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

void __socket_register_at_exit(void) {}

struct smb2_context *getsmb2(int unit)
{
  return rootsmb2[unit];
}

//----------------------------------------------------------------------------

// struct statのファイル情報を変換する
static void conv_statinfo(TYPE_STAT *st, void *v)
{
  struct dos_filesinfo *f = (struct dos_filesinfo *)v;

  f->atr = FUNC_FILEMODE_ATTR(st);
  f->filelen = htobe32(STAT_SIZE(st));
  time_t mtime = STAT_MTIME(st);
  struct tm *tm = localtime(&mtime);
  f->time = htobe16(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec >> 1);
  f->date = htobe16((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday);
}

// namestsのパスをホストのパスに変換する
// (derived from HFS.java by Makoto Kamada)
static int conv_namebuf(int unit, struct dos_namestbuf *ns, bool full, hostpath_t *path)
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
  if (len >= 1) {
    if (rootpath[unit][len - 1] == '/' && bb[0] == '/') {
      len--;                  //rootpathの末尾から'/'が連続しないようにする
    } else if (rootpath[unit][len - 1] != '/' && bb[0] != '/') {
      dst_buf[len++] = '/';   //rootpathの末尾に'/'を追加する
    }
  }
  dst_buf += len;   //マウント先パス名を前置

  // SJIS -> UTF-8に変換
  size_t dst_len = sizeof(*path) - 1 - len;  //パス名バッファ残りサイズ
  char *src_buf = (char *)bb;
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
  case 0:
    return 0;
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

//----------------------------------------------------------------------------

// 次のデバイスが next となるデバイスヘッダを探す
static struct dos_devheader *find_devheader(struct dos_devheader *next)
{
  // Human68kからNULデバイスドライバを探す
  char *p = *(char **)0x001c20;   // 先頭のメモリブロック
  while (memcmp(p, "NUL     ", 8) != 0) {
    p += 2;
  }

  // デバイスドライバのリンクをたどって next の前のデバイスヘッダを探す
  struct dos_devheader *devh = (struct dos_devheader *)(p - 14);
  while (devh->next != (struct dos_devheader *)-1) {
    if (devh->next == next) {
      return devh;
    }
    devh = devh->next;
  }
  return NULL;
}

//----------------------------------------------------------------------------

static int my_atoi(char *p)
{
  int res = 0;
  while (*p >= '0' && *p <= '9') {
    res = res * 10 + *p++ - '0';
  }
  return res;
}

//****************************************************************************
// Device driver interrupt rountine
//****************************************************************************

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
  hostpath_t pathold;
  hostpath_t pathnew;

  DNAMEPRINT(req->addr, true, "RENAME: ");

  if (conv_namebuf(req->unit, req->addr, true, &pathold) < 0) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  }
  if (conv_namebuf(req->unit, (void *)req->status, true, &pathnew) < 0) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  }

  int err;
  FUNC_RENAME(req->unit, &err, pathold, pathnew);

  DPRINTF1("RENAME: %s to %s  -> %d\r\n", pathold, pathnew, err);

  switch (err) {
  case ENOTEMPTY:
    DPRINTF1("-> CANTREN\r\n");
    return _DOSE_CANTREN;
  default:
    err = conv_errno(err);
    return err;
  }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_delete(struct dos_req_header *req)
{
  hostpath_t path;

  DNAMEPRINT(req->addr, true, "DELETE: ");

  if (conv_namebuf(req->unit, req->addr, true, &path) < 0) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  }

  int err;
  FUNC_UNLINK(req->unit, &err, path);
  err = conv_errno(err);
  DPRINTF1("-> %d\r\n", err);
  return err;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_chmod(struct dos_req_header *req)
{
  hostpath_t path;

  DNAMEPRINT(req->addr, true, "CHMOD: ");

  if (conv_namebuf(req->unit, req->addr, true, &path) < 0) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  }

  DPRINTF1(" 0x%02x ", req->attr);

  TYPE_STAT st;
  int err;
  if (FUNC_STAT(req->unit, &err, path, &st) < 0) {
    err = conv_errno(err);
    DPRINTF1("-> %d\r\n", err);
    return err;
  } else {
    err = FUNC_FILEMODE_ATTR(&st);
  }

  if (req->attr != 0xff) {
    FUNC_CHMOD(req->unit, &err, path, FUNC_ATTR_FILEMODE(req->attr, &st));
    err = conv_errno(err);
  }

  DPRINTF1("-> %d\r\n", err);
  return err;
}

//****************************************************************************
// Directory operations
//****************************************************************************

// directory list management structure
// Human68kから渡されるFILBUFのアドレスをキーとしてディレクトリリストを管理する
typedef struct {
  uint32_t filep;       // FILBUFアドレス
  int unit;             // ドライブのユニット番号
  bool isroot;          // ルートディレクトリか
  bool isfirst;         // 最初のディレクトリエントリか
  uint8_t attr;         // 検索するファイル属性
  uint8_t fname[21];    // 検索するファイル名(ワイルドカード付き)
  TYPE_DIR dir;         // ディレクトリディスクリプタ
  hostpath_t hostpath;  // ホスト側検索パス名
} dirlist_t;

static dirlist_t *dl_store;
static int dl_size = 0;

// 不要になったバッファを解放する
static void dl_free(dirlist_t *dl)
{
  // ディレクトリがオープンされていたら閉じる
  if (dl->dir != DIR_BADDIR) {
    FUNC_CLOSEDIR(dl->unit, NULL, dl->dir);
  }
  dl->dir = DIR_BADDIR;
  dl->filep = 0;
}

// FILBUFに対応するバッファを探す
static dirlist_t *dl_alloc(uint32_t filep, bool create)
{
  for (int i = 0; i < dl_size; i++) {
    dirlist_t *dl = &dl_store[i];
    if (dl->filep == filep) {
      if (create) {         // 新規作成で同じFILBUFを見つけたらバッファを再利用
        dl_free(dl);
        dl->filep = filep;
      }
      return dl;
    }
  }
  if (!create)
    return NULL;

  for (int i = 0; i < dl_size; i++) {
    dirlist_t *dl = &dl_store[i];
    if (dl->filep == 0) {   // 新規作成で未使用のバッファを見つけた
      dl->filep = filep;
      dl->dir = DIR_BADDIR;
      return dl;
    }
  }
  dl_size++;                // バッファが不足しているので拡張する
  dirlist_t *dl_new = realloc(dl_store, sizeof(dirlist_t) * dl_size);
  if (dl_new == NULL) {
    dl_size--;
    return NULL;
  }
  dl_store = dl_new;
  dirlist_t *dl = &dl_store[dl_size - 1];
  dl->filep = filep;
  dl->dir = DIR_BADDIR;
  return dl;
}

static void dl_freeall(int unit)
{
  for (int i = 0; i < dl_size; i++) {
    dirlist_t *dl = &dl_store[i];
    if (dl->filep != 0 && dl->unit == unit) {
      dl_free(dl);
    }
  }
}

static int dl_opendir(dirlist_t **dlp, struct dos_req_header *req)
{
  dirlist_t *dl;
  *dlp = NULL;
  struct dos_namestbuf *ns = req->addr;

  if ((dl = dl_alloc(req->status, true)) == NULL) {
    return ENOMEM;
  }

  if (conv_namebuf(req->unit, req->addr, false, &dl->hostpath) < 0) {
    dl_free(dl);
    return ENOENT;
  }
  dl->unit = req->unit;
  dl->isroot = strcmp(ns->path, "\t") == 0;
  dl->isfirst = true;
  dl->attr = req->attr;

  // (derived from HFS.java by Makoto Kamada)
  //検索するファイル名の順序を入れ替える
  //  主ファイル名1の末尾が'?'で主ファイル名2の先頭が'\0'のときは主ファイル名2を'?'で充填する
  memset(dl->fname, 0, sizeof(dl->fname));
  memcpy(&dl->fname[0], ns->name1, 8);    //主ファイル名1
  if (ns->name1[7] == '?' && ns->name2[0] == '\0') {  //主ファイル名1の末尾が'?'で主ファイル名2の先頭が'\0'
    memset(&dl->fname[8], '?', 10);           //主ファイル名2
  } else {
    memcpy(&dl->fname[8], ns->name2, 10); //主ファイル名2
  }
  for (int i = 17; i >= 0 && (dl->fname[i] == '\0' || dl->fname[i] == ' '); i--) {  //主ファイル名1+主ファイル名2の空き
    dl->fname[i] = '\0';
  }
  memcpy(&dl->fname[18], ns->ext, 3);     //拡張子
  for (int i = 20; i >= 18 && (dl->fname[i] == ' '); i--) { //拡張子の空き
    dl->fname[i] = '\0';
  }
  //検索するファイル名を小文字化する
  for (int i = 0; i < 21; i++) {
    int c = dl->fname[i];
    if ((0x81 <= c && c <= 0x9f) || (0xe0 <= c && c <= 0xef)) {  //SJISの1バイト目
      i++;
    } else {
      dl->fname[i] = tolower(dl->fname[i]);
    }
  }

  DPRINTF2("dl_opendir: %02x ", dl->attr);
  for (int i = 0; i < 21; i++)
    DPRINTF2("%c", dl->fname[i] == 0 ? '_' : dl->fname[i]);
  DPRINTF2("\r\n");

  //ディレクトリを開いてディスクリプタを得る
  int err;
  if ((dl->dir = FUNC_OPENDIR(req->unit, &err, dl->hostpath)) == DIR_BADDIR) {
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
  while ((d = FUNC_READDIR(dl->unit, NULL, dl->dir))) {
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
      if ((0x81 <= c && c <= 0x9f) || (0xe0 <= c && c <= 0xef)) {  //SJISの1バイト目
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
      strncpy((char *)&w2[18], &b[m + 1], 3); //拡張子

    for (int i = 0; i < 21; i++)
      DPRINTF2("%c", w2[i] == 0 ? '_' : w2[i]);
    DPRINTF2("\r\n");

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
        f = f != 0x00 && ((0x81 <= c && c <= 0x9f) || (0xe0 <= c && c <= 0xef)) ? 0x00 : 0x20;  //このバイトがSJISの2バイト目ではなくてSJISの1バイト目ならば次のバイトはSJISの2バイト目
      }
      if (i < 21) { //ファイル名がマッチしなかった
        continue;
      }
    }

    //属性、時刻、日付、ファイルサイズを取得する
    if (0xffffffffL < STAT_SIZE(DIRENT_STAT(d))) {  //4GB以上のファイルは検索できないことにする
      continue;
    }
    conv_statinfo(DIRENT_STAT(d), fi);
    if ((fi->atr & dl->attr) == 0) {  //属性がマッチしない
      continue;
    }

    return 1;
  }

  dl_free(dl);
  return 0;   // もうファイルがない
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_files(struct dos_req_header *req)
{
  dirlist_t *dl;
  struct dos_filbuf *fb = (struct dos_filbuf *)req->status;

  DNAMEPRINT(req->addr, true, "FILES: ");
  DPRINTF1("\r\n");

  int err = dl_opendir(&dl, req);
  if (err) {
    switch (err) {
    case ENOENT:
      DPRINTF1("-> NODIR\r\n");
      return _DOSE_NODIR;    //ディレクトリが存在しない場合に_DOSE_NOENTを返すと正常動作しない
    default:
      err = conv_errno(err);
      DPRINTF1("-> %d\r\n", err);
      return err;
    }
  }

  if (dl_readdir(dl, &fb->ext[2]) == 0) {
    DPRINTF1("-> NOMORE\r\n");
    return _DOSE_NOMORE;
  }

  DPRINTF1("FILES: attr=0x%02x filep=0x%08x -> %s\r\n", req->attr, req->status, fb->name);
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_nfiles(struct dos_req_header *req)
{
  dirlist_t *dl;
  struct dos_filbuf *fb = (struct dos_filbuf *)req->status;

  DPRINTF1("NFILES: ");

    if ((dl = dl_alloc(req->status, false)) == NULL) {
    DPRINTF1("-> ILGARG\r\n");
    return _DOSE_ILGARG;
  }

  if (dl_readdir(dl, &fb->ext[2]) == 0) {
    DPRINTF1("-> NOMORE\r\n");
    return _DOSE_NOMORE;
  }

  DPRINTF1("-> %s\r\n", fb->name);
  return 0;
}

//****************************************************************************
// File operations
//****************************************************************************

// file descriptor management structure
// Human68kから渡されるFCBのアドレスをキーとしてfdを管理する
typedef struct {
  uint32_t fcb;
  TYPE_FD fd;
  off_t pos;
  int unit;
} fdinfo_t;

static fdinfo_t *fi_store;
static int fi_size = 0;

// FCBに対応するバッファを探す
static fdinfo_t *fi_alloc(int unit, uint32_t fcb, bool alloc)
{
  for (int i = 0; i < fi_size; i++) {
    if (fi_store[i].fcb == fcb) {
      if (alloc) {              // 新規作成で同じFCBを見つけたらバッファを再利用
        FUNC_CLOSE(unit, NULL, fi_store[i].fd);
        fi_store[i].fd = FD_BADFD;
        fi_store[i].unit = unit;
      }
      return &fi_store[i];
    }
  }
  if (!alloc)
    return NULL;

  for (int i = 0; i < fi_size; i++) {
    if (fi_store[i].fcb == 0) { // 新規作成で未使用のバッファを見つけた
      fi_store[i].fcb = fcb;
      fi_store[i].unit = unit;
      return &fi_store[i];
    }
  }
  fi_size++;                    // バッファが不足しているので拡張する
  fi_store = realloc(fi_store, sizeof(fdinfo_t) * fi_size);   // TBD: エラー処理
  fi_store[fi_size - 1].fcb = fcb;
  fi_store[fi_size - 1].fd = FD_BADFD;
  fi_store[fi_size - 1].unit = unit;
  return &fi_store[fi_size - 1];
}

// 不要になったバッファを解放する
static void fi_free(uint32_t fcb)
{
  for (int i = 0; i < fi_size; i++) {
    if (fi_store[i].fcb == fcb) {
      fi_store[i].fcb = 0;
      fi_store[i].fd = FD_BADFD;
      return;
    }
  }
}

static void fi_freeall(int unit)
{
  for (int i = 0; i < fi_size; i++) {
    if (fi_store[i].fd != FD_BADFD && fi_store[i].unit == unit) {
      FUNC_CLOSE(unit, NULL, fi_store[i].fd);
      fi_store[i].fd = FD_BADFD;
      fi_store[i].fcb = 0;
    }
  }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_create(struct dos_req_header *req)
{
  hostpath_t path;
  TYPE_FD filefd;

  DNAMEPRINT(req->addr, true, "CREATE: ");

  if (conv_namebuf(req->unit, req->addr, true, &path) < 0) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  }

  int mode = O_CREAT|O_RDWR|O_TRUNC|O_BINARY;
  mode |= req->status ? 0 : O_EXCL;

  int err;
  if ((filefd = FUNC_OPEN(req->unit, &err, path, mode)) == FD_BADFD) {
    switch (err) {
    case ENOSPC:
      DPRINTF1("-> DIRFULL\r\n");
      return _DOSE_DIRFULL;
    default:
      err = conv_errno(err);
      DPRINTF1("-> %d\r\n", err);
      return err;
    }
  }
  
  fdinfo_t *fi = fi_alloc(req->unit, (uint32_t)req->fcb, true);
  fi->fd = filefd;
  fi->pos = 0;
  dos_fcb_size(req->fcb) = 0;

  DPRINTF1(" fcb=0x%08x attr=0x%02x mode=%d\r\n", (uint32_t)req->fcb, req->attr, req->status);
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_open(struct dos_req_header *req)
{
  hostpath_t path;
  int mode;
  TYPE_FD filefd;

  DNAMEPRINT(req->addr, true, "OPEN: ");

  if (conv_namebuf(req->unit, req->addr, true, &path) < 0) {
    DPRINTF1("-> NODIR\r\n");
    return _DOSE_NODIR;
  }

  switch (dos_fcb_mode(req->fcb)) {
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
    DPRINTF1("-> ILGARG\r\n");
    return _DOSE_ILGARG;
  }

  int err;
  if ((filefd = FUNC_OPEN(req->unit, &err, path, mode)) == FD_BADFD) {
    switch (err) {
    case EINVAL:
      DPRINTF1("-> ILGARG\r\n");
      return _DOSE_ILGARG;
    default:
      err = conv_errno(err);
      DPRINTF1("-> %d\r\n", err);
      return err;
    }
  }
  
  fdinfo_t *fi = fi_alloc(req->unit, (uint32_t)req->fcb, true);
  fi->fd = filefd;
  fi->pos = 0;
  uint32_t len = FUNC_LSEEK(req->unit, NULL, filefd, 0, SEEK_END);
  dos_fcb_size(req->fcb) = len;
  FUNC_LSEEK(req->unit, NULL, filefd, 0, SEEK_SET);

  DPRINTF1(" fcb=0x%08x mode=%d -> %d\r\n", (uint32_t)req->fcb, dos_fcb_mode(req->fcb), len);
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_close(struct dos_req_header *req)
{
  DPRINTF1("CLOSE: ");

  fdinfo_t *fi = fi_alloc(req->unit, (uint32_t)req->fcb, false);
  if (fi == NULL) {
    DPRINTF1("-> BADF\r\n");
    return _DOSE_BADF;
  }

  int err;
  if (FUNC_CLOSE(req->unit, &err, fi->fd) < 0) {
    err = conv_errno(err);
  }

  fi_free((uint32_t)req->fcb);
  DPRINTF1("fcb=0x%08x err=%d\r\n", req->fcb, err);
  return err;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_read(struct dos_req_header *req)
{
  DPRINTF1("READ: ");

  fdinfo_t *fi = fi_alloc(req->unit, (uint32_t)req->fcb, false);
  if (fi == NULL) {
    DPRINTF1("-> BADF\r\n");
    return _DOSE_BADF;
  }

  uint32_t *pp = &dos_fcb_fpos(req->fcb);
  ssize_t bytes = 0;
  int err;
  if (fi->pos != *pp) {
    if (FUNC_LSEEK(req->unit, &err, fi->fd, *pp, SEEK_SET) < 0) {
      err = conv_errno(err);
      DPRINTF1("-> %d\r\n", err);
      return err;
    }
    fi->pos = *pp;
  }
  bytes = FUNC_READ(req->unit, &err, fi->fd, req->addr, req->status);
  if (bytes < 0) {
    err = conv_errno(err);
    DPRINTF1("-> %d\r\n", err);
    return err;
  }

  fi->pos += bytes;
  *pp = fi->pos;

  DPRINTF1(" fcb=0x%08x addr=0x%08x len=%d -> pos=%d len=%d\r\n",
           (uint32_t)req->fcb, (uint32_t)req->addr, req->status, *pp, bytes);
  return bytes;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_write(struct dos_req_header *req)
{
  DPRINTF1("WRITE: ");

  fdinfo_t *fi = fi_alloc(req->unit, (uint32_t)req->fcb, false);
  if (fi == NULL) {
    DPRINTF1("-> BADF\r\n");
    return _DOSE_BADF;
  }

  uint32_t *pp = &dos_fcb_fpos(req->fcb);
  uint32_t *sp = &dos_fcb_size(req->fcb);
  ssize_t bytes = 0;
  int err;
  if (req->status == 0) {     // 0バイトのwriteはファイル長を切り詰める
    if (FUNC_FTRUNCATE(req->unit, &err, fi->fd, *pp) < 0) {
      err = conv_errno(err);
      DPRINTF1("-> %d\r\n", err);
      return err;
    } else {
      *sp = *pp;      //0バイト書き込み=truncateなのでFCBのファイルサイズをポインタ位置にする
    }
  } else {
    if (fi->pos != *pp) {
      if (FUNC_LSEEK(req->unit, &err, fi->fd, *pp, SEEK_SET) < 0) {
        err = conv_errno(err);
        DPRINTF1("-> %d\r\n", err);
        return err;
      }
      fi->pos = *pp;
    }
    bytes = FUNC_WRITE(req->unit, &err, fi->fd, req->addr, req->status);
    if (bytes < 0) {
      err = conv_errno(err);
      DPRINTF1("-> %d\r\n", err);
      return err;
    }

    fi->pos += bytes;
    *pp = fi->pos;
    if (*pp > *sp) {
      *sp = *pp;    //FCBのファイルサイズを増やす
    }
  }

  DPRINTF1(" fcb=0x%08x addr=0x%08x len=%d -> pos=%d size=%d len=%d\r\n",
           (uint32_t)req->fcb, (uint32_t)req->addr, req->status, *pp, *sp, bytes);
  return bytes;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_seek(struct dos_req_header *req)
{
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
  return pos;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_filedate(struct dos_req_header *req)
{
  DPRINTF1("FILEDATE: ");

  fdinfo_t *fi = fi_alloc(req->unit, (uint32_t)req->fcb, false);
  if (fi == NULL) {
    DPRINTF1("-> BADF\r\n");
    return _DOSE_BADF;
  }

  int res;
  int err;
  if (req->status == 0) {   // 更新日時取得
    TYPE_STAT st;
    if (FUNC_FSTAT(req->unit, &err, fi->fd, &st) < 0) {
      err = conv_errno(err);
      DPRINTF1("-> %d\r\n", err);
      return err;
    }
    struct dos_filesinfo fi;
    conv_statinfo(&st, &fi);
    res = fi.time + (fi.date << 16);
  } else {                  // 更新日時設定
    if (FUNC_FILEDATE(req->unit, &err, fi->fd, req->status & 0xffff, req->status >> 16) < 0) {
      err = conv_errno(err);
      DPRINTF1("-> %d\r\n", err);
      return err;
    }
    res = 0;
  }

  DPRINTF1("fcb=0x%08x 0x%08x -> 0x%08x\r\n", (uint32_t)req->fcb, req->status, res);
  return res;
}

//****************************************************************************
// Misc functions
//****************************************************************************

int op_dskfre(struct dos_req_header *req)
{
  int resfree = 0;
  struct {
    uint16_t freeclu;
    uint16_t totalclu;
    uint16_t clusect;
    uint16_t sectsize;
  } *res = (void *)req->addr;

  res->freeclu = res->totalclu = res->clusect = res->sectsize = 0;

  if (rootpath[req->unit] != NULL) {
    uint64_t total;
    uint64_t free;
    FUNC_STATFS(req->unit, NULL, rootpath[req->unit], &total, &free);
    total = total > 0x7fffffff ? 0x7fffffff : total;
    free = free > 0x7fffffff ? 0x7fffffff : free;
    res->freeclu = htobe16(free / 32768);
    res->totalclu = htobe16(total /32768);
    res->clusect = htobe16(128);
    res->sectsize = htobe16(1024);
    resfree = free;
  }

  DPRINTF1("DSKFRE: free=%u total=%u clusect=%u sectsz=%u res=%d\r\n", res->freeclu, res->totalclu, res->clusect, res->sectsize, resfree);
  return resfree;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_drvctrl(struct dos_req_header *req)
{
  DPRINTF1("DRVCTRL:\r\n");
  req->attr = 2;
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_getdpb(struct dos_req_header *req)
{
  DPRINTF1("GETDPB:\r\n");
  uint8_t *p = (uint8_t *)req->addr;
  memset(p, 0, 16);
  *(uint16_t *)&p[0] = 512;   // 一部のアプリがエラーになるので仮のセクタ長を設定しておく
  p[2] = 1;
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_diskred(struct dos_req_header *req)
{
  DPRINTF1("DISKRED:\r\n");
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_diskwrt(struct dos_req_header *req)
{
  DPRINTF1("DISKWRT:\r\n");
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_ioctl(struct dos_req_header *req)
{
  int unit = req->unit;
  int func = (int)req->status >> 16;
  DPRINTF1("IOCTL: cmd=%d buf=%p\r\n", func, req->addr);

  switch (func) {
  case SMBCMD_GETNAME:
    memcpy(req->addr, SMBFS_SIGNATURE, 8);
    return 0;
  case SMBCMD_NOP:
    return 0;

  case SMBCMD_MOUNT:
    {
      struct smbcmd_mount *mnt = (struct smbcmd_mount *)req->addr;

      DPRINTF1(" MOUNT url=%s user=%s pass=%s\r\n",
               mnt->url, mnt->username, mnt->password);


      if (rootsmb2[unit] != NULL) {
        DPRINTF1(" already mounted\r\n");
        return -1;
      }

      struct smb2_context *smb2 =smb2_init_context();
      if (smb2 == NULL) {
        DPRINTF1("  -> NOMEM\r\n");
        return -1;
      }

      // NTLM_USER_FILEを参照するため、smbmount実行時に設定された環境変数を引き継ぐ
      environ = mnt->environ;

      struct smb2_url *url = smb2_parse_url(smb2, mnt->url);
      if (url == NULL) {
        environ = environ_none;
        smb2_destroy_context(smb2);
        return -1;
      }

      if (url->user) {
        smb2_set_user(smb2, url->user);
      }
      if (mnt->username && mnt->username[0] != '\0') {
        smb2_set_user(smb2, mnt->username);
      }
      if (mnt->password) {
        smb2_set_password(smb2, mnt->password);
      }

      DPRINTF1("server=%s share=%s path=%s user=%s\r\n",
               url->server, url->share, url->path, smb2_get_user(smb2));

      // 環境変数を元に戻す
      environ = environ_none;

      if (smb2->password == NULL) {
        strcpy(mnt->username, smb2->user);
        smb2_destroy_url(url);
        smb2_destroy_context(smb2);
        DPRINTF1("  -> NOPASS\r\n");
        return -2;
      }

      smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
      DPRINTF1("smb2_connect_share\r\n");
      if (smb2_connect_share(smb2, url->server, url->share, NULL) < 0) {
        DPRINTF1("smb2_connect_share failed. %s\r\n", smb2_get_error(smb2));
        smb2_destroy_url(url);
        smb2_destroy_context(smb2);
        return -1;
      }

      DPRINTF1("smb2_connect_share succeeded.\r\n");

      char *rootpath_buf = malloc(url->path ? strlen(url->path) + 1 : 1);
      if (rootpath_buf == NULL) {
        smb2_disconnect_share(smb2);
        smb2_destroy_url(url);
        smb2_destroy_context(smb2);
        DPRINTF1("  -> NOMEM\r\n");
        return -1;
      }
      if (url->path && url->path[0] != '\0') {
        strcpy(rootpath_buf, url->path);
      } else {
        rootpath_buf[0] = '\0';
      }

      rootsmb2[unit] = smb2;
      rootpath[unit] = rootpath_buf;
      DPRINTF1("rootsmb2[%d]=%p rootpath=%s\r\n", unit, rootsmb2[unit], rootpath[unit]);
    }
    return 0;

  case SMBCMD_UNMOUNT:
    DPRINTF1(" UNMOUNT\r\n");
    if (rootsmb2[unit] == NULL) {
      DPRINTF1(" not mounted\r\n");
      return -1;
    }

    fi_freeall(0);
    dl_freeall(0);

    smb2_disconnect_share(rootsmb2[unit]);
    smb2_destroy_context(rootsmb2[unit]);
    rootsmb2[unit] = NULL;
    free(rootpath[unit]);
    rootpath[unit] = NULL;

    DPRINTF1(" unmounted\r\n");
    return 0;

  case SMBCMD_GETTIME:
    DPRINTF1(" GETTIME\r\n");
    return 0;

  default:
    return -1;
  }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_abort(struct dos_req_header *req)
{
  DPRINTF1("ABORT:\r\n");
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_mediacheck(struct dos_req_header *req)
{
  DPRINTF1("MEDIACHECK:\r\n");
  return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int op_lock(struct dos_req_header *req)
{
  DPRINTF1("LOCK:\r\n");
  return 0;
}

//****************************************************************************
// Device driver interrupt rountine
//****************************************************************************

int interrupt(void)
{
  uint16_t err = 0;
  struct dos_req_header *req = reqheader;

//  if (setjmp(jenv)) {
//    return 0;
//    return com_timeout(req);
//  }

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
  case 0x44: /* rename */
    req->status = op_rename(req);
    break;
  case 0x45: /* remove */
    req->status = op_delete(req);
    break;
  case 0x46: /* chmod */
    req->status = op_chmod(req);
    break;

  case 0x47: /* files */
    req->status  = op_files(req);
    break;
  case 0x48: /* nfiles */
    req->status  = op_nfiles(req);
    break;

  case 0x49: /* create */
    req->status = op_create(req);
    break;
  case 0x4a: /* open */
    req->status = op_open(req);
    break;
  case 0x4b: /* close */
    req->status = op_close(req);
    break;
  case 0x4c: /* read */
    req->status = op_read(req);
    break;
  case 0x4d: /* write */
    req->status = op_write(req);
    break;
  case 0x4e: /* seek */
    req->status = op_seek(req);
    break;
  case 0x4f: /* filedate */
    req->status = op_filedate(req);
    break;

  case 0x50: /* dskfre */
    req->status = op_dskfre(req);
    break;
  case 0x51: /* drvctrl */
    req->status = op_drvctrl(req);
    break;
  case 0x52: /* getdpb */
    req->status = op_getdpb(req);
    break;

  case 0x53: /* diskred */
    req->status = op_diskred(req);
    break;
  case 0x54: /* diskwrt */
    req->status = op_diskwrt(req);
    break;
  case 0x55: /* ioctl */
    req->status = op_ioctl(req);
    break;
  case 0x56: /* abort */
    req->status = op_abort(req);
    break;
  case 0x57: /* mediacheck */
    req->status = op_mediacheck(req);
    break;
  case 0x58: /* lock */
    req->status = op_lock(req);
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

struct smbfs_data {
  struct dos_devheader *devheader;
  struct dos_dpb *dpbs;
  int units;
  int freeable;
} smbfs_data = {
  .devheader = &devheader
};

struct dos_comline *cmdline;
char *memblock;
extern char _end;

void _start()
{
  __asm__ volatile ("move.l %%a0,%0" : "=m"(memblock));
  __asm__ volatile ("move.l %%a2,%0" : "=m"(cmdline));
  __asm__ volatile ("move.l %0,%%sp" : : "a"(stack + 32 * 1024));

  environ = environ_none;

  _dos_setblock(memblock + 0x10, (int)&_end - (int)&devheader + 0xf0);

  int units = 1;
  int release = 0;

  char *p = (char *)cmdline->buffer;
  DPRINTF1("commandline: %s\r\n", p);
  while (*p != '\0') {
    if (*p == '/' || *p == '-') {
      p++;
      switch (*p | 0x20) {
      case 'd':
      case 'u':
        if (p[1] >= '1' && p[1] <= '8') {
          units = p[1] - '0';
          DPRINTF1("drives:%d\r\n", units);
          p++;
        }
        p++;
        break;
      case 'r':
        release = 1;
        DPRINTF1("release\r\n");
        p++;
      break;
      }
    } else {
      p++;
    }
  }

  // Check whether TCP/IP is available
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    DPRINTF1("TCP/IP ドライバが常駐していません\r\n");
    _dos_exit();
  }
  DPRINTF1("socket fd=%d\r\n", fd);
  close(fd);

  _dos_super(0);

  uint8_t *drvxtbl = (uint8_t *)0x1c7e;     // ドライブ交換テーブル
  uint8_t lastdrive = *(uint8_t *)0x1c73;   // LASTDRIVEの値
  struct dos_curdir *curdir_table = *(struct dos_curdir **)0x1c38;

  if (release) {
    //////////////////////////////////////////////////////////////////////////
    // 常駐解除処理

    // TODO: バッファフラッシュ
    // TODO: ディレクトリ移動、subst設定時の対応

    struct dos_devheader *r_devheader = NULL;
    struct smbfs_data *r_smbfs_data = NULL;

    // Human68kのカレントディレクトリテーブルからsmbfsの常駐状況を確認する
    for (int drv = 0; drv < 26; drv++) {
      struct dos_curdir *curdir = &curdir_table[drvxtbl[drv]];
      if (curdir->type == 0x40) {
        struct dos_dpb *dpb = curdir->dpb;
        if (memcmp(dpb->devheader->name, CONFIG_DEVNAME, 8) == 0) {
          r_devheader = dpb->devheader;
          r_smbfs_data = *((struct smbfs_data **)(&dpb->devheader[1]));
          break;
        }
      }
    }

    if (r_devheader == NULL) {
      printf("SMBFSは常駐していません\n");
      _dos_exit();
    }
    if (!r_smbfs_data->freeable) {
      printf("SMBFSはCONFIG.SYSで常駐しているため常駐解除できません\n");
      _dos_exit();
    }

    // デバイスドライバのリンクリストからsmbfsを外す
    struct dos_devheader *prev = find_devheader(r_devheader);
    if (prev != NULL) {
      prev->next = r_devheader->next;
    }

    // 常駐しているSMBFSのドライブを削除する
    for (int drv = 0; drv < 26; drv++) {
      struct dos_curdir *curdir = &curdir_table[(int)drvxtbl[drv]];
      if (curdir->type != 0x40 || curdir->dpb->devheader != r_devheader) {
        continue;
      }

      // Human68kのカレントディレクトリテーブルからsmbfsを外す
      curdir->type = 0;
      for (int i = 0; i < 26; i++) {
        if (curdir_table[i].type == 0x40 &&
            curdir_table[i].dpb->next == curdir->dpb) {
            curdir_table[i].dpb->next = curdir->dpb->next;
        }
      }

      // 接続ドライブ数を減少
      (*(uint8_t *)0x1c75)--;

      printf("ドライブ %c: のSMBFSを常駐解除しました\n", 'A' + drv);
    }

    _dos_mfree((char *)r_devheader - 0xf0);
    _dos_exit();

  } else {

    //////////////////////////////////////////////////////////////////////////
    // 常駐処理

    int freedrive = 0;

    // Human68kのカレントディレクトリテーブルからsmbfsの常駐状況を確認する
    for (int drv = 0; drv < 26; drv++) {
      int realdrv = drvxtbl[drv];
      struct dos_curdir *curdir = &curdir_table[realdrv];
      if (curdir->type == 0x40) {
        struct dos_dpb *dpb = curdir->dpb;
        if (memcmp(dpb->devheader->name, CONFIG_DEVNAME, 8) == 0) {
        printf("SMBFSは既にドライブ %c: に常駐しています\n", 'A' + drv);
        _dos_exit();
        }
      } else if (curdir->type == 0 && realdrv <= lastdrive) {
        // LASTDRIVEまでの間の空きドライブをカウント
        freedrive++;
      }
    }

    if (freedrive < units) {
      printf("割り当て可能なドライブが不足しています\n");
      _dos_exit();
    }

    // DPB領域を確保する
    smbfs_data.units = units;
    smbfs_data.dpbs = calloc(units, sizeof(struct dos_dpb));
    if (smbfs_data.dpbs == NULL) {
      printf("メモリ不足で常駐できません\n");
      _dos_exit();
    }

    int cur_unit = 0;

    // 空きドライブを探してSMBFSのドライブに設定する
    for (int drv = 0; drv < 26; drv++) {
      int realdrv = drvxtbl[drv];
      struct dos_curdir *curdir = &curdir_table[realdrv];
      if (curdir->type != 0 || realdrv > lastdrive) {
        continue;
      }

      // DPBを初期化
      struct dos_dpb *dpb = &smbfs_data.dpbs[cur_unit];
      dpb->unit = cur_unit;
      dpb->drive = realdrv;
      dpb->devheader = &devheader;
      dpb->next = (cur_unit < units - 1) ? dpb + 1 : (struct dos_dpb *)-1;

      // Human68kのDPBリストにsmbfsのDPBを繋ぐ
      struct dos_dpb *prev_dpb = NULL;
      for (int i = 0; i < realdrv; i++) {
        if (curdir_table[i].type == 0x40) {
          prev_dpb = curdir_table[i].dpb;
        }
      }
      if (prev_dpb != NULL) {
        dpb->next = prev_dpb->next;
        prev_dpb->next = dpb;
      }

      // Human68kのカレントディレクトリテーブルを設定する
      curdir->drive = 'A' + realdrv;
      curdir->coron = ':';
      curdir->path[0] = '\t';
      curdir->path[1] = '\0';
      curdir->type = 0x40;
      curdir->dpb = dpb;
      curdir->fatno = (int)-1;
      curdir->pathlen = 2;

      printf("常駐しました。ドライブ %c: をSMBFSとして使用できます\n", 'A' + drv);

      // 接続ドライブ数を増加
      (*(uint8_t *)0x1c75)++;
      cur_unit++;
      if (cur_unit >= units) {
        break;
      }
    }

    // デバイスドライバのリンクリストにsmbfsを繋ぐ
    struct dos_devheader *prev = find_devheader((struct dos_devheader *)-1);
    if (prev != NULL) {
      prev->next = &devheader;
    }

    smbfs_data.freeable = 1;

    _dos_keeppr((int)&_end - (int)&devheader, 0);
  }
}
