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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <pthread.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

#include <smb2.h>
#include <libsmb2.h>
#include <libsmb2-raw.h>
#include <libsmb2-private.h>
#include "iconv_mini.h"

//****************************************************************************
// Macros and definitions
//****************************************************************************

#define PATH_LEN 256
#define TIMEZONE (9 * 3600) // JST (UTC+9)

//****************************************************************************
// Global variables
//****************************************************************************

//****************************************************************************
// Local variables
//****************************************************************************

static jmp_buf jmp_env;

static char current_dir[PATH_LEN] = "/";
static int is_finished = false;

static pthread_t keepalive_thread;
static pthread_mutex_t keepalive_mutex = PTHREAD_MUTEX_INITIALIZER;

static const struct cmd_table {
  const char *name;
  int (*func)();
  int num_args;
  const char *option;
  const char *usage;
} cmd_table[];

//****************************************************************************
// Utility routine
//****************************************************************************

static void trim_newline(char *str)
{
  char *p = strchr(str, '\n');
  if (p) *p = '\0';
  p = strchr(str, '\r');
  if (p) *p = '\0';
}

static char* trim_spaces(char *arg)
{
  if (arg == NULL || strlen(arg) == 0) {
    return NULL;
  }
  
  // Remove leading spaces
  while (*arg == ' ' || *arg == '\t') arg++;
  if (*arg == '\0') return NULL;
  
  // Remove trailing spaces
  char *end = arg + strlen(arg) - 1;
  while (end > arg && (*end == ' ' || *end == '\t')) *end-- = '\0';
  
  return arg;
}

static int parse_two_args(char *arg_str, char **arg1, char **arg2)
{
  *arg1 = NULL;
  *arg2 = NULL;
  
  if (arg_str == NULL) return 0;
  
  // Skip leading spaces
  while (*arg_str == ' ' || *arg_str == '\t') arg_str++;
  if (*arg_str == '\0') return 0;
  
  *arg1 = arg_str;
  
  // Find end of first argument (space or tab)
  while (*arg_str && *arg_str != ' ' && *arg_str != '\t') arg_str++;
  
  if (*arg_str) {
    *arg_str = '\0';  // Null terminate first arg
    arg_str++;
    
    // Skip spaces before second argument
    while (*arg_str == ' ' || *arg_str == '\t') arg_str++;
    
    if (*arg_str) {
      *arg2 = arg_str;
      // Remove trailing spaces from second argument
      char *end = arg_str + strlen(arg_str) - 1;
      while (end > arg_str && (*end == ' ' || *end == '\t')) *end-- = '\0';
      return 2;
    }
  }
  
  return 1;
}

//----------------------------------------------------------------------------

static struct cmd_table *find_command(const char *name)
{
  struct cmd_table *cmd;
  for (cmd = (struct cmd_table *)cmd_table; cmd->name != NULL; cmd++) {
    const char *p = cmd->name;
    const char *q;
    while ((q = strchr(p, '|')) != NULL) {
      if (strncmp(p, name, q - p) == 0 && strlen(name) == (size_t)(q - p)) {
        return cmd;
      }
      p = q + 1;
    }
    if (strcmp(p, name) == 0) {
      return cmd;
    }
  }
  return NULL;
}

//----------------------------------------------------------------------------

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

// Compare SJIS characters (returns the length if equal, 0 if not equal)
static int sjis_chars_equal(const char *s1, const char *s2)
{
  int len1 = sjis_char_len(s1);
  int len2 = sjis_char_len(s2);
  
  if (len1 != len2) return 0;
  
  if (len1 == 1 && tolower(*s1) == tolower(*s2)) {
    return 1; // case insensitive match for single-byte characters
  }

  for (int i = 0; i < len1; i++) {
    if (s1[i] != s2[i]) return 0;
  }
  
  return len1;
}

// SJIS aware wildcard matching function
static int match_wildcard(const char *pattern, const char *string)
{
  const char *p = pattern;
  const char *s = string;
  const char *star = NULL;
  const char *ss = s;
  int len;

  if (strlen(pattern) == 0) {
    return 1;
  }

  while (*s) {
    if (*p == '?') {
      // '?' matches any single SJIS character
      p++;
      s += sjis_char_len(s);
    } else if (*p == '*') {
      // '*' matches any sequence of characters
      star = p++;
      ss = s;
    } else if ((len = sjis_chars_equal(p, s))) {
      // SJIS characters match
      p += len;
      s += len;
    } else if (star) {
      // No match, but we have a '*' to backtrack to
      p = star + 1;
      ss += sjis_char_len(ss);
      s = ss;
    } else {
      // No match and no '*' to backtrack to
      return 0;
    }
  }

  // Skip any trailing '*' in pattern
  while (*p == '*') {
    p++;
  }

  // If we've consumed all of pattern, it's a match
  return *p == '\0';
}

//----------------------------------------------------------------------------

// SJIS -> UTF-8 conversion
static char* sjis_to_utf8(const char *sjis_str)
{
  static char buffer[PATH_LEN];

  if (sjis_str == NULL) return NULL;

  char *src_buf = (char *)sjis_str;  // Cast away const for iconv API
  size_t src_len = strlen(sjis_str);
  char *dst_buf = buffer;
  size_t dst_len = sizeof(buffer) - 1; // Reserve space for null terminator

  if (iconv_s2u(&src_buf, &src_len, &dst_buf, &dst_len) < 0) {
    return NULL;
  }
  
  // Add null terminator
  *dst_buf = '\0';
  
  return buffer;
}

