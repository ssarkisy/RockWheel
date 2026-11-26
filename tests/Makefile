#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Tests on the Mac: the firmware's own sources built for the host with clang,
# AddressSanitizer and UBSan, the firmware they call stubbed; and the codecs
# through warble against fixtures made by codecs/make_fixtures.sh.
#
#   make -C tests check          everything; non-zero on a failure
#   make -C tests check-keys     the sort and album keys
#   make -C tests check-itunesdb iTunesDB and ArtworkDB, whole and damaged
#   make -C tests check-covers   the prepared covers' sums and tables
#   make -C tests check-codecs   the codecs through warble
#   make -C tests check-compilations
#                                the parsers' compilation marks, through
#                                warble
#   make -C tests check-wheel    the click wheel's policy, and its drivers
#                                against 1be9ae95b6's (wheel/ref)
#   make -C tests check-disk     the "disk free, don't wake it" predicate
#   make -C tests check-lines    the playlists' line reader against the loops
#                                it replaced
#   make -C tests check-panic    the crash record, its copies and their vote
#   make -C tests check-hang     the hang catcher's detectors, phases and holds
#   make -C tests check-fw       file_write.c with each operation failing or
#                                cut
#   make -C tests check-orders   the Songs orders' sort from a hint, and its
#                                proof
#   make -C tests check-bar      a bar's slider's place and the skins'
#                                percentages, against the old 32-bit maths
#   make -C tests clean
#
# Built under tests/build. The sources are built with the simulator's
# configuration of the iPod Video (include/autoconf.h), as the simulator
# builds them, but with buflib's malloc backend, so that ASan sees each
# allocation on its own. include/autoconf.h is configure's output, kept: made
# again when configure's template of it changes.
#
# Needs Xcode's Command Line Tools; check-codecs also SDL2's headers and
# sdl2-config (Homebrew's sdl2), which configure asks for and warble.c
# includes. The library itself isn't linked (lib/sdl_stub.c).

ROOT    := $(abspath $(CURDIR)/..)
TESTS   := $(CURDIR)
BUILD   := $(TESTS)/build
OBJ     := $(BUILD)/obj

HOST_CC ?= clang
PYTHON  ?= python3

# Every error ends the test, but for those ubsan.supp lists: an alignment
# error is reported by a handler that can go on, which reads the list
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=all \
            -fsanitize-recover=alignment -fno-omit-frame-pointer
export UBSAN_OPTIONS := suppressions=$(TESTS)/ubsan.supp:halt_on_error=1:print_stacktrace=1
export ASAN_OPTIONS := suppressions=$(TESTS)/asan.supp

SIM_INC := -I$(TESTS)/include -I$(TESTS)/lib \
	-I$(ROOT)/firmware/target/arm/ipod/video \
	-I$(ROOT)/firmware/target/hosted/sdl -I$(ROOT)/firmware/target/hosted \
	-I$(ROOT)/firmware/target/arm/ipod -I$(ROOT)/firmware/target/arm/pp \
	-I$(ROOT)/firmware/target/arm -I$(ROOT)/firmware \
	-I$(ROOT)/firmware/export -I$(ROOT)/firmware/drivers \
	-I$(ROOT)/firmware/include -I$(ROOT)/firmware/kernel/include \
	-I$(ROOT)/apps -I$(ROOT)/apps/gui -I$(ROOT)/apps/recorder \
	-I$(ROOT)/apps/radio -I$(ROOT)/lib/rbcodec -I$(ROOT)/lib/rbcodec/codecs \
	-I$(ROOT)/lib/rbcodec/dsp -I$(ROOT)/lib/rbcodec/metadata \
	-I$(ROOT)/lib/fixedpoint -I$(ROOT)/lib/skin_parser \
	-I$(ROOT)/lib/tlsf/src -I$(ROOT)/uisimulator/common
