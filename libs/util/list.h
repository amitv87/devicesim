#ifndef UTIL_LIST_H
#define UTIL_LIST_H

#include "../common.h"

typedef struct list_item_s list_item_t;

typedef struct list_item_s{
  void* next;
} list_item_t;

bool list_add(list_item_t** list, list_item_t* item);
bool list_rem(list_item_t** list, list_item_t* item);

#define LIST_ADD(list, item) list_add((list_item_t**)list, (list_item_t*)item)
#define LIST_REM(list, item) list_rem((list_item_t**)list, (list_item_t*)item)

#endif
