// Compilar:
// mpicc -o chandy_lamport chandy_lamport.c -lpthread
// Executar:
// mpiexec -n 3 ./chandy_lamport

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <mpi.h>
#include <stdbool.h>
#include <string.h>

#define THREAD_NUM 3
#define BUFFER_SIZE 10
#define MAX_SNAPSHOTS 5

typedef struct {
    int clock[THREAD_NUM];
} VectorClock;

typedef struct {
    VectorClock vc;
    bool is_marker;
    int sender;
} Message;

typedef struct {
    VectorClock vc;
    Message channel_state[THREAD_NUM][BUFFER_SIZE];
    int channel_state_count[THREAD_NUM];
} ProcessState;

Message buffer_entrada[BUFFER_SIZE];
Message buffer_saida[BUFFER_SIZE];
int buffer_entrada_count = 0;
int buffer_saida_count = 0;

pthread_mutex_t mutex_entrada, mutex_saida, mutex_snapshot;
pthread_cond_t can_produce_entrada, can_consume_entrada;
pthread_cond_t can_produce_saida, can_consume_saida;

ProcessState local_state;
bool recording[THREAD_NUM] = {false};
bool snapshot_initiated = false;
int markers_received = 0;
int total_snapshots = 0;
bool should_terminate = false;

void print_vector_clock(VectorClock *vc, int rank) {
    printf("Processo %d - Relógio: [", rank);
    for (int i = 0; i < THREAD_NUM; i++) {
        printf("%d", vc->clock[i]);
        if (i < THREAD_NUM - 1) printf(", ");
    }
    printf("]\n");
}

void Event(int id, VectorClock *vc) {
    vc->clock[id]++;
}

void Send(Message *msg, int rank, int dest) {
    msg->sender = rank;
    printf("Processo %d: Enviando mensagem para o processo %d\n", rank, dest);
    MPI_Send(msg, sizeof(Message), MPI_BYTE, dest, 0, MPI_COMM_WORLD);
}

