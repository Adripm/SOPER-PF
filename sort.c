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

Status bubble_sort(int *vector, int n_elements, int delay)
{
    int i, j;
    int temp;

    if ((!(vector)) || (n_elements <= 0))
    {
        return ERROR;
    }

    for (i = 0; i < n_elements - 1; i++)
    {
        for (j = 0; j < n_elements - i - 1; j++)
        {
            /* Delay. */
            fast_sleep(delay);
            if (vector[j] > vector[j + 1])
            {
                temp = vector[j];
                vector[j] = vector[j + 1];
                vector[j + 1] = temp;
            }
        }
    }

    return OK;
}

Status merge(int *vector, int middle, int n_elements, int delay)
{
    int *aux = NULL;
    int i, j, k, l, m;

    if (!(aux = (int *)malloc(n_elements * sizeof(int))))
    {
        return ERROR;
    }

    for (i = 0; i < n_elements; i++)
    {
        aux[i] = vector[i];
    }

    i = 0;
    j = middle;
    for (k = 0; k < n_elements; k++)
    {
        /* Delay. */
        fast_sleep(delay);
        if ((i < middle) && ((j >= n_elements) || (aux[i] < aux[j])))
        {
            vector[k] = aux[i];
            i++;
        }
        else
        {
            vector[k] = aux[j];
            j++;
        }

        /* This part is not needed, and it is computationally expensive, but
        it allows to visualize a partial mixture. */
        m = k + 1;
        for (l = i; l < middle; l++)
        {
            vector[m] = aux[l];
            m++;
        }
        for (l = j; l < n_elements; l++)
        {
            vector[m] = aux[l];
            m++;
        }
    }

    free((void *)aux);
    return OK;
}

int get_number_parts(int level, int n_levels)
{
    /* The number of parts is 2^(n_levels - 1 - level). */
    return 1 << (n_levels - 1 - level);
}

Status init_sort(char *file_name, Sort *sort, int n_levels, int n_processes, int delay)
{
    char string[MAX_STRING];
    FILE *file = NULL;
    int i, j, log_data;
    int block_size, modulus;

    if ((!(file_name)) || (!(sort)))
    {
        fprintf(stderr, "init_sort - Incorrect arguments\n");
        return ERROR;
    }

    /* At most MAX_LEVELS levels. */
    sort->n_levels = MAX(1, MIN(n_levels, MAX_LEVELS));
    /* At most MAX_PARTS processes can work together. */
    sort->n_processes = MAX(1, MIN(n_processes, MAX_PARTS));
    /* The main process PID is stored. */
    sort->ppid = getpid();
    /* Delay for the algorithm in ns (less than 1s). */
    sort->delay = MAX(1, MIN(999999999, delay));

    if (!(file = fopen(file_name, "r")))
    {
        perror("init_sort - fopen");
        return ERROR;
    }

    /* The first line contains the size of the data, truncated to MAX_DATA. */
    if (!(fgets(string, MAX_STRING, file)))
    {
        fprintf(stderr, "init_sort - Error reading file\n");
        fclose(file);
        return ERROR;
    }
    sort->n_elements = atoi(string);
    if (sort->n_elements > MAX_DATA)
    {
        sort->n_elements = MAX_DATA;
    }

    /* The remaining lines contains one integer number each. */
    for (i = 0; i < sort->n_elements; i++)
    {
        if (!(fgets(string, MAX_STRING, file)))
        {
            fprintf(stderr, "init_sort - Error reading file\n");
            fclose(file);
            return ERROR;
        }
        sort->data[i] = atoi(string);
    }
    fclose(file);

    /* Each task should have at least one element. */
    log_data = compute_log(sort->n_elements);
    if (n_levels > log_data)
    {
        n_levels = log_data;
    }
    sort->n_levels = n_levels;

    /* The data is divided between the tasks, which are also initialized. */
    block_size = sort->n_elements / get_number_parts(0, sort->n_levels);
    modulus = sort->n_elements % get_number_parts(0, sort->n_levels);
    sort->tasks[0][0].completed = INCOMPLETE;
    sort->tasks[0][0].ini = 0;
    sort->tasks[0][0].end = block_size + (modulus > 0);
    sort->tasks[0][0].mid = NO_MID;
    for (j = 1; j < get_number_parts(0, sort->n_levels); j++)
    {
        sort->tasks[0][j].completed = INCOMPLETE;
        sort->tasks[0][j].ini = sort->tasks[0][j - 1].end;
        sort->tasks[0][j].end = sort->tasks[0][j].ini + block_size + (modulus > j);
        sort->tasks[0][j].mid = NO_MID;
    }
    for (i = 1; i < n_levels; i++)
    {
        for (j = 0; j < get_number_parts(i, sort->n_levels); j++)
        {
            sort->tasks[i][j].completed = INCOMPLETE;
            sort->tasks[i][j].ini = sort->tasks[i - 1][2 * j].ini;
            sort->tasks[i][j].mid = sort->tasks[i - 1][2 * j].end;
            sort->tasks[i][j].end = sort->tasks[i - 1][2 * j + 1].end;
        }
    }

    return OK;
}

