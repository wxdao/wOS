#ifndef __HEAP_H
#define __HEAP_H

typedef struct _heap_t heap_t;

heap_t *heap_init(void *pool, size_t pool_size);
void *heap_alloc(heap_t *heap, size_t size);
void heap_free(heap_t *heap, void *ptr);

#endif