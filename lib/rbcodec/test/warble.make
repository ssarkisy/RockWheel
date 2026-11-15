#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

RBCODECLIB_DIR = $(ROOTDIR)/lib/rbcodec
RBCODEC_BLD = $(BUILDDIR)/lib/rbcodec

GCCOPTS += -D__PCTOOL__ $(TARGET) -DDEBUG -g -std=gnu99 \
	`$(SDLCONFIG) --cflags` -DCODECDIR="\"$(CODECDIR)\""
RBCODEC_CFLAGS += -D_FILE_H_ #-DLOGF_H -DDEBUG_H -D_KERNEL_H_ # will be removed later

SRC= $(call preprocess, $(ROOTDIR)/lib/rbcodec/test/SOURCES)

# macOS lacks glibc's <endian.h> and <byteswap.h>, and its fortified
# <string.h> makes strlcpy() a macro, which firmware/common/strlcpy.c defines
ifeq ($(UNAME),Darwin)
INCLUDES += -I$(ROOTDIR)/lib/rbcodec/test/darwin
GCCOPTS += -D_FORTIFY_SOURCE=0
# Apple clang has no libgcc
ifneq (,$(findstring clang,$(shell $(CC) --version 2>/dev/null)))
CODEC_LIBGCC :=
endif
endif

INCLUDES += -I$(ROOTDIR)/lib/rbcodec/test \
	-I$(ROOTDIR)/apps -I$(ROOTDIR)/apps/gui \
	-I$(FIRMDIR)/kernel/include \
    -I$(ROOTDIR)/firmware/export \
    -I$(ROOTDIR)/firmware/include \
	-I$(ROOTDIR)/firmware/target/hosted \
	-I$(ROOTDIR)/firmware/target/hosted/sdl

.SECONDEXPANSION: # $$(OBJ) is not populated until after this

$(BUILDDIR)/$(BINARY): $(CODECS)

$(BUILDDIR)/$(BINARY): $$(OBJ) $$(CORE_LIBS)
	@echo LD $(BINARY)
	$(SILENT)$(HOSTCC) $(LDOPTS) -o $@ $(OBJ) \
		-L$(BUILDDIR)/lib $(call a2lnk, $(CORE_LIBS)) \
		$(LDOPTS) $(GLOBAL_LDOPTS)
