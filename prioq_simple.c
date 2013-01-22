#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <pthread.h>
#include <math.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <limits.h>

#include "hp.h"
#include "j_util.h"
#include "prioq_simple.h"


/* An implementation of a skiplist based priority queue, with locks.
 * Based on Herlihy et al. "A Simple Optimistic Skiplist Algorithm". 
 */


/* If defined, hazard pointers will be used to reclaim memory of
 * nodes. If not, no memory reclamation of nodes will be done.
 */
#define HP

/* the lock functions. */
#define lock(l) pthread_spin_lock((l));
#define unlock(l) pthread_spin_unlock((l));




/* Record pointer peek read.
 * After having added it as a hazard pointer, make sure
 * the pointer is still valid, otherwise return NULL.
 */
static inline void *
pptr(const sq_t *q, sq_node_t **node, int tid)
{
    sq_node_t *tmp = *node;
    q->hp->recs[tid].peek = tmp;
    if (q->hp->recs[tid].peek != *node) return NULL;
    return tmp;
}

/* promote the peek hp to regular hp. */
static inline void *
lptr(const sq_t *q, sq_node_t **node, const uint level, int tid)
{
    q->hp->recs[tid].node[level] = *node;
    return *node;
}


/*
 * Unlock all locked nodes in preds
 */
static void
unlock_preds(sq_node_t **preds, const int highestLocked)
{
    sq_node_t *prev = NULL;
    for (int i = 0; i <= highestLocked; i++) {
	if (preds[i] != prev)
	    unlock(&preds[i]->lock);
	prev = preds[i];
    }
}

/*
 * Allowing for different topLevels in order to be able to change
 * the fan out during execution.
 */
static sq_node_t *
create_node(const int topLevel, const key_t key, const val_t val)
{
    sq_node_t *node;
    E_0(node = (sq_node_t *) malloc(sizeof(sq_node_t) + (topLevel * sizeof(sq_node_t *))));

    node->key = key;
    node->val = val;
    node->topLevel = topLevel;
    node->marked = 0;
    node->fullyLinked = 0;

    E(pthread_spin_init(&node->lock, 0));

    // TODO: change maxLevel of queue?

    return node;
}


void
destroy_node(sq_node_t *node)
{
    pthread_spin_destroy(&node->lock);
    free(node);
}


/* Used by add function. Locate predecessors and successor nodes to a 
 * specific key value. */
static int
sq_search(const sq_t *q, const key_t key,
	   sq_node_t ***_preds,
	  sq_node_t ***_succs, const int tid)
{
    int lFound;
    int level;
    sq_node_t *pred, *curr;
    sq_node_t **preds = *_preds;
    sq_node_t **succs = *_succs;

restart:
    pred = q->head;
    lFound = -1;

    for (level = q->maxLevel - 1; level >= 0; level--) {

#ifdef HP
	/* be careful here */
	curr = pptr(q, &pred->nexts[level], tid);
	if (NULL == curr) {
	    goto restart;
	}
#else
	curr = pred->nexts[level];
#endif
        // insert after nodes with the same key
	while (key >= curr->key) {

#ifdef HP
	    /* promote the peek ptr to hz pointer */
	    pred = lptr(q, &curr, level, tid);

	    /* advance the peek pointer */
	    curr = pptr(q, &pred->nexts[level], tid);
	    if (NULL == curr) {
	        goto restart;
	    }
#else
	    pred = curr;
	    curr = pred->nexts[level];
#endif
	}

	if (key == curr->key && lFound == -1) {
	    lFound = level;
	}
      
	preds[level] = pred;
	succs[level] = curr;
    }
    return lFound;
}

static void
sq_search_preds(const sq_t *q, sq_node_t *node,
		sq_node_t ***_preds, const int tid)
{
    int level;
    sq_node_t *pred, *curr;
    sq_node_t **preds = *_preds;

restart:
    pred = q->head;

    for (level = node->topLevel; level >= 0; level--) {

#ifdef HP
	/* be careful here */
	curr = pptr(q, &pred->nexts[level], tid);
	if (NULL == curr) {
	    goto restart;
	}
#else
	curr = pred->nexts[level];
#endif

	while (curr != node) {

#ifdef HP
	    /* promote the peek ptr to hz pointer */
	    pred = lptr(q, &curr, level, tid);

	    /* advance the peek pointer */
	    curr = pptr(q, &pred->nexts[level], tid);
	    if (NULL == curr) {
		goto restart;
	    }
#else
	    pred = curr;
	    curr = pred->nexts[level];
#endif
	}
      
	preds[level] = pred;
    }
}