// UTF-8 -> SJIS conversion
static char* utf8_to_sjis(const char *utf8_str)
{
  static char buffer[PATH_LEN];

  if (utf8_str == NULL) return NULL;

  char *src_buf = (char *)utf8_str;  // Cast away const for iconv API
  size_t src_len = strlen(utf8_str);
  char *dst_buf = buffer;
  size_t dst_len = sizeof(buffer) - 1; // Reserve space for null terminator
  
  if (iconv_u2s(&src_buf, &src_len, &dst_buf, &dst_len) < 0) {
    return NULL;
  }
  
  // Add null terminator
  *dst_buf = '\0';

  return buffer;
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

// Path normalization
static void normalize_path(char *path)
{
  char *p = path;
  if (*p == '/') {
    p++; // Skip leading slash for processing
  }
  char *q = p;
  char *r = q;
  while (*p != '\0') {
    while (*p == '/') {
      p++;  // Skip multiple slashes
    }
    if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) {
      // Current directory - skip
      p += 1 + (p[1] == '/' ? 1 : 0);
    } else if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
      // Parent directory - remove last segment if exists
      while (q > r && --q > r && *(q - 1) != '/') {
        // Move q back to previous slash
      }
      p += 2 + (p[2] == '/' ? 1 : 0);
    } else {
      // Regular segment
      char *next_slash = strchr(p, '/');
      if (next_slash) {
        memcpy(q, p, next_slash - p + 1);
        q += next_slash - p + 1;
        p = next_slash + 1;
      } else {
        strcpy(q, p);
        return;
      }
    }
  }
  *q = '\0';  // Null terminate the result
}

static char* resolve_path(const char *path)
{
  static char resolved[PATH_LEN];
  
  if (path[0] == '/') {
    // Absolute path
    strcpy(resolved, path);
  } else {
    // Relative path - append to current directory
    strcpy(resolved, current_dir);
    strncat(resolved, "/", PATH_LEN - strlen(resolved) - 1);
    strncat(resolved, path, PATH_LEN - strlen(resolved) - 1);
  }
  
  normalize_path(resolved);
  return resolved;
}

//----------------------------------------------------------------------------

static const char *format_time(uint64_t timestamp)
{
  static char time_str[64];
  time_t t = (time_t)timestamp;

  t += TIMEZONE;
  struct tm *tm_info = localtime(&t);
  
  if (tm_info) {
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
  } else {
    strcpy(time_str, "Invalid time");
  }
  
  return time_str;
}

static const char *format_size(uint64_t size)
{
  static char size_str[32];

  if (size >= 1024ULL * 1024 * 1024 * 1024) {
    sprintf(size_str, "%.1f TB", (double)size / (1024ULL * 1024 * 1024 * 1024));
  } else if (size >= 1024ULL * 1024 * 1024) {
    sprintf(size_str, "%.1f GB", (double)size / (1024ULL * 1024 * 1024));
  } else if (size >= 1024ULL * 1024) {
    sprintf(size_str, "%.1f MB", (double)size / (1024ULL * 1024));
  } else if (size >= 1024ULL) {
    sprintf(size_str, "%.1f KB", (double)size / 1024ULL);
  } else {
    sprintf(size_str, "%lu bytes", (unsigned long)size);
  }
  
  return size_str;
}

static const char *format_uint64(uint64_t value)
{
  static char buf[32];
  char *p = buf;

  if (value >= 1000000000ULL) {
    sprintf(p, "%lu", (unsigned long)(value / 1000000000ULL));
    p += strlen(buf);
    sprintf(p, "%09lu", (unsigned long)(value % 1000000000ULL));
  } else {
    sprintf(p, "%lu", (unsigned long)value);
  }
  return buf;
}

//****************************************************************************
// Command implementations
//****************************************************************************

static int cmd_ls(struct smb2_context *smb2, const char *path)
{
  struct smb2dir *dir;
  struct smb2dirent *ent;
  const char *type;
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    // No argument - list current directory
    target_path = current_dir;
  } else {
    target_path = resolve_path(path);
  }

  char directory_path[PATH_LEN];
  char pattern[PATH_LEN];

  // Check if remote path is a directory
  struct smb2_stat_64 remote_st;
  if (smb2_stat(smb2, sjis_to_utf8(target_path + 1), &remote_st) == 0 && 
      remote_st.smb2_type == SMB2_TYPE_DIRECTORY) {
    strcpy(directory_path, target_path);
    strcpy(pattern, "*");
  } else {
    // Separate directory path and filename pattern
    char *last_slash = strrchr(target_path, '/');

    if (last_slash == NULL) {
      // No slash found - treat as pattern in current directory
      strcpy(directory_path, "/");
      strcpy(pattern, target_path);
    } else if (last_slash == target_path) {
      // Slash is at the beginning - root directory
      strcpy(directory_path, "/");
      strcpy(pattern, last_slash + 1);
    } else {
      // General case
      strncpy(directory_path, target_path, last_slash - target_path);
      directory_path[last_slash - target_path] = '\0';
      strcpy(pattern, last_slash + 1);
    }
  }

  dir = smb2_opendir(smb2, sjis_to_utf8(directory_path + 1));
  if (dir == NULL) {
    printf("ディレクトリ '%s'を開けません: %s\n", directory_path, smb2_get_error(smb2));
    return 0;
  }

  int found = false;
  while ((ent = smb2_readdir(smb2, dir))) {
    char *sjis_name = utf8_to_sjis(ent->name);
    if (sjis_name == NULL) {
      continue; // Skip if conversion failed
    }

    if (!match_wildcard(pattern, sjis_name)) {
      continue; // Skip non-matching entries
    }

    if (!found) {
      printf("Directory listing for '%s':\n", target_path);
      printf("  %-30s %-8s %10s %s\n", "Name", "Type", "Size", "Time");
      printf("  %-30s %-8s %10s %s\n", "----", "----", "----", "----");
      found = true;
    }

    switch (ent->st.smb2_type) {
    case SMB2_TYPE_LINK:
      type = "LINK";
      break;
    case SMB2_TYPE_FILE:
      type = "FILE";
      break;
    case SMB2_TYPE_DIRECTORY:
      type = "DIR";
      break;
    default:
      type = "UNKNOWN";
      break;
    }

    printf("  %-30s %-8s %10lu %s\n",
           sjis_name ? sjis_name : ent->name,
           type,
           (unsigned long)ent->st.smb2_size,
           format_time(ent->st.smb2_mtime));
  }

  smb2_closedir(smb2, dir);

  if (!found) {
    printf("ファイルがありません\n");
  }
  return 0;
}

//----------------------------------------------------------------------------

