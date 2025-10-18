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
#include <x68k/dos.h>
#include <x68k/iocs.h>

#include <smb2.h>
#include <libsmb2.h>

//****************************************************************************
// Global variables
//****************************************************************************

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

static void cmd_ls(struct smb2_context *smb2, const char *path)
{
  struct smb2dir *dir;
  struct smb2dirent *ent;
  const char *type;

  dir = smb2_opendir(smb2, path);
  if (dir == NULL) {
    printf("Failed to open directory '%s': %s\n", path, smb2_get_error(smb2));
    return;
  }

  printf("Directory listing for '%s':\n", path);
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

//****************************************************************************
// Dummy program entry
//****************************************************************************

int main(int argc, char *argv[])
{
  struct smb2_context *smb2;
  struct smb2_url *url;

  if (argc < 2) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "smbclient <smb2-url>\n\n");
    fprintf(stderr, "URL format: smb://[<domain;][<username>@]<host>>[:<port>]/<share>/<path>\n");
    exit(1);
  }

  smb2 = smb2_init_context();
  if (smb2 == NULL) {
    fprintf(stderr, "Failed to init context\n");
    exit(0);
  }

  url = smb2_parse_url(smb2, argv[1]);
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
  
  printf("SMB Client - Type 'help' for commands, 'quit' to exit\n");
  
  while (1) {
    printf("smb> ");
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
      printf("  ls <path>  - List directory contents\n");
      printf("  quit       - Exit the program\n");
      printf("  help       - Show this help\n");
    } else if (strcmp(cmd, "ls") == 0) {
      if (arg == NULL || strlen(arg) == 0) {
        printf("Usage: ls <path>\n");
      } else {
        // Remove leading/trailing spaces
        while (*arg == ' ' || *arg == '\t') arg++;
        char *end = arg + strlen(arg) - 1;
        while (end > arg && (*end == ' ' || *end == '\t')) *end-- = '\0';
        
        cmd_ls(smb2, arg);
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
