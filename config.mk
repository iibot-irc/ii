# Customize to fit your system

# paths
PREFIX      = /usr/local
MANPREFIX   = ${PREFIX}/share/man

INCDIR      = ${PREFIX}/include
LIBDIR      = ${PREFIX}/lib
VERSION     = 1-rc2

# includes and libs
INCLUDES    = -I. -I${INCDIR} -I/usr/include
LIBS        = -L${LIBDIR} -L/usr/lib -lc

# compiler
CC          = cc
CFLAGS      = -g -O0 -W -Wall ${INCLUDES} -DVERSION=\"${VERSION}\"
LDFLAGS     = ${LIBS}