/* Allocates and returns a pointer to a priority queue. */
sq_t *
sq_init(const uint maxLevel, const key_t min, const key_t max, int nthreads)
{
    sq_t *q;
    sq_node_t *head, *tail;

    E_0((q = (sq_t *) malloc (sizeof(sq_t))));
    E_0((q->rng_state = (gsl_rng **) malloc (nthreads * sizeof(gsl_rng *))));
    
    for (int i = 0; i < nthreads; i++){
	E_0(q->rng_state[i] = gsl_rng_alloc(gsl_rng_mt19937));
	gsl_rng_set(q->rng_state[i], time(NULL)+i);
    }


    q->nthreads = nthreads;
    q->maxLevel = maxLevel;

    head = create_node(maxLevel, min, NULL_VAL);
    tail = create_node(maxLevel, max, NULL_VAL);

    head->fullyLinked = 1;
    tail->fullyLinked = 1;


    for (int i = 0; i <= maxLevel; i++) {
	head->nexts[i] = tail;
	tail->nexts[i] = NULL;
    }

    /* add head and tail to the queue */
    q->head = head;
    q->tail = tail;

    /* set up hazard pointers */
#ifdef HP
    q->hp = hp_init(maxLevel, nthreads, (void (*)(void *))destroy_node);
#endif

    E_0(q->thread_ws = malloc(q->nthreads*sizeof(sq_ti_t)));
    for (int i = 0; i < q->nthreads; i++)
    {
	E_0(q->thread_ws[i].preds = malloc(q->maxLevel * sizeof(sq_node_t*)));
	E_0((q->thread_ws[i].succs = malloc(q->maxLevel * sizeof(sq_node_t*))));

	
    }

    return q;
}

void
clean_ptrs(sq_t *q, int tid)
{
    for (int i = 0; i < q->maxLevel; i++)
    {
	q->thread_ws[tid].preds[i] = NULL;
	q->thread_ws[tid].succs[i] = NULL;
    }
}


/* Add a key-value pair to the priority queue. Multiple identical keys 
 * are allowed, and will be removed in a FIFO manner. */
int
sq_add(sq_t *q, const key_t key, const val_t val, const int tid)
{
    uint topLevel, highestLocked, level;
    sq_node_t *newNode, *pred, *succ, *prev;
    int valid;
    sq_node_t ** preds, **succs;

    preds = q->thread_ws[tid].preds;
    succs = q->thread_ws[tid].succs;
    
    topLevel = gsl_ran_binomial(q->rng_state[tid], 0.5, q->maxLevel - 1);

    assert(0 <= topLevel && topLevel < q->maxLevel);

    /* Repeat until value actually inserted
     */
    while (1) {
	/* prepare preds and succs, insert after any already existing
	 * keys with the same value as key.
	 */
	sq_search(q, key, &preds, &succs, tid);

	/* Try to lock all preds, from lowest to highest
	 */
	highestLocked = -1;
	prev = NULL;
	valid = 1;
	for (level = 0; (valid) && (level <= topLevel); level++) {
	    pred = preds[level];
	    succ = succs[level];

	    assert(pred);
	
	    if (pred != prev)
	    {
		lock(&pred->lock);
		highestLocked = level;
		prev = pred;
	    }

	    valid = (!pred->marked) && (!succ->marked)
		&& (pred->nexts[level] == succ);

	}
      
	if (!valid) {
	    unlock_preds(preds, highestLocked);
	    continue;  // retry adding the node, just update preds
	}

	assert(valid);
	for (level = 0; (level <= topLevel); level++) {
	    pred = preds[level];	
	    succ = succs[level];
	    //assert(!pred->marked);
	    assert(pred->nexts[level] == succ);
	}

	newNode = create_node(topLevel, key, val); 
	newNode->topLevel = topLevel;
	
	for (level = 0; level <= topLevel; level++) {
	    newNode->nexts[level] = succs[level];
	    preds[level]->nexts[level] = newNode;
	}
	assert(preds[topLevel]->nexts[topLevel] == newNode);
	
	/* can't delete before this is set */
	newNode->fullyLinked = 1;
	break;      
    }
    // unlock the updated nodes
    unlock_preds(preds, highestLocked);
    
    // clean threads working area.
    clean_ptrs(q, tid);

    return 1;
}

/* Delete smallest key from queue. Blocks if queue is empty. */
int
sq_delmin(sq_t *q, sq_node_t **node, int tid)
{
    int level;
    sq_node_t *del;

    /* repeat until success */
    while (1) {
	// delete the first elem of the queue
#ifdef HP
	del = pptr(q, &q->head->nexts[0], tid);
	if (NULL == del) continue;
#else
	del = q->head->nexts[0];
#endif
	assert(del);

	// block if queue is empty. Alternative: return 0 here.
	while (del == q->tail)
	    ;
	

	// No one is currently inserting or deleting the node.
	if (del->fullyLinked && !del->marked) {
	    lock(&del->lock);
	    if (del->marked) {
		unlock(&del->lock);
		continue;
	    }
	    // we've got the node, we're probably deleting it.
	    del->marked = 1;
	} else {
	    continue;
	}
	
	// lock head
	lock(&q->head->lock);

	// this guarantees that we remove the first item
	if (q->head->nexts[0] != del) { 
	    unlock(&q->head->lock);
	    del->marked = 0;
	    unlock(&del->lock);
	    continue;
	}
	/* point of no return */
	
	/* unlink */
	for (level = (int) del->topLevel; level >= 0; level--) {
	    q->head->nexts[level] = del->nexts[level];
	}

	unlock(&del->lock);
	unlock(&q->head->lock);
	break; //goto end
    }

    *node = del;
#ifdef HP
    retire_node(q->hp, del);
#endif
    return 1;
}


