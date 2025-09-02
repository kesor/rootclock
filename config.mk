# paths
PREFIX = /usr/local

# X11
X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

# compiler/linker
CC      = cc
CPPFLAGS=
CFLAGS  = -std=c99 -O2 -Wall -Wextra -Wpedantic $(CPPFLAGS) -D_DEFAULT_SOURCE
LDFLAGS =
INCS    = -I. -I/usr/include -I$(X11INC) -I/usr/include/freetype2
LIBS    = -L/usr/lib -L$(X11LIB) -lX11 -lXft -lXinerama -lfontconfig -lXrender -lfreetype