SIM_DEF := -DROCKBOX -DMEMORYSIZE=64 -DIPOD_VIDEO -DTARGET_ID=15 \
	-DTARGET_NAME=\"ipodvideo\" -DSIMULATOR -DHAVE_UTF8_NORMALIZE \
	-DCONFIG_BUFLIB_BACKEND=1 -D_FORTIFY_SOURCE=0

CFLAGS := -std=gnu11 -g -O1 $(SANITIZE) $(SIM_INC) $(SIM_DEF) \
	-Wno-pointer-sign -Wno-unknown-warning-option
LDFLAGS := $(SANITIZE)

# the firmware's sources the tests link, as they are
FIRMWARE_SRC := firmware/common/unicode.c firmware/common/pathfuncs.c \
	firmware/common/crc32.c firmware/common/strmemccpy.c \
	firmware/core_alloc.c firmware/buflib_malloc.c \
	lib/fixedpoint/fixedpoint.c apps/sortkey.c
FIRMWARE_OBJ := $(addprefix $(OBJ)/,$(FIRMWARE_SRC:.c=.o))
# the tests' own: the checks, and the firmware the sources call, with
# file_write.c as it is
LIB_OBJ := $(OBJ)/lib/check.o $(OBJ)/lib/stubs.o $(OBJ)/lib/simfs.o \
	$(OBJ)/apps/file_write.o

.PHONY: all check check-keys check-itunesdb check-covers check-codecs \
	check-wheel check-disk check-lines check-panic check-hang check-fw \
	check-orders check-bar check-iap check-compilations clean
all: $(BUILD)/test_keys $(BUILD)/test_itunesdb $(BUILD)/test_covers \
	$(BUILD)/test_wheel $(BUILD)/test_panic_record $(BUILD)/test_hang \
	$(BUILD)/test_fw $(BUILD)/test_orders $(BUILD)/test_skin_ratio \
	$(BUILD)/test_iap

check: check-keys check-itunesdb check-covers check-wheel check-codecs \
	check-compilations check-disk check-lines check-panic check-hang \
	check-fw check-orders check-bar check-iap

$(OBJ)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -MMD -c $< -o $@

$(OBJ)/lib/%.o: $(TESTS)/lib/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -MMD -c $< -o $@

$(OBJ)/keys/%.o: $(TESTS)/keys/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -MMD -c $< -o $@

$(OBJ)/covers/%.o: $(TESTS)/covers/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -MMD -c $< -o $@

$(OBJ)/panic/%.o: $(TESTS)/panic/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -MMD -c $< -o $@

$(OBJ)/itunesdb/%.o: $(TESTS)/itunesdb/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -MMD -c $< -o $@

# The keys: sortkey.c, album_years.c's album_date_order() and itunesdb.c's
# itunesdb_album_key()
$(BUILD)/test_keys: $(OBJ)/keys/test_keys.o $(OBJ)/apps/album_years.o \
		$(OBJ)/apps/itunesdb.o $(OBJ)/apps/covers_table.o $(FIRMWARE_OBJ) \
		$(LIB_OBJ)
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

check-keys: $(BUILD)/test_keys
	@$(BUILD)/test_keys

# iTunesDB and ArtworkDB: test_itunesdb.c builds itunesdb.c itself, to reach
# its parser's static functions
$(BUILD)/test_itunesdb: $(OBJ)/itunesdb/test_itunesdb.o \
		$(OBJ)/apps/covers_table.o $(FIRMWARE_OBJ) $(LIB_OBJ)
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

# The covers' parts that need no disk: covers_table.c
$(BUILD)/test_covers: $(OBJ)/covers/test_covers.o $(OBJ)/apps/covers_table.o \
		$(OBJ)/lib/check.o
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

check-covers: $(BUILD)/test_covers
	@$(BUILD)/test_covers

# The crash record: panic_record.c, which the panic and the boot's reader use
$(BUILD)/test_panic_record: $(OBJ)/panic/test_panic_record.o \
		$(OBJ)/firmware/common/panic_record.o \
		$(OBJ)/firmware/common/crc32.o $(OBJ)/lib/check.o
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

