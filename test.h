#ifndef TEST_H
#define TEST_H

#include <gsl/gsl_rng.h>

typedef struct thread_args_s
{
    pthread_t thread;
    int tid;
    int core;
    gsl_rng *rng;
    int mu;
    unsigned long long measure;
} thread_args_t;


#endif // TEST_H
