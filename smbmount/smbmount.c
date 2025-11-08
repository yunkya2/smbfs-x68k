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
#include <unistd.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

#include <smb2.h>
#include <libsmb2.h>

#include <smbfscmd.h>

//****************************************************************************
// Macros and definitions
//****************************************************************************

#define PATH_LEN 256

//****************************************************************************
// Global variables
//****************************************************************************

//****************************************************************************
// Local variables
//****************************************************************************

//****************************************************************************
// Utility routine
//****************************************************************************

// Get the byte length of the first SJIS character at the given position
static int sjis_char_len(const char *s)
{
  unsigned char c = (unsigned char)*s;
  
  if (c == 0) return 0;  // End of string
  
  // SJIS first byte ranges:
  // 0x81-0x9F, 0xE0-0xFC are double-byte characters
  if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
    return 2;  // Double-byte character
  }
  
  return 1;  // Single-byte character (ASCII or half-width katakana)
}

//----------------------------------------------------------------------------

// Convert backslashes to forward slashes in SJIS path
static void convert_path_separator(char *sjis_path)
{
  char *p = sjis_path;

  if (sjis_path == NULL) return;

  while (*p) {
    int char_len = sjis_char_len(p);
    if (char_len == 1 && *p == '\\') {
      // Convert backslash to forward slash
      *p = '/';
      p++;
    } else {
      // Skip SJIS character
      p += char_len;
    }
  }
}

//----------------------------------------------------------------------------

char *getpass(const char *prompt)
{
  static char password[32];
  char *p = password;

  printf("%s", prompt);
  fflush(stdout);
  while (1) {
    int ch = _iocs_b_keyinp() & 0xff;
    switch (ch) {
    case '\0':
      continue;
    case '\r':
    case '\n':
      *p = '\0';
      printf("\n");
      return password;
    case '\b':
      if (p > password) {
        p--;
        printf("\b \b");
        fflush(stdout);
      }
      break;
    case '\x03':  // Ctrl-C
    case '\x1b':  // ESC
      printf("\n");
      return NULL;
    case '\x17':  // Ctrl-W
    case '\x15':  // Ctrl-U
      while (p > password && *(p - 1) != ' ') {
        p--;
        printf("\b \b");
        fflush(stdout);
      }
      break;
    default:
      if (p - password < sizeof(password) - 1 &&
          ch >= 32 && ch <= 126) {
        *p++ = (char)ch;
        printf("*");
        fflush(stdout);
      }
      break;
    }
  }
}

static char* normalize_smb_url(const char *input_url)
{
  static char normalized_url[PATH_LEN];
  const char *url = input_url;

  // Skip leading whitespace
  while (*url == ' ' || *url == '\t') url++;

  // If URL is empty after trimming, return error
  if (*url == '\0') {
    strcpy(normalized_url, "smb://");
    return normalized_url;
  }

  if (strncmp(url, "smb://", 6) == 0) {
    // If URL already starts with smb://, use as is
    strncpy(normalized_url, url, sizeof(normalized_url) - 1);
    normalized_url[sizeof(normalized_url) - 1] = '\0';
  } else {
    if (strncmp(url, "//", 2) == 0) {
      // If starts with //, add smb: prefix
      strcpy(normalized_url, "smb:");
    } else if (url[0] == '/') {
      // If starts with single /, it might be an absolute path, treat as server/share
      strcpy(normalized_url, "smb:/");
    } else {
    // Otherwise, assume it's server/share format
      strcpy(normalized_url, "smb://");
    }
    strncat(normalized_url, url, sizeof(normalized_url) - strlen(normalized_url) - 1);
  }

  if (strchr(normalized_url + 6, '/') == NULL) {
    // If no slash after smb://, it's just a server name
    // Append a trailing slash to indicate root
    strncat(normalized_url, "/", sizeof(normalized_url) - strlen(normalized_url) - 1);
  }

  return normalized_url;
}

//----------------------------------------------------------------------------

// 指定したドライブがSMBFSであることを確認する (0が渡されたら最初のSMBFSドライブを探す)
// ()
int get_smbfs_drive(int drive)
{
  char signature[8];
  if (drive == 0) {
    for (drive = 1; drive <= 26; drive++) {
      if (_dos_ioctrlfdctl(drive, SMBCMD_GETNAME, signature) == 0 &&
          memcmp(signature, SMBFS_SIGNATURE, 8) == 0) {
        return drive;
      }
    }
    if (drive > 26) {
      return -1;    // SMBFSドライブが見つからなかった
    }
  }

  if (_dos_ioctrlfdctl(drive, SMBCMD_GETNAME, signature) != 0 ||
      memcmp(signature, SMBFS_SIGNATURE, 8) != 0) {
    return 0;       // 指定したドライブはSMBFSではない
  }

  return drive;
}

//----------------------------------------------------------------------------

static void usage(void)
{
  fprintf(stderr, "%s",
    "使用法: smbmount <smb2-url> [drive:] [options]\n"
    "        smbmount -D [drive:]\n"
    "オプション:\n"
    "    -U <username[%password]>   - 接続時のユーザ名とパスワードを指定\n"
    "    -D                         - マウントを解除\n\n"
    "URL フォーマット:\n"
    "    [smb://][<domain>;][<username>@]<host>[:<port>][/<share>]\n\n"
    "環境変数 NTLM_USER_FILE で指定したファイルがユーザ情報に使用されます\n"
  );
}

