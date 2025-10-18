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
#include <unistd.h>
#include <sys/stat.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

#include <smb2.h>
#include <libsmb2.h>
#include <libsmb2-raw.h>
#include <libsmb2-dcerpc-srvsvc.h>

//****************************************************************************
// Global variables
//****************************************************************************

static char current_dir[1024] = "/";
static char local_dir[1024] = ".";
static int is_finished = 0;

//****************************************************************************
// for debugging
//****************************************************************************

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

static void normalize_path(char *path)
{
  int len = strlen(path);
  // Ensure path ends with '/' if it's not root
  if (len > 1 && path[len-1] != '/') {
    strcat(path, "/");
  }
  // Ensure path starts with '/'
  if (path[0] != '/') {
    memmove(path + 1, path, strlen(path) + 1);
    path[0] = '/';
  }
}

static char* resolve_path(const char *path)
{
  static char resolved[1024];
  
  if (path[0] == '/') {
    // Absolute path
    strcpy(resolved, path);
  } else {
    // Relative path - append to current directory
    strcpy(resolved, current_dir);
    if (strcmp(current_dir, "/") != 0) {
      strcat(resolved, "/");
    }
    strcat(resolved, path);
  }
  
  normalize_path(resolved);
  return resolved;
}

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
  dir = smb2_opendir(smb2, resolved_path);
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

static void cmd_mkdir(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    printf("Usage: mkdir <path>\n");
    return;
  }
  
  target_path = resolve_path(path);
  
  if (smb2_mkdir(smb2, target_path) != 0) {
    printf("Failed to create directory '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
  }
  
  printf("Directory '%s' created successfully\n", target_path);
}

static void cmd_rmdir(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    printf("Usage: rmdir <path>\n");
    return;
  }
  
  target_path = resolve_path(path);
  
  if (smb2_rmdir(smb2, target_path) != 0) {
    printf("Failed to remove directory '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
  }
  
  printf("Directory '%s' removed successfully\n", target_path);
}

static void cmd_rm(struct smb2_context *smb2, const char *path)
{
  char *target_path;
  
  if (path == NULL || strlen(path) == 0) {
    printf("Usage: rm <path>\n");
    return;
  }
  
  target_path = resolve_path(path);
  
  if (smb2_unlink(smb2, target_path) != 0) {
    printf("Failed to remove file '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
  }
  
  printf("File '%s' removed successfully\n", target_path);
}

static const char* format_time(uint64_t timestamp)
{
  static char time_str[64];
  time_t t = (time_t)timestamp;
  struct tm *tm_info = localtime(&t);
  
  if (tm_info) {
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
  } else {
    strcpy(time_str, "Invalid time");
  }
  
  return time_str;
}

static const char* format_size(uint64_t size)
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
    sprintf(size_str, "%llu bytes", (unsigned long long)size);
  }
  
  return size_str;
}

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
  
  if (smb2_stat(smb2, target_path, &st) != 0) {
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
  printf("Size: %s (%llu bytes)\n", format_size(st.smb2_size), (unsigned long long)st.smb2_size);
  printf("Inode: %llu\n", (unsigned long long)st.smb2_ino);
  printf("Links: %lu\n", (unsigned long)st.smb2_nlink);
  printf("Access time: %s\n", format_time(st.smb2_atime));
  printf("Modify time: %s\n", format_time(st.smb2_mtime));
  printf("Change time: %s\n", format_time(st.smb2_ctime));
  printf("Birth time:  %s\n", format_time(st.smb2_btime));
}

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
  
  if (smb2_statvfs(smb2, target_path, &statvfs) != 0) {
    printf("Failed to get filesystem statistics for '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
  }
  
  total_space = (uint64_t)statvfs.f_blocks * statvfs.f_bsize;
  free_space = (uint64_t)statvfs.f_bavail * statvfs.f_bsize;
  used_space = total_space - ((uint64_t)statvfs.f_bfree * statvfs.f_bsize);
  usage_percent = total_space > 0 ? ((double)used_space * 100.0) / total_space : 0.0;
  
  printf("Filesystem statistics for: %s\n", target_path);
  printf("Block size:       %lu bytes\n", (unsigned long)statvfs.f_bsize);
  printf("Fragment size:    %lu bytes\n", (unsigned long)statvfs.f_frsize);
  printf("Total blocks:     %llu\n", (unsigned long long)statvfs.f_blocks);
  printf("Free blocks:      %llu\n", (unsigned long long)statvfs.f_bfree);
  printf("Available blocks: %llu\n", (unsigned long long)statvfs.f_bavail);
  printf("Total space:      %s (%llu bytes)\n", format_size(total_space), total_space);
  printf("Used space:       %s (%llu bytes)\n", format_size(used_space), used_space);
  printf("Free space:       %s (%llu bytes)\n", format_size(free_space), free_space);
  printf("Usage:            %.1f%%\n", usage_percent);
  printf("Total inodes:     %lu\n", (unsigned long)statvfs.f_files);
  printf("Free inodes:      %lu\n", (unsigned long)statvfs.f_ffree);
  printf("Available inodes: %lu\n", (unsigned long)statvfs.f_favail);
  printf("Filesystem ID:    0x%08lx\n", (unsigned long)statvfs.f_fsid);
  printf("Mount flags:      0x%08lx\n", (unsigned long)statvfs.f_flag);
  printf("Max filename:     %lu\n", (unsigned long)statvfs.f_namemax);
}

