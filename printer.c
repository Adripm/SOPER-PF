#define _POSIX_C_SOURCE 200112L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mqueue.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "sort.h"
#include "utils.h"

Sort* shm_sort;
sem_t* sem_printer;

void terminate_printer(){
    sem_close(sem_printer);
    munmap(shm_sort,sizeof(*shm_sort));
    exit(EXIT_SUCCESS);
}

void term_handler_printer_func(int sig){
    terminate_printer();
}

pid_t new_printer(Sort* sort_pointer, int* printer_pipe){

    pid_t pid;

    pid = fork();
    if(pid == 0){

        struct sigaction handler_term;
        Bool bucle_printer = TRUE;

        /* La estructura Sort ya está mapeada en este proceso */
        shm_sort = sort_pointer;

        /* Inicializar el manejador para la señal SIGTERM */
        handler_term.sa_handler = term_handler_printer_func;
        sigemptyset(&(handler_term.sa_mask));
        handler_term.sa_flags = 0;

        if(sigaction(SIGTERM, &handler_term, NULL) < 0){
            perror("sigaction");
            terminate_printer();
        }

        /* Printer pipe */
        /* Cerrar el extremo de escritura */
        /*close(printer_pipe[0]);*/

        /* Abrir semáforo del printer */
        /* El semáforo ya debe existir */
        sem_printer = sem_open(SEM_PRINTER,0);
        if(sem_printer==SEM_FAILED){
            terminate_printer();
        }

        #ifdef DEBUG
        printf("Ilustrador %d entrando en bucle\n",getpid());
        #endif

        while(bucle_printer){

            #ifdef DEBUG
            printf("Ilustrador espera\n");
            #endif
            /* El ilustrador espera a que los procesos trabajadores */
            /* abran el semaforo tras actualizar una tarea para dibujar */
            sem_wait(sem_printer);

            plot_vector(sort_pointer->data, sort_pointer->n_elements);

        }

    }

    return pid;

}
