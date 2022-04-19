libexec_PROGRAMS = nfsrahead
nfsrahead_SOURCES = main.c

udev_rulesdir = /usr/lib/udev/rules.d/
udev_rules_DATA = 99-nfs.rules

99-nfs.rules: 99-nfs.rules.in $(builddefs)
	$(SED) "s|_libexecdir_|@libexecdir@|g" 99-nfs.rules.in > $@

clean-local:
	$(RM) 99-nfs.rules
