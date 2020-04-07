//
// Created by pdvrieze on 07/04/2020.
//

#ifndef DROIDCAM_UTIL_H
#define DROIDCAM_UTIL_H


#define FREE_OBJECT(obj, free_func) { dbgprint(" " #obj " %p\n", obj); freeObject(&obj, free_func); }

void freeObject(void ** ptr, void (freeFunc)(void *));

#endif //DROIDCAM_UTIL_H
