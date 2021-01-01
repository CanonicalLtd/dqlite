#ifndef LIB_REGISTRY_H_
#define LIB_REGISTRY_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sqlite3.h>

#include "../../include/dqlite.h"

#include "assert.h"

#define DQLITE_NOTFOUND 1002

/**
 * Define a type-safe registry able to allocate and lookup items of a given
 * type.
 *
 * The item TYPE is required to implement three methods: TYPE##_init,
 * TYPE##_close and TYPE##Hash.
 */
#define REGISTRY(NAME, TYPE)                                                   \
                                                                               \
	struct NAME                                                            \
	{                                                                      \
		struct TYPE **buf; /* Array of registry item slots */          \
		size_t len;        /* Index of the highest used slot */        \
		size_t cap;        /* Total number of slots */                 \
	};                                                                     \
                                                                               \
	/* Initialize the registry. */                                         \
	void NAME##_init(struct NAME *r);                                      \
                                                                               \
	/* Close the registry. */                                              \
	void NAME##_close(struct NAME *r);                                     \
                                                                               \
	/* Add an item to the registry.                                        \
	 *                                                                     \
	 * Return a pointer to a newly allocated an initialized item.          \
	 * The "id" field of the item will be set to a unique value            \
	 * identifying the item in the registry. */                            \
	int NAME##_add(struct NAME *r, struct TYPE **item);                    \
                                                                               \
	/* Given its ID, retrieve an item previously added to the              \
	 * registry. */                                                        \
	struct TYPE *NAME##_get(struct NAME *r, size_t id);                    \
                                                                               \
	/* Get the index of the first item matching the given hash key. Return \
	 * 0 on success and DQLITE_NOTFOUND otherwise. */                      \
	int NAME##Idx(struct NAME *r, const char *key, size_t *i);             \
                                                                               \
	/* Delete a previously added item. */                                  \
	int NAME##Del(struct NAME *r, struct TYPE *item)

/**
 * Define the methods of a registry
 */