check-panic: $(BUILD)/test_panic_record
	@$(BUILD)/test_panic_record

# The hang catcher: firmware/hang.c, built by test_hang.c itself with the
# catcher on, the scheduler and the record stubbed
$(OBJ)/hang/%.o: $(TESTS)/hang/%.c $(ROOT)/firmware/hang.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -Wall -MMD -c $< -o $@

$(BUILD)/test_hang: $(OBJ)/hang/test_hang.o $(OBJ)/lib/check.o
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

check-hang: $(BUILD)/test_hang
	@$(BUILD)/test_hang

# skin_ratio.h: a bar's slider's place and the skins' percentages, against
# the expressions they replaced, in 32 bits as the iPod worked them out
$(OBJ)/bar/%.o: $(TESTS)/bar/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -I$(ROOT)/apps/gui/skin_engine -Wall -MMD -c $< -o $@

$(BUILD)/test_skin_ratio: $(OBJ)/bar/test_skin_ratio.o $(OBJ)/lib/check.o
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

check-bar: $(BUILD)/test_skin_ratio
	@$(BUILD)/test_skin_ratio

# The Songs orders' sort from the order made before, and its proof:
# orders/make_src.py takes the parts out of apps/tagtree.c as they are
$(BUILD)/orders/orders_src.c: $(ROOT)/apps/tagtree.c $(TESTS)/orders/make_src.py
	@mkdir -p $(dir $@)
	@$(PYTHON) -I -B $(TESTS)/orders/make_src.py $@

$(OBJ)/orders/test_orders.o: $(TESTS)/orders/test_orders.c \
		$(BUILD)/orders/orders_src.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -DORDERS_SRC=\"$(BUILD)/orders/orders_src.c\" \
		-MMD -c $< -o $@

$(BUILD)/test_orders: $(OBJ)/orders/test_orders.o $(FIRMWARE_OBJ) $(LIB_OBJ)
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

check-orders: $(BUILD)/test_orders
	@$(BUILD)/test_orders

# made again for each run: an import leaves its files in the case's folder
ITUNESDB_CASES := $(BUILD)/itunesdb-cases
check-itunesdb: $(BUILD)/test_itunesdb
	@rm -rf $(ITUNESDB_CASES)
	@$(PYTHON) -I -B $(TESTS)/itunesdb/make_itunesdb.py $(ITUNESDB_CASES)
	@$(BUILD)/test_itunesdb $(ITUNESDB_CASES)

# The click wheel: wheel_policy.c as the simulator builds it; and the driver,
# 1be9ae95b6's (wheel/ref, verbatim) and today's, each built against
# wheel/stub's headers, its register at 0x7000c140 a stub's variable. The
# base's lines of button.c, list.c and wps.c are wheel/ref/apps.c, made by
# wheel/make_ref.py. Today's driver is built twice: new-driver, in step with
# the base's, with the base's stage time of 0.3 s; and real-driver, as it is,
# its names apart (wheel/real_names.h). Each copy gets an accessor of its
# wheel_config.
WHEEL     := $(TESTS)/wheel
WHEEL_OBJ := $(OBJ)/wheel
WHEEL_DRV_CFLAGS := -std=gnu11 -g -O1 $(SANITIZE) -I$(WHEEL)/stub \
	-I$(ROOT)/firmware/export -I$(ROOT)/firmware/target/arm/ipod \
	-Wall -Wno-unused-function -Wno-unknown-warning-option
WHEEL_DATA_REG := (\*(volatile unsigned long\*)(0x7000c140))

define wheel_driver_copy
	@mkdir -p $(dir $@)
	@sed -e 's/$(WHEEL_DATA_REG)/(stub_env.data)/' $< > $@.tmp
	@test "$$(grep -c 'stub_env.data' $@.tmp)" = 1 \
		|| { echo "$<: CLICKWHEEL_DATA not found once"; exit 1; }