static void cmd_lcd(const char *path)
{
  if (path == NULL || strlen(path) == 0) {
    // No argument - show current local directory
    printf("Local directory: %s\n", local_dir);
    return;
  }
  
  if (chdir(path) != 0) {
    printf("Failed to change local directory to '%s': %s\n", path, strerror(errno));
    return;
  }
  
  // Update local directory (simple path tracking)
  if (path[0] == '/') {
    strcpy(local_dir, path);
  } else if (strcmp(path, "..") == 0) {
    char *last_slash = strrchr(local_dir, '/');
    if (last_slash && last_slash != local_dir) {
      *last_slash = '\0';
    } else if (strcmp(local_dir, ".") != 0) {
      strcpy(local_dir, ".");
    }
  } else if (strcmp(path, ".") != 0) {
    if (strcmp(local_dir, ".") == 0) {
      strcpy(local_dir, path);
    } else {
      strcat(local_dir, "/");
      strcat(local_dir, path);
    }
  }
  
  printf("Changed local directory to: %s\n", local_dir);
}

static void cmd_get(struct smb2_context *smb2, const char *remote_path, const char *local_path)
{
  struct smb2fh *fh;
  char *target_remote;
  const char *target_local;
  char local_full_path[1024];
  int local_fd;
  uint8_t buffer[8192];
  int bytes_read, bytes_written;
  struct stat st;
  
  if (remote_path == NULL || strlen(remote_path) == 0) {
    printf("Usage: get <remote_path> [local_path]\n");
    return;
  }
  
  target_remote = resolve_path(remote_path);
  
  if (local_path == NULL || strlen(local_path) == 0) {
    // Extract filename from remote path
    const char *filename = strrchr(target_remote, '/');
    if (filename) {
      filename++; // Skip the '/'
    } else {
      filename = target_remote;
    }
    snprintf(local_full_path, sizeof(local_full_path), "%s/%s", local_dir, filename);
    target_local = local_full_path;
  } else {
    if (local_path[0] == '/') {
      target_local = local_path;
    } else {
      snprintf(local_full_path, sizeof(local_full_path), "%s/%s", local_dir, local_path);
      target_local = local_full_path;
    }
  }
  
  // Check if local path is a directory
  if (stat(target_local, &st) == 0 && S_ISDIR(st.st_mode)) {
    const char *filename = strrchr(target_remote, '/');
    if (filename) {
      filename++; // Skip the '/'
    } else {
      filename = target_remote;
    }
    snprintf(local_full_path, sizeof(local_full_path), "%s/%s", target_local, filename);
    target_local = local_full_path;
  }
  
  // Open remote file for reading
  fh = smb2_open(smb2, target_remote, O_RDONLY);
  if (fh == NULL) {
    printf("Failed to open remote file '%s': %s\n", target_remote, smb2_get_error(smb2));
    return;
  }
  
  // Open local file for writing
  local_fd = open(target_local, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (local_fd < 0) {
    printf("Failed to create local file '%s': %s\n", target_local, strerror(errno));
    smb2_close(smb2, fh);
    return;
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
  } else {
    printf("Download completed successfully\n");
  }
  
  close(local_fd);
  smb2_close(smb2, fh);
}

static void cmd_put(struct smb2_context *smb2, const char *local_path, const char *remote_path)
{
  struct smb2fh *fh;
  const char *target_local;
  char *target_remote;
  char local_full_path[1024];
  char remote_full_path[1024];
  int local_fd;
  uint8_t buffer[8192];
  int bytes_read, bytes_written;
  struct smb2_stat_64 remote_st;
  
  if (local_path == NULL || strlen(local_path) == 0) {
    printf("Usage: put <local_path> [remote_path]\n");
    return;
  }
  
  if (local_path[0] == '/') {
    target_local = local_path;
  } else {
    snprintf(local_full_path, sizeof(local_full_path), "%s/%s", local_dir, local_path);
    target_local = local_full_path;
  }
  
  if (remote_path == NULL || strlen(remote_path) == 0) {
    // Extract filename from local path
    const char *filename = strrchr(target_local, '/');
    if (filename) {
      filename++; // Skip the '/'
    } else {
      filename = target_local;
    }
    snprintf(remote_full_path, sizeof(remote_full_path), "%s/%s", 
             strcmp(current_dir, "/") == 0 ? "" : current_dir, filename);
    target_remote = remote_full_path;
  } else {
    target_remote = resolve_path(remote_path);
  }
  
  // Check if remote path is a directory
  if (smb2_stat(smb2, target_remote, &remote_st) == 0 && 
      remote_st.smb2_type == SMB2_TYPE_DIRECTORY) {
    const char *filename = strrchr(target_local, '/');
    if (filename) {
      filename++; // Skip the '/'
    } else {
      filename = target_local;
    }
    snprintf(remote_full_path, sizeof(remote_full_path), "%s/%s", target_remote, filename);
    target_remote = remote_full_path;
  }
  
  // Open local file for reading
  local_fd = open(target_local, O_RDONLY);
  if (local_fd < 0) {
    printf("Failed to open local file '%s': %s\n", target_local, strerror(errno));
    return;
  }
  
  // Open remote file for writing
  fh = smb2_open(smb2, target_remote, O_WRONLY | O_CREAT | O_TRUNC);
  if (fh == NULL) {
    printf("Failed to create remote file '%s': %s\n", target_remote, smb2_get_error(smb2));
    close(local_fd);
    return;
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
  } else {
    printf("Upload completed successfully\n");
  }
  
  close(local_fd);
  smb2_close(smb2, fh);
}

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

  dir = smb2_opendir(smb2, target_path);
  if (dir == NULL) {
    printf("Failed to open directory '%s': %s\n", target_path, smb2_get_error(smb2));
    return;
  }

  printf("Directory listing for '%s':\n", target_path);
  printf("%-30s %-10s %15s\n", "Name", "Type", "Size");
  printf("%-30s %-10s %15s\n", "----", "----", "----");

  while ((ent = smb2_readdir(smb2, dir))) {
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
    printf("%-30s %-10s %15llu\n", ent->name, type, (unsigned long long)ent->st.smb2_size);
  }

  smb2_closedir(smb2, dir);
}