static int cmd_cd(struct smb2_context *smb2, const char *path)
{
  char *resolved_path;
  struct smb2dir *dir;
  
  if (path == NULL || strlen(path) == 0) {
    // No argument - show current directory
    printf("%s\n", current_dir);
    return 0;
  }
  
  resolved_path = resolve_path(path);
  
  // Test if the directory exists by trying to open it
  dir = smb2_opendir(smb2, sjis_to_utf8(resolved_path + 1));
  if (dir == NULL) {
    printf("ディレクトリ '%s' に移動できません: %s\n", resolved_path, smb2_get_error(smb2));
    return 0;
  }
  
  smb2_closedir(smb2, dir);
  
  // Update current directory
  strcpy(current_dir, resolved_path);
  
  // Remove trailing slash if not root
  int len = strlen(current_dir);
  if (len > 1 && current_dir[len-1] == '/') {
    current_dir[len-1] = '\0';
  }
  
  printf("ディレクトリ '%s' に移動しました\n", current_dir);
  return 0;
}

//----------------------------------------------------------------------------

static int cmd_mkdir(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    return -1;
  }
  
  target_path = resolve_path(path);
  
  if (smb2_mkdir(smb2, sjis_to_utf8(target_path + 1)) != 0) {
    printf("ディレクトリ '%s' を作成できません: %s\n", target_path, smb2_get_error(smb2));
    return 0;
  }
  
  printf("ディレクトリ '%s' を作成しました\n", target_path);
  return 0;
}

//----------------------------------------------------------------------------

static int cmd_rmdir(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    return -1;
  }
  
  target_path = resolve_path(path);
  
  if (smb2_rmdir(smb2, sjis_to_utf8(target_path + 1)) != 0) {
    printf("ディレクトリ '%s' を削除できません: %s\n", target_path, smb2_get_error(smb2));
    return 0;
  }
  
  printf("ディレクトリ '%s' を削除しました\n", target_path);
  return 0;
}

//----------------------------------------------------------------------------

static int cmd_rm(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    return -1;
  }
  
  target_path = resolve_path(path);
  
  if (smb2_unlink(smb2, sjis_to_utf8(target_path + 1)) != 0) {
    printf("ファイル '%s' を削除できません: %s\n", target_path, smb2_get_error(smb2));
    return 0;
  }
  
  printf("ファイル '%s' を削除しました\n", target_path);
  return 0;
}

//----------------------------------------------------------------------------

static int cmd_rename(struct smb2_context *smb2, const char *old_path, const char *new_path)
{
  if (old_path == NULL || strlen(old_path) == 0 || 
      new_path == NULL || strlen(new_path) == 0) {
    return -1;
  }

  char target_old[PATH_LEN];
  char target_new[PATH_LEN];
  strcpy(target_old, resolve_path(old_path));
  strcpy(target_new, resolve_path(new_path));

  char target_old_utf8[PATH_LEN];
  char target_new_utf8[PATH_LEN];
  strcpy(target_old_utf8, sjis_to_utf8(target_old + 1));
  strcpy(target_new_utf8, sjis_to_utf8(target_new + 1));

  if (smb2_rename(smb2, target_old_utf8, target_new_utf8) != 0) {
    printf("ファイル名 '%s' を '%s' に変更できません: %s\n", target_old, target_new, smb2_get_error(smb2));
    return 0;
  }
  
  printf("ファイル名 '%s' を '%s' に変更しました\n", target_old, target_new);
  return 0;
}

//----------------------------------------------------------------------------

static int cmd_stat(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  struct smb2_stat_64 st;
  const char *type;
  
  if (path == NULL || strlen(path) == 0) {
    return -1;
  }

  target_path = resolve_path(path);

  if (smb2_stat(smb2, sjis_to_utf8(target_path + 1), &st) != 0) {
    printf("ファイル '%s' の情報を取得できません: %s\n", target_path, smb2_get_error(smb2));
    return 0;
  }
  
  switch (st.smb2_type) {
  case SMB2_TYPE_LINK:
    type = "symbolic link";
    break;
  case SMB2_TYPE_FILE:
    type = "regular file";
    break;
  case SMB2_TYPE_DIRECTORY:
    type = "directory";
    break;
  default:
    type = "unknown";
    break;
  }
  
  printf("File: %s\n", target_path);
  printf("Type: %s\n", type);
  printf("Size: %s (%s bytes)\n", format_size(st.smb2_size), format_uint64(st.smb2_size));
  printf("Inode: %s\n", format_uint64(st.smb2_ino));
  printf("Links: %lu\n", (unsigned long)st.smb2_nlink);
  printf("Access time: %s\n", format_time(st.smb2_atime));
  printf("Modify time: %s\n", format_time(st.smb2_mtime));
  printf("Change time: %s\n", format_time(st.smb2_ctime));
  printf("Birth time:  %s\n", format_time(st.smb2_btime));
  return 0;
}

//----------------------------------------------------------------------------

static int cmd_statvfs(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  struct smb2_statvfs statvfs;
  uint64_t total_space, free_space, used_space;
  double usage_percent;
  
  if (path == NULL || strlen(path) == 0) {
    // No path specified, use current directory
    target_path = current_dir;
  } else {
    target_path = resolve_path(path);
  }

  if (smb2_statvfs(smb2, sjis_to_utf8(target_path + 1), &statvfs) != 0) {
    printf("ファイルシステム '%s' の情報を取得できません: %s\n", target_path, smb2_get_error(smb2));
    return 0;
  }
  
  total_space = (uint64_t)statvfs.f_blocks * statvfs.f_bsize;
  free_space = (uint64_t)statvfs.f_bavail * statvfs.f_bsize;
  used_space = total_space - ((uint64_t)statvfs.f_bfree * statvfs.f_bsize);
  usage_percent = total_space > 0 ? ((double)used_space * 100.0) / total_space : 0.0;
  
  printf("Filesystem statistics for: %s\n", target_path);
  printf("Block size:       %lu bytes\n", (unsigned long)statvfs.f_bsize);
  printf("Total blocks:     %s\n", format_uint64(statvfs.f_blocks));
  printf("Free blocks:      %s\n", format_uint64(statvfs.f_bfree));
  printf("Total space:      %s (%s bytes)\n", format_size(total_space), format_uint64(total_space));
  printf("Used space:       %s (%s bytes)\n", format_size(used_space), format_uint64(used_space));
  printf("Free space:       %s (%s bytes)\n", format_size(free_space), format_uint64(free_space));
  printf("Usage:            %.1f%%\n", usage_percent);
  return 0;
}

