.PHONY: all libsql lsmove compact clean

all: libsql lsmove compact

LibSQL/Makefile:
	cd LibSQL && ./configure

libsql: LibSQL/Makefile
	$(MAKE) -C LibSQL

lsmove:
	$(MAKE) -C LSMoVe

compact: lsmove
	$(CC) -O2 LSMoVe/compact_db.c -ILSMoVe -ILSMoVe/src -LLSMoVe \
		-lsqlite4 -lpthread -lm -lz -llz4 -o LSMoVe/compact_db

clean:
	@if [ -f LibSQL/Makefile ]; then \
		$(MAKE) -C LibSQL clean; \
	fi
	$(MAKE) -C LSMoVe clean
	$(RM) LSMoVe/compact_db
