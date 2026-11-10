# The crash test's program, from the file system of the tree at SRC (this
# one by default; another one's for a comparison) into O. MEMORYSIZE sizes
# the disk cache as for a target of that many MB: 256 sectors from 32 on,
# 32 below 8.
#   make [SRC=<rockbox tree>] [O=<build dir>] [MEMORYSIZE=<MB>]

HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SRC ?= $(abspath $(HERE)../..)
O ?= $(HERE)work/build
CC ?= cc

FS := fat disk_cache file file_internal fileobj_mgr dir pathfuncs \
      rb_namespace linked_list unicode timefuncs fdprintf vuprintf \
      strmemccpy strlcat strlcpy
FS_OBJ := $(addprefix $(O)/,$(addsuffix .o,$(FS)))

MEMORYSIZE ?= 32

CFLAGS := -std=gnu99 -g -O1 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
          -DYEAR=2026 -DMONTH=11 -DDAY=09 -DMEMORYSIZE=$(MEMORYSIZE)
# the firmware's own warnings are the firmware build's business
FWFLAGS := -w
RBFLAGS := -include $(HERE)stub/fstest_config.h -I$(HERE)stub \
           -I$(SRC)/firmware/include -I$(SRC)/firmware/export

$(O)/fstest: $(FS_OBJ) $(O)/fstest.o $(O)/stubs.o $(O)/host.o
	$(CC) -o $@ $^

$(O)/%.o: $(SRC)/firmware/common/%.c $(HERE)stub/fstest_config.h | $(O)
	$(CC) $(CFLAGS) $(FWFLAGS) $(RBFLAGS) -c -o $@ $<

$(O)/settings_fns.c: $(SRC)/apps/settings.c $(HERE)extract.py | $(O)
	python3 -I $(HERE)extract.py $< $@

$(O)/fstest.o: $(HERE)fstest.c $(O)/settings_fns.c $(HERE)host.h | $(O)
	$(CC) $(CFLAGS) -Wall $(RBFLAGS) -I$(O) -I$(HERE) -c -o $@ $<

$(O)/stubs.o: $(HERE)stubs.c $(HERE)host.h | $(O)
	$(CC) $(CFLAGS) -Wall $(RBFLAGS) -I$(HERE) -c -o $@ $<

$(O)/host.o: $(HERE)host.c $(HERE)host.h | $(O)
	$(CC) $(CFLAGS) -Wall -c -o $@ $<

$(O):
	mkdir -p $@

clean:
	rm -rf $(O)

.PHONY: clean
