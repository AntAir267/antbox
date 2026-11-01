#include "kernel.h"
#include "recomp_env.h"
#include <stdio.h>
#include <stdlib.h>
/* This settings-only test must never dispatch guest code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }
/* game.language as XC_LANGUAGE reports it: English unless set to a name or
 * number the dashboard knows; read again on each query. */
static int expect(const char *setting, ULONG want) {
    ULONG value=0xdeadbeef, type=0, length=0;
    NTSTATUS status;
    recomp_env_set(RENV_GAME_LANGUAGE, setting);
    status=xbox_ExQueryNonVolatileSetting(XC_LANGUAGE,&type,&value,sizeof(value),&length);
    if(status || value!=want || type!=4 || length!=sizeof(value)) {
        fprintf(stderr,"game.language=%s: status=%lx value=%lu (want %lu) type=%lu length=%lu\n",
                setting ? setting : "(unset)",status,value,want,type,length);
        return 1;
    }
    return 0;
}
int main(void) {
    int bad=0;
    bad|=expect(NULL,1);
    bad|=expect("",1);
    bad|=expect("german",3);
    bad|=expect("italian",6);
    bad|=expect("portuguese",9);
    bad|=expect("4",4);
    bad|=expect("0",1);
    bad|=expect("10",1);
    bad|=expect("klingon",1);
    bad|=expect(NULL,1);
    if(bad) return 1;
    puts("PASS: XC_LANGUAGE follows game.language, English otherwise.");
    return 0;
}
