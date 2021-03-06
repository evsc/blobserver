AM_CPPFLAGS = \
	-DDATADIR=\"$(datadir)/blobserver/\"

pluginsdir = $(prefix)/lib/blobserver-@LIBBLOBSERVER_API_VERSION@

plugins_LTLIBRARIES = \
	libactuatorbgsubtractor.la \
	libactuatordepthtouch.la \
	libactuatorface.la \
	libactuatorfiducialtracker.la \
	libactuatorhog.la \
	libactuatorlightspots.la \
	libactuatormeanoutliers.la \
    libactuatormirrorball.la \
	libactuatornop.la \
	libactuatorobjonaplane.la \
	libactuatorstitch.la

if GLFW3
plugins_LTLIBRARIES += libactuatorglsl.la
endif

if PYTHON
plugins_LTLIBRARIES += libactuatorpython.la
endif

if PCL
plugins_LTLIBRARIES += \
    libactuatorarmpcl.la \
	libactuatorclusterpcl.la
endif

DEFAULT_CFLAGS = \
    $(GLIB_CFLAGS) \
    $(OPENCV_CFLAGS) \
    $(LIBATOM_CFLAGS)

#-------------#
if PCL
libactuatorarmpcl_la_SOURCES = armpcl.cpp
libactuatorarmpcl_la_CXXFLAGS = \
    $(DEFAULT_CFLAGS) \
	$(PCL_CFLAGS)
libactuatorarmpcl_la_LIBADD = $(PCL_LIBS)
endif

#-------------#
libactuatorbgsubtractor_la_SOURCES = bgsubtractor.cpp
libactuatorbgsubtractor_la_CXXFLAGS = $(DEFAULT_CFLAGS)

#-------------#
if PCL
libactuatorclusterpcl_la_SOURCES = clusterPcl.cpp
libactuatorclusterpcl_la_CXXFLAGS = \
    $(DEFAULT_CFLAGS) \
	$(PCL_CFLAGS)
libactuatorclusterpcl_la_LDFLAGS = $(PCL_LIBS)
endif

#-------------#
libactuatordepthtouch_la_SOURCES = depthtouch.cpp
libactuatordepthtouch_la_CXXFLAGS = $(DEFAULT_CFLAGS)

#-------------#
libactuatorface_la_SOURCES = face.cpp
libactuatorface_la_CXXFLAGS = $(DEFAULT_CFLAGS)
libactuatorface_la_LIBADD = -L/usr/local/lib -lopencv_objdetect

#-------------#
libactuatorfiducialtracker_la_SOURCES = fiducialtracker.cpp
libactuatorfiducialtracker_la_CXXFLAGS = \
	-I$(top_srcdir)/ext/libfidtrack \
    $(DEFAULT_CFLAGS)
# External libs, compiled along blobserver
libactuatorfiducialtracker_la_SOURCES += \
    $(top_srcdir)/ext/libfidtrack/dump_graph.c \
    $(top_srcdir)/ext/libfidtrack/fidtrack120.c \
    $(top_srcdir)/ext/libfidtrack/fidtrackX.c \
    $(top_srcdir)/ext/libfidtrack/segment.c \
    $(top_srcdir)/ext/libfidtrack/threshold.c \
    $(top_srcdir)/ext/libfidtrack/tiled_bernsen_threshold.c \
    $(top_srcdir)/ext/libfidtrack/topologysearch.c \
    $(top_srcdir)/ext/libfidtrack/treeidmap.cpp

#-------------#
if GLFW3
if GLM
libactuatorglsl_la_SOURCES = glsl.cpp
libactuatorglsl_la_CXXFLAGS = \
	$(DEFAULT_CFLAGS) \
	$(GLFW3_CFLAGS)
libactuatorglsl_la_LDFLAGS = $(GLFW3_LIBS)
endif
endif

#-------------#
libactuatorhog_la_SOURCES = \
	hog.cpp \
	$(top_srcdir)/src/descriptor_hog.cpp
libactuatorhog_la_CXXFLAGS = $(DEFAULT_CFLAGS)
libactuatorhog_la_LDFLAGS = -lopencv_ml

#-------------#
libactuatorlightspots_la_SOURCES = lightSpots.cpp
libactuatorlightspots_la_CXXFLAGS = $(DEFAULT_CFLAGS)
libactuatorlightspots_la_LDFLAGS = -lopencv_features2d

#-------------#
libactuatormeanoutliers_la_SOURCES = meanOutliers.cpp
libactuatormeanoutliers_la_CXXFLAGS = $(DEFAULT_CFLAGS)

#-------------#
libactuatormirrorball_la_SOURCES = mirrorball.cpp
libactuatormirrorball_la_CXXFLAGS = $(DEFAULT_CFLAGS)

#-------------#
libactuatornop_la_SOURCES = nop.cpp
libactuatornop_la_CXXFLAGS = $(DEFAULT_CFLAGS)

#-------------#
libactuatorobjonaplane_la_SOURCES = objOnAPlane.cpp
libactuatorobjonaplane_la_CXXFLAGS = $(DEFAULT_CFLAGS)

#-------------#
if PYTHON
libactuatorpython_la_SOURCES = python.cpp
libactuatorpython_la_CXXFLAGS = \
    $(DEFAULT_CFLAGS) \
    `python-config --cflags`
libactuatorpython_la_LDFLAGS = \
    `python-config --ldflags` \
    -Xlinker -export-dynamic
endif

#-------------#
libactuatorstitch_la_SOURCES = stitch.cpp
libactuatorstitch_la_CXXFLAGS = $(DEFAULT_CFLAGS)