//----------------------------------------------------------------------------

static int cmd_lcd(struct smb2_context *smb2, const char *path)
{
  if (path != NULL && chdir(path) != 0) {
    printf("ローカルディレクトリ '%s' に移動できません: %s\n", path, strerror(errno));
    return 0;
  }

  // No argument - show current local directory
  char local_dir[PATH_LEN];
  int curdrv = _dos_curdrv();
  local_dir[0] = 'A' + curdrv;
  local_dir[1] = ':';
  local_dir[2] = '\\';
  _dos_curdir(curdrv + 1, &local_dir[3]);
  convert_path_separator(local_dir);

  printf(path == NULL ? "%s\n" : "ローカルディレクトリ '%s' に移動しました\n", local_dir);
  return 0;
}

//----------------------------------------------------------------------------

static int get_one_file(struct smb2_context *smb2, const char *target_remote, const char *target_local)
{
  struct smb2fh *fh;
  int local_fd;
  uint8_t buffer[8192];
  int bytes_read, bytes_written;

#ifndef SIMULATE_XFER
  // Open remote file for reading
  fh = smb2_open(smb2, sjis_to_utf8(target_remote + 1), O_RDONLY);
  if (fh == NULL) {
    printf("リモートファイル '%s' を開けません: %s\n", target_remote, smb2_get_error(smb2));
    return -1;
  }
  
  // Open local file for writing
  local_fd = open(target_local, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
  if (local_fd < 0) {
    printf("ローカルファイル '%s' を作成できません: %s\n", target_local, strerror(errno));
    smb2_close(smb2, fh);
    return -1;
  }
  
  printf("ファイル '%s' を '%s' にダウンロードします\n", target_remote, target_local);
  
  // Copy data
  while ((bytes_read = smb2_read(smb2, fh, buffer, sizeof(buffer))) > 0) {
    bytes_written = write(local_fd, buffer, bytes_read);
    if (bytes_written != bytes_read) {
      printf("ローカルファイルに書き込めません: %s\n", strerror(errno));
      break;
    }
  }
  
  if (bytes_read < 0) {
    printf("リモートファイルを読み込めません: %s\n", smb2_get_error(smb2));
    close(local_fd);
    smb2_close(smb2, fh);
    return -1;
  }

  struct smb2_stat_64 st;
  if (smb2_fstat(smb2, fh, &st) == 0) {
    time_t mtime = (time_t)st.smb2_mtime + TIMEZONE;
    struct tm *tm = localtime(&mtime);
    uint32_t datetime;
    datetime = ((tm->tm_year - 80) << 25) |
               ((tm->tm_mon + 1) << 21) |
               ((tm->tm_mday) << 16) |
                ((tm->tm_hour) << 11) |
                ((tm->tm_min) << 5) |
                ((tm->tm_sec) >> 1);
    _dos_filedate(local_fd, datetime);
  }

  close(local_fd);
  smb2_close(smb2, fh);
#else
  printf("Simulated download of '%s' to '%s'\n", target_remote, target_local);
#endif
  return 0;
}

static int get_multiple_files(struct smb2_context *smb2, const char *target_remote, const char *target_local)
{
  char local_path[PATH_LEN];
  char remote_path[PATH_LEN];
  char directory_path[PATH_LEN];
  char pattern[PATH_LEN];
  struct smb2dir *dir;
  struct smb2dirent *ent;
  int files_downloaded = 0;
  int sub_files;

#ifdef VERBOSE
  printf("Getting multiple files from '%s' to '%s'\n", target_remote, target_local);
#endif

  // Separate directory path and filename pattern
  strcpy(remote_path, target_remote);
  char *last_slash = strrchr(remote_path, '/');

  if (last_slash == NULL) {
    // No slash found - treat as pattern in current directory
    strcpy(directory_path, "/");
    strcpy(pattern, remote_path);
  } else if (last_slash == remote_path) {
    // Slash is at the beginning - root directory
    strcpy(directory_path, "/");
    strcpy(pattern, last_slash + 1);
  } else {
    // General case
    strncpy(directory_path, remote_path, last_slash - remote_path);
    directory_path[last_slash - remote_path] = '\0';
    strcpy(pattern, last_slash + 1);
  }

#ifdef VERBOSE
  printf("Listing directory '%s' for pattern '%s'\n", directory_path, pattern);
#endif

  dir = smb2_opendir(smb2, sjis_to_utf8(directory_path + 1));
  if (dir == NULL) {
    printf("リモートディレクトリ '%s' を開けません: %s\n", directory_path, smb2_get_error(smb2));
    return -1;
  }

  while ((ent = smb2_readdir(smb2, dir))) {
    // Skip . and .. directories
    if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
      continue;
    }

    char *sjis_name = utf8_to_sjis(ent->name);
    if (sjis_name == NULL) {
      continue; // Skip if conversion failed
    }

    // Check if filename matches the pattern
    if (match_wildcard(pattern, sjis_name)) {
      // Build full remote path
      strcpy(remote_path, directory_path);
      strncat(remote_path, "/", sizeof(remote_path) - strlen(remote_path) - 1);
      strncat(remote_path, sjis_name, sizeof(remote_path) - strlen(remote_path) - 1);
      normalize_path(remote_path);

      // Build local path
      strcpy(local_path, target_local);
      strncat(local_path, "/", sizeof(local_path) - strlen(local_path) - 1);
      strncat(local_path, sjis_name, sizeof(local_path) - strlen(local_path) - 1);

#ifdef VERBOSE
      printf("Processing '%s' ('%s')\n", remote_path, local_path);
#endif

      if (ent->st.smb2_type == SMB2_TYPE_DIRECTORY) {
        strncat(remote_path, "/", sizeof(remote_path) - strlen(remote_path) - 1);
        normalize_path(remote_path);

        // Create local directory
#ifndef SIMULATE_XFER
        if (mkdir(local_path, 0755) != 0 && errno != EEXIST) {
          // Continue anyway -- directory might already exist
        } else
#endif
        printf("ローカルディレクトリ '%s' を作成しました\n", local_path);

        // Process both files and directories through get_multiple_files
        sub_files = get_multiple_files(smb2, remote_path, local_path);
      } else {
        sub_files = get_one_file(smb2, remote_path, local_path) < 0 ? -1 : 1;
      }

      if (sub_files > 0) {
        files_downloaded += sub_files;
      } else {
        files_downloaded = -1;
        break;
      }
    }
  }
  smb2_closedir(smb2, dir);

  return files_downloaded;
}

