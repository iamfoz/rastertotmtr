CC      ?= clang
ARCHS   ?= -arch arm64 -arch x86_64
MINOS   ?= -mmacosx-version-min=11.0
CFLAGS  ?= -Wall -Wextra -O2 -Wno-deprecated-declarations
LDLIBS   = -lcups -lcupsimage

BUNDLE   = rastertotmtr.app
BIN      = $(BUNDLE)/Contents/MacOS/rastertotmtr
INSTALL_DIR = /Library/Printers/EPSON/tmprinter/filter

all: $(BIN)

rastertotmtr: rastertotmtr.c
	$(CC) $(CFLAGS) $(ARCHS) $(MINOS) -o $@ $< $(LDLIBS)
	codesign -s - -f --identifier com.epson.tmprinter.rastertotmtr $@

$(BIN): rastertotmtr Info.plist
	mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources
	cp Info.plist $(BUNDLE)/Contents/Info.plist
	printf 'APPL????' > $(BUNDLE)/Contents/PkgInfo
	cp rastertotmtr $(BIN)
	codesign -s - -f --deep $(BUNDLE)

install: $(BIN)
	./install.sh

clean:
	rm -rf rastertotmtr $(BUNDLE)

.PHONY: all install clean
