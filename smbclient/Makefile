#
# Copyright (c) 2025 Yuichi Nakamura (@yunkya2)
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
LDFLAGS = -lsocket -lpthread
LDFLAGS += -Wl,-Map,$(@:.x=.map)

INC += -I. -I../include -I../iconv_mini/
INC += -I../libsmb2/include -I../libsmb2/include/smb2
DEFS +=

OBJS += smbclient.o
OBJS += iconv_mini.o

LIBSMB2 = libsmb2.a
LIBS += $(LIBSMB2)

all: smbclient.x

smbclient.x: $(OBJS) $(LIBS)
	$(LD) -o $@ $^ $(LDFLAGS) -s

$(LIBSMB2): ../libsmb2/x68k/$(LIBSMB2)
	cp $< $@

../libsmb2/x68k/$(LIBSMB2): FORCE
	$(MAKE) -C ../libsmb2/x68k

FORCE:

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: ../iconv_mini/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

DEPS = $(OBJS:.o=.d)

clean:
	-rm -f $(OBJS) $(DEPS) $(LIBSMB2) *.x *.elf* *.map

distclean: clean
	$(MAKE) -C ../libsmb2/x68k clean

-include $(DEPS)

.PHONY: all clean distclean FORCE
