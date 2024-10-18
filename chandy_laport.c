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
    bool channel_state[THREAD_NUM]; // Armazena estado dos canais
} ProcessState;

pthread_mutex_t mutex_snapshot;
bool recording[THREAD_NUM] = {false};
bool snapshot_initiated = false;
int markers_received = 0;
int total_snapshots = 0;
bool should_terminate = false;
ProcessState local_state;


int MAX_TIME = 8;

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
    Event(rank, &msg->vc);
    msg->sender = rank;
    printf("Processo %d: Enviando mensagem para o processo %d\n", rank, dest);
    MPI_Send(msg, sizeof(Message), MPI_BYTE, dest, 0, MPI_COMM_WORLD);
}

void Receive(Message *msg, int rank, int source) {
    MPI_Recv(msg, sizeof(Message), MPI_BYTE, source, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    Event(rank, &msg->vc);
    printf("Processo %d: Mensagem recebida do processo %d\n", rank, source);

}

void print_snapshot(int rank) {
    printf("\n=== SNAPSHOT DO PROCESSO %d ===\n", rank);
    printf("Estado Local:\n");
    print_vector_clock(&local_state.vc, rank);
    printf("Estado dos Canais:\n");
    for (int i = 0; i < THREAD_NUM; i++) {
        if (i != rank) {
            printf("Canal de %d para %d: %s\n", i, rank, local_state.channel_state[i] ? "Ativo" : "Inativo");
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

    // Armazena o estado local
    memset(&local_state, 0, sizeof(ProcessState));
    Event(rank, &local_state.vc);

    // Envia marcadores para os outros processos
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

    // Se já estamos gravando, então finalizamos a gravação do canal do remetente
    recording[source] = false;
    markers_received++;

    // Se todos os marcadores foram recebidos, imprime o snapshot
    if (markers_received == THREAD_NUM) {
        print_snapshot(rank);

        snapshot_initiated = false;
        markers_received = 0;
        memset(recording, false, sizeof(recording));
        total_snapshots++;

        if (total_snapshots >= 5) { // Limite de snapshots
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
        if (rank == 0){
           Receive(&msg, rank, 1);
           Receive(&msg, rank, 2);
        } else if (rank == 1){
            Receive(&msg, rank, 0);
            Receive(&msg, rank, 0);
        } else if (rank == 2){
            Receive(&msg, rank, 0);
        }

        pthread_mutex_lock(&mutex_snapshot);
        if (msg.is_marker) {
            process_marker(rank, source);
        } else {
            // Atualiza o relógio vetorial com a mensagem recebida
            for (int i = 0; i < THREAD_NUM; i++) {
                if (msg.vc.clock[i] > local_state.vc.clock[i]) {
                    local_state.vc.clock[i] = msg.vc.clock[i];
                }
            }
            // Se estamos gravando o estado do canal do remetente, salva a mensagem no estado do canal
            if (recording[source]) {
                local_state.channel_state[source] = true;
            }
        }
        pthread_mutex_unlock(&mutex_snapshot);

        print_vector_clock(&local_state.vc, rank);
    }
    return NULL;
}

void *thread_saida(void *arg) {
    int rank = *(int *)arg;
    int destination = (rank + 1) % THREAD_NUM;

    while (!should_terminate) {
        Message msg;
        pthread_mutex_lock(&mutex_snapshot);
        msg.vc = local_state.vc;
        msg.is_marker = false;
        pthread_mutex_unlock(&mutex_snapshot);

        if (rank == 0){
           Send(&msg, rank, 1);
           Send(&msg, rank, 2);
           Send(&msg, rank, 1);
        } else if (rank == 1){
           Send(&msg, rank, 0);
        } else if (rank == 2){
           Send(&msg, rank, 0);
        }
        
        sleep(1);  // Delay para simular processamento
    }
    return NULL;
}

void *thread_relogios(void *arg) {
    int rank = *(int *)arg;

    while (!should_terminate) {
        pthread_mutex_lock(&mutex_snapshot);
        // Evento interno, atualizando o relógio vetorial
        Event(rank, &local_state.vc);
        if (rank == 0){
           Event(rank, &local_state.vc);
           Event(rank, &local_state.vc);
        } else if (rank == 2){
           Event(rank, &local_state.vc);
        }
        
        printf("Processo %d: Evento interno ocorrido\n", rank);
        print_vector_clock(&local_state.vc, rank);
        pthread_mutex_unlock(&mutex_snapshot);
        sleep(2); // Delay para simular eventos internos
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

    pthread_mutex_init(&mutex_snapshot, NULL);

    pthread_t entrada_thread, saida_thread, relogios_thread;
    pthread_create(&entrada_thread, NULL, thread_entrada, &rank);
    pthread_create(&saida_thread, NULL, thread_saida, &rank);
    pthread_create(&relogios_thread, NULL, thread_relogios, &rank);

    if (rank == 0) {
        sleep(3);  // Pequeno atraso inicial
        initiate_snapshot(rank);
    }

    pthread_join(entrada_thread, NULL);
    pthread_join(saida_thread, NULL);
    pthread_join(relogios_thread, NULL);

    pthread_mutex_destroy(&mutex_snapshot);
    MPI_Finalize();
    return 0;
}