void Receive(Message *msg, int rank, int source) {
    MPI_Recv(msg, sizeof(Message), MPI_BYTE, source, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    printf("Processo %d: Mensagem recebida do processo %d\n", rank, source);
    
    if (!msg->is_marker) {
        for (int i = 0; i < THREAD_NUM; i++) {
            if (msg->vc.clock[i] > local_state.vc.clock[i]) {
                local_state.vc.clock[i] = msg->vc.clock[i];
            }
        }
        Event(rank, &local_state.vc);
    }
    
    print_vector_clock(&local_state.vc, rank);
}

void print_snapshot(int rank) {
    printf("\n=== SNAPSHOT DO PROCESSO %d ===\n", rank);
    printf("Estado Local:\n");
    print_vector_clock(&local_state.vc, rank);
    
    printf("Estado dos Canais:\n");
    for (int i = 0; i < THREAD_NUM; i++) {
        if (i != rank) {
            printf("Canal de %d para %d:\n", i, rank);
            for (int j = 0; j < local_state.channel_state_count[i]; j++) {
                printf("  Mensagem %d: ", j + 1);
                print_vector_clock(&local_state.channel_state[i][j].vc, local_state.channel_state[i][j].sender);
            }
            if (local_state.channel_state_count[i] == 0) {
                printf("  (Vazio)\n");
            }
        }
    }
    printf("==============================\n\n");
}

void initiate_snapshot(int rank) {
    pthread_mutex_lock(&mutex_snapshot);
    if (snapshot_initiated) {
        pthread_mutex_unlock(&mutex_snapshot);
        return;
    }
    snapshot_initiated = true;
    markers_received = 1;
    
    memset(&local_state, 0, sizeof(ProcessState));
    Event(rank, &local_state.vc);
    
    for (int i = 0; i < THREAD_NUM; i++) {
        if (i != rank) {
            Message marker = {{0}, true, rank};
            Send(&marker, rank, i);
            recording[i] = true;
        }
    }
    
    pthread_mutex_unlock(&mutex_snapshot);
    printf("Processo %d: Snapshot iniciado\n", rank);
}

void process_marker(int rank, int source) {
    pthread_mutex_lock(&mutex_snapshot);
    if (!snapshot_initiated) {
        initiate_snapshot(rank);
    }
    
    recording[source] = false;
    markers_received++;
    
    if (markers_received == THREAD_NUM) {
        print_snapshot(rank);
        
        snapshot_initiated = false;
        markers_received = 0;
        memset(recording, false, sizeof(recording));
        
        total_snapshots++;
        if (total_snapshots >= MAX_SNAPSHOTS) {
            printf("Processo %d: Número máximo de snapshots atingido.\n", rank);
            should_terminate = true;
        }
    }
    pthread_mutex_unlock(&mutex_snapshot);
}

void *thread_entrada(void *arg) {
    int rank = *(int *)arg;
    int source = (rank == 0) ? THREAD_NUM - 1 : rank - 1;
    
    while (!should_terminate) {
        Message msg;
        Receive(&msg, rank, source);

        if (msg.is_marker) {
            process_marker(rank, source);
        } else {
            pthread_mutex_lock(&mutex_snapshot);
            if (recording[source]) {
                int count = local_state.channel_state_count[source];
                if (count < BUFFER_SIZE) {
                    local_state.channel_state[source][count] = msg;
                    local_state.channel_state_count[source]++;
                }
            }
            pthread_mutex_unlock(&mutex_snapshot);
        }

        pthread_mutex_lock(&mutex_entrada);
        while (buffer_entrada_count == BUFFER_SIZE && !should_terminate) {
            pthread_cond_wait(&can_produce_entrada, &mutex_entrada);
        }
        if (!should_terminate) {
            buffer_entrada[buffer_entrada_count++] = msg;
            pthread_cond_signal(&can_consume_entrada);
        }
        pthread_mutex_unlock(&mutex_entrada);
    }
    return NULL;
}

void *thread_relogios(void *arg) {
    int rank = *(int *)arg;
    
    while (!should_terminate) {
        pthread_mutex_lock(&mutex_entrada);
        while (buffer_entrada_count == 0 && !should_terminate) {
            pthread_cond_wait(&can_consume_entrada, &mutex_entrada);
        }
        if (should_terminate) {
            pthread_mutex_unlock(&mutex_entrada);
            break;
        }
        Message msg = buffer_entrada[--buffer_entrada_count];
        pthread_cond_signal(&can_produce_entrada);
        pthread_mutex_unlock(&mutex_entrada);

        if (!msg.is_marker) {
            Event(rank, &msg.vc);
            print_vector_clock(&msg.vc, rank);
        }

        pthread_mutex_lock(&mutex_saida);
        while (buffer_saida_count == BUFFER_SIZE && !should_terminate) {
            pthread_cond_wait(&can_produce_saida, &mutex_saida);
        }
        if (!should_terminate) {
            buffer_saida[buffer_saida_count++] = msg;
            pthread_cond_signal(&can_consume_saida);
        }
        pthread_mutex_unlock(&mutex_saida);

        usleep(10000);  // Sleep for 10ms
    }
    return NULL;
}

void *thread_saida(void *arg) {
    int rank = *(int *)arg;
    int destination = (rank + 1) % THREAD_NUM;

    while (!should_terminate) {
        pthread_mutex_lock(&mutex_saida);
        while (buffer_saida_count == 0 && !should_terminate) {
            pthread_cond_wait(&can_consume_saida, &mutex_saida);
        }
        if (should_terminate) {
            pthread_mutex_unlock(&mutex_saida);
            break;
        }
        Message msg = buffer_saida[--buffer_saida_count];
        pthread_cond_signal(&can_produce_saida);
        pthread_mutex_unlock(&mutex_saida);

        Send(&msg, rank, destination);

        if (!msg.is_marker) {
            print_vector_clock(&msg.vc, rank);
        } else {
            printf("Processo %d: Marcador enviado para o processo %d\n", rank, destination);
        }

        usleep(10000);  // Sleep for 10ms
    }
    return NULL;
}

int main(int argc, char** argv) {
    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size != THREAD_NUM) {
        if (rank == 0) {
            printf("Este programa deve ser executado com exatamente %d processos.\n", THREAD_NUM);
        }
        MPI_Finalize();
        return 1;
    }

    pthread_t entrada_thread, relogios_thread, saida_thread;
    pthread_mutex_init(&mutex_entrada, NULL);
    pthread_mutex_init(&mutex_saida, NULL);
    pthread_mutex_init(&mutex_snapshot, NULL);
    pthread_cond_init(&can_produce_entrada, NULL);
    pthread_cond_init(&can_consume_entrada, NULL);
    pthread_cond_init(&can_produce_saida, NULL);
    pthread_cond_init(&can_consume_saida, NULL);

    pthread_create(&entrada_thread, NULL, thread_entrada, &rank);
    pthread_create(&relogios_thread, NULL, thread_relogios, &rank);
    pthread_create(&saida_thread, NULL, thread_saida, &rank);

    if (rank == 0) {
        sleep(1);  // Pequeno atraso inicial
        for (int i = 0; i < MAX_SNAPSHOTS; i++) {
            initiate_snapshot(rank);
            sleep(2);  // Espera 2 segundos entre snapshots
        }
    }

    pthread_join(entrada_thread, NULL);
    pthread_join(relogios_thread, NULL);
    pthread_join(saida_thread, NULL);

    pthread_mutex_destroy(&mutex_entrada);
    pthread_mutex_destroy(&mutex_saida);
    pthread_mutex_destroy(&mutex_snapshot);
    pthread_cond_destroy(&can_produce_entrada);
    pthread_cond_destroy(&can_consume_entrada);
    pthread_cond_destroy(&can_produce_saida);
    pthread_cond_destroy(&can_consume_saida);

    MPI_Finalize();
    return 0;
}
