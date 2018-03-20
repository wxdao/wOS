#include <stddef.h>

typedef struct _heap_free_info_t {
  size_t size;
  struct _heap_free_info_t *next;
} heap_free_info_t;

typedef struct _heap_t {
  void *pool;
  size_t pool_size;
  heap_free_info_t *free_head;
} heap_t;

static size_t align_size(size_t align, size_t size) {
  return align * ((size - 1) / align + 1);
}

static heap_free_info_t **heap_find_free_info(heap_t *heap, size_t size) {
  heap_free_info_t **found_info = NULL;
  for (heap_free_info_t **info = &heap->free_head; *info != NULL;
       info = &(*info)->next) {
    // join continuous free space if possible
    while (&(*info)->next != NULL &&
           (*info)->next ==
               *info + (*info)->size / sizeof(heap_free_info_t) + 1) {
      (*info)->size += (*info)->next->size + sizeof(heap_free_info_t);
      (*info)->next = (*info)->next->next;
    }
    if ((*info)->size >= size) {
      found_info = info;
      break;
    }
  }
  if (found_info == NULL) {
    return NULL;
  }
  return found_info;
}

heap_t *heap_init(void *pool, size_t pool_size) {
  if (pool_size < sizeof(heap_free_info_t) + sizeof(heap_t)) {
    return NULL;
  }
  heap_t *heap = (heap_t *)pool;
  heap->pool = pool;
  heap->pool_size = pool_size;
  // put first free info on the top of pool
  heap->free_head = (heap_free_info_t *)pool;
  heap->free_head->next = NULL;
  heap->free_head->size = pool_size - sizeof(heap_free_info_t);
  return heap;
}

void *heap_alloc(heap_t *heap, size_t size) {
  // align size
  size = align_size(sizeof(heap_free_info_t), size);
  // find suitable free info
  heap_free_info_t **found_info = heap_find_free_info(heap, size);
  if (found_info == NULL) {
    return NULL;
  }
  // split
  size_t free_size = (*found_info)->size;
  void *ret = *found_info + 1;
  (*found_info)->size = size;
  if (free_size == size) {
    *found_info = (*found_info)->next;
  } else {
    heap_free_info_t *next = *found_info + size / sizeof(heap_free_info_t) + 1;
    next->size = free_size - size - sizeof(heap_free_info_t);
    next->next = (*found_info)->next;
    *found_info = next;
  }
  return ret;
}

void heap_free(heap_t *heap, void *ptr) {
  heap_free_info_t *to_free_info = (heap_free_info_t *)ptr - 1;
  // find free info gap to insert into
  heap_free_info_t **found_info;
  for (found_info = &heap->free_head; (*found_info)->next != NULL;
       found_info = &(*found_info)->next) {
    if (*found_info < (heap_free_info_t *)ptr && (*found_info)->next >= (heap_free_info_t *)ptr) {
      break;
    }
  }
  // insert
  if (*found_info >= (heap_free_info_t *)ptr) {
    to_free_info->next = *found_info;
    *found_info = to_free_info;
  } else {
    to_free_info->next = (*found_info)->next;
    (*found_info)->next = to_free_info;
  }
}
