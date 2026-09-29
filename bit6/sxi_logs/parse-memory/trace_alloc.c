#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <malloc.h>
#include <sys/resource.h>
#include <time.h>
/* Linux/glibc experiment only. No allocation bookkeeping table, so logging
 * cannot silently add a second large workspace. Pair pointers to recover live
 * intervals; realloc emits old and new addresses. */
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t,size_t);
extern void *__libc_realloc(void*,size_t);
extern void __libc_free(void*);
static void trace(const char *op,void *p,void *q,size_t old,size_t bytes){
    if(old<10000000 && bytes<10000000)return;
    struct rusage u;struct timespec t;getrusage(RUSAGE_SELF,&u);clock_gettime(CLOCK_MONOTONIC,&t);
    fprintf(stderr,"ALLOC_TRACE t=%ld.%09ld op=%s old=%p new=%p old_bytes=%zu bytes=%zu peak_kib=%ld\n",t.tv_sec,t.tv_nsec,op,p,q,old,bytes,u.ru_maxrss);
}
void *malloc(size_t n){void *p=__libc_malloc(n);trace("malloc",0,p,0,n);return p;}
void *calloc(size_t n,size_t s){void *p=__libc_calloc(n,s);trace("calloc",0,p,0,n*s);return p;}
void *realloc(void *p,size_t n){size_t old=p?malloc_usable_size(p):0;void *q=__libc_realloc(p,n);trace("realloc",p,q,old,n);return q;}
void free(void *p){size_t old=p?malloc_usable_size(p):0;trace("free",p,0,old,0);__libc_free(p);}
