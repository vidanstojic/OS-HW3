#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
    int shm = shm_open("shm_demo");
    int provera = shm_trunc(shm, 4096);
    int shm2 = shm_open("shm_demo2");
    int provera2 = shm_trunc(shm2, 8096);
    int shm3 = shm_open("shm_demo3");
    int provera3 = shm_trunc(shm3, 4096);
    int shm4 = shm_open("shm_demo");
    int provera4 = shm_trunc(shm4, 4096);
    if(shm < 0)
    {
        printf("failed to open shm\n");
        exit();
    }
    //int provera = shm_trunc(shm, 4096);
    printf("provera: %d %d %d %d\n", provera, provera2, provera3, provera4);
    printf("provera shm: %d %d %d %d\n", shm, shm2, shm3, shm4);
    /*void* shm_reg_;
    if(shm_map(shm, &shm_reg_, O_RDONLY) < 0)
    {
        printf("failed to map shm\n");
        exit();
    }
    const char* shm_reg = shm_reg_;
    printf("message: %s\n", shm_reg);*/
    exit();
}