Bool check_task_ready(Sort *sort, int level, int part)
{
    if (!(sort))
    {
        return FALSE;
    }

    if ((level < 0) || (level >= sort->n_levels) || (part < 0) || (part >= get_number_parts(level, sort->n_levels)))
    {
        return FALSE;
    }

    if (sort->tasks[level][part].completed != INCOMPLETE)
    {
        return FALSE;
    }

    /* The tasks of the first level are always ready. */
    if (level == 0)
    {
        return TRUE;
    }

    /* Other tasks depend on the hierarchy. */
    if ((sort->tasks[level - 1][2 * part].completed == COMPLETED) &&
        (sort->tasks[level - 1][2 * part + 1].completed == COMPLETED))
    {
        return TRUE;
    }

    return FALSE;
}

Status solve_task(Sort *sort, int level, int part)
{
    /* In the first level, bubble-sort. */
    if (sort->tasks[level][part].mid == NO_MID)
    {
        return bubble_sort(
            sort->data + sort->tasks[level][part].ini,
            sort->tasks[level][part].end - sort->tasks[level][part].ini,
            sort->delay);
    }
    /* In other levels, merge. */
    else
    {
        return merge(
            sort->data + sort->tasks[level][part].ini,
            sort->tasks[level][part].mid - sort->tasks[level][part].ini,
            sort->tasks[level][part].end - sort->tasks[level][part].ini,
            sort->delay);
    }
}

Status sort_single_process(char *file_name, int n_levels, int n_processes, int delay)
{
    int i, j;
    Sort sort;

    /* The data is loaded and the structure initialized. */
    if (init_sort(file_name, &sort, n_levels, n_processes, delay) == ERROR)
    {
        fprintf(stderr, "sort_single_process - init_sort\n");
        return ERROR;
    }

    plot_vector(sort.data, sort.n_elements);
    printf("\nStarting algorithm with %d levels and %d processes...\n", sort.n_levels, sort.n_processes);
    /* For each level, and each part, the corresponding task is solved. */
    for (i = 0; i < sort.n_levels; i++)
    {
        for (j = 0; j < get_number_parts(i, sort.n_levels); j++)
        {
            solve_task(&sort, i, j);
            plot_vector(sort.data, sort.n_elements);
            printf("\n%10s%10s%10s%10s%10s\n", "PID", "LEVEL", "PART", "INI",
                   "END");
            printf("%10d%10d%10d%10d%10d\n", getpid(), i, j,
                   sort.tasks[i][j].ini, sort.tasks[i][j].end);
        }
    }

    plot_vector(sort.data, sort.n_elements);
    printf("\nAlgorithm completed\n");

    return OK;
}

void usr1_handler_func(int sig)
{
    /*printf("Señal %d recibida\n",sig);*/
}

