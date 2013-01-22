#ifndef PRIOQ_SIMPLE_H
#define PRIOQ_SIMPLE_H

#include <inttypes.h>
#include <gsl/gsl_rng.h>
#include "hp.h"

#define key_t double
#define val_t int

#define NULL_VAL 0


typedef struct sq_node_s {
  key_t key;
  val_t val;
  uint topLevel;
  uint64_t marked;
  int fullyLinked;
  pthread_spinlock_t lock;
    
    struct sq_node_s *nexts[1];
    
} sq_node_t;


typedef struct sq_ti_s {
    sq_node_t **preds;
    sq_node_t **succs;
} sq_ti_t;


typedef struct sq_s {
    int	maxLevel;
    int nthreads;
    
    sq_node_t *head;
    sq_node_t *tail;

    sq_ti_t *thread_ws;
    hp_rec_list_t *hp;
    gsl_rng **rng_state;
    
} sq_t;



extern sq_t *
sq_init(const uint maxLevel, const key_t min, const key_t max, const int nthreads);

extern int
sq_add(sq_t * q, const key_t key, const val_t val, const int tid);

extern int
sq_update(sq_t *q, key_t newkey, val_t newval, val_t *old, const int tid);

extern int
sq_delmin(sq_t *q, sq_node_t **node, const int tid);

extern void
sq_destroy(sq_t *q);


#endif //PRIOQ_SIMPLE_H