#define REGISTRY_METHODS(NAME, TYPE)                                         \
	void NAME##_init(struct NAME *r)                                     \
	{                                                                    \
		assert(r != NULL);                                           \
                                                                             \
		r->buf = NULL;                                               \
		r->len = 0;                                                  \
		r->cap = 0;                                                  \
	}                                                                    \
                                                                             \
	void NAME##_close(struct NAME *r)                                    \
	{                                                                    \
		size_t i;                                                    \
		struct TYPE *item;                                           \
                                                                             \
		assert(r != NULL);                                           \
                                                                             \
		/* Loop through all items currently in the registry,         \
		 * and close them. */                                        \
		for (i = 0; i < r->len; i++) {                               \
			item = *(r->buf + i);                                \
			/* Some slots may have been deleted, so we need      \
			 * to check if the slot is actually used. */         \
			if (item != NULL) {                                  \
				TYPE##_close(item);                          \
				sqlite3_free(item);                          \
			}                                                    \
		}                                                            \
                                                                             \
		if (r->buf != NULL) {                                        \
			sqlite3_free(r->buf);                                \
		}                                                            \
	}                                                                    \
                                                                             \
	int NAME##_add(struct NAME *r, struct TYPE **item)                   \
	{                                                                    \
		struct TYPE **buf;                                           \
		size_t cap;                                                  \
		size_t i;                                                    \
                                                                             \
		assert(r != NULL);                                           \
		assert(item != NULL);                                        \
                                                                             \
		/* Check if there is an unllocated slot. */                  \
		for (i = 0; i < r->len; i++) {                               \
			if (*(r->buf + i) == NULL) {                         \
				goto okSlot;                                 \
			}                                                    \
		}                                                            \
                                                                             \
		/* There are no unallocated slots. */                        \
		assert(i == r->len);                                         \
                                                                             \
		/* If we are full, then	 double the capacity. */             \
		if (r->len + 1 > r->cap) {                                   \
			cap = (r->cap == 0) ? 1 : r->cap * 2;                \
			buf = sqlite3_realloc(r->buf,                        \
					      (int)(cap * sizeof(*r->buf))); \
			if (buf == NULL) {                                   \
				return DQLITE_NOMEM;                         \
			}                                                    \
			r->buf = buf;                                        \
			r->cap = cap;                                        \
		}                                                            \
		r->len++;                                                    \
                                                                             \
	okSlot:                                                              \
		assert(i < r->len);                                          \
                                                                             \
		/* Allocate and initialize the new item */                   \
		*item = sqlite3_malloc(sizeof **item);                       \
		if (*item == NULL)                                           \
			return DQLITE_NOMEM;                                 \
                                                                             \
		(*item)->id = i;                                             \
                                                                             \
		TYPE##_init(*item);                                          \
                                                                             \
		/* Save the item in its registry slot */                     \
		*(r->buf + i) = *item;                                       \
                                                                             \
		return 0;                                                    \
	}                                                                    \
                                                                             \
	struct TYPE *NAME##_get(struct NAME *r, size_t id)                   \
	{                                                                    \
		struct TYPE *item;                                           \
		size_t i = id;                                               \
                                                                             \
		assert(r != NULL);                                           \
                                                                             \
		if (i >= r->len) {                                           \
			return NULL;                                         \
		}                                                            \
                                                                             \
		item = *(r->buf + i);                                        \
                                                                             \
		assert(item->id == id);                                      \
                                                                             \
		return item;                                                 \
	}                                                                    \
                                                                             \
	int NAME##Idx(struct NAME *r, const char *key, size_t *i)            \
	{                                                                    \
		struct TYPE *item;                                           \
                                                                             \
		assert(r != NULL);                                           \
		assert(key != NULL);                                         \
		assert(i != NULL);                                           \
                                                                             \
		for (*i = 0; *i < r->len; (*i)++) {                          \
			const char *hash;                                    \
                                                                             \
			item = *(r->buf + *i);                               \
                                                                             \
			if (item == NULL) {                                  \
				continue;                                    \
			}                                                    \
                                                                             \
			hash = TYPE##Hash(item);                             \
                                                                             \
			if (hash != NULL && strcmp(hash, key) == 0) {        \
				return 0;                                    \
			}                                                    \
		}                                                            \
                                                                             \
		return DQLITE_NOTFOUND;                                      \
	}                                                                    \
                                                                             \
	int NAME##Del(struct NAME *r, struct TYPE *item)                     \
	{                                                                    \
		struct TYPE **buf;                                           \
		size_t cap;                                                  \
		size_t i = item->id;                                         \
                                                                             \
		assert(r != NULL);                                           \
                                                                             \
		if (i >= r->len) {                                           \
			return DQLITE_NOTFOUND;                              \
		}                                                            \
                                                                             \
		/* Check that the item address actually matches the one      \
		 * we have in the registry */                                \
		if (*(r->buf + i) != item) {                                 \
			return DQLITE_NOTFOUND;                              \
		}                                                            \
                                                                             \
		TYPE##_close(item);                                          \
		sqlite3_free(item);                                          \
                                                                             \
		*(r->buf + i) = NULL;                                        \
                                                                             \
		/* If this was the last item in the registry buffer,         \
		 * decrease the length. */                                   \
		if (i == r->len - 1) {                                       \
			r->len--;                                            \
		}                                                            \
                                                                             \
		/* If the new length is less than half of the capacity,      \
		 * try to shrink the registry. */                            \
		if (r->len < (r->cap / 2)) {                                 \
			cap = r->cap / 2;                                    \
			buf = sqlite3_realloc(r->buf,                        \
					      (int)(cap * sizeof *r->buf));  \
			if (buf != NULL) {                                   \
				r->buf = buf;                                \
				r->cap = cap;                                \
			}                                                    \
		}                                                            \
                                                                             \
		return 0;                                                    \
	}

#endif /* LIB_REGISTRY_H_ */