Status sort_multi_process(char *file_name, int n_levels, int n_processes, int delay)
{
    Sort sort;
    Sort *sort_pointer = &sort;
    int fd_shm;
    struct sigaction handler_usr1;
    mqd_t queue;
    struct mq_attr attributes;
    /*int fd_trabajadores[n_processes][2];
    int fd_ilustrador[n_processes][2];*/ /* ISO C90 forbids variable length array, allocate memory instead */
    sem_t *sem_file;
    int i, j; /*status_pipe;*/
    sigset_t process_mask, empty_set;
    Bool bucle_principal_interno = TRUE;
    pid_t* trabajadores; /* Lista de PIDs de los trabajadores */

    attributes.mq_maxmsg = 10;
    attributes.mq_msgsize = sizeof(Mensaje);

    sigemptyset(&process_mask);
    sigaddset(&process_mask,SIGUSR1);
    sigemptyset(&empty_set);

    sigprocmask(SIG_BLOCK,&process_mask,NULL);

    /* Inicializar cola de mensajes */
    queue = mq_open(MQ_NAME, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, &attributes);
    if (queue == (mqd_t)-1)
    {
        perror("");
        fprintf(stderr, "Error opening the queue.\n");
        return ERROR;
    }

    /* Inicializar la estructura sort en memoria compartida */
    if (init_sort(file_name, &sort, n_levels, n_processes, delay) == ERROR)
    {
        fprintf(stderr, "sort_multi_process - init_sort\n");
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
    if (ftruncate(fd_shm, MAX_DATA) == -1)
    { /* @PLACEHOLDER - Comprobar tamaño necesitado */
        fprintf(stderr, "Error resizing the shared memory segment\n");
        shm_unlink(SHM_NAME);
        return ERROR;
    }

    /* Mapear segmento de memoria al proceso principal y cerrar el descriptor de fichero de la memoria compartida */
    sort_pointer = mmap(NULL, sizeof(*sort_pointer), PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
    close(fd_shm);
    if (sort_pointer == MAP_FAILED)
    {
        fprintf(stderr, "Error mapping the shared memory segment\n");
        shm_unlink(SHM_NAME);
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

    /*Crear semáforo*/
    sem_file = sem_open(SEM_NAME, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 1);

    /*Crear pipes*/

    /*for (i = 0; i < n_processes; i++)
    {
        status_pipe = pipe(fd_trabajadores[i]);
        if (status_pipe == -1)
        {
            perror("Error creando la tuberia\n");
            exit(EXIT_FAILURE);
        }
    }*/

    /*for (i = 0; i < n_processes; i++)
    {
        status_pipe = pipe(fd_ilustrador[i]);
        if (status_pipe == -1)
        {
            perror("Error creando la tuberia\n");
            exit(EXIT_FAILURE);
        }
    }*/

    /* Iniciar trabajadores */
    /* ################################### */
    trabajadores = (pid_t*) malloc(sizeof(pid_t)*n_processes);
    if(!trabajadores){
        return ERROR;
    }
    for(i=0;i<n_processes;i++){
        trabajadores[i]=new_worker(sort_pointer,sem_file);
    }
    /* ################################### */

    /* Bucle del proceso principal */
    printf("PID Proceso principal %d\n",getpid());
    for(i=0;i<sort.n_levels;i++)
    {

        printf("-------------Nivel %d-------------\n",i);

        bucle_principal_interno = TRUE;

        /* Encontrar tareas en nivel correspondiente */
        for(j=0;j<get_number_parts(i, sort.n_levels);j++){
            /* Enviar tareas a cola de mensajes */
            Mensaje new_msg;
            new_msg.level=i;
            new_msg.part=j;

            printf("Enviando tarea %d del nivel %d\n",j,i);
            mq_send(queue,(char*)&new_msg,sizeof(new_msg),0);
            printf("Tarea enviada\n");

            /* Indicar tarea como SENT */
            sort_pointer->tasks[i][j].completed = SENT;

        }

        while (bucle_principal_interno==TRUE)
        {

            printf("Proceso principal bloqueado hasta recibir SIGUSR1\n");
            /* Desbloquea señal SIGUSR1 */
            /* Bloquear proceso hasta señal SIGUSR1 */
            sigsuspend(&empty_set);
            printf("Proceso principal reanudado\n");

            /* Comprobar si las tareas en el nivel se han terminado */
            bucle_principal_interno = FALSE;
            for(j=0;j<get_number_parts(i,sort.n_levels);j++){
                if(sort.tasks[i][j].completed!=COMPLETED){
                    bucle_principal_interno = TRUE;
                    printf("Todavía existen tareas en este nivel\n");
                    break;
                }
            }

        }
    }

    /* Cleanup */ /* @PLACEHOLDER - Pasar a una funcion que maneje la salida del proceso */

    /* Cerrar los trabajadores */
    for(i=0;i<n_processes;i++){
        kill(trabajadores[i],SIGTERM);
        waitpid(trabajadores[i],NULL,0);
    }
    free(trabajadores);

    /* Cerrar la cola de mensajes */
    mq_close(queue);
    mq_unlink(MQ_NAME);

    /* Cerrar memoria compartida */
    munmap(sort_pointer, sizeof(*sort_pointer));
    shm_unlink(SHM_NAME);

    /* Cerrar el semaforo */
    sem_close(sem_file);
    sem_unlink(SEM_NAME);

    return OK;
}
