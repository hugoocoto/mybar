/* cum.h - A bunch of macros by Hugo Coto Florez 2026 */
#ifndef CUM_H_
#define CUM_H_

/* helpers for macro recursion */
#define _EMPTY()
#define _DEFER(_id_) _id_ _EMPTY()
#define OBSTRUCT(_id_) _id_ _DEFER(_EMPTY)()

#define EVAL(...) EVAL1(EVAL1(EVAL1(__VA_ARGS__)))
#define EVAL1(...) EVAL2(EVAL2(EVAL2(__VA_ARGS__)))
#define EVAL2(...) EVAL3(EVAL3(EVAL3(__VA_ARGS__)))
#define EVAL3(...) EVAL4(EVAL4(EVAL4(__VA_ARGS__)))
#define EVAL4(...) EVAL5(EVAL5(EVAL5(__VA_ARGS__)))
#define EVAL5(...) __VA_ARGS__

// join a,b,c,d, ... like a OP b OP c OP ...
// if OP is comma, use __VA_ARGS__ directly
#define Join(_op_, ...) __VA_OPT__(EVAL(JOIN_INNER(_op_, __VA_ARGS__)))
#define JOIN_INNER(_op_, a, ...) a __VA_OPT__(_op_ OBSTRUCT(JOIN_INDIRECT)()(_op_, __VA_ARGS__))
#define JOIN_INDIRECT() JOIN_INNER

// Supress unused warning
#define Unused(x, ...) EVAL(UNUSED_INNER(x, ##__VA_ARGS__))
#define UNUSED_INNER(x, ...) \
        (void) x;            \
        __VA_OPT__(OBSTRUCT(UNUSED_INDIRECT)()(__VA_ARGS__))
#define UNUSED_INDIRECT() UNUSED_INNER