/* Alternative strategy to delete smallest key from queue. Blocks if
 * queue is empty. */
int
sq_alt_delmin(sq_t *q, sq_node_t **node, int tid)
{
    int level, highestLocked, valid;
    sq_node_t *del, *pred, *prev;
    sq_node_t **preds;

    // load workspace
    preds = q->thread_ws[tid].preds;

    /* repeat until success */
    while (1) {
	// delete the first elem of the queue

	del = q->head;
	
	do {
#ifdef HP
	    del = pptr(q, &del->nexts[0], tid);
	    if (NULL == del) continue;
#else
	    del = del->nexts[0];
#endif
	    assert(del);

	    // fail if queue is empty.
	    if (del == q->tail)
		return 0;

	} while(asm_xchg(&del->marked, 1));

	// we have unique delete access to del 
	
	// block if inserting (could just as well just lock?)
	while (!del->fullyLinked)
	    ;

	lock(&del->lock);
	
	while(1) {
	    sq_search_preds(q, del, &preds, tid);
	    
	    valid = 1;
	    // lock preds
	    for (level = 0; (valid) && (level <= del->topLevel); level++) {
		pred = preds[level];

		assert(pred);
	
		if (pred != prev)
		{
		    lock(&pred->lock);
		    highestLocked = level;
		    prev = pred;
		}
		valid &= pred->nexts[level] == del && !pred->marked;
	    }
      
	    if (!valid) {
		unlock_preds(preds, highestLocked);
		continue;  
	    }
	    break;
	    
	}
	
	assert(valid);

    
	/* unlink */
	for (level = del->topLevel; level >= 0; level--) {
	    preds[level]->nexts[level] = del->nexts[level];
	}

        // unlock the updated nodes
	unlock_preds(preds, highestLocked);
	unlock(&del->lock);
	
	
	break; //goto end
    }

    // clean threads working area.
    clean_ptrs(q, tid);

    *node = del;
#ifdef HP
    retire_node(q->hp, del);
#endif
    return 1;
}





/* pprint helper function */
static unsigned int 
num_digits (unsigned int i)
{
    return i > 0 ? (int) log10 ((double) i) + 1 : 1;
}

static void
sq_print (sq_t *q)
{
    sq_node_t *n, *bottom;
    char *seps[] = {"------", "-----", "----"};

    for (int i = q->maxLevel - 1; i >= 0; i--) {
	printf("l%2d: ", i);

	n = q->head;
	printf("%d ", (int) n->key);

	n = q->head->nexts[i];
	bottom = q->head->nexts[0];
	
	while (n != NULL) {
	    while (bottom != n)
	    {
		printf("%s", seps[num_digits(bottom->key) -1 ]);
		bottom = bottom->nexts[0];
	    }

	    printf("-> %d ", (int) n->key);

	    bottom = bottom->nexts[0];	    
	    n = n->nexts[i];
	}
	printf(" -|\n");
    }

    printf("top: ");
    n = q->head;
    while (n != NULL) {
	printf("t:%d  ", (int) n->topLevel);
	n = n->nexts[0];
    }
    printf("\n");
    printf("--------------------------------------------------\n");
    
}

int
sq_update(sq_t *q, key_t newkey, val_t newval, val_t *old, int tid)
{
    sq_node_t *old_node = NULL;
    sq_delmin(q, &old_node, tid);
    assert(old_node);
    *old = old_node->val;
    sq_add(q, newkey, newval, tid);
    return 1;
}


void
sq_destroy(sq_t *q)
{
    // free all remaining nodes
    assert(q->head->nexts[0]);
    sq_node_t *tmp, *cur = q->head->nexts[0];
    while (cur != q->tail) {
	tmp = cur->nexts[0];
	destroy_node(cur);
	cur = tmp;
    }
    free(q->head);
    free(q->tail);

    for (int i = 0; i < q->nthreads; i++)
    {
	free(q->thread_ws[i].preds);
	free(q->thread_ws[i].succs);
	
	gsl_rng_free(q->rng_state[i]);

    }
    free(q->rng_state);
    free(q->thread_ws);
#ifdef HP
    hp_destroy(q->hp);
#endif
    free(q);
}

