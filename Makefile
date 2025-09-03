include config.mk

SRC = rootclock.c drw.c util.c
OBJ = ${SRC:.c=.o}

all: rootclock

.c.o:
	${CC} -c ${CFLAGS} ${INCS} -o $@ $<

# Ensure config.h exists and is a dependency of all objects
${OBJ}: config.h config.mk

config.h:
	cp config.def.h $@

rootclock: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS} ${LIBS}

clean:
	rm -f rootclock ${OBJ}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f rootclock ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/rootclock

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/rootclock

.PHONY: all clean install uninstall
