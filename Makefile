.PHONY: all libsql lsmobivec compact clean

all: libsql lsmobivec compact

LibSQL/Makefile:
	cd LibSQL && ./configure

libsql: LibSQL/Makefile
	$(MAKE) -C LibSQL

lsmobivec:
	$(MAKE) -C LSMobiVec

compact: lsmobivec
	$(CC) -O2 LSMobiVec/compact_db.c -ILSMobiVec -ILSMobiVec/src -LLSMobiVec \
		-lsqlite4 -lpthread -lm -lz -llz4 -o LSMobiVec/compact_db

clean:
	@if [ -f LibSQL/Makefile ]; then \
		$(MAKE) -C LibSQL clean; \
	fi
	$(MAKE) -C LSMobiVec clean
	$(RM) LSMobiVec/compact_db
