#ifndef LIB_QUEUE_H_
#define LIB_QUEUE_H_

#include <stddef.h>

typedef void *queue[2];

/* Private macros. */
#define QUEUE_NEXT(q) (*(queue **)&((*(q))[0]))
#define QUEUE_PREV(q) (*(queue **)&((*(q))[1]))

#define QUEUE_PREV_NEXT(q) (QUEUE_NEXT(QUEUE_PREV(q)))
#define QUEUE_NEXT_PREV(q) (QUEUE_PREV(QUEUE_NEXT(q)))

/**
 * Initialize an empty queue.
 */
#define QUEUE_INIT(q)                \
	{                            \
		QUEUE_NEXT(q) = (q); \
		QUEUE_PREV(q) = (q); \
	}

/**
 * Return true if the queue has no element.
 */
#define QUEUE_IS_EMPTY(q) ((const queue *)(q) == (const queue *)QUEUE_NEXT(q))

/**
 * Insert an element at the back of a queue.
 */
#define QUEUE_PUSH(q, e)                       \
	{                                      \
		QUEUE_NEXT(e) = (q);           \
		QUEUE_PREV(e) = QUEUE_PREV(q); \
		QUEUE_PREV_NEXT(e) = (e);      \
		QUEUE_PREV(q) = (e);           \
	}

/**
 * Remove the given element from the queue. Any element can be removed at any
 * time.
 */
#define QUEUE_REMOVE(e)                             \
	{                                           \
		QUEUE_PREV_NEXT(e) = QUEUE_NEXT(e); \
		QUEUE_NEXT_PREV(e) = QUEUE_PREV(e); \
	}

/**
 * Return the element at the front of the queue.
 */
#define QUEUE_HEAD(q) (QUEUE_NEXT(q))

/**
 * Return the element at the back of the queue.
 */
#define QUEUE_TAIL(q) (QUEUE_PREV(q))

/**
 * Iternate over the element of a queue.
 *
 * Mutating the queue while iterating results in undefined behavior.
 */
#define QUEUE_FOREACH(q, e) \
	for ((q) = QUEUE_NEXT(e); (q) != (e); (q) = QUEUE_NEXT(q))

/**
 * Return the structure holding the given element.
 */
#define QUEUE_DATA(e, type, field) ((type *)((char *)(e)-offsetof(type, field)))

#endif /* LIB_QUEUE_H_*/
