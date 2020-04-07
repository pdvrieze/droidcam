//
// Created by pdvrieze on 07/04/2020.
//

#include "util.h"
#include "common.h"

void freeObject(void ** ptr, void (freeFunc)(void *)) {
	if (*ptr) {
		freeFunc(*ptr);
		*ptr = 0;
	}
}
