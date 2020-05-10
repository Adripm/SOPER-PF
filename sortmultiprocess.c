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

int num_workers;
pid_t* trabajadores; /* Lista de PIDs de los trabajadores */
pid_t* printer;
mqd_t queue;
Sort *sort_pointer;
sem_t *sem_file = NULL;
int alarm_flag = 0;

void terminate_process()
{
    int i;
    /* Cerrar los trabajadores */
    for (i = 0; i < num_workers; i++)
    {
        kill(trabajadores[i], SIGTERM);
        waitpid(trabajadores[i], NULL, 0);
    }
    free(trabajadores);

    /* Cerrar el printer */
    kill(*printer,SIGTERM);
    waitpid(*printer, NULL, 0);
    free(printer);

    /* Cerrar la cola de mensajes */
    mq_close(queue);
    mq_unlink(MQ_NAME);

    /* Cerrar memoria compartida */
    munmap(sort_pointer, sizeof(*sort_pointer));
    shm_unlink(SHM_NAME);

    /* Cerrar el semaforo */
    sem_close(sem_file);
    sem_unlink(SEM_NAME);

    /* Cerrar semáforo printer */
    sem_unlink(SEM_PRINTER);

    exit(EXIT_SUCCESS);
}

void usr1_handler_func(int sig)
{
    /*printf("Señal %d recibida\n",sig);*/
}

void int_handler_func(int sig)
{
#ifdef DEBUG
    printf("Señal %d recibida. Terminando proceso...\n", sig);
#endif

    terminate_process();
}

void alarm_handler_func_main(int sig){
    alarm_flag = 1;
}