endef
define wheel_config_accessor
	@printf '%s\n' 'const struct wheel_config *test_wheel_config(void)' \
		'{ return &wheel_config; }' >> $@.tmp
endef

$(WHEEL_OBJ)/ref-driver.c: $(WHEEL)/ref/button-clickwheel.c
	$(wheel_driver_copy)
	@mv $@.tmp $@

$(WHEEL_OBJ)/new-driver.c: $(ROOT)/firmware/target/arm/ipod/button-clickwheel.c
	$(wheel_driver_copy)
	@sed -e 's/^#define WHEEL_STAGE_USEC [0-9]*$$/#define WHEEL_STAGE_USEC 300000/' \
		$@.tmp > $@.tmp2
	@mv $@.tmp2 $@.tmp
	@test "$$(grep -c '^#define WHEEL_STAGE_USEC 300000$$' $@.tmp)" = 1 \
		|| { echo "$<: WHEEL_STAGE_USEC not found once"; exit 1; }
	$(wheel_config_accessor)
	@mv $@.tmp $@

$(WHEEL_OBJ)/real-driver.c: $(ROOT)/firmware/target/arm/ipod/button-clickwheel.c
	$(wheel_driver_copy)
	$(wheel_config_accessor)
	@mv $@.tmp $@

# its (1<<31) shifts into the sign bit, which GCC defines as the firmware
# takes it, 0x80000000
$(WHEEL_OBJ)/ref-driver.o: $(WHEEL_OBJ)/ref-driver.c $(WHEEL)/ref/names.h
	@echo CC tests/wheel/ref/button-clickwheel.c
	@$(HOST_CC) $(WHEEL_DRV_CFLAGS) -fno-sanitize=shift-base \
		-include $(WHEEL)/ref/names.h -MMD -c $< -o $@

$(WHEEL_OBJ)/new-driver.o: $(WHEEL_OBJ)/new-driver.c
	@echo CC firmware/target/arm/ipod/button-clickwheel.c, at 0.3 s
	@$(HOST_CC) $(WHEEL_DRV_CFLAGS) -MMD -c $< -o $@

$(WHEEL_OBJ)/real-driver.o: $(WHEEL_OBJ)/real-driver.c $(WHEEL)/real_names.h
	@echo CC firmware/target/arm/ipod/button-clickwheel.c
	@$(HOST_CC) $(WHEEL_DRV_CFLAGS) -include $(WHEEL)/real_names.h -MMD \
		-c $< -o $@

$(WHEEL_OBJ)/%.o: $(WHEEL)/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -I$(WHEEL) -MMD -c $< -o $@

$(BUILD)/test_wheel: $(WHEEL_OBJ)/test_wheel.o $(WHEEL_OBJ)/ref/apps.o \
		$(WHEEL_OBJ)/ref-driver.o $(WHEEL_OBJ)/new-driver.o \
		$(WHEEL_OBJ)/real-driver.o \
		$(OBJ)/firmware/drivers/wheel_policy.o $(OBJ)/lib/check.o
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

check-wheel: $(BUILD)/test_wheel
	@$(BUILD)/test_wheel

# misc.c's disk_free_for_ui() against the copies it replaced
# (disk/ref/disk_ref.c, made by disk/make_ref.py from git), built for each
# configuration they tell apart: the functions alone, without config.h
DISK_CFLAGS := -std=gnu11 -g -O1 $(SANITIZE) -I$(TESTS)/disk/ref -I$(BUILD)/disk
DISK_CONFIGS := ipod sim flash notata
DISK_DEF_ipod   := -DHAVE_DISK_STORAGE
DISK_DEF_sim    := -DHAVE_DISK_STORAGE -DSIMULATOR
DISK_DEF_flash  := -DCONFIG_STORAGE=STORAGE_SD
DISK_DEF_notata := -DHAVE_DISK_STORAGE -DCONFIG_STORAGE=STORAGE_SD -DDISK_NOT_ATA