static void share_enum_cb(struct smb2_context *smb2, int status,
                          void *command_data, void *private_data)
{
  struct srvsvc_NetrShareEnum_rep *rep = command_data;
  int i;

  if (status) {
    printf("Failed to enumerate shares (%s) %s\n",
           strerror(-status), smb2_get_error(smb2));
    is_finished = 1;
    return;
  }

  printf("Available services on the server:\n");
  printf("%-20s %-30s %s\n", "Share name", "Comment", "Type");
  printf("%-20s %-30s %s\n", "----------", "-------", "----");

  for (i = 0; i < rep->ses.ShareInfo.Level1.EntriesRead; i++) {
    const char *share_name = rep->ses.ShareInfo.Level1.Buffer->share_info_1[i].netname.utf8;
    const char *comment = rep->ses.ShareInfo.Level1.Buffer->share_info_1[i].remark.utf8;
    uint32_t type = rep->ses.ShareInfo.Level1.Buffer->share_info_1[i].type;
    
    printf("%-20s %-30s ", share_name, comment ? comment : "");
    
    switch (type & 3) {
    case SHARE_TYPE_DISKTREE:
      printf("Disk");
      break;
    case SHARE_TYPE_PRINTQ:
      printf("Printer");
      break;
    case SHARE_TYPE_DEVICE:
      printf("Device");
      break;
    case SHARE_TYPE_IPC:
      printf("IPC");
      break;
    default:
      printf("Unknown");
      break;
    }
    
    if (type & SHARE_TYPE_TEMPORARY) {
      printf(" (Temporary)");
    }
    if (type & SHARE_TYPE_HIDDEN) {
      printf(" (Hidden)");
    }
    
    printf("\n");
  }

  smb2_free_data(smb2, rep);
  is_finished = 1;
}