static int cmd_get(struct smb2_context *smb2, const char *remote_path, const char *local_path)
{
  char *target_remote;
  const char *target_local;
  char local_full_path[PATH_LEN];
  struct stat st;

  if (remote_path == NULL || strlen(remote_path) == 0) {
    return -1;
  }
  
  target_remote = resolve_path(remote_path);
  
  if (local_path == NULL || strlen(local_path) == 0) {
    // Extract filename from remote path
    local_path = strrchr(target_remote, '/');
    if (local_path) {
      local_path++; // Skip the '/'
    } else {
      local_path = target_remote;
    }
  }
  target_local = local_path;

  // Check if local path is a directory
  if (stat(target_local, &st) == 0 && S_ISDIR(st.st_mode)) {
    const char *filename = strrchr(target_remote, '/');
    if (filename) {
      filename++; // Skip the '/'
    } else {
      filename = target_remote;
    }
    strcpy(local_full_path, target_local);
    strncat(local_full_path, "/", sizeof(local_full_path) - strlen(local_full_path) - 1);
    strncat(local_full_path, filename, sizeof(local_full_path) - strlen(local_full_path) - 1);
    target_local = local_full_path;
  }

  get_one_file(smb2, target_remote, target_local);
  return 0;
}

static int cmd_mget(struct smb2_context *smb2, const char *remote_path, const char *local_path)
{
  char *target_remote;
  struct stat st;

  if (remote_path == NULL || strlen(remote_path) == 0) {
    return -1;
  }
  
  if (local_path == NULL || strlen(local_path) == 0) {
    local_path = ".";
  }
  target_remote = resolve_path(remote_path);

  // Check if local path is a directory
  if (!(strcmp(local_path, ".") == 0 ||
        strcmp(local_path, "..") == 0 ||
        (stat(local_path, &st) == 0 && S_ISDIR(st.st_mode)))) {
    printf("ローカルパス '%s' はディレクトリではありません\n", local_path);
    return 0;
  }

  int files_downloaded = get_multiple_files(smb2, target_remote, local_path);
  
  if (files_downloaded < 0) {
    printf("ファイルのダウンロード中にエラーが発生しました\n");
  } else {
    printf("%d 個のファイルをダウンロードしました\n", files_downloaded);
  }
  return 0;
}

//----------------------------------------------------------------------------

static int put_one_file(struct smb2_context *smb2, const char *target_local, const char *target_remote)
{
  struct smb2fh *fh;
  int local_fd;
  uint8_t buffer[8192];
  int bytes_read, bytes_written;

#ifndef SIMULATE_XFER
  // Open local file for reading
  local_fd = open(target_local, O_RDONLY | O_BINARY);
  if (local_fd < 0) {
    printf("ローカルファイル '%s' を開けません: %s\n", target_local, strerror(errno));
    return -1;
  }
  
  // Open remote file for writing
  fh = smb2_open(smb2, sjis_to_utf8(target_remote + 1), O_WRONLY | O_CREAT | O_TRUNC);
  if (fh == NULL) {
    printf("リモートファイル '%s' を作成できません: %s\n", target_remote, smb2_get_error(smb2));
    close(local_fd);
    return -1;
  }
  
  printf("ファイル '%s' を '%s' にアップロードします\n", target_local, target_remote);
  
  // Copy data
  while ((bytes_read = read(local_fd, buffer, sizeof(buffer))) > 0) {
    bytes_written = smb2_write(smb2, fh, buffer, bytes_read);
    if (bytes_written != bytes_read) {
      printf("リモートファイルに書き込めません: %s\n", smb2_get_error(smb2));
      break;
    }
  }
  
  if (bytes_read < 0) {
    printf("ローカルファイルを読み込めません: %s\n", strerror(errno));
    close(local_fd);
    smb2_close(smb2, fh);
    return -1;
  }

  uint32_t datetime;
  datetime = _dos_filedate(local_fd, 0);
  if (datetime < 0xffff0000) {
    struct smb2_timeval tv[2];
    struct tm tm;
    tm.tm_year = ((datetime >> 25) & 0x7f) + 80;
    tm.tm_mon  = ((datetime >> 21) & 0x0f) - 1;
    tm.tm_mday = (datetime >> 16) & 0x1f;
    tm.tm_hour = (datetime >> 11) & 0x1f;
    tm.tm_min  = (datetime >> 5) & 0x3f;
    tm.tm_sec  = (datetime << 1) & 0x3f;
    time_t mtime = mktime(&tm) - TIMEZONE;
    tv[0].tv_sec = mtime;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = mtime;
    tv[1].tv_usec = 0;
    smb2_futimes(smb2, fh, tv);
  }

  close(local_fd);
  smb2_close(smb2, fh);
#else
  printf("Simulated upload of '%s' to '%s'\n", target_local, target_remote);
#endif
  return 0;
}

