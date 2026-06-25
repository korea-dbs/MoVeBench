.PHONY: all libsql lsmobivec compact clean

all: libsql lsmobivec compact

LibSQL/Makefile:
	cd LibSQL && ./configure

libsql: LibSQL/Makefile
	$(MAKE) -C LibSQL

lsmobivec:
	$(MAKE) -C LSMoVe

compact: lsmobivec
	$(CC) -O2 LSMoVe/compact_db.c -ILSMoVe -ILSMoVe/src -LLSMoVe \
		-lsqlite4 -lpthread -lm -lz -llz4 -o LSMoVe/compact_db

clean:
	@if [ -f LibSQL/Makefile ]; then \
		$(MAKE) -C LibSQL clean; \
	fi
	$(MAKE) -C LSMoVe clean
	$(RM) LSMoVe/compact_db