//****************************************************************************
// Main program
//****************************************************************************

int main(int argc, char **argv)
{
  int unmount_mode = 0;
  int url_index = 0;
  int drive = 0;         // 0=最初のSMBFSドライブ 1=A: 2=B: ...
  char *username = NULL;
  char *password = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-D") == 0) {
      unmount_mode = 1;
    } else if (strcmp(argv[i], "-U") == 0) {
      if (i + 1 < argc) {
        username = argv[++i];
        if ((password = strchr(username, '%')) != NULL) {
          *password++ = '\0';  // Split username and password
          if (*username == '\0') {
            username = NULL;  // Empty username
          }
        }
      } else {
        usage();
        exit(1);
      }
    } else if (strlen(argv[i]) == 2 && argv[i][1] == ':' &&
               isalpha((unsigned char)argv[i][0])) {
      drive = toupper((unsigned char)argv[i][0]) - 'A' + 1;
    } else if (argv[i][0] != '-' && url_index == 0) {
      url_index = i;  // First non-option argument is URL
    } else {
      // Unknown option
      usage();
      exit(1);
    }
  }

  ////////////////////////////////////////////////////////////////////////////
  // アンマウント処理

  if (unmount_mode) {
    if (url_index != 0 || username != NULL || password != NULL) {
      // アンマウント時はドライブ名以外の引数は不要
      usage();
      exit(1);
    }

    if (drive == 0) {
      // 全ドライブのSMBFSをアンマウント
      for (drive = 1; drive <= 26; drive++) {
        if (_dos_ioctrlfdctl(drive, SMBCMD_GETNAME, NULL) == 0) {
          _dos_ioctrlfdctl(drive, SMBCMD_UNMOUNT, NULL);
        }
      }
      printf("全ドライブのSMBFSをマウント解除しました\n");
      exit(0);
    }

    int res = get_smbfs_drive(drive);
    if (res < 0) {
      printf("SMBFSが常駐していません\n");
      exit(1);
    } else if (res == 0) {
      printf("ドライブ %c: はSMBFSではありません\n", 'A' + drive - 1);
      exit(1);
    }
    _dos_ioctrlfdctl(drive, SMBCMD_UNMOUNT, NULL);
    printf("ドライブ %c: のSMBFSをマウント解除しました\n", 'A' + drive - 1);
    exit(0);
  }

  ////////////////////////////////////////////////////////////////////////////
  // マウント処理

  if (url_index != 0) {
    convert_path_separator(argv[url_index]);
    char *normalized_url = normalize_smb_url(argv[url_index]);

    int res = get_smbfs_drive(drive);
    if (res < 0) {
      printf("SMBFSが常駐していません\n");
      exit(1);
    } else if (res == 0) {
      printf("ドライブ %c: はSMBFSではありません\n", 'A' + drive - 1);
      exit(1);
    }
    drive = res;

    char username_buf[64];
    username_buf[0] = '\0';
    if (username != NULL) {
      strncpy(username_buf, username, sizeof(username_buf) - 1);
      username_buf[sizeof(username_buf) - 1] = '\0';
    }

    struct smbcmd_mount mount_info = {
      .url = normalized_url,
      .username = username_buf,
      .password = password,
      .environ = environ,
    };
    mount_info.username_len = sizeof(username_buf);

    res  = _dos_ioctrlfdctl(drive, SMBCMD_MOUNT, (void *)&mount_info);
    if (res == -2) {
      printf("ユーザ名 %s のパスワードを入力: ", mount_info.username);
      char *password = getpass("");
      if (password == NULL) {
        exit(1);
      }
      mount_info.password = password;
      res  = _dos_ioctrlfdctl(drive, SMBCMD_MOUNT, (void *)&mount_info);
    }

    if (res < 0) {
      printf("ドライブ %c: のSMBFSマウントに失敗しました (エラーコード: %d)\n", 'A' + drive - 1, res);
      exit(1);
    }

    return 0;
  }

  ////////////////////////////////////////////////////////////////////////////
  // マウント状態の表示

  for (drive = 1; drive <= 26; drive++) {
    int res = get_smbfs_drive(drive);
    if (res < 0) {
      printf("SMBFSが常駐していません\n");
      exit(1);
    } else if (res == 0) {
      continue;
    }

    char server[64];
    char share[64];
    char rootpath[PATH_LEN];
    char username[64];

    struct smbcmd_getmount getmount_info ={
      .server_len = sizeof(server),
      .share_len = sizeof(share),
      .rootpath_len = sizeof(rootpath),
      .username_len = sizeof(username),
      .server = server,
      .share = share,
      .rootpath = rootpath,
      .username = username,
    };

    res = _dos_ioctrlfdctl(drive, SMBCMD_GETMOUNT, (void *)&getmount_info);

    printf("%c: ", 'A' + drive - 1);
    if (res < 0) {
      printf("--\n");
    } else {
      printf("//%s@%s/%s/%s\n",
             getmount_info.username,
             getmount_info.server,
             getmount_info.share,
             getmount_info.rootpath);
    }
  }

  return 0;
}