static int put_multiple_files(struct smb2_context *smb2, const char *target_local, const char *target_remote)
{
  char local_path[PATH_LEN];
  char remote_path[PATH_LEN];
  char directory_path[PATH_LEN];
  char pattern[PATH_LEN];
  DIR *dir;
  struct dirent *ent;
  int files_uploaded = 0;
  int sub_files;

#ifdef VERBOSE
  printf("Putting multiple files from '%s' to '%s'\n", target_local, target_remote);
#endif

  strcpy(local_path, target_local);
  char *last_slash = strrchr(local_path, '/');

  if (last_slash == NULL) {
    // No slash found - treat as pattern in current directory
    strcpy(directory_path, "./");
    strcpy(pattern, local_path);
  } else if (last_slash == local_path) {
    // Slash is at the beginning - root directory
    strcpy(directory_path, "/");
    strcpy(pattern, last_slash + 1);
  } else {
    // General case
    strncpy(directory_path, local_path, last_slash - local_path);
    directory_path[last_slash - local_path] = '\0';
    strcpy(pattern, last_slash + 1);
  }

#ifdef VERBOSE
  printf("Listing local directory '%s' for pattern '%s'\n", directory_path, pattern);
#endif

  dir = opendir(directory_path);
  if (dir == NULL) {
    printf("ローカルディレクトリ '%s' を開けません: %s\n", directory_path, strerror(errno));
    return -1;
  }

  while ((ent = readdir(dir))) {
    // Skip . and .. directories
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    // Check if filename matches the pattern
    if (match_wildcard(pattern, ent->d_name)) {
      // Build full local path
      strcpy(local_path, directory_path);
      strncat(local_path, "/", sizeof(local_path) - strlen(local_path) - 1);
      strncat(local_path, ent->d_name, sizeof(local_path) - strlen(local_path) - 1);

      // Build remote path
      strcpy(remote_path, target_remote);
      strncat(remote_path, "/", sizeof(remote_path) - strlen(remote_path) - 1);
      strncat(remote_path, ent->d_name, sizeof(remote_path) - strlen(remote_path) - 1);
      normalize_path(remote_path);

#ifdef VERBOSE
      printf("Processing '%s' ('%s')\n", local_path, remote_path);
#endif

      if (ent->d_type == DT_DIR) {
        strncat(local_path, "/", sizeof(local_path) - strlen(local_path) - 1);

        // Create remote subdirectory
#ifndef SIMULATE_XFER
        if (smb2_mkdir(smb2, sjis_to_utf8(remote_path + 1)) != 0) {
          // Continue anyway - directory might already exist
        } else
#endif
        printf("リモートディレクトリ '%s' を作成しました\n", remote_path);

        // Process both files and directories through put_multiple_files
        sub_files = put_multiple_files(smb2, local_path, remote_path);
      } else {
        sub_files = put_one_file(smb2, local_path, remote_path) < 0 ? -1 : 1;
      }

      if (sub_files > 0) {
        files_uploaded += sub_files;
      } else {
        while ((ent = readdir(dir)))
          ;
        files_uploaded = -1;
        break;
      }
    }
  }
  closedir(dir);

  return files_uploaded;
}

static int cmd_put(struct smb2_context *smb2, const char *local_path, const char *remote_path)
{
  const char *target_local;
  char *target_remote;
  char remote_full_path[PATH_LEN];
  struct smb2_stat_64 remote_st;
  
  if (local_path == NULL || strlen(local_path) == 0) {
    return -1;
  }
  
  target_local = local_path;
  if (remote_path == NULL || strlen(remote_path) == 0) {
    // Extract filename from local path
    remote_path = strrchr(target_local, '/');
    if (remote_path) {
      remote_path++; // Skip the '/'
    } else {
      remote_path = target_local;
    }
  }
  target_remote = resolve_path(remote_path);
  
  // Check if remote path is a directory
  if (smb2_stat(smb2, sjis_to_utf8(target_remote + 1), &remote_st) == 0 && 
      remote_st.smb2_type == SMB2_TYPE_DIRECTORY) {
    const char *filename = strrchr(target_local, '/');
    if (filename) {
      filename++; // Skip the '/'
    } else {
      filename = target_local;
    }
    strcpy(remote_full_path, target_remote);
    strncat(remote_full_path, "/", sizeof(remote_full_path) - strlen(remote_full_path) - 1);
    strncat(remote_full_path, filename, sizeof(remote_full_path) - strlen(remote_full_path) - 1);
    target_remote = remote_full_path;
  }

  put_one_file(smb2, target_local, target_remote);
  return 0;
}

static int cmd_mput(struct smb2_context *smb2, const char *local_path, const char *remote_path)
{
  char *target_remote;
  struct smb2_stat_64 remote_st;
  
  if (local_path == NULL || strlen(local_path) == 0) {
    return -1;
  }
  
  if (remote_path == NULL || strlen(remote_path) == 0) {
    remote_path = "";
  }
  target_remote = resolve_path(remote_path);
  
  // Check if remote path is a directory
  if (!(smb2_stat(smb2, sjis_to_utf8(target_remote + 1), &remote_st) == 0 && 
        remote_st.smb2_type == SMB2_TYPE_DIRECTORY)) {
    printf("リモートパス '%s' はディレクトリではありません\n", target_remote);
    return 0;
  }

  int files_uploaded = put_multiple_files(smb2, local_path, target_remote);
  
  if (files_uploaded < 0) {
    printf("ファイルのアップロード中にエラーが発生しました\n");
  } else {
    printf("%d 個のファイルをアップロードしました\n", files_uploaded);
  }
  return 0;
}

//----------------------------------------------------------------------------

static int cmd_help(struct smb2_context *smb2, const char *command)
{
  struct cmd_table *cmd;
  
  if (command != NULL && (cmd = find_command(command)) != NULL) {
    printf("使用法: %s %s  -- %s\n", command, cmd->option, cmd->usage);
  } else {
    for (cmd = (struct cmd_table *)cmd_table; cmd->name != NULL; cmd++) {
      printf("%-15s %s\n", cmd->name, cmd->usage);
    }
  }
  return 0;
}

static int cmd_shell(struct smb2_context *smb2, const char *command)
{
  system(command ? command : "");
  return 0;
}

//----------------------------------------------------------------------------

