#include "arith.h"

int abs(int __x)
{
	if (0 <= __x) {
		return __x;
	}
	return -__x;
}