$(BUILD)/disk/disk_new.c: $(ROOT)/apps/misc.c $(TESTS)/disk/make_ref.py
	@mkdir -p $(dir $@)
	@$(PYTHON) -I -B $(TESTS)/disk/make_ref.py --new $@

$(BUILD)/disk/test_disk_%: $(TESTS)/disk/test_disk.c $(BUILD)/disk/disk_new.c \
		$(TESTS)/disk/ref/disk_ref.c
	@echo CC $(notdir $@)
	@$(HOST_CC) $(DISK_CFLAGS) $(DISK_DEF_$*) -DCONFIG_NAME=\"$*\" $< -o $@

check-disk: $(foreach c,$(DISK_CONFIGS),$(BUILD)/disk/test_disk_$(c))
	@$(PYTHON) -I -B $(TESTS)/disk/make_ref.py --check > /dev/null
	@$(PYTHON) -I -B $(TESTS)/disk/make_ref.py --targets
	@$(foreach c,$(DISK_CONFIGS),$(BUILD)/disk/test_disk_$(c) &&) true

# library_playlists.c's and on_the_go.c's readers of a playlist's lines:
# lines/test_lines.c built with BASE's files (lines/ref/, made by
# lines/make_ref.py from git) and with the tree's, which read through
# line_reader.c; each runs the same cases. The traces of the runs without a
# failure are compared between the two; that of the runs with one, where a
# failed read now stops the readers, is the tree's against
# lines/fault-baseline.txt (make lines-baseline makes it again, for a change
# meant). The tree's build also fails if a failure left a playlist shorter,
# gone or rewritten. simfs.c's open, read and write go through the test's,
# which cut reads short and fail them. lines/unused.c stands in for the
# firmware the two files call from what the test doesn't run.
LINES_DEF_ref := -DLP_SOURCE=\"$(TESTS)/lines/ref/library_playlists.c\" \
	-DOTG_SOURCE=\"$(TESTS)/lines/ref/on_the_go.c\" -DLINES_NAME=\"ref\"
LINES_DEF_new := -DLP_SOURCE=\"$(ROOT)/apps/library_playlists.c\" \
	-DOTG_SOURCE=\"$(ROOT)/apps/on_the_go.c\" -DLINES_NAME=\"new\" \
	-DLINES_STRICT
LINES_OBJ := $(OBJ)/lines/host.o $(OBJ)/lines/simfs.o $(OBJ)/lines/unused.o \
	$(OBJ)/apps/file_write.o $(FIRMWARE_OBJ)
LINES_CFLAGS := -I$(TESTS)/lines $(CFLAGS) -Wno-format-security \
	-Wno-string-plus-int

$(OBJ)/lines/test_lines_ref.o $(OBJ)/lines/test_lines_new.o: \
		$(OBJ)/lines/test_lines_%.o: $(TESTS)/lines/test_lines.c
	@mkdir -p $(dir $@)
	@echo CC $(notdir $@)
	@$(HOST_CC) $(LINES_CFLAGS) $(LINES_DEF_$*) -MMD -c $< -o $@

$(OBJ)/lines/simfs.o: $(TESTS)/lib/simfs.c
	@mkdir -p $(dir $@)
	@echo CC lines/simfs.o
	@$(HOST_CC) $(CFLAGS) -Dsim_open=simfs_sim_open \
		-Dsim_read=simfs_sim_read -Dsim_write=simfs_sim_write -MMD -c $< -o $@

$(OBJ)/lines/host.o $(OBJ)/lines/unused.o: $(OBJ)/lines/%.o: $(TESTS)/lines/%.c
	@mkdir -p $(dir $@)
	@echo CC lines/$*.o
	@$(HOST_CC) -std=gnu11 -g -O1 $(SANITIZE) -MMD -c $< -o $@

