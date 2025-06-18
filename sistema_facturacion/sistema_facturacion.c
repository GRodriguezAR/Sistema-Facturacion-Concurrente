// GRodriguezAR

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>

#define MAX_PRODUCTOS 100
#define TAM_BUFFER_PEDIDOS 50
#define TAM_BUFFER_FACTURAS 50
#define TAM_BUFFER_STOCKBAJO 15
#define MAX_ITEMS_PEDIDO 5
#define MIN_CANT_POR_PROD 10
#define MAX_CANT_POR_PROD 50
#define UMBRAL_BAJO_STOCK 20
#define NUM_HIJOS 5

#define INTERVALO_RESTOCK 5
#define INTERVALO_PEDIDOS 1

#define FORMATO_FECHA(ts, buf)         \
    struct tm *_tm = localtime(&(ts)); \
    strftime((buf), 20, "%Y-%m-%d %H:%M:%S", _tm)

// Índices de semáforos en el conjunto
int SEM_DISPONIBLE_PEDIDOS, SEM_PENDIENTES_PEDIDOS, SEM_MUTEX_PEDIDOS;
int SEM_DISPONIBLE_FACTURAS, SEM_PENDIENTES_FACTURAS, SEM_MUTEX_FACTURAS;
int SEM_DISPONIBLE_STOCKBAJO, SEM_PENDIENTES_STOCKBAJO, SEM_MUTEX_STOCKBAJO;
int SEM_MUTEX_INVENTARIO;

// P/V para System V semáforos
void P(int sem_id)
{
    struct sembuf op = {0, -1, 0};
    semop(sem_id, &op, 1);
}
void V(int sem_id)
{
    struct sembuf op = {0, +1, 0};
    semop(sem_id, &op, 1);
}

