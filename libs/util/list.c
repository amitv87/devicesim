#include "list.h"

bool list_add(list_item_t** list, list_item_t* item){
  if(item->next) return false;
  item->next = NULL;
  if(!*list) *list = item;
  else{
    list_item_t* tail = *list;
    while(tail){
      if(tail == item) break;
      if(!tail->next){
        tail->next = item;
        return true;
      }
      tail = tail->next;
    }
    return false;
  }
  return true;
}

bool list_rem(list_item_t** list, list_item_t* item){
  if(*list){
    list_item_t* tail = *list;
    list_item_t* prev = NULL;
    while(tail){
      if(tail == item){
        if(prev) prev->next = tail->next;
        else *list = tail->next;
        tail->next = NULL;
        return true;
      }
      prev = tail;
      tail = tail->next;
    }
  }
  return false;
}