$(BUILD)/test_lines_ref: $(OBJ)/lines/test_lines_ref.o $(LINES_OBJ)
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_lines_new: $(OBJ)/lines/test_lines_new.o \
		$(OBJ)/apps/line_reader.o $(LINES_OBJ)
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

# each run's disk is its own folder, which the test empties as it ends
LINES_RUN := $(BUILD)/lines
check-lines: $(BUILD)/test_lines_ref $(BUILD)/test_lines_new
	@$(PYTHON) -I -B $(TESTS)/lines/make_ref.py --check > /dev/null
	@mkdir -p $(LINES_RUN)
	@$(BUILD)/test_lines_ref $(LINES_RUN)/disk-ref \
		$(LINES_RUN)/ref-faults.txt > $(LINES_RUN)/ref.txt
	@$(BUILD)/test_lines_new $(LINES_RUN)/disk-new \
		$(LINES_RUN)/new-faults.txt > $(LINES_RUN)/new.txt
	@cmp $(LINES_RUN)/ref.txt $(LINES_RUN)/new.txt
	@echo "lines: without a failure, $$(grep -c '^case' $(LINES_RUN)/ref.txt)" \
		"playlists, $$(grep -c '^ [a-z]' $(LINES_RUN)/ref.txt) runs," \
		"$$(wc -c < $(LINES_RUN)/ref.txt | tr -d ' ') bytes of trace, the same"
	@$(PYTHON) -I -B $(TESTS)/lines/baseline.py --check \
		$(LINES_RUN)/new-faults.txt $(TESTS)/lines/fault-baseline.txt

.PHONY: lines-baseline
lines-baseline: $(BUILD)/test_lines_new
	@mkdir -p $(LINES_RUN)
	@$(BUILD)/test_lines_new $(LINES_RUN)/disk-new \
		$(LINES_RUN)/new-faults.txt > $(LINES_RUN)/new.txt
	@$(PYTHON) -I -B $(TESTS)/lines/baseline.py --write \
		$(LINES_RUN)/new-faults.txt $(TESTS)/lines/fault-baseline.txt

# file_write.c: tests/fw/test_fw.c's own open, write, close, lseek, rename
# and remove over simfs.c's, which fail each operation in turn or cut the
# power before or inside it
FW_SIMFS_DEF := -Dsim_open=simfs_sim_open -Dsim_write=simfs_sim_write \
	-Dsim_close=simfs_sim_close -Dsim_rename=simfs_sim_rename \
	-Dsim_remove=simfs_sim_remove -Dsim_lseek=simfs_sim_lseek

$(OBJ)/fw/simfs.o: $(TESTS)/lib/simfs.c
	@mkdir -p $(dir $@)
	@echo CC fw/simfs.o
	@$(HOST_CC) $(CFLAGS) $(FW_SIMFS_DEF) -MMD -c $< -o $@

$(OBJ)/fw/%.o: $(TESTS)/fw/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(CFLAGS) -Wall -MMD -c $< -o $@

$(BUILD)/test_fw: $(OBJ)/fw/test_fw.o $(OBJ)/fw/simfs.o \
		$(OBJ)/apps/file_write.o $(OBJ)/lib/stubs.o $(OBJ)/lib/check.o \
		$(FIRMWARE_OBJ)
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

FW_RUN := $(BUILD)/fw
check-fw: $(BUILD)/test_fw
	@rm -rf $(FW_RUN)
	@$(BUILD)/test_fw $(FW_RUN)

