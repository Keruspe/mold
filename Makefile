PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
MANDIR ?= $(PREFIX)/share/man

D = $(DESTDIR)

ifeq ($(origin CC), default)
  CC = clang
endif

ifeq ($(origin CXX), default)
  CXX = clang++
endif

OS ?= $(shell uname -s)

CPPFLAGS = -pthread -std=c++20 -fPIE -DMOLD_VERSION=\"0.9.6\" \
	   -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
	   -DLIBDIR="\"$(LIBDIR)\"" $(EXTRA_CPPFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)
LIBS = -pthread -lz -lxxhash -ldl -lm

SRCS=$(wildcard *.cc elf/*.cc macho/*.cc)
HEADERS=$(wildcard *.h elf/*.h macho/*.h)
OBJS=$(SRCS:%.cc=out/%.o)

DEBUG ?= 0
LTO ?= 0
ASAN ?= 0
TSAN ?= 0

GIT_HASH ?= $(shell [ -d .git ] && git rev-parse HEAD)
ifneq ($(GIT_HASH),)
  CPPFLAGS += -DGIT_HASH=\"$(GIT_HASH)\"
endif

ifeq ($(DEBUG), 1)
  CPPFLAGS += -O0 -g
else
  CPPFLAGS += -O2
endif

ifeq ($(LTO), 1)
  CPPFLAGS += -flto -O3
  LDFLAGS  += -flto
endif

ifeq ($(ASAN), 1)
  CPPFLAGS += -fsanitize=address
  LDFLAGS  += -fsanitize=address
else ifeq ($(TSAN), 1)
  CPPFLAGS += -fsanitize=thread
  LDFLAGS  += -fsanitize=thread
else ifneq ($(OS), Darwin)
  # By default, we want to use mimalloc as a memory allocator.
  # Since replacing the standard malloc is not compatible with ASAN,
  # we do that only when ASAN is not enabled.
  ifdef SYSTEM_MIMALLOC
    LIBS += -lmimalloc
  else
    MIMALLOC_LIB = out/mimalloc/libmimalloc.a
    CPPFLAGS += -Ithird-party/mimalloc/include
    LIBS += -Wl,-whole-archive $(MIMALLOC_LIB) -Wl,-no-whole-archive
  endif
endif

# Homebrew on macOS/ARM installs packages under /opt/homebrew
# instead of /usr/local
ifneq ($(wildcard /opt/homebrew/.),)
  CPPFLAGS += -I/opt/homebrew/include
  LIBS += -L/opt/homebrew/lib
endif

ifdef SYSTEM_TBB
  LIBS += -ltbb
else
  TBB_LIB = out/tbb/libs/libtbb.a
  LIBS += $(TBB_LIB)
  CPPFLAGS += -Ithird-party/tbb/include
endif

ifneq ($(OS), Darwin)
  LIBS += -lcrypto
endif

all: mold mold-wrapper.so

mold: $(OBJS) $(MIMALLOC_LIB) $(TBB_LIB)
	$(CXX) $(CPPFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	ln -sf mold ld
	ln -sf mold ld64.mold

mold-wrapper.so: elf/mold-wrapper.c Makefile
	$(CC) -fPIC -shared -o $@ $< -ldl

out/%.o: %.cc $(HEADERS) Makefile out/elf/.keep out/macho/.keep
	$(CXX) $(CPPFLAGS) -c -o $@ $<

out/elf/.keep:
	mkdir -p out/elf
	touch $@

out/macho/.keep:
	mkdir -p out/macho
	touch $@

$(MIMALLOC_LIB):
	mkdir -p out/mimalloc
	(cd out/mimalloc; CFLAGS=-DMI_USE_ENVIRON=0 cmake -G'Unix Makefiles' ../../third-party/mimalloc)
	$(MAKE) -C out/mimalloc mimalloc-static

$(TBB_LIB):
	mkdir -p out/tbb
	(cd out/tbb; cmake -G'Unix Makefiles' -DBUILD_SHARED_LIBS=OFF -DTBB_TEST=OFF -DCMAKE_CXX_FLAGS=-D__TBB_DYNAMIC_LOAD_ENABLED=0 -DTBB_STRICT=OFF ../../third-party/tbb)
	$(MAKE) -C out/tbb tbb
	(cd out/tbb; ln -sf *_relwithdebinfo libs)

ifeq ($(OS), Darwin)
test tests check: all
	$(MAKE) -C test -f Makefile.darwin --no-print-directory
else
test tests check: all
	$(MAKE) -C test -f Makefile.linux --no-print-directory --output-sync
endif

install: all
	install -m 755 -d $D$(BINDIR)
	install -m 755 mold $D$(BINDIR)
	strip $D$(BINDIR)/mold

	install -m 755 -d $D$(LIBDIR)/mold
	install -m 644 mold-wrapper.so $D$(LIBDIR)/mold
	strip $D$(LIBDIR)/mold/mold-wrapper.so

	install -m 755 -d $D$(MANDIR)/man1
	install -m 644 docs/mold.1 $D$(MANDIR)/man1
	rm -f $D$(MANDIR)/man1/mold.1.gz
	gzip -9 $D$(MANDIR)/man1/mold.1

	ln -sf mold $D$(BINDIR)/ld.mold
	ln -sf mold $D$(BINDIR)/ld64.mold

uninstall:
	rm -f $D$(BINDIR)/mold $D$(BINDIR)/ld.mold $D$(BINDIR)/ld64.mold
	rm -f $D$(MANDIR)/man1/mold.1.gz
	rm -rf $D$(LIBDIR)/mold

clean:
	rm -rf *~ mold mold-wrapper.so out ld ld64.mold

.PHONY: all test tests check clean
