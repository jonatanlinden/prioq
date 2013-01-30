/** 
 * Example test case for a priority queue.
 *
 * Author: Jonatan Linden <jonatan.linden@it.uu.se>
 *
 * Time-stamp: <2013-01-30 15:09:26 jonatanlinden>
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/time.h>
#include <assert.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <limits.h>
#include <float.h>

#include "j_util.h"
#include "prioq_simple.h"
#include "test.h"

/* the priority queue */
sq_t *sq;

void *run (void *_args);

__thread thread_args_t thread_args;
thread_args_t *t_args;

#define NOW() nsec_now()

/* global timestamp deciding when all threads should stop */
uint64_t end;


/* mapping of threads to cores */
int *cpu_map;

int
main (int argc, char **argv) 
{
    int opt;
    extern char *optarg;
    extern int optind, optopt;
    int nthreads = 1;
    int mcycles = 10;
    
    while ((opt = getopt(argc, argv, "t:n:")) != -1) {
	switch (opt) {
	case 'n': if ((nthreads = atoi(optarg)) < 1) {
		fprintf(stderr, 
			"%s: Option 'n' takes a positive integer\n",
			argv[0]);
		exit(EXIT_FAILURE);
	    }
	    break;
	case 't': mcycles = atoi(optarg); break;
	case ':': fprintf(stderr, "%s: Option '%c' needs a value\n",
			  argv[0], optopt);
	    break;
	default: 
	    fprintf(stderr, "Usage: %s -t nmillioncycles -n nthreads\n",
		    argv[0]);
	    exit(EXIT_FAILURE);
	}
    }
    
    /* INIT */
    end = mcycles * 1000000L + NOW();

    E_0(t_args = (thread_args_t *) malloc(sizeof(thread_args_t) * nthreads));
    E_0(cpu_map = (int*) malloc (sizeof(int)*nthreads));

    // define how the threads should be pinned to cores
    for (int i = 0; i < nthreads; i++) 
	cpu_map[i] = i;

    // initialize the queue.
    //E_0(sq = sq_init(3, 0.0, DBL_MAX, nthreads));
    E_0(sq = sq_init(3, 0, INT_MAX, nthreads));
    

    for (int i = 1; i < 4096; i++)
	sq_add(sq, i, i, 0);

    for (int i = 0; i < nthreads; i++)
    {
	thread_args_t *t = &t_args[i];
	t->tid = i;
	t->measure = 0;

	E_0(t->rng = gsl_rng_alloc(gsl_rng_mt19937));
	gsl_rng_set(t->rng, time(NULL)+i);
    }

    /* RUN the workload */
    for (int i = 0; i < nthreads; i++)
	E_en(pthread_create(&(t_args[i].thread), NULL, run, &t_args[i]));

    /* JOIN them */
    for (int i = 0; i < nthreads; i++)
	E_en(pthread_join(t_args[i].thread, NULL));

    sq_node_t *oldnode;
    for (int i = 1; i < 4096; i++)
	sq_del(sq, i, 1, &oldnode, 0);
    
    sq_print(sq);
    
    printf("****** Stats *******\n");

    int sum = 0;
    for (int i = 0; i < nthreads; i++)
	sum += t_args[i].measure;

    printf("total thread measure: %d\n", sum);
    
    /* FREE */
    sq_destroy(sq);
    
    for (int i = 0; i < nthreads; i++)
	gsl_rng_free(t_args[i].rng);
    free (t_args);
    free(cpu_map);
}

static inline int
work(thread_args_t *ta)
{
    int old;

    int idx = gsl_rng_uniform (ta->rng);
    return sq_update(sq, idx, idx, &old, ta->tid);

    return 0;
}


void *
run (void *_args)
{
    // measure something
    int cnt = 0;

    //tls
    thread_args = *(thread_args_t *)_args;
    
    pin(gettid(), cpu_map[thread_args.tid]);
    
    do {
        /* BEGIN work */
	work(&thread_args);
        cnt++;
	/* END work */
    } while (NOW() < end);
    /* end of measured execution */
    thread_args.measure = cnt;
    dprintf("thread %d bailing out.\n", thread_args.tid);
    // write back the local data to global
    *(thread_args_t *)_args = thread_args;

    return NULL;
}