# warble, from its own configure in build/warble, with clang and ASan; only
# the codecs the fixtures need, and without SDL (lib/sdl_stub.c). DEBUG makes
# its codecs copied, not stripped by an objcopy macOS doesn't have.
WARBLE_DIR    := $(BUILD)/warble
WARBLE        := $(WARBLE_DIR)/warble.ipodvideo
WARBLE_CODECS := mpa aac alac flac vorbis opus wavpack
WARBLE_SAN    := -fsanitize=address -fno-omit-frame-pointer
WARBLE_MAKE   := $(MAKE) -C $(WARBLE_DIR) CC=$(HOST_CC) \
	CPP="$(HOST_CC) -E" AR=ar RANLIB=ranlib DEBUG=-DDEBUG \
	EXTRA_DEFINES="-DDEBUG -DWARBLE $(WARBLE_SAN)" \
	LDOPTS="-lm -L$(OBJ)/warble -lsdlstub $(WARBLE_SAN)" \
	CODECS="$(foreach c,$(WARBLE_CODECS),$(WARBLE_DIR)/lib/rbcodec/codecs/$(c).codec)"

$(WARBLE_DIR)/Makefile:
	@echo CONFIGURE warble
	@mkdir -p $(WARBLE_DIR)
	@cd $(WARBLE_DIR) && $(ROOT)/tools/configure --target=ipodvideo --type=w \
		> configure.log 2>&1

$(OBJ)/warble/libsdlstub.a: $(TESTS)/lib/sdl_stub.c
	@mkdir -p $(dir $@)
	@$(HOST_CC) -g $(WARBLE_SAN) -c $< -o $(OBJ)/warble/sdl_stub.o
	@ar rcs $@ $(OBJ)/warble/sdl_stub.o

.PHONY: warble
warble: $(WARBLE_DIR)/Makefile $(OBJ)/warble/libsdlstub.a
	@echo MAKE warble
	@$(WARBLE_MAKE) > $(WARBLE_DIR)/make.log 2>&1 \
		|| { tail -40 $(WARBLE_DIR)/make.log; exit 1; }

check-codecs: warble
	@$(PYTHON) -I -B $(TESTS)/codecs/check_codecs.py $(WARBLE) \
		$(TESTS)/codecs/fixtures $(TESTS)/codecs/expected.txt

# the parsers' compilation mark, through warble, in the codecs' fixtures
# tagged again (metadata/make_compilations.py)
COMPILATION_CASES := $(BUILD)/compilation-cases
check-compilations: warble
	@rm -rf $(COMPILATION_CASES)
	@$(PYTHON) -I -B $(TESTS)/metadata/make_compilations.py \
		$(TESTS)/codecs/fixtures $(COMPILATION_CASES)
	@$(PYTHON) -I -B $(TESTS)/metadata/check_compilations.py $(WARBLE) \
		$(COMPILATION_CASES)

# libiap protocol state machine, IDPS, DB browsing, and playback/skip controls
IAP_LIB_DIR := $(ROOT)/firmware/usbstack/iap/libiap
IAP_LIB_SRC := debug.c fid-token-values.c hid.c iap.c notification.c span.c
IAP_LIB_OBJ := $(addprefix $(OBJ)/iap/lib/,$(IAP_LIB_SRC:.c=.o))
IAP_CFLAGS  := -std=gnu11 -g -O1 $(SANITIZE) -I$(TESTS)/iap/host_inc \
	-I$(ROOT)/firmware/usbstack/iap -Wall -Wextra

$(OBJ)/iap/lib/%.o: $(IAP_LIB_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(IAP_CFLAGS) -MMD -c $< -o $@

$(OBJ)/iap/test_iap.o: $(TESTS)/iap/test_iap.c
	@mkdir -p $(dir $@)
	@echo CC $(subst $(ROOT)/,,$<)
	@$(HOST_CC) $(IAP_CFLAGS) -I$(TESTS)/lib -MMD -c $< -o $@

$(BUILD)/test_iap: $(OBJ)/iap/test_iap.o $(IAP_LIB_OBJ) $(OBJ)/lib/check.o
	@echo LD $(notdir $@)
	@$(HOST_CC) $(LDFLAGS) -o $@ $^

check-iap: $(BUILD)/test_iap
	@$(BUILD)/test_iap

clean:
	rm -rf $(BUILD)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
