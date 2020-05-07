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

Sort* sort_pointer = NULL;
sem_t* sem = NULL;
mqd_t queue;

void terminate_worker(){
    munmap(sort_pointer,sizeof(*sort_pointer));
    mq_close(queue);
    sem_close(sem);
    exit(EXIT_SUCCESS);
}

void alarm_handler_func(int sig)
{
    /* Cuando llega la señal SIGALRM se enviará de nuevo un segundo después */
    alarm(1);
}

void term_handler_func(int sig){
    terminate_worker();
}

pid_t new_worker(Sort* shm_map_segment, sem_t* semaphore)
{

    pid_t pid;

    pid = fork();
    if (pid == 0)
    {
        struct sigaction handler_alarm, handler_term;
        Bool bucle_trabajador = TRUE;

        /* La memoria compartida ya está mapeada en este proceso */
        /* porque se hereda del proceso padre */
        sort_pointer = shm_map_segment;

        /* Semaforo */
        sem = semaphore;

        /* Inicializar el manejador para la señal SIGALARM*/
        /* Mandar una sñal SIGALARM cada segundo*/
        handler_alarm.sa_handler = alarm_handler_func; /* funcion manejador */
        sigemptyset(&(handler_alarm.sa_mask));
        handler_alarm.sa_flags = 0;

        if (sigaction(SIGALRM, &handler_alarm, NULL) < 0)
        {
            perror("sigaction");
            terminate_worker();
        }

        /* Inicializar el manejador para la señal SIGTERM */
        /* Debe terminar la ejecucion del bucle del trabajador */
        handler_term.sa_handler = term_handler_func;
        sigemptyset(&(handler_alarm.sa_mask));
        handler_alarm.sa_flags = 0;

        if(sigaction(SIGTERM, &handler_term, NULL) < 0){
            perror("sigaction");
            terminate_worker();
        }

        /* Abrir la cola de mensajes */
        /* O_CREAT no se especifica, una cola con ese nombre ya debe existir */
        queue = mq_open(MQ_NAME,O_RDONLY);
        if(queue==(mqd_t)-1){
            perror("");
            fprintf(stderr,"Error opening the queue.\n");
            terminate_worker();
        }

        /* Inicia el bucle de señales SIGALARM */
        alarm(1);

        /* Bucle del proceso trabajador */
        while(bucle_trabajador){
            /* Esperar una tarea - BLOCK */
            Mensaje new_task;
            Status result;

            if(mq_receive(queue,(char*)&new_task,sizeof(new_task),NULL)==-1){
                fprintf(stderr,"Error reading new task on worker %d\n",getpid());
                terminate_worker();
            }

            /* Indicar tarea como PROCESSING */
            sort_pointer->tasks[new_task.level][new_task.part].completed = PROCESSING;

            /* Resolver tarea - CONCURRENCIA */
            sem_wait(sem);
            result = solve_task(sort_pointer, new_task.level, new_task.part);
            sem_post(sem);

            if(result==ERROR){
                sort_pointer->tasks[new_task.level][new_task.part].completed = INCOMPLETE;
            }else{
                sort_pointer->tasks[new_task.level][new_task.part].completed = COMPLETED;
                kill(getppid(),SIGUSR1);
            }

        }

        terminate_worker();
    }

    return pid;
}
