## Process this file with automake to produce Makefile.in

AUTOMAKE_OPTIONS = foreign

if HAVE_JACK

jackltlibs = libjack.la
jacksources = ao_jack.c ao_jack_resample.c

else

jackltlibs =
jacksources =

endif

AM_CPPFLAGS = -I$(top_builddir)/include/ao -I$(top_srcdir)/include

libdir = $(plugindir)
lib_LTLIBRARIES = $(jackltlibs)

libjack_la_CFLAGS = @JACK_CFLAGS@
libjack_la_LDFLAGS = @PLUGIN_LDFLAGS@ @JACK_LDFLAGS@
libjack_la_LIBADD = @JACK_LIBS@ ../../libao.la
libjack_la_SOURCES = $(jacksources)

EXTRA_DIST = ao_jack.c
