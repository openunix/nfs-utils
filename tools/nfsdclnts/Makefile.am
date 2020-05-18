## Process this file with automake to produce Makefile.in
PYTHON_FILES = nfsdclnts.py

all-local: $(PYTHON_FILES)

install-data-hook:
	$(INSTALL) -m 755 nfsdclnts.py $(DESTDIR)$(sbindir)/nfsdclnts

MAINTAINERCLEANFILES=Makefile.in