static int list_shares(const char *server, const char *user)
{
  struct smb2_context *smb2;
  int ret = 0;

  smb2 = smb2_init_context();
  if (smb2 == NULL) {
    fprintf(stderr, "Failed to init context\n");
    return 1;
  }

  if (user) {
    smb2_set_user(smb2, user);
  }

  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);

  if (smb2_connect_share(smb2, server, "IPC$", NULL) < 0) {
    printf("Failed to connect to IPC$ on %s: %s\n", server, smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return 1;
  }

  if (smb2_share_enum_async(smb2, SHARE_INFO_1, share_enum_cb, NULL) != 0) {
    printf("Failed to start share enumeration: %s\n", smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return 1;
  }

  // Simple event loop
  while (!is_finished) {
    int events = smb2_which_events(smb2);
    
    // Simple polling mechanism - just service the connection
    if (smb2_service(smb2, events) < 0) {
      printf("smb2_service failed: %s\n", smb2_get_error(smb2));
      ret = 1;
      break;
    }
  }

  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  return ret;
}

//****************************************************************************
// Dummy program entry
//****************************************************************************

static void usage(void)
{
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "smbclient <smb2-url>                  - Interactive mode\n");
  fprintf(stderr, "smbclient -L <smb2-url>               - List available services\n\n");
  fprintf(stderr, "URL format: smb://[<domain;][<username>@]<host>>[:<port>][/<share>/<path>]\n");
}

