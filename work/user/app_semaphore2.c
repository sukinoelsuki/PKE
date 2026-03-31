 /*
* This app create two child process.
* Use semaphores to control the order of
* the main process and two child processes print info. 
*/
#include "user/user_lib.h"
#include "util/types.h"

int main(void) {
    int sem_resouce;
    sem_resouce = sem_new(0); 
    int pid = fork();
    if (pid == 0) {
        pid = fork();
        printu("Product resource\n");
        sem_V(sem_resouce);
    } else {
        // apply for two resources to work
        for (int i = 0; i < 2; i++) {
            sem_P(sem_resouce);
            printu("Parent got %d source\n", i);
        }
        printu("Parent working\n");
        // return resources
        sem_V(sem_resouce);
        sem_V(sem_resouce);
        printu("Finished\n");
    }
    exit(0);
    return 0;
}
