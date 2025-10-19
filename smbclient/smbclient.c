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
#include <sys/stat.h>
#include <sys/socket.h>
#include <pthread.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

#include <smb2.h>
#include <libsmb2.h>
#include <libsmb2-raw.h>
#include "iconv_mini.h"

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

static char current_dir[PATH_LEN] = "/";
static int is_finished = false;

static pthread_t keepalive_thread;
static pthread_mutex_t keepalive_mutex = PTHREAD_MUTEX_INITIALIZER;

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

// Compare SJIS characters (returns 1 if they match, 0 if not)
static int sjis_chars_equal(const char *s1, const char *s2)
{
  int len1 = sjis_char_len(s1);
  int len2 = sjis_char_len(s2);
  
  if (len1 != len2) return 0;
  
  for (int i = 0; i < len1; i++) {
    if (s1[i] != s2[i]) return 0;
  }
  
  return 1;
}

// SJIS aware wildcard matching function
static int match_wildcard(const char *pattern, const char *string)
{
  const char *p = pattern;
  const char *s = string;
  const char *star = NULL;
  const char *ss = s;

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
    } else if (sjis_chars_equal(p, s)) {
      // SJIS characters match
      int p_len = sjis_char_len(p);
      int s_len = sjis_char_len(s);
      p += p_len;
      s += s_len;
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

  t += 9 * 3600; // Convert UTC to JST (UTC+9)
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

static void cmd_ls(struct smb2_context *smb2, const char *path)
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

  dir = smb2_opendir(smb2, sjis_to_utf8(target_path + 1));
  if (dir == NULL) {
    printf("Failed to open directory '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
  }

  printf("Directory listing for '%s':\n", target_path);
  printf("  %-30s %-8s %10s %s\n", "Name", "Type", "Size", "Time");
  printf("  %-30s %-8s %10s %s\n", "----", "----", "----", "----");

  while ((ent = smb2_readdir(smb2, dir))) {
    char *sjis_name;
    
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
    
    // Convert filename from UTF-8 to SJIS for display
    sjis_name = utf8_to_sjis(ent->name);
    printf("  %-30s %-8s %10lu %s\n",
           sjis_name ? sjis_name : ent->name,
           type,
           (unsigned long)ent->st.smb2_size,
           format_time(ent->st.smb2_mtime));
  }

  smb2_closedir(smb2, dir);
}

//----------------------------------------------------------------------------

static void cmd_cd(struct smb2_context *smb2, const char *path)
{
  char *resolved_path;
  struct smb2dir *dir;
  
  if (path == NULL || strlen(path) == 0) {
    // No argument - show current directory
    printf("Current directory: %s\n", current_dir);
    return;
  }
  
  resolved_path = resolve_path(path);
  
  // Test if the directory exists by trying to open it
  dir = smb2_opendir(smb2, sjis_to_utf8(resolved_path + 1));
  if (dir == NULL) {
    printf("Failed to change directory to '%s': %s\n", resolved_path, smb2_get_error(smb2));
    return;
  }
  
  smb2_closedir(smb2, dir);
  
  // Update current directory
  strcpy(current_dir, resolved_path);
  
  // Remove trailing slash if not root
  int len = strlen(current_dir);
  if (len > 1 && current_dir[len-1] == '/') {
    current_dir[len-1] = '\0';
  }
  
  printf("Changed directory to: %s\n", current_dir);
}

//----------------------------------------------------------------------------

static void cmd_mkdir(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    printf("Usage: mkdir <path>\n");
    return;
  }
  
  target_path = resolve_path(path);
  
  if (smb2_mkdir(smb2, sjis_to_utf8(target_path + 1)) != 0) {
    printf("Failed to create directory '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
  }
  
  printf("Directory '%s' created successfully\n", target_path);
}

//----------------------------------------------------------------------------

static void cmd_rmdir(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    printf("Usage: rmdir <path>\n");
    return;
  }
  
  target_path = resolve_path(path);
  
  if (smb2_rmdir(smb2, sjis_to_utf8(target_path + 1)) != 0) {
    printf("Failed to remove directory '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
  }
  
  printf("Directory '%s' removed successfully\n", target_path);
}

//----------------------------------------------------------------------------

static void cmd_rm(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    printf("Usage: rm <path>\n");
    return;
  }
  
  target_path = resolve_path(path);
  
  if (smb2_unlink(smb2, sjis_to_utf8(target_path + 1)) != 0) {
    printf("Failed to remove file '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
  }
  
  printf("File '%s' removed successfully\n", target_path);
}

//----------------------------------------------------------------------------

static void cmd_stat(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  struct smb2_stat_64 st;
  const char *type;
  
  if (path == NULL || strlen(path) == 0) {
    printf("Usage: stat <path>\n");
    return;
  }

  target_path = resolve_path(path);

  if (smb2_stat(smb2, sjis_to_utf8(target_path + 1), &st) != 0) {
    printf("Failed to stat '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
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
}

//----------------------------------------------------------------------------

static void cmd_statvfs(struct smb2_context *smb2, const char *path)
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
    printf("Failed to get filesystem statistics for '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
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
}

//----------------------------------------------------------------------------

static void cmd_lcd(const char *path)
{
  if (chdir(path ? path : "") != 0) {
    printf("Failed to change local directory to '%s': %s\n", path, strerror(errno));
    return;
  }
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
    printf("Failed to open remote file '%s': %s\n", target_remote, smb2_get_error(smb2));
    return -1;
  }
  
  // Open local file for writing
  local_fd = open(target_local, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
  if (local_fd < 0) {
    printf("Failed to create local file '%s': %s\n", target_local, strerror(errno));
    smb2_close(smb2, fh);
    return -1;
  }
  
  printf("Downloading '%s' to '%s'\n", target_remote, target_local);
  
  // Copy data
  while ((bytes_read = smb2_read(smb2, fh, buffer, sizeof(buffer))) > 0) {
    bytes_written = write(local_fd, buffer, bytes_read);
    if (bytes_written != bytes_read) {
      printf("Failed to write to local file: %s\n", strerror(errno));
      break;
    }
  }
  
  if (bytes_read < 0) {
    printf("Failed to read from remote file: %s\n", smb2_get_error(smb2));
    close(local_fd);
    smb2_close(smb2, fh);
    return -1;
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
    printf("Failed to open remote directory '%s': %s\n", directory_path, smb2_get_error(smb2));
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
        printf("Created local directory '%s'\n", local_path);

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

static void cmd_get(struct smb2_context *smb2, const char *remote_path, const char *local_path)
{
  char *target_remote;
  const char *target_local;
  char local_full_path[PATH_LEN];
  struct stat st;

  if (remote_path == NULL || strlen(remote_path) == 0) {
    printf("Usage: get <remote_path> [local_path]\n");
    return;
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
}

static void cmd_mget(struct smb2_context *smb2, const char *remote_path, const char *local_path)
{
  char *target_remote;
  struct stat st;

  if (remote_path == NULL || strlen(remote_path) == 0) {
    printf("Usage: mget <remote_path> [local_path]\n");
    return;
  }
  
  if (local_path == NULL || strlen(local_path) == 0) {
    local_path = ".";
  }
  target_remote = resolve_path(remote_path);

  // Check if local path is a directory
  if (!(strcmp(local_path, ".") == 0 ||
        strcmp(local_path, "..") == 0 ||
        (stat(local_path, &st) == 0 && S_ISDIR(st.st_mode)))) {
    printf("Local path '%s' is not a directory\n", local_path);
    return;
  }

  int files_downloaded = get_multiple_files(smb2, target_remote, local_path);
  
  if (files_downloaded < 0) {
    printf("Error occurred during multiple file download\n");
  } else {
    printf("Downloaded %d file(s)\n", files_downloaded);
  }
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
    printf("Failed to open local file '%s': %s\n", target_local, strerror(errno));
    return -1;
  }
  
  // Open remote file for writing
  fh = smb2_open(smb2, sjis_to_utf8(target_remote + 1), O_WRONLY | O_CREAT | O_TRUNC);
  if (fh == NULL) {
    printf("Failed to create remote file '%s': %s\n", target_remote, smb2_get_error(smb2));
    close(local_fd);
    return -1;
  }
  
  printf("Uploading '%s' to '%s'\n", target_local, target_remote);
  
  // Copy data
  while ((bytes_read = read(local_fd, buffer, sizeof(buffer))) > 0) {
    bytes_written = smb2_write(smb2, fh, buffer, bytes_read);
    if (bytes_written != bytes_read) {
      printf("Failed to write to remote file: %s\n", smb2_get_error(smb2));
      break;
    }
  }
  
  if (bytes_read < 0) {
    printf("Failed to read from local file: %s\n", strerror(errno));
    close(local_fd);
    smb2_close(smb2, fh);
    return -1;
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
    printf("Failed to open local directory '%s': %s\n", directory_path, strerror(errno));
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
        printf("Created remote directory '%s'\n", remote_path);

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

static void cmd_put(struct smb2_context *smb2, const char *local_path, const char *remote_path)
{
  const char *target_local;
  char *target_remote;
  char remote_full_path[PATH_LEN];
  struct smb2_stat_64 remote_st;
  
  if (local_path == NULL || strlen(local_path) == 0) {
    printf("Usage: put <local_path> [remote_path]\n");
    return;
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
}

static void cmd_mput(struct smb2_context *smb2, const char *local_path, const char *remote_path)
{
  char *target_remote;
  struct smb2_stat_64 remote_st;
  
  if (local_path == NULL || strlen(local_path) == 0) {
    printf("Usage: mput <local_path> [remote_path]\n");
    return;
  }
  
  if (remote_path == NULL || strlen(remote_path) == 0) {
    remote_path = "";
  }
  target_remote = resolve_path(remote_path);
  
  // Check if remote path is a directory
  if (!(smb2_stat(smb2, sjis_to_utf8(target_remote + 1), &remote_st) == 0 && 
        remote_st.smb2_type == SMB2_TYPE_DIRECTORY)) {
    printf("Remote path '%s' is not a directory\n", target_remote);
    return;
  }

  int files_uploaded = put_multiple_files(smb2, local_path, target_remote);
  
  if (files_uploaded < 0) {
    printf("Error occurred during multiple file upload\n");
  } else {
    printf("Uploaded %d file(s)\n", files_uploaded);
  }
}

//----------------------------------------------------------------------------

static int execute_command(struct smb2_context *smb2, char *cmdline)
{
  char *cmd, *arg;
  
  // Skip empty lines
  if (strlen(cmdline) == 0) {
    return 0;
  }

  // Parse command and argument
  if (cmdline[0] == '!') {
    // Shell command
    cmd = "shell";
    arg = cmdline + 1;
  } else {
    cmd = strtok(cmdline, " \t");
    arg = strtok(NULL, "");  // Get rest of line as argument
  }
  arg = trim_spaces(arg);
  
  if (cmd == NULL) {
    return 0;
  }
  
  if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
    return 1;  // Signal to exit
  } else if (strcmp(cmd, "help") == 0) {
    printf("Available commands:\n");
    printf("  ls [path]     - List directory contents (current dir if no path)\n");
    printf("  cd <path>     - Change directory\n");
    printf("  cd            - Show current directory\n");
    printf("  mkdir <path>  - Create directory\n");
    printf("  rmdir <path>  - Remove empty directory\n");
    printf("  rm <path>     - Remove file\n");
    printf("  stat <path>   - Show file/directory information\n");
    printf("  statvfs [path]- Show filesystem statistics\n");
    printf("  get <remote> [local] - Download file from server\n");
    printf("  put <local> [remote] - Upload file to server\n");
    printf("  mget <remote> [local] - Download multiple files from server\n");
    printf("  mput <local> [remote] - Upload multiple files to server\n");
    printf("  lcd [path]    - Change local directory\n");
    printf("  shell [command] - Execute shell command\n");
    printf("  quit/exit     - Exit the program\n");
    printf("  help          - Show this help\n");
  } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0 || strcmp(cmd, "l") == 0) {
    cmd_ls(smb2, arg);  // arg can be NULL for current directory
  } else if (strcmp(cmd, "cd") == 0 || strcmp(cmd, "chdir") == 0) {
    cmd_cd(smb2, arg);  // arg can be NULL to show current directory
  } else if (strcmp(cmd, "mkdir") == 0 || strcmp(cmd, "md") == 0) {
    cmd_mkdir(smb2, arg);
  } else if (strcmp(cmd, "rmdir") == 0 || strcmp(cmd, "rd") == 0) {
    cmd_rmdir(smb2, arg);
  } else if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "del") == 0) {
    cmd_rm(smb2, arg);
  } else if (strcmp(cmd, "stat") == 0) {
    cmd_stat(smb2, arg);
  } else if (strcmp(cmd, "statvfs") == 0) {
    cmd_statvfs(smb2, arg);  // arg can be NULL for current directory
  } else if (strcmp(cmd, "lcd") == 0) {
    cmd_lcd(arg);  // arg can be NULL to show current local directory
  } else if (strcmp(cmd, "shell") == 0) {
    system(arg ? arg : "");
  } else {
    char *arg1 = NULL, *arg2 = NULL;
    parse_two_args(arg, &arg1, &arg2);
    if (strcmp(cmd, "get") == 0) {
      cmd_get(smb2, arg1, arg2);
    } else if (strcmp(cmd, "mget") == 0) {
      cmd_mget(smb2, arg1, arg2);
    } else if (strcmp(cmd, "put") == 0) {
      cmd_put(smb2, arg1, arg2);
    } else if (strcmp(cmd, "mput") == 0) {
      cmd_mput(smb2, arg1, arg2);
    } else {
      printf("Unknown command: %s\n", cmd);
      printf("Type 'help' for available commands\n");
    }
  }
  
  return 0;  // Continue execution
}

static int execute_command_string(struct smb2_context *smb2, const char *command_string)
{
  char *commands = strdup(command_string);
  char *cmd_copy;
  char *token;
  int result = 0;
  
  if (commands == NULL) {
    printf("Memory allocation failed\n");
    return 1;
  }
  
  // Split commands by semicolon
  cmd_copy = commands;
  while ((token = strtok(cmd_copy, ";")) != NULL) {
    cmd_copy = NULL;  // For subsequent calls to strtok
    
    // Trim leading/trailing spaces
    char *trimmed = trim_spaces(token);
    if (trimmed != NULL) {
      char *cmd_line = strdup(trimmed);
      if (cmd_line != NULL) {
        result = execute_command(smb2, cmd_line);
        free(cmd_line);
        if (result) {
          break;  // Exit command was issued
        }
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
    printf("Failed to enumerate shares (%s) %s\n",
           strerror(-status), smb2_get_error(smb2));
    is_finished = true;
    return;
  }

  printf("Available services on the server:\n");
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
    printf("Failed to connect to IPC$ on %s: %s\n", server, smb2_get_error(smb2));
    return 1;
  }

  if (smb2_share_enum_async(smb2, SHARE_INFO_1, share_enum_cb, NULL) != 0) {
    printf("Failed to start share enumeration: %s\n", smb2_get_error(smb2));
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
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "smbclient <smb2-url>                  - Interactive mode\n");
  fprintf(stderr, "smbclient -L <smb2-url>               - List available services\n");
  fprintf(stderr, "smbclient <smb2-url> -c \"<commands>\"   - Execute commands\n\n");
  fprintf(stderr, "URL format: \n");
  fprintf(stderr, "  [smb://][<domain;][<username>@]<host>[:<port>][/<share>/<path>]\n");
  fprintf(stderr, "\nCommands can be separated by semicolons (;)\n");
  fprintf(stderr, "Examples:\n");
  fprintf(stderr, "  smbclient server/share\n");
  fprintf(stderr, "  smbclient //server/share\n");
  fprintf(stderr, "  smbclient smb://server/share\n");
  fprintf(stderr, "  smbclient server/share -c \"ls; cd dir; ls\"\n");
}

int main(int argc, char *argv[])
{
  struct smb2_context *smb2;
  struct smb2_url *url;
  int list_mode = 0;
  int command_mode = 0;
  int url_index = 1;
  char *command_string = NULL;

  // Parse command line options
  if (argc < 2) {
    usage();
    exit(1);
  }

  // Check for -L option first
  if (strcmp(argv[1], "-L") == 0) {
    list_mode = 1;
    url_index = 2;
    if (argc < 3) {
      usage();
      exit(1);
    }
  } else {
    // Check for -c option in various positions
    for (int i = 1; i < argc - 1; i++) {
      if (strcmp(argv[i], "-c") == 0) {
        command_mode = 1;
        command_string = argv[i + 1];
        
        // URL should be before -c option
        if (i == 1) {
          printf("Error: URL must be specified before -c option\n");
          usage();
          exit(1);
        }
        url_index = 1;  // URL is always first when -c is used
        break;
      }
    }
    
    // If no -c option found, check if we have enough arguments
    if (!command_mode && argc < 2) {
      usage();
      exit(1);
    }
  }

  // Check whether TCP/IP is available
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    fprintf(stderr, "TCP/IP socket creation failed\n");
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
    fprintf(stderr, "Failed to parse url: %s\n", smb2_get_error(smb2));
    exit(1);
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
    printf("smb2_connect_share failed. %s\n", smb2_get_error(smb2));
    exit(1);
  }
  smb2_destroy_url(url);

  if (command_mode) {
    // Execute the specified command(s) and exit
    execute_command_string(smb2, command_string);
  } else {
    // Interactive mode
    char cmdline[256];

    pthread_mutex_lock(&keepalive_mutex);
    if (pthread_create(&keepalive_thread, NULL, keepalive_thread_func, smb2) != 0) {
      printf("Failed to create keepalive thread\n");
      smb2_disconnect_share(smb2);
      smb2_destroy_context(smb2);
      exit(1);
    }

    printf("SMB Client - Type 'help' for commands, 'quit' to exit\n");
    
    while (1) {
      printf("smb:%s> ", current_dir);

      pthread_mutex_unlock(&keepalive_mutex);
      char *res = fgets(cmdline, sizeof(cmdline), stdin);
      pthread_mutex_lock(&keepalive_mutex);
      if (res == NULL) {
        break;
      }

      trim_newline(cmdline);
      
      if (execute_command(smb2, cmdline)) {
        break;  // Exit command was issued
      }
    }
  }

  pthread_cancel(keepalive_thread);
  pthread_join(keepalive_thread, NULL);
  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  return 0;
}