static const struct cmd_table cmd_table[] = {
  {"ls|dir|l",    cmd_ls,      1, "[path]",                     "ディレクトリ内容の表示"},
  {"cd|chdir",    cmd_cd,      1, "[path]",                     "カレントディレクトリの変更/表示"},
  {"mkdir|md",    cmd_mkdir,   1, "<path>",                     "ディレクトリの作成"},
  {"rmdir|rd",    cmd_rmdir,   1, "<path>",                     "ディレクトリの削除"},
  {"rm|del",      cmd_rm,      1, "<path>",                     "ファイルの削除"},
  {"rename|ren",  cmd_rename,  2, "<old_path> <new_path>",      "ファイル/ディレクトリの名前変更"},
  {"stat",        cmd_stat,    1, "<path>",                     "ファイル/ディレクトリ情報の表示"},
  {"statvfs|df",  cmd_statvfs, 1, "[path]",                     "ファイルシステム情報の表示"},
  {"lcd",         cmd_lcd,     1, "[path]",                     "ローカルカレントディレクトリの変更/表示"},
  {"shell",       cmd_shell,   1, "[shell command]",            "シェルコマンドの実行"},
  {"get",         cmd_get,     2, "<remote_path> [local_path]", "リモートファイルのダウンロード"},
  {"mget",        cmd_mget,    2, "<remote_path> [local_path]", "複数リモートファイルのダウンロード"},
  {"put",         cmd_put,     2, "<local_path> [remote_path]", "ローカルファイルのアップロード"},
  {"mput",        cmd_mput,    2, "<local_path> [remote_path]", "複数ローカルファイルのアップロード"},
  {"quit|exit",   NULL,        0, "",                           "プログラムの終了"},
  {"help",        cmd_help,    0, "[command]",                  "ヘルプの表示"},
  {NULL,          NULL,        0, NULL,                         NULL}
};

static int execute_command(struct smb2_context *smb2, char *cmdline)
{
  char *cmd, *arg;
  
  // Skip empty lines
  if (strlen(cmdline) == 0) {
    return 0;
  }

  // Parse command and argument
  arg = cmdline + 1;
  if (cmdline[0] == '!') {
    cmd = "shell";
  } else if (cmdline[0] == '?') {
    cmd = "help";
  } else if (cmdline[0] == '\x1a') {
    cmd = "quit";
  } else {
    cmd = strtok(cmdline, " \t");
    arg = strtok(NULL, "");  // Get rest of line as argument
  }
  arg = trim_spaces(arg);
  
  if (cmd == NULL) {
    return 0;
  }

  // Find command in table
  struct cmd_table *c = find_command(cmd);
  int res;
  if (c == NULL) {
    printf("コマンドが違います: %s\n", cmd);
    printf("'help' でコマンド一覧が表示されます\n");
    return -1; // Unknown command
  }

  // Execute command
  if (c->func == NULL) {
    return 1;  // Signal to exit
  } else if (c->num_args < 2) {
    res = c->func(smb2, arg);
  } else {
    char *arg1 = NULL, *arg2 = NULL;
    parse_two_args(arg, &arg1, &arg2);
    res = c->func(smb2, arg1, arg2);
  }

  if (res < 0) {
    cmd_help(smb2, cmd);
  }
  return 0;  // Continue execution
}

static int execute_command_string(struct smb2_context *smb2, const char *command_string)
{
  char *commands = strdup(command_string);
  int result = 0;
  
  if (commands == NULL) {
    printf("Memory allocation failed\n");
    return 1;
  }
  
  // Split commands by semicolon
  char *cmd = commands;
  char *nextcmd = commands;
  while (nextcmd != NULL) {
    cmd = nextcmd;
    nextcmd = strchr(cmd, ';');
    if (nextcmd != NULL) {
      *nextcmd++ = '\0';
    }
    char *trimmed = trim_spaces(cmd);
    if (trimmed != NULL) {
      result = execute_command(smb2, cmd);
      if (result != 0) {
        break;  // Exit command was issued
      }
    }
  }

  free(commands);
  return result;
}

//****************************************************************************
// Share enumeration
//****************************************************************************

static void share_enum_cb(struct smb2_context *smb2, int status,
                          void *command_data, void *private_data)
{
  struct srvsvc_NetrShareEnum_rep *rep = command_data;
  int i;

  if (status) {
    printf("ファイル共有一覧の取得でエラーが発生しました (%s) %s\n",
           strerror(-status), smb2_get_error(smb2));
    is_finished = true;
    return;
  }

  printf("利用可能なファイル共有:\n");
  printf("%-20s %-10s %s\n", "Share name", "Type", "Comment");
  printf("%-20s %-10s %s\n", "----------", "----", "-------");

  for (i = 0; i < rep->ses.ShareInfo.Level1.EntriesRead; i++) {
    const char *share_name = rep->ses.ShareInfo.Level1.Buffer->share_info_1[i].netname.utf8;
    const char *comment = rep->ses.ShareInfo.Level1.Buffer->share_info_1[i].remark.utf8;
    uint32_t type = rep->ses.ShareInfo.Level1.Buffer->share_info_1[i].type;

    printf("%-20s ", share_name);

    const char *typestr;
    switch (type & 3) {
    case SHARE_TYPE_DISKTREE:
      typestr = "Disk";
      break;
    case SHARE_TYPE_PRINTQ:
      typestr = "Printer";
      break;
    case SHARE_TYPE_DEVICE:
      typestr = "Device";
      break;
    case SHARE_TYPE_IPC:
      typestr = "IPC";
      break;
    default:
      typestr = "Unknown";
      break;
    }

    printf("%-10s %s\n", typestr, comment ? comment : "");
  }

  smb2_free_data(smb2, rep);
  is_finished = true;
}

#define POLLIN      0x0001
#define POLLOUT     0x0004

static int list_shares(struct smb2_context *smb2, const char *server, const char *user)
{
  int ret = 0;

  if (user) {
    smb2_set_user(smb2, user);
  }

  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

  if (smb2_connect_share(smb2, server, "IPC$", NULL) < 0) {
    printf("サーバ %s の IPC$ に接続できません: %s\n", server, smb2_get_error(smb2));
    return 1;
  }

  if (smb2_share_enum_async(smb2, SHARE_INFO_1, share_enum_cb, NULL) != 0) {
    printf("ファイル共有一覧を取得できません: %s\n", smb2_get_error(smb2));
    return 1;
  }

  // Simple event loop
  while (!is_finished) {
    int revents = smb2_which_events(smb2) == POLLIN ? POLLIN : POLLOUT;
    // Simple polling mechanism - just service the connection
    if (smb2_service(smb2, revents) < 0) {
      printf("smb2_service failed: %s\n", smb2_get_error(smb2));
      ret = 1;
      break;
    }
  }

  smb2_disconnect_share(smb2);
  return ret;
}

