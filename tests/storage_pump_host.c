/* Deterministic scheduling contract for the exact extracted production pump.
 * This is a host unit test, not a QEMU/main-loop responsiveness substitute. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "fs.h"
#include "platform.h"

static unsigned now, steps, polls, clock_calls, status_calls, sync_ticks;
static unsigned step_tick_at, poll_tick_at, status_change_at, plan_length;
static enum FsSyncProgress fallback, plan[260];
static int dirty;
static const char unchanged[]="stable", changed[]="changed";
static const char *storage_status;
static char trace[1024];
static unsigned trace_length;

void native_sync_tick(void){sync_ticks++;}
uint32_t timer_ticks(void){clock_calls++;return now;}
const char *fs_storage_status(void){status_calls++;return storage_status;}
enum FsSyncProgress fs_sync_step(void){
    assert(steps<256);assert(trace_length<sizeof trace);
    trace[trace_length++]='S';steps++;
    if(step_tick_at&&steps==step_tick_at)now++;
    if(status_change_at&&steps==status_change_at)storage_status=changed;
    return steps<=plan_length?plan[steps-1]:fallback;
}
void platform_poll(void){
    /* No before-first poll, repeated device polls, or other hidden callback. */
    assert(trace_length&&trace[trace_length-1]=='S');
    assert(trace_length<sizeof trace);trace[trace_length++]='P';polls++;
    if(poll_tick_at&&polls==poll_tick_at)now++;
}
#include "storage_pump.inc"

static void reset(enum FsSyncProgress progress){
    now=42;steps=polls=clock_calls=status_calls=sync_ticks=0;
    step_tick_at=poll_tick_at=status_change_at=plan_length=0;
    fallback=progress;dirty=0;storage_status=unchanged;trace_length=0;
    memset(plan,0,sizeof plan);memset(trace,0,sizeof trace);
}
static void counts(unsigned expected_steps,unsigned expected_polls){
    assert(steps==expected_steps);assert(polls==expected_polls);
    assert(status_calls==2);assert(clock_calls>=1);assert(sync_ticks==2);
    assert(trace_length==steps+polls);
    for(unsigned i=0;i<polls;i++){assert(trace[2*i]=='S');assert(trace[2*i+1]=='P');}
    if(steps>polls)assert(trace[trace_length-1]=='S');
}
static void test_hard_cap(void){
    for(unsigned i=0;i<2;i++){
        enum FsSyncProgress p=i?FS_SYNC_MORE:FS_SYNC_WAIT;reset(p);
        assert(storage_poll()==p);counts(256,256);assert(!dirty);
        /* Final device-only service at the hard cap is intentional/bounded. */
    }
}
static void test_tick_change_after_step(void){
    const unsigned stop[]={1,17,256};
    for(unsigned p=0;p<2;p++)for(unsigned i=0;i<3;i++){
        enum FsSyncProgress progress=p?FS_SYNC_MORE:FS_SYNC_WAIT;
        reset(progress);step_tick_at=stop[i];
        assert(storage_poll()==progress);counts(stop[i],stop[i]-1);assert(now==43);
    }
    reset(FS_SYNC_WAIT);now=UINT_MAX;step_tick_at=1;
    assert(storage_poll()==FS_SYNC_WAIT);counts(1,0);assert(now==0);
}
static void test_tick_change_in_device_service(void){
    const unsigned stop[]={1,17,256};
    for(unsigned p=0;p<2;p++)for(unsigned i=0;i<3;i++){
        enum FsSyncProgress progress=p?FS_SYNC_MORE:FS_SYNC_WAIT;
        reset(progress);poll_tick_at=stop[i];
        assert(storage_poll()==progress);
        /* Strict post-device check: no additional filesystem step after a
         * device-only service call has crossed the one-tick budget. */
        counts(stop[i],stop[i]);assert(now==43);
    }
}
static void test_terminal_exit(void){
    const enum FsSyncProgress final[]={FS_SYNC_IDLE,FS_SYNC_FINISHED};
    for(unsigned i=0;i<2;i++){
        reset(final[i]);assert(storage_poll()==final[i]);counts(1,0);
        assert(clock_calls==1); /* Short-circuit: no clock poll after terminal. */
        reset(FS_SYNC_WAIT);plan_length=5;
        plan[0]=FS_SYNC_WAIT;plan[1]=FS_SYNC_MORE;plan[2]=FS_SYNC_WAIT;
        plan[3]=FS_SYNC_MORE;plan[4]=final[i];
        assert(storage_poll()==final[i]);counts(5,4);
    }
}
static void test_status_redraw(void){
    reset(FS_SYNC_FINISHED);status_change_at=1;
    assert(storage_poll()==FS_SYNC_FINISHED);counts(1,0);assert(dirty==1);
    reset(FS_SYNC_WAIT);status_change_at=3;step_tick_at=3;
    assert(storage_poll()==FS_SYNC_WAIT);counts(3,2);assert(dirty==1);
    reset(FS_SYNC_IDLE);dirty=7;
    assert(storage_poll()==FS_SYNC_IDLE);counts(1,0);assert(dirty==7);
}
int main(void){
    test_hard_cap();test_tick_change_after_step();test_tick_change_in_device_service();
    test_terminal_exit();test_status_redraw();
    puts("production storage pump bounds and device-only order passed");return 0;
}
