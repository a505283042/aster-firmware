/* SPDX-License-Identifier: MIT
 * Host simulation of the real compressed-ring producer with a disk file. */
#define _POSIX_C_SOURCE 200809L
#include "aster_audio_prefetch.h"
#include "aster_os.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned allocations;
static uint64_t read_total;
void *aster_os_alloc(size_t n, unsigned region) {
    (void)region; void *p=malloc(n); if(p) ++allocations; return p;
}
void aster_os_free(void *p, unsigned region) {
    (void)region; if(p){assert(allocations);--allocations;free(p);}
}
int aster_os_file_open(aster_os_file_t *f,const char *path,unsigned flags) {
    assert(flags==ASTER_OS_READ);
    f->handle=fopen(path,"rb");return f->handle?0:-ENOENT;
}
int aster_os_file_close(aster_os_file_t *f) {
    int r=fclose(f->handle);f->handle=NULL;return r?-EIO:0;
}
int aster_os_file_read(aster_os_file_t *f,void *p,size_t n) {
    size_t got=fread(p,1,n,(FILE *)f->handle);
    read_total+=got;return ferror((FILE *)f->handle)?-EIO:(int)got;
}
int aster_os_file_seek(aster_os_file_t *f,int64_t n,unsigned origin) {
    int whence=origin==ASTER_OS_SEEK_SET?SEEK_SET:
               origin==ASTER_OS_SEEK_CUR?SEEK_CUR:SEEK_END;
    return fseek((FILE *)f->handle,(long)n,whence)?-EIO:0;
}
int aster_os_mutex_create(aster_os_mutex_t *m) {
    pthread_mutex_t *p=malloc(sizeof(*p));
    if(!p)return -ENOMEM;
    assert(!pthread_mutex_init(p,NULL));m->handle=p;return 0;
}
int aster_os_mutex_lock(aster_os_mutex_t *m,uint32_t ms) {
    (void)ms;return pthread_mutex_lock(m->handle)?-EIO:0;
}
int aster_os_mutex_unlock(aster_os_mutex_t *m) {
    return pthread_mutex_unlock(m->handle)?-EIO:0;
}
int aster_os_mutex_destroy(aster_os_mutex_t *m) {
    assert(!pthread_mutex_destroy(m->handle));
    free(m->handle);m->handle=NULL;return 0;
}
typedef struct { pthread_t thread;aster_os_task_fn fn;void *ctx; } task_t;
static void *worker(void *p) {
    task_t *t=p;t->fn(t->ctx);return NULL;
}
int aster_os_task_start(aster_os_task_t *out,const aster_os_task_config_t *cfg,
                        aster_os_task_fn fn,void *ctx) {
    (void)cfg;task_t *t=calloc(1,sizeof(*t));if(!t)return -ENOMEM;
    t->fn=fn;t->ctx=ctx;
    if(pthread_create(&t->thread,NULL,worker,t)){free(t);return -EAGAIN;}
    out->handle=t;return 0;
}
int aster_os_task_join(aster_os_task_t *out,uint32_t ms) {
    (void)ms;task_t *t=out->handle;
    assert(!pthread_join(t->thread,NULL));
    free(t);out->handle=NULL;return 0;
}
void aster_os_sleep_ms(uint32_t ms) {
    struct timespec t={.tv_sec=ms/1000u,.tv_nsec=(ms%1000u)*1000000u};
    nanosleep(&t,NULL);
}
uint64_t aster_os_uptime_ms(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static bool cancel(void *ctx){return *(bool *)ctx;}
static uint8_t pattern(uint64_t i){
    return (uint8_t)((i*17u+(i>>4)*31u+97u)&255u);
}
static void exercise(const char *path,uint64_t size,uint64_t start){
    aster_audio_prefetch_t *p=aster_audio_prefetch_start(path,start,size);
    assert(p);
    aster_audio_prefetch_window_t w;
    assert(aster_audio_prefetch_window(&w) && w.active);
    assert(w.capacity_bytes==128u*1024u);
    uint8_t buf[19001];uint64_t cursor=start;
    while(cursor<size){
        size_t want=1+(size_t)((cursor*19u)%sizeof(buf));
        if(want>size-cursor)want=(size_t)(size-cursor);
        int n=aster_audio_prefetch_read(p,buf,want,NULL,NULL);
        assert(n>0 && (size_t)n<=want);
        for(int i=0;i<n;i++)assert(buf[i]==pattern(cursor+(unsigned)i));
        cursor+=(unsigned)n;
    }
    assert(aster_audio_prefetch_read(p,buf,1,NULL,NULL)==0);
    bool abort=true;
    assert(aster_audio_prefetch_read(p,buf,1,cancel,&abort)==-ECANCELED);
    aster_audio_prefetch_stop(p);
    assert(aster_audio_prefetch_window(&w) && !w.active);
    assert(w.buffered_bytes==0 && w.capacity_bytes==0);
}
int main(int argc,char **argv){
    assert(argc==2);
    const uint64_t size=512u*1024u+777u;
    FILE *f=fopen(argv[1],"wb");assert(f);
    for(uint64_t i=0;i<size;i++)fputc(pattern(i),f);
    assert(!fclose(f));
    exercise(argv[1],size,0);
    exercise(argv[1],size,234567);
    assert(!allocations);
    assert(read_total>=size);
    return 0;
}
