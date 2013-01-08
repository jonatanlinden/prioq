CC          = gcc
LD	    = ld
CFLAGS_B    = -Wall -std=c99 -pedantic `pkg-config --cflags --libs glib-2.0 gsl` -DLEVEL1_DCACHE_LINESIZE=`getconf LEVEL1_DCACHE_LINESIZE`
CFLAGS      = $(CFLAGS_B) -DNDEBUG -O2
LDFLAGS	    = -pthread `pkg-config --libs glib-2.0 gsl`

PRIOQ_IMPLS = simple

EXEC = test 

debug: CC += -g -O0 -DDEBUG
debug: CFLAGS = $(CFLAGS_B)
debug: all

all:	$(EXEC)

test: $(PRIOQ_IMPLS:%=prioq_%.o) j_util.o hp.o

clean:
	$(RM) -f *.o $(EXEC)

.PHONY: all debug clean
