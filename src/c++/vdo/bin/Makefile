# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright 2023 Red Hat
#
# VDO C bin Makefile
#

SRC_DIR = ../../..
include $(SRC_DIR)/c++/defines
include $(CURRENT_VERSION_FILE)

PROGS      =
SUBDIRS    =

all: $(PROGS)
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

clean:
	rm -f $(PROGS) *.o core*
	rm -fr $(DEPDIR)
	set -e;					\
	for d in $(SUBDIRS); do			\
		$(MAKE) -C $$d $@;		\
	done

.PHONY: all clean
