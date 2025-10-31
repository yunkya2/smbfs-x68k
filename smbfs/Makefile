#
# Copyright (c) 2023,2025 Yuichi Nakamura (@yunkya2)
#
# The MIT License (MIT)
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

CROSS = m68k-xelf-
CC = $(CROSS)gcc
AS = $(CROSS)gcc
LD = $(CROSS)gcc
AR = $(CROSS)ar
RANLIB = $(CROSS)ranlib

GIT_REPO_VERSION=$(shell git describe --tags --always)

CFLAGS = -Wall -Os -g $(INC) $(DEFS) -MMD -MP
CFLAGS += -DGIT_REPO_VERSION=\"$(GIT_REPO_VERSION)\"
ASFLAGS = -I. -MMD -MP

INC += -I. -I../include -I../iconv_mini/
INC += -I../libsmb2/include -I../libsmb2/include/smb2
LIBS += -lsocket
DEFS +=

ifneq ($(DEBUG),)
DEFS += -DDEBUG
endif

OBJS = head.o smbfs.o
OBJS += iconv_mini.o

LIBSMB2 = libsmb2.a
LIBS += libsmb2.a

all: smbfs.sys

smbfs.sys: $(OBJS) $(LIBSMB2)
	$(LD) -o $@ $^ $(LIBS) -nostartfiles -s

$(LIBSMB2):
	$(MAKE) -C ../libsmb2/x68k
	cp ../libsmb2/x68k/$@ .

%.o: %.c
	$(CC) $(CFLAGS) -c $<

%.o: ../iconv_mini/%.c
	$(CC) $(CFLAGS) -c $<

%.o: %.S
	$(AS) $(ASFLAGS) -c $<

DEPS = $(OBJS:.o=.d)

clean:
	-rm -f $(OBJS) $(DEPS) $(LIBSMB2) *.sys *.elf*

-include $(DEPS)

.PHONY: all clean
