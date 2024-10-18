# Etapa 3 - Projeto PPC

## Equipe responsável:
 * Nayla Sahra Santos das Chagas - 202000024525  
 * Túlio Sousa de Gois - 202000024599
   
---

# Explicação da Implementação do Algoritmo de Snapshot de Chandy-Lamport

## Solução

Nossa solução implementa o algoritmo de snapshot de Chandy-Lamport em um sistema distribuído usando MPI para comunicação entre processos e pthreads para concorrência dentro de cada processo. O sistema segue um modelo de comunicação em anel, onde cada processo pode iniciar um snapshot global.

### Componentes Principais

1. **Estruturas de Dados**
   - `VectorClock`: Representa o relógio vetorial de cada processo.
   - `Message`: Encapsula uma mensagem, que pode ser uma mensagem normal ou um marcador.
   - `ProcessState`: Representa o estado local de um processo, incluindo seu relógio vetorial e o estado dos canais de entrada.

2. **Threads**
   - **Thread de Entrada**: Recebe mensagens via MPI e processa marcadores.
   - **Thread de Relógios**: Atualiza o relógio vetorial local e gerencia o buffer de mensagens.
   - **Thread de Saída**: Envia mensagens para o próximo processo no anel.

3. **Funções Principais**
   - `initiate_snapshot`: Inicia o processo de snapshot.
   - `process_marker`: Processa a recepção de um marcador.
   - `print_snapshot`: Imprime o estado local e dos canais após um snapshot.

### Funcionamento

1. **Inicialização**
   - O programa inicia `THREAD_NUM` processos MPI.
   - Cada processo cria 3 threads: entrada, relógios e saída.

2. **Comunicação**
   - Os processos formam um anel lógico.
   - Cada processo envia mensagens para o próximo e recebe do anterior.

3. **Snapshot**
   - O processo 0 inicia periodicamente um snapshot global.
   - Ao iniciar um snapshot, um processo:
     - Salva seu estado local.
     - Envia marcadores para todos os outros processos.
     - Começa a gravar mensagens recebidas em seus canais de entrada.
   - Ao receber um marcador, um processo:
     - Se for o primeiro marcador, inicia seu próprio snapshot.
     - Para de gravar mensagens no canal do remetente do marcador.
   - O snapshot termina quando um processo recebeu marcadores de todos os outros.

4. **Atualização do Relógio Vetorial**
   - Na recepção de uma mensagem, o relógio é atualizado com o máximo entre o local e o recebido.
   - Um evento local incrementa apenas o contador do processo atual.

### Cenários de teste

Os cenários de teste estão implementados nas funções de cada thread e no loop principal do processo 0.

1. **Múltiplos Snapshots**
   ```c
   if (rank == 0) {
       sleep(1);  // Pequeno atraso inicial
       for (int i = 0; i < MAX_SNAPSHOTS; i++) {
           initiate_snapshot(rank);
           sleep(2);  // Espera 2 segundos entre snapshots
       }
   }
   ```

2. **Processamento de Marcadores**
   ```c
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
   ```

## Dificuldades encontradas
- Sincronização correta entre as threads e processos MPI.
- Implementação precisa do algoritmo de Chandy-Lamport, especialmente o tratamento de marcadores e a gravação do estado dos canais.
- Gerenciamento de condições de corrida e deadlocks potenciais.
- Garantir a terminação adequada do programa após um número fixo de snapshots.
