.PHONY: all libsql lsmove compact clean

all: libsql lsmove compact

libSQL/Makefile:
	cd libSQL && ./configure

libsql: libSQL/Makefile
	$(MAKE) -C libSQL

lsmove:
	$(MAKE) -C LSMoVe

compact: lsmove
	$(CC) -O2 LSMoVe/compact_db.c -ILSMoVe -ILSMoVe/src -LLSMoVe \
		-lsqlite4 -lpthread -lm -lz -llz4 -o LSMoVe/compact_db

clean:
	@if [ -f libSQL/Makefile ]; then \
		$(MAKE) -C libSQL clean; \
	fi
	$(MAKE) -C LSMoVe clean
	$(RM) LSMoVe/compact_db
