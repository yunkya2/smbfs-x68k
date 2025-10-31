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

SUBDIR = smbfs smbmount smbclient

all:
	for dir in $(SUBDIR); do \
	  $(MAKE) -C $$dir all; \
	done

clean distclean:
	for dir in $(SUBDIR); do \
	  $(MAKE) -C $$dir $@; \
	done
	-rm -f *.x README.txt

GIT_REPO_VERSION=$(shell git describe --tags --always)

release: distclean all
	cp smbclient/smbclient.x .
	./md2txtconv.py README.md
	zip -r smbclient-$(GIT_REPO_VERSION).zip README.txt smbclient.x

.PHONY: all clean distclean release
.PHONY: smbfs smbmount smbclient
