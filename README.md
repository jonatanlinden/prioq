PQ
==

An implementation of a parallel skiplist based priority queue, with
locks.  Based on Herlihy et al., "A Simple Optimistic Skiplist
Algorithm".

Features:
* Bad scaling


### Build

    make all/debug

### Usage

Run the test application with 8 threads, during 100 million cycles.

    ./test -n 8 -t 100
    
### Build Dependencies

    gsl, glib
