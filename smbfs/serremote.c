/*
 * Copyright (c) 2023 Yuichi Nakamura (@yunkya2)
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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

#include <config.h>
#include <x68kremote.h>
#include "remotedrv.h"

//****************************************************************************
// Global variables
//****************************************************************************

bool recovery = false;  //エラー回復モードフラグ
int timeout = 500;      //コマンド受信タイムアウト(5sec)
int resmode = 0;        //登録モード (0:常に登録 / 1:起動時にサーバと通信できたら登録)

#ifdef DEBUG
int debuglevel = 0;
#endif

//****************************************************************************
// for debugging
//****************************************************************************

#ifdef DEBUG
char heap[1024];                // temporary heap for debug print
void *_HSTA = heap;
void *_HEND = heap + 1024;
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
// Communication
//****************************************************************************

static void out232c(uint8_t c)
{
  while (_iocs_osns232c() == 0)
    ;
  _iocs_out232c(c);
  DPRINTF3("%02X ", c);
}

static void serout(void *buf, size_t len)
{
  uint8_t *p = buf;

  if (recovery) {
    // エラー状態からの回復
    // パケットサイズ以上の同期バイトを送ってサーバ側をコマンド受信待ち状態に戻す
    // その間に受信したデータはすべて捨てる
    DPRINTF1("error recovery\r\n");
    for (int i = 0; i < 1030; i++) {
      if (_iocs_isns232c()) {
        _iocs_inp232c();
      }
      out232c('Z');
    }
    while (_iocs_isns232c())
      _iocs_inp232c();
    recovery = false;
  }

  out232c('Z');
  out232c('Z');
  out232c('X');
  out232c(len >> 8);
  out232c(len & 0xff);
  DPRINTF3("\r\n");
  for (int i = 0; i < len; i++) {
    if ((i % 16) == 0) DPRINTF3("%03X: ", i);
    out232c(*p++);
    if ((i % 16) == 15) DPRINTF3("\r\n");
  }
  DPRINTF3("\r\n");
  DPRINTF2("send %d bytes\r\n", len);
}

static int inp232c(void)
{
  struct iocs_time tim;
  int sec;
  tim = _iocs_ontime();
  sec = tim.sec;

  while (_iocs_isns232c() == 0) {
    //データ受信がタイムアウトしたらエラー回復モードに移行
    tim = _iocs_ontime();
    if (((tim.sec - sec) % 8640000) > timeout)
      longjmp(jenv, -1);
  }
  int c = _iocs_inp232c() & 0xff;
  DPRINTF3("%02X ", c);
  return c;
}

static void serin(void *buf, size_t len)
{
  uint8_t *p = buf;
  uint8_t c;
  size_t size;

  // 同期バイトをチェック:  ZZZ...ZZZX でデータ転送開始
  do {
      c = inp232c();
  } while (c != 'Z');
  do {
      c = inp232c();
  } while (c == 'Z');
  if (c != 'X') {
    longjmp(jenv, -1);
  }

  // データサイズを取得
  size = inp232c() << 8;
  size += inp232c();
  DPRINTF3("\r\n");
  if (size > len) {
    longjmp(jenv, -1);
  }

  // データを読み込み
  for (int i = 0; i < size; i++) {
    if ((i % 16) == 0) DPRINTF3("%03X: ", i);
    *p++ = inp232c();
    if ((i % 16) == 15) DPRINTF3("\r\n");
  }
  DPRINTF3("\r\n");
  DPRINTF2("recv %d bytes\r\n", size);
}

void com_cmdres(void *wbuf, size_t wsize, void *rbuf, size_t rsize)
{
  serout(wbuf, wsize);
  serin(rbuf, rsize);
}

//****************************************************************************
// Utility routine
//****************************************************************************

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

int com_timeout(struct dos_req_header *req)
{
  if (resmode == 1) {     // 起動時にサーバが応答しなかった
    _dos_print("リモートドライブサービスが応答しないため組み込みません\r\n");
  }
  DPRINTF1("command timeout\r\n");
  req->status = -1;
  recovery = true;
  return 0x1002;
}

int com_init(struct dos_req_header *req)
{
  int units = 1;
#ifdef CONFIG_BOOTDRIVER
  _iocs_b_print
#else
  _dos_print
#endif
    ("\r\nX68000 Serial Remote Drive Driver (version " GIT_REPO_VERSION ")\r\n");

#ifdef CONFIG_BOOTDRIVER
  int baudrate = 9600;
  char *baudstr = "9600";
  int bdset = 7;
#else
  int baudrate = 38400;
  char *baudstr = "38400";
  char *p = (char *)req->status;
  p += strlen(p) + 1;
  while (*p != '\0') {
    if (*p == '/' || *p =='-') {
      p++;
      switch (*p | 0x20) {
#ifdef DEBUG
      case 'd':         // /D .. デバッグレベル増加
        debuglevel++;
        break;
#endif
      case 's':         // /s<speed> .. 通信速度設定
        p++;
        baudrate = my_atoi(p);
        baudstr = p;
        break;
      case 'r':         // /r<mode> .. 登録モード
        p++;
        resmode = my_atoi(p);
        break;
      case 't':         // /t<timeout> .. タイムアウト設定
        p++;
        timeout = my_atoi(p) * 100;
        if (timeout == 0)
          timeout = 500;
        break;
      case 'u':         // /u<units> .. ユニット数設定
        p++;
        units = my_atoi(p);
        if (units < 1 || units > 7)
          units = 1;
        break;
      }
    } else if (*p >= '0' && *p <= '9') {
      baudrate = my_atoi(p);
      baudstr = p;
    }
    p += strlen(p) + 1;
  }

  static const int bauddef[] = { 75, 150, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400 };
  int bdset = -1;
  for (int i = 0; i < sizeof(bauddef) / sizeof(bauddef[0]); i++) {
    if (baudrate == bauddef[i]) {
      bdset = i;
      break;
    }
  }
  if (bdset < 0) {
    bdset = 9;
    baudstr = "38400";
  }
#endif

  // stop 1 / nonparity / 8bit / nonxoff
  _iocs_set232c(0x4c00 | bdset);

#ifndef CONFIG_BOOTDRIVER
  if (resmode != 0) {     // サーバが応答するか確認する
    struct cmd_init cmd;
    struct res_init res;
    cmd.command = 0x00; /* init */
    com_cmdres(&cmd, sizeof(cmd), &res, sizeof(res));
    DPRINTF1("CHECK:\r\n");
  }
  resmode = 0;  // 応答を確認できたのでモードを戻す

  _dos_print("ドライブ");
  _dos_putchar('A' + *(char *)&req->fcb);
  if (units > 1) {
    _dos_print(":-");
    _dos_putchar('A' + *(char *)&req->fcb + units - 1);
  }
  _dos_print(":でRS-232Cに接続したリモートドライブが利用可能です (");
  _dos_print(baudstr);
  _dos_print("bps)\r\n");
#endif
  DPRINTF1("Debug level: %d\r\n", debuglevel);

  return units;
}