Status sort_multi_process(char *file_name, int n_levels, int n_processes, int delay)
{
    int fd_shm;
    struct sigaction handler_usr1, handler_int, handler_alarm;
    struct mq_attr attributes;
    int i, j;
    sigset_t process_mask, empty_set;
    Bool bucle_principal_interno = TRUE;

    num_workers = n_processes;
    attributes.mq_maxmsg = 10;
    attributes.mq_msgsize = sizeof(Mensaje);

    sigemptyset(&process_mask);
    sigaddset(&process_mask, SIGUSR1);
    sigaddset(&process_mask, SIGALRM);
    sigemptyset(&empty_set);

    sigprocmask(SIG_BLOCK, &process_mask, NULL);

    /* Inicializar cola de mensajes */
    queue = mq_open(MQ_NAME, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, &attributes);
    if (queue == (mqd_t)-1)
    {
        perror("");
        fprintf(stderr, "Error opening the queue.\n");
        return ERROR;
    }

    /* Crear memoria compartida */
    fd_shm = shm_open(SHM_NAME, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd_shm == -1)
    {
        fprintf(stderr, "Error creating the shared memory segment\n");
        return ERROR;
    }

    /* Redimensionar memoria compartida */
    if (ftruncate(fd_shm, sizeof(Sort)) == -1)
    { /* @PLACEHOLDER - Comprobar tamaño necesitado */
        fprintf(stderr, "Error resizing the shared memory segment\n");
        shm_unlink(SHM_NAME);
        return ERROR;
    }

    /* Mapear segmento de memoria al proceso principal y cerrar el descriptor de fichero de la memoria compartida */
    sort_pointer = (Sort *)mmap(NULL, sizeof(Sort), PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
    close(fd_shm);
    if (sort_pointer == MAP_FAILED)
    {
        fprintf(stderr, "Error mapping the shared memory segment\n");
        shm_unlink(SHM_NAME);
        return ERROR;
    }

    /* Inicializar la estructura sort en memoria compartida */
    if (init_sort(file_name, sort_pointer, n_levels, n_processes, delay) == ERROR)
    {
        fprintf(stderr, "sort_multi_process - init_sort\n");
        return ERROR;
    }

    /* Inicializar manejador del proceso principal para la señal SIGUSR1 */
    handler_usr1.sa_handler = usr1_handler_func; /* funcion manejador */
    sigemptyset(&(handler_usr1.sa_mask));
    handler_usr1.sa_flags = 0;

    if (sigaction(SIGUSR1, &handler_usr1, NULL) < 0)
    {
        perror("sigaction");
        return ERROR;
    }

    /* Inicializar el manejador del proceso principal para la señal SIGUSR1 */
    handler_int.sa_handler = int_handler_func;
    sigemptyset(&(handler_int.sa_mask));
    handler_int.sa_flags = 0;

    if (sigaction(SIGINT, &handler_int, NULL) < 0)
    {
        perror("sigaction");
        return ERROR;
    }

    /* Inicializar el manejador del proceso principal para la señal SIGALRM */
    handler_alarm.sa_handler = alarm_handler_func_main;
    sigemptyset(&(handler_alarm.sa_mask));
    handler_alarm.sa_flags = 0;

    if (sigaction(SIGALRM, &handler_alarm, NULL) < 0)
    {
        perror("sigaction");
        return ERROR;
    }

    /*Crear semáforo*/
    sem_file = sem_open(SEM_NAME, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 1);
    if (sem_file == SEM_FAILED)
    {
        terminate_process();
    }

    /* Crear semáforo printer */
    if(sem_open(SEM_PRINTER, O_CREAT|O_EXCL, S_IRUSR|S_IWUSR, 0)==SEM_FAILED){
        terminate_process();
    }

    /* Iniciar trabajadores */
    /* ################################### */
    trabajadores = (pid_t *)malloc(sizeof(pid_t) * n_processes);
    if (!trabajadores)
    {
        terminate_process();
    }
    for (i = 0; i < n_processes; i++)
    {
        trabajadores[i] = new_worker(sort_pointer);
    }
    /* ################################### */

    /* Inicializar printer */
    printer = (pid_t*) malloc(sizeof(pid_t));
    *printer = new_printer(sort_pointer);

    /* Iniciar alarma */
    alarm(1);

    /* Bucle del proceso principal */
#ifdef DEBUG
    printf("PID Proceso principal %d\n", getpid());
#endif

    for (i = 0; i < sort_pointer->n_levels; i++)
    {
#ifdef DEBUG
        printf("-------------Nivel %d-------------\n", i);
#endif

        bucle_principal_interno = TRUE;

        /* Encontrar tareas en nivel correspondiente */
        for (j = 0; j < get_number_parts(i, sort_pointer->n_levels); j++)
        {
            /* Enviar tareas a cola de mensajes */
            Mensaje new_msg;
            new_msg.level = i;
            new_msg.part = j;

#ifdef DEBUG
            printf("Enviando tarea %d del nivel %d\n", j, i);
#endif

            /* Si los trabajadores resuelven la tarea antes de que el proceso principal la marque como enviada ocurrirá un error */
            /* Por ello los trabajadores esperaran al semaforo despues de leer la tarea */

            sem_wait(sem_file);
            mq_send(queue, (char *)&new_msg, sizeof(new_msg), 0);
            sort_pointer->tasks[i][j].completed = SENT; /* Indicar tarea como SENT */
            sem_post(sem_file);

#ifdef DEBUG
            printf("Tarea enviada\n");
#endif
        }

        while (bucle_principal_interno == TRUE)
        {

#ifdef DEBUG
            printf("Proceso principal bloqueado hasta recibir SIGUSR1\n");
#endif
            /* Desbloquea señal SIGUSR1 */
            /* Bloquear proceso hasta señal SIGUSR1 */
            sigsuspend(&empty_set);
            /* Se vuelven a bloquar las señales USR1 que se puedan recibir durante la comprobacion */

#ifdef DEBUG
            printf("Proceso principal reanudado\n");
#endif

            if(alarm_flag==1){
                int r;
                for(r=0;r<num_workers;r++){
                    kill(trabajadores[i], SIGALRM);
                }
                alarm_flag=0;
                alarm(1);
            }

            /* Comprobar si las tareas en el nivel se han terminado */
            bucle_principal_interno = FALSE;
            for (j = 0; j < get_number_parts(i, sort_pointer->n_levels); j++)
            {
                if (sort_pointer->tasks[i][j].completed != COMPLETED)
                {
                    bucle_principal_interno = TRUE;

#ifdef DEBUG
                    printf("Todavía existen tareas en este nivel (Nivel %d, Tarea %d)\n", i, j);
                    printf("El estado de la tarea es: :%d\n", sort_pointer->tasks[i][j].completed);
#endif

                    if (sort_pointer->tasks[i][j].completed == INCOMPLETE)
                    {
                        /* Si la tarea está incompleta, se volverá a mandar */
                        /* No debería de ocurrir en condiciones normales */
                        /* El estado incompleto solo se indica si solve_task ha retornado un error */
                        /* Por lo tanto, aunque se vuelva a mandar la misma tarea, nunca habrá más de un proceso resolviéndola */

                        Mensaje new_msg;
                        new_msg.level = i;
                        new_msg.part = j;

                        sem_wait(sem_file);
                        mq_send(queue, (char *)&new_msg, sizeof(new_msg), 0);
                        sort_pointer->tasks[i][j].completed = SENT;
                        sem_post(sem_file);

#ifdef DEBUG
                        printf("La tarea %d del nivel %d ha sido enviada de nuevo porque estaba incompleta", j, i);
#endif
                    }

                    break;
                }
            }
        }
#ifdef DEBUG

       printf("Siguiente nivel de tareas\n");
#endif
    }

    /*printf("Algoritmo finalizado. Resultado:\n");
    for (i = 0; i<sort_pointer->n_elements; i++)
    {
        printf("%d ", sort_pointer->data[i]);
    }*/

    /* Cleanup */ /* Funcion que maneja la salida del proceso */
    terminate_process();

    return OK;
}