// todo() macro - print and exit
#define Todo(fmt, ...)                                                                   \
        do {                                                                             \
                fprintf(stderr, "%s:%d: TODO: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
                abort();                                                                 \
        } while (0)

#ifdef __cplusplus
#define Let auto
#else
#define Let __auto_type
#endif

#ifdef __cplusplus
#define Typeof(x) typeof(x)
#else
#define Typeof(x) __typeof__(x)
#endif

/* Type agnostic dynamic array */

#define Da(type)              \
        struct {              \
                int capacity; \
                int count;    \
                type *items;  \
        }

/* Add E to DA_PTR (pointer to DA). */
#define Da_append(da_ptr, ...)                                                                \
        do {                                                                                  \
                if ((da_ptr)->count >= (da_ptr)->capacity) {                                  \
                        (da_ptr)->capacity = (da_ptr)->capacity ? (da_ptr)->capacity * 2 : 4; \
                        (da_ptr)->items    = (Typeof((da_ptr)->items)) realloc(               \
                        (da_ptr)->items,                                                      \
                        sizeof(*((da_ptr)->items)) * (da_ptr)->capacity);                     \
                }                                                                             \
                (da_ptr)->items[(da_ptr)->count++] = (__VA_ARGS__);                           \
        } while (0)

/* Destroy DA pointed by DA_PTR. */
#define Da_destroy(da_ptr)                                       \
        do {                                                     \
                if ((da_ptr) == NULL) break;                     \
                if ((da_ptr)->items && (da_ptr)->capacity > 0) { \
                        free((da_ptr)->items);                   \
                }                                                \
                (da_ptr)->capacity = 0;                          \
                (da_ptr)->count    = 0;                          \
                (da_ptr)->items    = NULL;                       \
        } while (0)

/* Insert element E into DA_PTR at index I. */
#define Da_insert(da_ptr, e, i)                                                   \
        do {                                                                      \
                Da_append((da_ptr), (Typeof((e))) { 0 });                         \
                memmove((da_ptr)->items + (i) + 1, (da_ptr)->items + (i),         \
                        ((da_ptr)->count - (i) - 1) * sizeof *((da_ptr)->items)); \
                (da_ptr)->items[(i)] = (e);                                       \
        } while (0)

/* Get the index of a element given a pointer to this element */
#define Da_index(da_elem_ptr, da_ptr) ((int) ((da_elem_ptr) - ((da_ptr)->items)))

/* Remove the last element */
#define Da_remove_last(da_ptr)                          \
        do {                                            \
                if ((da_ptr)->count) --(da_ptr)->count; \
        } while (0)

/* Remove element at index I. */
#define Da_remove(da_ptr, i)                                                        \
        do {                                                                        \
                if ((i) >= 0 && (i) < (da_ptr)->count) {                            \
                        --(da_ptr)->count;                                          \
                        memmove((da_ptr)->items + (i), (da_ptr)->items + (i) + 1,   \
                                ((da_ptr)->count - (i)) * sizeof *(da_ptr)->items); \
                }                                                                   \
        } while (0)

/* Iterate the DA. I is a pointer to each element (from first to last). */
#define Da_foreach(_i_, da)                           \
        for (Let _i_ = (da).items;                    \
             (int) ((_i_) - (da).items) < (da).count; \
             ++(_i_))

/* Duplicate struct and item buffer (element data copied byte-wise). */
#define Da_dup(da_ptr)                                                                                 \
        ({                                                                                             \
                Let cpy   = *(da_ptr);                                                                 \
                cpy.items = (Typeof(cpy.items)) malloc((da_ptr)->capacity * sizeof(da_ptr)->items[0]); \
                memcpy(cpy.items, (da_ptr)->items, (da_ptr)->capacity * sizeof(da_ptr)->items[0]);     \
                cpy;                                                                                   \
        })

/* Command wrapper over DA */
typedef Da(char *) Command;

/* Append args to COMMAND. Wrapped in do/while so it is a single statement: an
 * unbraced `if (x) Command_add(&c, "a", "b");` guards all the appends. The
 * (char *) cast lets string literals in under C++. */
#define Command_add(command, ...)                                          \
        do {                                                               \
                __VA_OPT__(EVAL(COMMAND_ADD_INNER(command, __VA_ARGS__));) \
        } while (0)
#define COMMAND_ADD_INNER(command, a, ...) Da_append(command, (char *) (a)) __VA_OPT__(; OBSTRUCT(COMMAND_ADD_INDIRECT)()(command, __VA_ARGS__))
#define COMMAND_ADD_INDIRECT() COMMAND_ADD_INNER

#define Command_destroy Da_destroy

/* Define Command_run(): run COMMAND and wait for it. Returns its exit status:
 * 127 if exec failed, 1 if fork failed or it was killed by a signal.
 * Needs <stdio.h>, <unistd.h> and <sys/wait.h>. */
#define DECLARE_COMMAND_RUN()                                            \
                                                                         \
        static int Command_run(Command command)                          \
        {                                                                \
                if (command.count < 1) return 1;                         \
                int status = 0;                                          \
                int pid    = fork();                                     \
                if (pid < 0) {                                           \
                        perror("fork");                                  \
                        return 1;                                        \
                }                                                        \
                if (pid == 0) {                                          \
                        Da_append(&command, NULL);                       \
                        execvp(command.items[0], command.items);         \
                        perror(command.items[0]);                        \
                        _exit(127); /* exit() would flush stdio twice */ \
                }                                                        \
                if (waitpid(pid, &status, 0) == -1) {                    \
                        perror("waitpid");                               \
                        return 1;                                        \
                }                                                        \
                return WIFEXITED(status) ? WEXITSTATUS(status) : 1;      \
        }

/* Stack wrapper over DA (LIFO). */
#define Ss Da
#define Ss_foreach Da_foreach
#define Ss_top(s_ptr) ((s_ptr)->items[(s_ptr)->count - 1])
#define Ss_push(s_ptr, ...) Da_append((s_ptr), __VA_ARGS__)
#define Ss_pop(s_ptr) Da_remove_last((s_ptr))

#define Min(a, b) ({               \
        Let _a = (a);              \
        Let _b = (b);              \
        (_a) < (_b) ? (_a) : (_b); \
})

#define Max(a, b) ({               \
        Let _a = (a);              \
        Let _b = (b);              \
        (_a) > (_b) ? (_a) : (_b); \
})

#define Memdup(x) memcpy(malloc(sizeof(x)), &(x), sizeof(x))
#define Memzero(x) memset(&(x), 0, sizeof((x)))

#define KB(x) ((size_t) (x) << 10)
#define MB(x) ((size_t) (x) << 20)
#define GB(x) ((size_t) (x) << 30)
#define TB(x) ((size_t) (x) << 40)

#define Bf(n) ((n) = 0u)
#define Bf_set(n, f) ((n) |= (f))
#define Bf_clr(n, f) ((n) &= ~(f))
#define Bf_has(n, f) (((n) & (f)) == (f))

#define Tostr(x) #x

#endif // !CUM_H_

/*------------------------------------------------------------------------------
Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------*/
