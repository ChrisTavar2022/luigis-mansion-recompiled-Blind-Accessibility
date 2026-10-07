//https://github.com/doldecomp/dolsdk2001/blob/main/include/macros.h
#ifndef _H_MACROS_
#define _H_MACROS_

#ifdef DEBUG
#define ASSERTLINE(line, cond) \
    ((cond) || (OSPanic(__FILE__, line, "Failed assertion " #cond), 0))

#define ASSERTMSGLINE(line, cond, msg) \
    ((cond) || (OSPanic(__FILE__, line, msg), 0))

// This is dumb but we dont have a Metrowerks way to do variadic macros in the macro to make this done in a not scrubby way.
#define ASSERTMSG1LINE(line, cond, msg, arg1) \
    ((cond) || (OSPanic(__FILE__, line, msg, arg1), 0))
    
#define ASSERTMSG2LINE(line, cond, msg, arg1, arg2) \
    ((cond) || (OSPanic(__FILE__, line, msg, arg1, arg2), 0))

#define ASSERTMSGLINEV(line, cond, ...) \
    ((cond) || (OSPanic(__FILE__, line, __VA_ARGS__), 0))
#else
#define ASSERTLINE(line, cond) (void)0
#define ASSERTMSGLINE(line, cond, msg) (void)0
#define ASSERTMSG1LINE(line, cond, msg, arg1) (void)0
#define ASSERTMSG2LINE(line, cond, msg, arg1, arg2) (void)0
#define ASSERTMSGLINEV(line, cond, ...) (void)0
#endif

#define ASSERT(cond) ASSERTLINE(__LINE__, cond)
#define ARRAY_SIZE(o)        (sizeof((o)) / sizeof(*(o)))   // Array size define

/* Start of rip from  https://github.com/projectPiki/pikmin/blob/main/include/types.h */
// For undesireable code necessary to produce a matching build, use this instead of a clunky #if #else #endif.

// clang-format off
#define REPEAT1(x)  x
#define REPEAT2(x)  REPEAT1(x); x
#define REPEAT3(x)  REPEAT2(x); x
#define REPEAT4(x)  REPEAT3(x); x
#define REPEAT5(x)  REPEAT4(x); x
#define REPEAT6(x)  REPEAT5(x); x
#define REPEAT7(x)  REPEAT6(x); x
#define REPEAT8(x)  REPEAT7(x); x
#define REPEAT9(x)  REPEAT8(x); x
#define REPEAT10(x) REPEAT9(x); x
#define REPEAT11(x) REPEAT10(x); x
#define REPEAT12(x) REPEAT11(x); x
#define REPEAT13(x) REPEAT12(x); x
#define REPEAT14(x) REPEAT13(x); x
#define REPEAT15(x) REPEAT14(x); x
#define REPEAT16(x) REPEAT15(x); x
// clang-format on

#define REPEAT(x, n) REPEAT##n(x)

// Somehow this overwhelms the automatic inlining score and stops unwanted function inlining
#define FORCE_DONT_INLINE REPEAT((void)0;, 10)

//TODO: Merge with other asserts
#ifdef DEBUG // Currently necessary for dsp_cardunlock.c
#define OSAssertMsgLine(line, cond, ...) ((void)((cond) || (OSErrorLine(line, __VA_ARGS__), 0)))
#define OSAssertLine(line, cond) OSAssertMsgLine(line, cond, "Failed assertion " #cond)
#else
#define OSAssertMsgLine(line, cond, ...) ((void)(0))
#define OSAssertLine(line, cond) ((void)(0))
#endif

/*end of rip */

#endif // _H_MACROS_
