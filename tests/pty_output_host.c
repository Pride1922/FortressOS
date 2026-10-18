/* Actual PTY ring/write path, with host lock and scheduler adapters. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/drivers/pty.c"

static bool locked, close_on_wait;
static unsigned waits;
static pty_t terminal;
static vfs_node_t endpoint;

uint64_t spin_lock_irqsave(spinlock_t *lock) {
    (void)lock;assert(!locked);locked=true;return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)lock;(void)flags;assert(locked);locked=false;
}
bool process_signal_interrupt(void) {return false;}
void sched_wake_all(const void *channel) {(void)channel;assert(!locked);}
void sched_wait_until(const void *channel, bool (*ready)(void *), void *arg) {
    assert(!locked && channel==&terminal.out_space_bytes);
    assert(++waits==1);
    /* One free byte cannot satisfy a pending two-byte CRLF expansion. */
    assert(!ready(arg));
    if (close_on_wait) terminal.master_open=false;
    else {
        uint8_t byte;
        assert(pty_master_read(&endpoint,VFS_O_NONBLOCK,&byte,1)==1);
    }
    assert(ready(arg));
}
static void setup(size_t space) {
    memset(&terminal,0,sizeof(terminal));
    memset(terminal.out_buf,'X',sizeof(terminal.out_buf));
    terminal.master_open=terminal.slave_open=true;
    terminal.out_count=PTY_OUT_CAPACITY-space;
    terminal.out_head=terminal.out_count % PTY_OUT_CAPACITY;
    terminal.out_space_bytes=space;
    terminal.out_data_bytes=terminal.out_count;
    endpoint.fs_private=&terminal;
    waits=0;close_on_wait=false;
}
int main(void) {
    uint64_t flags=0;
    setup(1);
    assert(pty_slave_write(&endpoint,&flags,false,"\n",1)==1);
    assert(waits==1 && terminal.out_count==PTY_OUT_CAPACITY);
    assert(terminal.out_buf[PTY_OUT_CAPACITY-1]=='\r' && terminal.out_buf[0]=='\n');
    setup(1);flags=VFS_O_NONBLOCK;
    assert(pty_slave_write(&endpoint,&flags,false,"\n",1)==-VFS_EAGAIN && waits==0);
    assert(terminal.out_count==PTY_OUT_CAPACITY-1);
    setup(1);flags=0;terminal.last_slave_out_char='\r';
    assert(pty_slave_write(&endpoint,&flags,false,"\n",1)==1 && waits==0);
    setup(0);
    assert(pty_slave_write(&endpoint,&flags,false,"A",1)==1 && waits==1);
    setup(1);close_on_wait=true;
    assert(pty_slave_write(&endpoint,&flags,false,"\n",1)==-VFS_EIO && waits==1);
    puts("PASS actual PTY CRLF two-slot wait, wrap, nonblocking retry and master close");
    return 0;
}
