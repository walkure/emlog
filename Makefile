# By default, the build is done against the running kernel version.
# to build against a different kernel version, set KVER
#
#  make KVER=2.6.11-alpha
#
#  Alternatively, set KDIR
#
#  make KDIR=/usr/src/linux

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
MDIR := emlog
MVER := 0.72

DKMS ?= dkms

CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -O2
BINDIR ?= $(DESTDIR)/usr/bin

# pkg-config for cross-compilation:
# derive multiarch triplet from CROSS_COMPILE (e.g. arm-linux-gnueabihf- -> arm-linux-gnueabihf)
CROSS_TRIPLET := $(patsubst %-,%,$(CROSS_COMPILE))
ifneq ($(CROSS_TRIPLET),)
  PKG_CONFIG_ENV := PKG_CONFIG_PATH=/usr/lib/$(CROSS_TRIPLET)/pkgconfig
else
  PKG_CONFIG_ENV :=
endif
PKG_CONFIG ?= pkg-config

# FUSE (optional, for emlog_fuse)
FUSE_CFLAGS := $(shell $(PKG_CONFIG_ENV) $(PKG_CONFIG) fuse --cflags 2>/dev/null)
FUSE_LIBS   := $(shell $(PKG_CONFIG_ENV) $(PKG_CONFIG) fuse --libs 2>/dev/null)
HAS_FUSE    := $(if $(FUSE_LIBS),yes,)

# libcap (optional, for emlog_fuse non-root support)
CAP_LIBS    := $(shell $(PKG_CONFIG_ENV) $(PKG_CONFIG) libcap --libs 2>/dev/null || echo "-lcap")
HAS_CAP     := $(shell $(PKG_CONFIG_ENV) $(PKG_CONFIG) libcap --exists 2>/dev/null && echo yes)

.PHONY: modules modules_install modules_clean nbcat_install nbcat_clean mkemlog_install mkemlog_clean emlog_stat_install emlog_stat_clean emlog_fuse_clean dkms_install dkms_remove

TARGETS := modules nbcat mkemlog emlog_stat
INSTALL_TARGETS := modules_install nbcat_install mkemlog_install emlog_stat_install
CLEAN_TARGETS := modules_clean nbcat_clean mkemlog_clean emlog_stat_clean
ifeq ($(HAS_FUSE),yes)
  TARGETS += emlog_fuse
  INSTALL_TARGETS += emlog_fuse_install
  CLEAN_TARGETS += emlog_fuse_clean
endif

all: $(TARGETS)

install: $(INSTALL_TARGETS)

clean: $(CLEAN_TARGETS)

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

modules_install: modules
	$(MAKE) INSTALL_MOD_PATH=$(DESTDIR) INSTALL_MOD_DIR=$(MDIR) \
		-C $(KDIR) M=$(CURDIR) modules_install

modules_clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

nbcat: nbcat.c
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

mkemlog: mkemlog.c
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

emlog_stat: emlog_stat.c
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

nbcat_install: nbcat
	install -m 0755 -d '$(BINDIR)'
	install -m 0755 -s -t '$(BINDIR)' nbcat

mkemlog_install: mkemlog
	install -m 0755 -d '$(BINDIR)'
	install -m 0755 -s -t '$(BINDIR)' mkemlog

emlog_stat_install: emlog_stat
	install -m 0755 -d '$(BINDIR)'
	install -m 0755 -s -t '$(BINDIR)' emlog_stat

emlog_stat_clean:
	rm -f emlog_stat

emlog_fuse: emlog_fuse.c emlog_ioctl.h
ifeq ($(HAS_CAP),yes)
	$(CC) -o $@ emlog_fuse.c $(CFLAGS) $(FUSE_CFLAGS) $(LDFLAGS) $(FUSE_LIBS) -lpthread $(CAP_LIBS)
else
	$(CC) -o $@ emlog_fuse.c $(CFLAGS) -DNO_LIBCAP $(FUSE_CFLAGS) $(LDFLAGS) $(FUSE_LIBS) -lpthread
endif

emlog_fuse_install: emlog_fuse
	install -m 0755 -d '$(BINDIR)'
	install -m 0755 -s -t '$(BINDIR)' emlog_fuse

emlog_fuse_clean:
	rm -f emlog_fuse

nbcat_clean:
	rm -f nbcat

mkemlog_clean:
	rm -f mkemlog

dkms_install:
	$(DKMS) add .
	$(DKMS) install emlog/$(MVER)

dkms_remove:
	$(DKMS) remove emlog/$(MVER) --all
	#rm -rf /usr/src/emlog-$(MVER)