// Union para semctl
union semun
{
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

// Pedido y sus artículos
typedef struct
{
    int idProducto;
    int cantidad;
} Articulo;

typedef struct
{
    int idPedido;
    char cliente[32];
    Articulo articulos[MAX_ITEMS_PEDIDO];
    int cantArticulos;
    time_t timestamp;
} Pedido;

typedef struct
{
    int id;
    char nombre[50];
    int stock;
    float precio;
} Producto;

// Memoria compartida
typedef struct
{
    // Buffer pedidos: pedidos pendientes
    Pedido pedidos[TAM_BUFFER_PEDIDOS];
    int idxEscPedidos, idxLectPedidos;

    // Buffer facturas: pedidos aprobados que deben ser facturados
    Pedido facturas[TAM_BUFFER_FACTURAS];
    int idxEscFacturas, idxLectFacturas;

    // Buffer stock bajo: almacena índices de productos en bajo stock
    int stockBajo[TAM_BUFFER_STOCKBAJO];
    int idxEscStockBajo, idxLectStockBajo;

    // Inventario
    Producto inventario[MAX_PRODUCTOS];
    int cantProductos;
    int alertaStock[MAX_PRODUCTOS];

    // Estadísticas
    int totalPedidos, totalAprobados, totalRechazados;
    float ingresos;

    // Control de ejecución
    int ejecutando;
} MemoriaCompartida;

MemoriaCompartida *mem;
int shmId;

// Indice de producto en inventario
int buscarProducto(int id)
{
    for (int i = 0; i < mem->cantProductos; i++)
        if (mem->inventario[i].id == id)
            return i;
    return -1;
}

// Carga inventario_inicial.txt
int cargarInventarioInicial(const char *ruta)
{
    FILE *f = fopen(ruta, "r");
    if (!f)
    {
        perror("inventario_inicial.txt");
        mem->ejecutando = 0;
        return -1;
    }
    char linea[128];
    mem->cantProductos = 0;
    while (fgets(linea, sizeof linea, f))
    {
        Producto *p = &mem->inventario[mem->cantProductos];
        char *tok = strtok(linea, ",");
        p->id = atoi(tok);
        tok = strtok(NULL, ",");
        strcpy(p->nombre, tok);
        tok = strtok(NULL, ",");
        p->stock = atoi(tok);
        tok = strtok(NULL, ",\n");
        p->precio = atof(tok);

        if (p->stock < UMBRAL_BAJO_STOCK)
            mem->alertaStock[mem->cantProductos] = 1;
        else
            mem->alertaStock[mem->cantProductos] = 0;

        mem->cantProductos++;
    }
    fclose(f);
    return 0;
}

// Manejo de señales
void manejadorSigint(int signo)
{
    (void)signo;
    if (mem)
        mem->ejecutando = 0;
}

// Inserta encabezados en archivos
int insertarEncabezados()
{
    const char *nomArch[] = {"facturas.txt", "pedidos_aprobados.txt", "pedidos_rechazados.txt", "stock_bajo.log", "restock.log"};
    const char *encabezado[] = {"=== FACTURAS ===", "=== APROBADOS ===", "=== RECHAZADOS ===", "=== STOCK BAJO ===", "=== RESTOCK ==="};
    FILE *f;
    for (int i = 0; i < 5; i++)
    {
        f = fopen(nomArch[i], "w");
        if (!f)
        {
            perror(nomArch[i]);
            return -1;
        }
        fprintf(f, "%s\n\n", encabezado[i]);
        fclose(f);
    }
    return 0;
}

// Limpiar todo
void limpiarTodo(int semaforos, int memoria)
{
    if (semaforos)
    {
        // Liberar semáforos para posibles procesos en espera
        V(SEM_PENDIENTES_PEDIDOS);
        V(SEM_PENDIENTES_FACTURAS);
        V(SEM_PENDIENTES_STOCKBAJO);
        V(SEM_DISPONIBLE_PEDIDOS);
        V(SEM_DISPONIBLE_FACTURAS);
        V(SEM_DISPONIBLE_STOCKBAJO);

        // Esperar a que terminen los hijos
        for (int i = 0; i < 5; i++)
            wait(NULL);

        // Limpiar semáforos
        semctl(SEM_DISPONIBLE_PEDIDOS, 0, IPC_RMID);
        semctl(SEM_PENDIENTES_PEDIDOS, 0, IPC_RMID);
        semctl(SEM_MUTEX_PEDIDOS, 0, IPC_RMID);
        semctl(SEM_DISPONIBLE_FACTURAS, 0, IPC_RMID);
        semctl(SEM_PENDIENTES_FACTURAS, 0, IPC_RMID);
        semctl(SEM_MUTEX_FACTURAS, 0, IPC_RMID);
        semctl(SEM_MUTEX_INVENTARIO, 0, IPC_RMID);
        semctl(SEM_DISPONIBLE_STOCKBAJO, 0, IPC_RMID);
        semctl(SEM_PENDIENTES_STOCKBAJO, 0, IPC_RMID);
        semctl(SEM_MUTEX_STOCKBAJO, 0, IPC_RMID);
    }
    if (memoria)
    {
        shmdt(mem);
        shmctl(shmId, IPC_RMID, NULL);
    }
}

// Hijo 1: Generador de pedidos
void procesoGeneradorPedidos()
{
    int idPedido = 1, numClienteRandom;
    srand(time(NULL));
    while (mem->ejecutando)
    {
        Pedido p;
        p.idPedido = idPedido;
        numClienteRandom = (rand() % 99) + 1;
        snprintf(p.cliente, sizeof p.cliente, "Cliente %02d", numClienteRandom);

        // Generar artículos aleatorios
        p.cantArticulos = 1 + rand() % MAX_ITEMS_PEDIDO;
        for (int i = 0; i < p.cantArticulos; i++)
        {
            int idx = rand() % mem->cantProductos;
            p.articulos[i].idProducto = mem->inventario[idx].id;
            p.articulos[i].cantidad = MIN_CANT_POR_PROD + rand() % (MAX_CANT_POR_PROD - MIN_CANT_POR_PROD + 1);
        }
        p.timestamp = time(NULL);

        // No se requiere MUTEX ya que solo este proceso accede
        mem->totalPedidos++;

        // Se agrega el pedido al buffer de pedidos
        P(SEM_DISPONIBLE_PEDIDOS);
        P(SEM_MUTEX_PEDIDOS);
        mem->pedidos[mem->idxEscPedidos] = p;
        mem->idxEscPedidos = (mem->idxEscPedidos + 1) % TAM_BUFFER_PEDIDOS;
        V(SEM_MUTEX_PEDIDOS);
        V(SEM_PENDIENTES_PEDIDOS);

        idPedido++;

        for (int i = 0; i < INTERVALO_PEDIDOS * 1000 && mem->ejecutando == 1; i += 200)
            usleep(100000); // 1 milisegundo
    }
    exit(0);
}

// Hijo 2: Validador de stock, genera facturas y notificaciones
void procesoValidador()
{
    FILE *fa = fopen("pedidos_aprobados.txt", "a");
    if (!fa)
    {
        perror("pedidos_aprobados.txt");
        mem->ejecutando = 0;
        exit(1);
    }
    FILE *fr = fopen("pedidos_rechazados.txt", "a");
    if (!fr)
    {
        perror("pedidos_rechazados.txt");
        fclose(fa);
        mem->ejecutando = 0;
        exit(1);
    }

    // Solo va a finalizar cuando el padre de la señal y el buffer de pedidos estén vacíos
    while (1)
    {
        if (!mem->ejecutando)
        {
            // Si ya no se está ejecutando, hace una lectura de buffer "no bloqueante".
            struct sembuf op = {0, -1, IPC_NOWAIT};
            if (semop(SEM_PENDIENTES_PEDIDOS, &op, 1) < 0)
                break;
        }
        else
            // Consume pedido del buffer de pedidos
            P(SEM_PENDIENTES_PEDIDOS);

        // Si ya no se está ejecutando y los indices son iguales, indica que se produjo un bloqueo y fue liberado por el padre
        if (!mem->ejecutando && mem->idxLectPedidos == mem->idxEscPedidos)
            break;

        P(SEM_MUTEX_PEDIDOS);
        Pedido p = mem->pedidos[mem->idxLectPedidos];
        mem->idxLectPedidos = (mem->idxLectPedidos + 1) % TAM_BUFFER_PEDIDOS;
        V(SEM_MUTEX_PEDIDOS);
        V(SEM_DISPONIBLE_PEDIDOS);

        // Demanda por producto
        int cantidadPedida[MAX_PRODUCTOS] = {0};
        for (int i = 0; i < p.cantArticulos; i++)
        {
            int idx = buscarProducto(p.articulos[i].idProducto);
            cantidadPedida[idx] += p.articulos[i].cantidad;
        }

        // Validar stock suficiente para todo el pedido
        P(SEM_MUTEX_INVENTARIO);
        int ok = 1;
        for (int i = 0; i < mem->cantProductos && ok; i++)
        {
            if (cantidadPedida[i] > mem->inventario[i].stock)
                ok = 0;
        }

        char buf[20];
        FORMATO_FECHA(p.timestamp, buf);
        if (ok)
        {
            // Aprobado
            float total = 0;
            for (int i = 0; i < mem->cantProductos; i++)
            {
                if (cantidadPedida[i] > 0)
                {
                    // Actualizar stock y calcular total de ingresos
                    mem->inventario[i].stock -= cantidadPedida[i];
                    total += cantidadPedida[i] * mem->inventario[i].precio;

                    // Verificar si hay stock bajo
                    if (mem->inventario[i].stock < UMBRAL_BAJO_STOCK && !mem->alertaStock[i])
                    {
                        // Agregar al buffer de stock bajo
                        P(SEM_DISPONIBLE_STOCKBAJO);
                        P(SEM_MUTEX_STOCKBAJO);
                        mem->stockBajo[mem->idxEscStockBajo] = i;
                        mem->idxEscStockBajo = (mem->idxEscStockBajo + 1) % TAM_BUFFER_STOCKBAJO;
                        V(SEM_MUTEX_STOCKBAJO);
                        V(SEM_PENDIENTES_STOCKBAJO);

                        // Activar alerta para restock
                        mem->alertaStock[i] = 1;
                    }
                }
            }
            mem->totalAprobados++;
            mem->ingresos += total;

            // Agregar al buffer de facturas
            P(SEM_DISPONIBLE_FACTURAS);
            P(SEM_MUTEX_FACTURAS);
            mem->facturas[mem->idxEscFacturas] = p;
            mem->idxEscFacturas = (mem->idxEscFacturas + 1) % TAM_BUFFER_FACTURAS;
            V(SEM_MUTEX_FACTURAS);
            V(SEM_PENDIENTES_FACTURAS);

            // Escribir en archivo de pedidos aprobados
            fprintf(fa, "%s | Pedido%4d (%s) APROBADO | Total: $%.2f\n",
                    buf, p.idPedido, p.cliente, total);
            for (int i = 0; i < p.cantArticulos; i++)
            {
                int ip = buscarProducto(p.articulos[i].idProducto);
                fprintf(fa, "    - %s x%d @ $%.2f\n",
                        mem->inventario[ip].nombre,
                        p.articulos[i].cantidad,
                        mem->inventario[ip].precio);
            }
            fprintf(fa, "-----------------------------\n");
            fflush(fa);

            // Mostrar mensaje de aprobado
            printf("Pedido %4d (%s) APROBADO\n",
                   p.idPedido, p.cliente);
            fflush(stdout);
        }
        else
        {
            // Rechazado
            mem->totalRechazados++;

            // Escribir en archivo de pedidos rechazados
            fprintf(fr, "%s | Pedido%4d (%s) RECHAZADO |\nFaltantes:\n",
                    buf, p.idPedido, p.cliente);
            for (int i = 0; i < p.cantArticulos; i++)
            {
                int ip = buscarProducto(p.articulos[i].idProducto);
                if (cantidadPedida[ip] > mem->inventario[ip].stock)
                    fprintf(fr, "    - %s x%d (disponible: %d)\n",
                            mem->inventario[ip].nombre,
                            p.articulos[i].cantidad,
                            mem->inventario[ip].stock);
            }
            fprintf(fr, "-----------------------------\n");
            fflush(fr);

            // Mostrar mensaje de rechazo
            printf("Pedido %4d (%s) RECHAZADO\n",
                   p.idPedido, p.cliente);
            fflush(stdout);
        }
        V(SEM_MUTEX_INVENTARIO);
    }
    fclose(fa);
    fclose(fr);
    exit(0);
}

// Hijo 3: Logger de stock bajo inmediato
void procesoLoggerStockBajo()
{
    FILE *f = fopen("stock_bajo.log", "a");
    if (!f)
    {
        perror("stock_bajo.log");
        mem->ejecutando = 0;
        exit(1);
    }

    // Solo va a finalizar cuando el padre de la señal y el buffer de stock bajo estén vacíos
    while (1)
    {
        if (!mem->ejecutando)
        {
            // Si ya no se está ejecutando, hace una lectura de buffer "no bloqueante".
            struct sembuf op = {0, -1, IPC_NOWAIT};
            if (semop(SEM_PENDIENTES_STOCKBAJO, &op, 1) < 0)
                break;
        }
        else
            P(SEM_PENDIENTES_STOCKBAJO);

        // Si ya no se está ejecutando y los indices son iguales, indica que se produjo un bloqueo y fue liberado por el padre
        if (!mem->ejecutando && mem->idxLectStockBajo == mem->idxEscStockBajo)
            break;

        P(SEM_MUTEX_STOCKBAJO);
        int ip = mem->stockBajo[mem->idxLectStockBajo];
        mem->idxLectStockBajo = (mem->idxLectStockBajo + 1) % TAM_BUFFER_STOCKBAJO;
        V(SEM_MUTEX_STOCKBAJO);
        V(SEM_DISPONIBLE_STOCKBAJO);

        // Entra en zona crítica para mostrar stock de producto
        P(SEM_MUTEX_INVENTARIO);
        char buf[20];
        time_t ahora = time(NULL);
        FORMATO_FECHA(ahora, buf);
        fprintf(f,
                "%s | [%3d] %-20s %-12s %3d\n",
                buf,
                mem->inventario[ip].id,
                mem->inventario[ip].nombre,
                "stock bajo:",
                mem->inventario[ip].stock);
        fflush(f);
        V(SEM_MUTEX_INVENTARIO);
    }
    fclose(f);
    exit(0);
}

// Hijo 4: Restock periódico de todos los productos en bajo stock
void procesoRestocker()
{
    FILE *f = fopen("restock.log", "a");
    if (!f)
    {
        perror("restock.log");
        mem->ejecutando = 0;
        exit(1);
    }
    while (mem->ejecutando)
    {
        // Duermo el intervalo completo antes de restockear
        for (int i = 0; i < INTERVALO_RESTOCK && mem->ejecutando; i++)
            sleep(1);

        // INTERVALO_RESTOCK = 0 no puede ser instantáneo ya que bloquea el inventario
        if(INTERVALO_RESTOCK == 0)
            usleep(15000);

        // Evita que se realice siempre restock antes de finalizar
        if (!mem->ejecutando)
            break;

        // Entra en zona crítica para restockear
        P(SEM_MUTEX_INVENTARIO);
        for (int i = 0; i < mem->cantProductos; i++)
        {
            if (mem->alertaStock[i])
            {
                char buf[20];
                time_t ahora = time(NULL);
                FORMATO_FECHA(ahora, buf);
                int antes = mem->inventario[i].stock;
                mem->inventario[i].stock += MAX_CANT_POR_PROD;
                mem->alertaStock[i] = 0;

                // Escribir en log de restock
                fprintf(f,
                        "%s | [%3d] %-20s %9s: %3d -> %3d\n",
                        buf,
                        mem->inventario[i].id,
                        mem->inventario[i].nombre,
                        "restock",
                        antes,
                        mem->inventario[i].stock);
                fflush(f);
            }
        }
        V(SEM_MUTEX_INVENTARIO);
    }
    fclose(f);
    exit(0);
}

// Hijo 5: Facturador de pedidos aprobados
void procesoFacturador()
{
    FILE *f = fopen("facturas.txt", "a");
    if (!f)
    {
        perror("facturas.txt");
        mem->ejecutando = 0;
        exit(1);
    }

    while (1)
    {
        if (!mem->ejecutando)
        {
            // Si ya no se está ejecutando, hace una lectura de buffer "no bloqueante".
            struct sembuf op = {0, -1, IPC_NOWAIT};
            if (semop(SEM_PENDIENTES_FACTURAS, &op, 1) < 0)
                break;
        }
        else
            P(SEM_PENDIENTES_FACTURAS);

        // Si ya no se está ejecutando y los indices son iguales, indica que se produjo un bloqueo y fue liberado por el padre
        if (!mem->ejecutando && mem->idxLectFacturas == mem->idxEscFacturas)
            break;

        // Consume pedido del buffer de facturas
        P(SEM_MUTEX_FACTURAS);
        Pedido p = mem->facturas[mem->idxLectFacturas];
        mem->idxLectFacturas = (mem->idxLectFacturas + 1) % TAM_BUFFER_FACTURAS;
        V(SEM_MUTEX_FACTURAS);
        V(SEM_DISPONIBLE_FACTURAS);

        // Escribe en archivo de facturas
        char buf[20];
        FORMATO_FECHA(p.timestamp, buf);
        fprintf(f, "=== FACTURA %d (%s) ===\nCliente: %s\n",
                p.idPedido, buf, p.cliente);
        float total = 0;
        for (int i = 0; i < p.cantArticulos; i++)
        {
            int idProd = buscarProducto(p.articulos[i].idProducto);
            float subtotal = p.articulos[i].cantidad * mem->inventario[idProd].precio;
            total += subtotal;
            fprintf(f, "%-20s x%2d @ $%7.2f = $%7.2f\n",
                    mem->inventario[idProd].nombre,
                    p.articulos[i].cantidad,
                    mem->inventario[idProd].precio,
                    subtotal);
        }
        fprintf(f, "TOTAL: $%.2f\n\n", total);
        fflush(f);
    }
    fclose(f);
    exit(0);
}

// Main
int main()
{
    // Manejo de señales
    signal(SIGINT, manejadorSigint);

    // MemoriaCompartida
    shmId = shmget(IPC_PRIVATE, sizeof *mem, IPC_CREAT | 0666);
    mem = shmat(shmId, NULL, 0);
    memset(mem, 0, sizeof *mem);
    mem->ejecutando = 1;

    // Cargar inventario inicial en memoria compartida
    if (cargarInventarioInicial("inventario_inicial.txt") == -1)
    {
        perror("inventario_inicial.txt");
        limpiarTodo(0, 1);
        return 1;
    }

    // Insertar encabezados en archivos de salida
    if (insertarEncabezados() == -1)
    {

        perror("inicializar_archivos");
        limpiarTodo(0, 1);
        return 1;
    }

    // Crear e inicializar semáforos
    union semun arg;

    // 1. SEM_DISPONIBLE_PEDIDOS (Espacios disponibles en buffer pedidos)
    SEM_DISPONIBLE_PEDIDOS = semget(ftok(".", 'A'), 1, IPC_CREAT | 0666);
    arg.val = TAM_BUFFER_PEDIDOS;
    semctl(SEM_DISPONIBLE_PEDIDOS, 0, SETVAL, arg);

    // 2. SEM_PED_PENDIENTES (Pedidos pendientes de validación)
    SEM_PENDIENTES_PEDIDOS = semget(ftok(".", 'B'), 1, IPC_CREAT | 0666);
    arg.val = 0;
    semctl(SEM_PENDIENTES_PEDIDOS, 0, SETVAL, arg);

    // 3. SEM_MUTEX_PEDIDOS (Mutex buffer pedidos)
    SEM_MUTEX_PEDIDOS = semget(ftok(".", 'C'), 1, IPC_CREAT | 0666);
    arg.val = 1;
    semctl(SEM_MUTEX_PEDIDOS, 0, SETVAL, arg);

    // 4. SEM_DISPONIBLE_FACTURAS (Espacios en buffer facturas)
    SEM_DISPONIBLE_FACTURAS = semget(ftok(".", 'D'), 1, IPC_CREAT | 0666);
    arg.val = TAM_BUFFER_FACTURAS;
    semctl(SEM_DISPONIBLE_FACTURAS, 0, SETVAL, arg);

    // 5. SEM_FACT_PENDIENTES (Facturas pendientes)
    SEM_PENDIENTES_FACTURAS = semget(ftok(".", 'E'), 1, IPC_CREAT | 0666);
    arg.val = 0;
    semctl(SEM_PENDIENTES_FACTURAS, 0, SETVAL, arg);

    // 6. SEM_MUTEX_FACT (Mutex buffer facturas)
    SEM_MUTEX_FACTURAS = semget(ftok(".", 'F'), 1, IPC_CREAT | 0666);
    arg.val = 1;
    semctl(SEM_MUTEX_FACTURAS, 0, SETVAL, arg);

    // 7. SEM_DISPONIBLE_STOCKBAJO (Espacios en log bajo stock)
    SEM_DISPONIBLE_STOCKBAJO = semget(ftok(".", 'G'), 1, IPC_CREAT | 0666);
    arg.val = TAM_BUFFER_STOCKBAJO;
    semctl(SEM_DISPONIBLE_STOCKBAJO, 0, SETVAL, arg);

    // 8. SEM_PENDIENTES_LOG_STOCK (Ítems pendientes en log)
    SEM_PENDIENTES_STOCKBAJO = semget(ftok(".", 'H'), 1, IPC_CREAT | 0666);
    arg.val = 0;
    semctl(SEM_PENDIENTES_STOCKBAJO, 0, SETVAL, arg);

    // 9. SEM_MUTEX_LOG_STOCK (Mutex log bajo stock)
    SEM_MUTEX_STOCKBAJO = semget(ftok(".", 'I'), 1, IPC_CREAT | 0666);
    arg.val = 1;
    semctl(SEM_MUTEX_STOCKBAJO, 0, SETVAL, arg);

    // 10. SEM_MUTEX_INV (Mutex inventario)
    SEM_MUTEX_INVENTARIO = semget(ftok(".", 'J'), 1, IPC_CREAT | 0666);
    arg.val = 1;
    semctl(SEM_MUTEX_INVENTARIO, 0, SETVAL, arg);

    printf("Sistema en ejecucion. Presione ENTER para salir...\n");

    // Crear procesos hijos
    for (int i = 0; i < NUM_HIJOS; i++)
    {
        if (fork() == 0)
        {
            switch (i)
            {
            case 0:
                procesoGeneradorPedidos();
                break;
            case 1:
                procesoValidador();
                break;
            case 2:
                procesoLoggerStockBajo();
                break;
            case 3:
                procesoRestocker();
                break;
            case 4:
                procesoFacturador();
                break;
            }
        }
    }

    // Espera Enter para terminar
    getchar();
    mem->ejecutando = 0;

    // Esperar a que todos los hijos terminen y limpia semaforos
    limpiarTodo(1, 0);

    // Inventario final
    FILE *f = fopen("inventario_final.txt", "w");
    if (!f)
    {
        perror("inventario_final.txt");
        // Liberar memoria compartida
        limpiarTodo(0, 1);
        return 1;
    }
    for (int i = 0; i < mem->cantProductos; i++)
    {
        Producto *p = &mem->inventario[i];
        fprintf(f, "%d,%s,%d,%.2f\n", p->id, p->nombre, p->stock, p->precio);
    }
    fclose(f);

    // Resumen diario
    printf("Total pedidos    : %d\n", mem->totalPedidos);
    printf("Pedidos aprobados: %d\n", mem->totalAprobados);
    printf("Pedidos rechazados: %d\n", mem->totalRechazados);
    printf("Ingresos totales : $%.2f\n\n", mem->ingresos);

    // Liberar memoria compartida
    limpiarTodo(0, 1);

    printf("Sistema finalizado. Archivos generados.\n");
    return 0;
}

// Compilar con: gcc -o sistema_facturacion sistema_facturacion.c
// Ejecutar con: ./sistema_facturacion