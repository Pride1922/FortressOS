/* Actual layout publication/decoder, separate decoder state per reader.
 * Pthreads establish races; this is not keyboard IRQ or hardware evidence. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "keyboard.h"
static void *writer(void *unused) {
    (void)unused;
    for (unsigned i=0;i<100000;i++) keyboard_set_layout(i&1);
    return NULL;
}
static void *reader(void *unused) {
    (void)unused;keyboard_decoder_t decoder={0};
    for (unsigned i=0;i<100000;i++) {
        char c=keyboard_decode(&decoder,0x10);
        assert(c=='a' || c=='q');
        int layout=keyboard_get_layout();assert(layout==KBD_LAYOUT_US || layout==KBD_LAYOUT_AZERTY);
    }
    return NULL;
}
int main(void) {
    pthread_t threads[5];assert(!pthread_create(&threads[0],NULL,writer,NULL));
    for (unsigned i=1;i<5;i++) assert(!pthread_create(&threads[i],NULL,reader,NULL));
    for (unsigned i=0;i<5;i++) assert(!pthread_join(threads[i],NULL));
    puts("PASS actual keyboard: 100000 layout publications and 400000 separately-owned decoder reads (no IRQ claim)");
}