//****************************************************************************
// Keepalive thread
//****************************************************************************

__attribute__((noreturn))
static void* keepalive_thread_func(void *arg)
{
  struct smb2_context *smb2 = (struct smb2_context *)arg;

  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
  while (1) {
    sleep(30);
    pthread_mutex_lock(&keepalive_mutex);
    smb2_echo(smb2);
    pthread_mutex_unlock(&keepalive_mutex);
  }
}

//****************************************************************************
// Main program
//****************************************************************************

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

static void usage(void)
{
  fprintf(stderr, "%s",
    "smbclient for X68000 version " GIT_REPO_VERSION "\n\n"
    "使用法: smbclient <smb2-url> [options]\n"
    "オプション:\n"
    "    -U <username[%password]>   - 接続時のユーザ名とパスワードを指定\n"
    "    -L                         - サーバで利用可能なファイル共有一覧を表示\n"
    "    -c <commands>...           - コマンドを実行 (;で区切って複数指定可能)\n\n"
    "URL フォーマット:\n"
    "    [smb://][<domain>;][<username>@]<host>[:<port>][/<share>]\n\n"
    "環境変数 NTLM_USER_FILE で指定したファイルがユーザ情報に使用されます\n"
  );
}

static void ctrlc_handler(void)
{
  longjmp(jmp_env, 1);
}

int main(int argc, char *argv[])
{
  struct smb2_context *smb2;
  struct smb2_url *url;
  int list_mode = 0;
  int command_mode = 0;
  int url_index = 0;
  char *username = NULL;
  char *password = NULL;
  char *command_string = NULL;

  // Parse command line options
  if (argc < 2) {
    usage();
    exit(1);
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-L") == 0) {
      list_mode = 1;
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
    } else if (strcmp(argv[i], "-c") == 0) {
      command_mode = 1;
      int command_len = 1;
      command_string = malloc(command_len);
      if (command_string == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
      }
      command_string[0] = '\0';
      for (i++; i < argc; i++) {
        command_len += strlen(argv[i]) + 1;
        command_string = realloc(command_string, command_len);
        if (command_string == NULL) {
          fprintf(stderr, "Memory allocation failed\n");
          exit(1);
        }
        strcat(command_string, argv[i]);
        if (i < argc - 1) {
          strcat(command_string, " ");
        }
      }
    } else if (url_index == 0) {
      url_index = i;  // First non-option argument is URL
    } else {
      // Unknown option
      usage();
      exit(1);
    }
  }

  // Check whether TCP/IP is available
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    fprintf(stderr, "TCP/IP ドライバが常駐していません\n");
    exit(1);
  }
  close(fd);

  smb2 = smb2_init_context();
  if (smb2 == NULL) {
    fprintf(stderr, "Failed to init context\n");
    exit(1);
  }

  char *normalized_url = normalize_smb_url(argv[url_index]);

  // Debug: show URL conversion if input was modified
  if (strcmp(normalized_url, argv[url_index]) != 0) {
//    printf("URL normalized: '%s' -> '%s'\n", argv[url_index], normalized_url);
  }

  url = smb2_parse_url(smb2, normalized_url);
  if (url == NULL) {
    fprintf(stderr, "URL 指定に誤りがあります: %s\n", smb2_get_error(smb2));
    exit(1);
  }

  // Set username and password
  if (url->user) {                // Username is specified in URL
    smb2_set_user(smb2, url->user);
  }
  if (username) {                 // Username is specified in command line (-U)
    smb2_set_user(smb2, username);
  }
  if (password) {                 // Password is specified in command line (-U)
    smb2_set_password(smb2, password);
  }
  if (smb2->password == NULL) {   // Password is not specified yet
    printf("ユーザ名 %s のパスワードを入力: ", smb2->user);
    char *password = getpass("");
    if (password == NULL) {
      smb2_destroy_url(url);
      smb2_destroy_context(smb2);
      exit(1);
    }
    smb2_set_password(smb2, password);
  }

  // For list mode, we only need to connect to IPC$ and enumerate shares
  if (list_mode) {
    int result = list_shares(smb2, url->server, url->user);
    smb2_destroy_url(url);
    smb2_destroy_context(smb2);
    exit(result);
  }

  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  if (smb2_connect_share(smb2, url->server, url->share, url->user) != 0) {
    printf("ファイル共有サーバへの接続に失敗しました: %s\n", smb2_get_error(smb2));
    exit(1);
  }
  smb2_destroy_url(url);

  if (command_mode) {
    // Execute the specified command(s) and exit
    execute_command_string(smb2, command_string);
    free(command_string);
  } else {
    // Interactive mode
    pthread_mutex_lock(&keepalive_mutex);
    if (pthread_create(&keepalive_thread, NULL, keepalive_thread_func, smb2) != 0) {
      printf("Failed to create keepalive thread\n");
      smb2_disconnect_share(smb2);
      smb2_destroy_context(smb2);
      exit(1);
    }

    printf("SMB Client - Type 'help' for commands, 'quit' to exit\n");

    void *old_ctrlc = NULL;
    while (1) {
      if (old_ctrlc == NULL) {
        if (setjmp(jmp_env) == 0) {
          old_ctrlc = _dos_intvcs(0xfff1, ctrlc_handler);
        }
      }

      printf("smb:%s> ", current_dir);
      fflush(stdout);

      struct dos_inpptr cmdline;
      pthread_mutex_unlock(&keepalive_mutex);
      cmdline.max = 255;
      _dos_gets(&cmdline);
      pthread_mutex_lock(&keepalive_mutex);
      printf("smb:%s> %s\n", current_dir, cmdline.buffer);  // Echo command

      trim_newline(cmdline.buffer);
      
      if (execute_command(smb2, cmdline.buffer) == 1) {
        break;  // Exit command was issued
      }
    }

    _dos_intvcs(0xfff1, old_ctrlc);
  }

  pthread_cancel(keepalive_thread);
  pthread_join(keepalive_thread, NULL);
  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  return 0;
}
