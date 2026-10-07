#include "abort_exit.h"
#include "critical_regions.h"
#include "dolphin/macros.h"
#include "stddef.h"
#include "NMWException.h"
#include <signal.h>

void _ExitProcess();

extern void (*_dtors[])(void);

int __aborting = 0;

static void (*atexit_funcs[64])(void);
static int atexit_curr_func = 0;

static void (*__atexit_funcs[64])(void);
static int __atexit_curr_func = 0;

void (*__stdio_exit)(void)   = 0;
void (*__console_exit)(void) = 0;

void __exit(int status);

void abort(void) {
    raise(1);
    __aborting = 1;
    __exit(1);
}

void exit(int status)
{
	int i;
	void (**dtor)(void);

	if (!__aborting) {
		while (atexit_curr_func > 0)
			atexit_funcs[--atexit_curr_func]();

		__destroy_global_chain();

		dtor = _dtors;
		while (*dtor != NULL) {
			(*dtor)();
			dtor++;
		}

		if (__stdio_exit != NULL) {
			__stdio_exit();
			__stdio_exit = NULL;
		}
	}

	while (__atexit_curr_func > 0)
		__atexit_funcs[--__atexit_curr_func]();

	__kill_critical_regions();

	if (__console_exit != NULL) {
		__console_exit();
		__console_exit = NULL;
	}

	_ExitProcess();
}

void __exit(int status) {
    void (**var_r31)(void);

	//TODO: Remove this
	FORCE_DONT_INLINE

	while (__atexit_curr_func > 0)
		__atexit_funcs[--__atexit_curr_func]();

    __kill_critical_regions();

    if (__console_exit != 0) {
        __console_exit();
        __console_exit = 0;
    }
    _ExitProcess();
}
