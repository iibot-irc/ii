# ii - irc it - simple but flexible IRC client
#   (C)opyright MMV Anselm R. Garbe, Nico Golde

include config.mk

SRC      = ii.c
OBJ      = ${SRC:.c=.o}

all: options ii
	@echo built ii

options:
	@echo ii build options:
	@echo "LIBS     = ${LIBS}"
	@echo "INCLUDES = ${INCLUDES}"
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

dist: clean
	@mkdir -p ii-${VERSION}
	@cp -R Makefile README FAQ LICENSE config.mk ii.c ii.1 ii-${VERSION}
	@tar -cf ii-${VERSION}.tar ii-${VERSION}
	@gzip ii-${VERSION}.tar
	@rm -rf ii-${VERSION}
	@echo created distribution ii-${VERSION}.tar.gz

ii: ${OBJ}
	@echo LD $@
	@${CC} -o $@ ${OBJ} ${LDFLAGS}

install: all
	@cp -f ii ${DESTDIR}${PREFIX}/bin
	@chmod 775 ${DESTDIR}${PREFIX}/bin/ii
	@cp -f ii.1 ${DESTDIR}${MANPREFIX}/man1
	@chmod 444 ${DESTDIR}${MANPREFIX}/man1/ii.1
	@echo "installed ii"

uninstall: all
	rm -f ${DESTDIR}${MANPREFIX}/man1/ii.1
	rm -f ${DESTDIR}${PREFIX}/bin/ii

clean:
	rm -f ii *~ *.o *core
