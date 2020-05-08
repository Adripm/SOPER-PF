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

pid_t new_worker(Sort* shm_map_segment)
{

    pid_t pid;

    pid = fork();
    if (pid == 0)
    {
        struct sigaction handler_alarm, handler_term;
        Bool bucle_trabajador = TRUE;
        sigset_t waiting_message_set, empty_set;

        pid_t self_pid;

        /* La memoria compartida ya está mapeada en este proceso */
        /* porque se hereda del proceso padre */
        sort_pointer = shm_map_segment;

        /* Semaforo - El semáforo con ese nombre YA DEBE EXISTIR */
        sem = sem_open(SEM_NAME,0);
        if(sem==SEM_FAILED){
            terminate_worker();
        }

        /* Signal masks */
        sigemptyset(&empty_set);
        sigemptyset(&waiting_message_set);
        sigaddset(&waiting_message_set, SIGALRM);

        sigprocmask(SIG_BLOCK, &empty_set, NULL);

        /* Testing */
        self_pid = getpid();

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

        printf("Trabajador %d entrando en bucle\n",self_pid);
        /* Bucle del proceso trabajador */
        while(bucle_trabajador){
            /* Esperar una tarea - BLOCK */
            Mensaje new_task;
            Status result = ERROR;

            /* Mientras lee un mensaje bloqueará las señales SIGALRM */
            sigprocmask(SIG_BLOCK, &waiting_message_set, NULL);
            printf("Trabajador %d espera por una tarea\n",self_pid);
            if(mq_receive(queue,(char*)&new_task,sizeof(new_task),NULL)==-1){
                fprintf(stderr,"Error reading new task on worker %d\n",self_pid);
                /*terminate_worker();*/
                continue;
            }
            printf("Trabajador %d ha leido una tarea\n",self_pid);
            sigprocmask(SIG_BLOCK, &empty_set, NULL);
            /* Una vez lee el mensaje, desbloquea las señales*/

            /* Indicar tarea como PROCESSING */
            sort_pointer->tasks[new_task.level][new_task.part].completed = PROCESSING;

            printf("Trabajador %d espera para poder acceder al archivo\n",self_pid);
            /* Resolver tarea - CONCURRENCIA */
            sem_wait(sem);
            printf("Trabajador %d resuelve la tarea\n",self_pid);

            /*result = solve_task(sort_pointer, new_task.level, new_task.part);*/
            printf(" -> Trabajador %d resolverá una tarea\n",self_pid);
            result = OK;

            sem_post(sem);
            printf("Trabajador %d libera el archivo\n",self_pid);

            if(result==ERROR){
                sort_pointer->tasks[new_task.level][new_task.part].completed = INCOMPLETE;
            }else{
                sort_pointer->tasks[new_task.level][new_task.part].completed = COMPLETED;
                printf("Trabajador %d ha resuelto la tarea %d del nivel %d\n",self_pid,new_task.part,new_task.level);
                printf("Trabajador %d envía señal SIGUSR1 a proceso principal\n",self_pid);
                kill(getppid(),SIGUSR1);
            }

        }

        terminate_worker();
    }

    return pid;
}