int main(int argc, char *argv[])
{
  struct smb2_context *smb2;
  struct smb2_url *url;
  int list_mode = 0;
  int url_index = 1;

  // Parse command line options
  if (argc < 2) {
    usage();
    exit(1);
  }

  if (strcmp(argv[1], "-L") == 0) {
    list_mode = 1;
    url_index = 2;
    if (argc < 3) {
      usage();
      exit(1);
    }
  }

  // For list mode, handle URL parsing differently
  if (list_mode) {
    char *server_url = argv[url_index];
    char *user = NULL;
    char *server = NULL;
    
    // Parse the URL to extract server and user
    smb2 = smb2_init_context();
    if (smb2 == NULL) {
      fprintf(stderr, "Failed to init context\n");
      exit(1);
    }
    
    url = smb2_parse_url(smb2, server_url);
    if (url == NULL) {
      fprintf(stderr, "Failed to parse url: %s\n", smb2_get_error(smb2));
      exit(1);
    }
    
    server = (char *)url->server;
    user = (char *)url->user;
    
    int result = list_shares(server, user);
    
    smb2_destroy_url(url);
    smb2_destroy_context(smb2);
    
    exit(result);
  }

  // Interactive mode
  smb2 = smb2_init_context();
  if (smb2 == NULL) {
    fprintf(stderr, "Failed to init context\n");
    exit(0);
  }

  url = smb2_parse_url(smb2, argv[url_index]);
  if (url == NULL) {
    fprintf(stderr, "Failed to parse url: %s\n",
            smb2_get_error(smb2));
    exit(0);
  }

  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  if (smb2_connect_share(smb2, url->server, url->share, url->user) != 0) {
    printf("smb2_connect_share failed. %s\n", smb2_get_error(smb2));
    exit(10);
  }

  char cmdline[256];
  char *cmd, *arg;
  
  // Initialize local directory
  strcpy(local_dir, ".");
  
  printf("SMB Client - Type 'help' for commands, 'quit' to exit\n");
  
  while (1) {
    printf("smb:%s> ", current_dir);
    if (fgets(cmdline, sizeof(cmdline), stdin) == NULL) {
      break;
    }

    trim_newline(cmdline);
    
    // Skip empty lines
    if (strlen(cmdline) == 0) {
      continue;
    }
    
    // Parse command and argument
    cmd = strtok(cmdline, " \t");
    arg = strtok(NULL, "");  // Get rest of line as argument
    
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
      break;
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
      printf("  lcd [path]    - Change local directory\n");
      printf("  quit/exit     - Exit the program\n");
      printf("  help          - Show this help\n");
    } else if (strcmp(cmd, "ls") == 0) {
      arg = trim_spaces(arg);
      cmd_ls(smb2, arg);  // arg can be NULL for current directory
    } else if (strcmp(cmd, "cd") == 0 || strcmp(cmd, "chdir") == 0) {
      arg = trim_spaces(arg);
      cmd_cd(smb2, arg);  // arg can be NULL to show current directory
    } else if (strcmp(cmd, "mkdir") == 0) {
      arg = trim_spaces(arg);
      cmd_mkdir(smb2, arg);
    } else if (strcmp(cmd, "rmdir") == 0) {
      arg = trim_spaces(arg);
      cmd_rmdir(smb2, arg);
    } else if (strcmp(cmd, "rm") == 0) {
      arg = trim_spaces(arg);
      cmd_rm(smb2, arg);
    } else if (strcmp(cmd, "stat") == 0) {
      arg = trim_spaces(arg);
      cmd_stat(smb2, arg);
    } else if (strcmp(cmd, "statvfs") == 0) {
      arg = trim_spaces(arg);
      cmd_statvfs(smb2, arg);  // arg can be NULL for current directory
    } else if (strcmp(cmd, "lcd") == 0) {
      arg = trim_spaces(arg);
      cmd_lcd(arg);  // arg can be NULL to show current local directory
    } else if (strcmp(cmd, "get") == 0) {
      char *remote_arg = NULL, *local_arg = NULL;
      int argc = parse_two_args(arg, &remote_arg, &local_arg);
      if (argc == 0) {
        printf("Usage: get <remote_path> [local_path]\n");
      } else {
        cmd_get(smb2, remote_arg, local_arg);
      }
    } else if (strcmp(cmd, "put") == 0) {
      char *local_arg = NULL, *remote_arg = NULL;
      int argc = parse_two_args(arg, &local_arg, &remote_arg);
      if (argc == 0) {
        printf("Usage: put <local_path> [remote_path]\n");
      } else {
        cmd_put(smb2, local_arg, remote_arg);
      }
    } else {
      printf("Unknown command: %s\n", cmd);
      printf("Type 'help' for available commands\n");
    }
  }

  smb2_destroy_url(url);
  smb2_destroy_context(smb2);
  return 0;
}
