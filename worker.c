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

sem_t* sem;
sem_t* sem_printer;
mqd_t queue;
Sort* shm_sort;

void terminate_worker(){
    mq_close(queue);
    sem_close(sem);
    sem_close(sem_printer);
    munmap(shm_sort,sizeof(*shm_sort));
    exit(EXIT_SUCCESS);
}

void alarm_handler_func(int sig)
{
    /* Una vez ha terminado la tarea, se permite trabajar al ilustrador */
    sem_post(sem_printer);
    /* Cuando llega la señal SIGALRM se enviará de nuevo un segundo después */
    alarm(1);
}

void term_handler_func(int sig){
    terminate_worker();
}

pid_t new_worker(Sort* sort_pointer, int* printer_pipe)
{

    pid_t pid;

    pid = fork();
    if (pid == 0)
    {
        struct sigaction handler_alarm, handler_term;
        Bool bucle_trabajador = TRUE;
        sigset_t waiting_message_set, default_set;
        pid_t self_pid;

        /* Signal masks */
        sigemptyset(&default_set);
        sigemptyset(&waiting_message_set);

        sigaddset(&waiting_message_set, SIGINT);
        sigaddset(&waiting_message_set, SIGALRM);
        sigaddset(&default_set, SIGINT);

        sigprocmask(SIG_BLOCK, &default_set, NULL);

        /* La estructura Sort ya está mapeada en este proceso */
        shm_sort = sort_pointer;

        /* Semaforo - El semáforo con ese nombre YA DEBE EXISTIR */
        sem = sem_open(SEM_NAME,0);
        if(sem==SEM_FAILED){
            terminate_worker();
        }

        /* Semáforo - PRINTER */
        sem_printer = sem_open(SEM_PRINTER,0);
        if(sem_printer==SEM_FAILED){
            terminate_worker();
        }

        /* Debug */
        self_pid = getpid();

        /* Printer pipe */
        /* Cerrar extremo de lectura */
        /*close(printer_pipe[1]);*/

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
        sigemptyset(&(handler_term.sa_mask));
        handler_term.sa_flags = 0;

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

        #ifdef DEBUG
        printf("Trabajador %d entrando en bucle\n",self_pid);
        #endif

        /* Bucle del proceso trabajador */
        while(bucle_trabajador){
            /* Esperar una tarea - BLOCK */
            Mensaje new_task;
            Status result = ERROR;

            /* Mientras lee un mensaje bloqueará las señales SIGALRM */
            sigprocmask(SIG_BLOCK, &waiting_message_set, NULL);

            #ifdef DEBUG
            printf("Trabajador %d espera por una tarea\n",self_pid);
            #endif

            if(mq_receive(queue,(char*)&new_task,sizeof(new_task),NULL)==-1){
                fprintf(stderr,"Error reading new task on worker %d\n",self_pid);
                /*terminate_worker();*/
                continue;
            }

            #ifdef DEBUG
            printf("Trabajador %d ha leido una tarea\n",self_pid);
            #endif

            /* Resolver tarea - CONCURRENCIA */
            /* Nunca existirá concurrencia entre las tareas si los trabajadores acceden a diferentes tareas */
            /* Pero si podría existir concurrencia entre el proceso principal y el trabajador */
            sem_wait(sem);
            sem_post(sem);

            sigprocmask(SIG_BLOCK, &default_set, NULL);
            /* Una vez lee el mensaje, desbloquea las señales*/

            /* Indicar tarea como PROCESSING */
            sort_pointer->tasks[new_task.level][new_task.part].completed = PROCESSING;

            #ifdef DEBUG
            printf("Trabajador %d espera para poder acceder al archivo\n",self_pid);
            #endif

            #ifdef DEBUG
            printf("Trabajador %d resuelve la tarea\n",self_pid);
            #endif

            result = solve_task(sort_pointer, new_task.level, new_task.part);

            #ifdef DEBUG
            printf("Trabajador %d libera el archivo\n",self_pid);
            #endif

            if(result==OK){
                sort_pointer->tasks[new_task.level][new_task.part].completed = COMPLETED;
            }else{
                sort_pointer->tasks[new_task.level][new_task.part].completed = INCOMPLETE;
            }

            #ifdef DEBUG
            printf("Trabajador %d ha terminado la tarea %d del nivel %d\n",self_pid,new_task.part,new_task.level);
            printf("Trabajador %d envía señal SIGUSR1 a proceso principal\n",self_pid);
            #endif

            kill(getppid(),SIGUSR1);

        }

        terminate_worker();
    }

    return pid;
}
