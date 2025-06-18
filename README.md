# Sistema de Facturacion Concurrente
Sistema de Facturación Concurrente en Lengueje C.
Véase el documento de diseño funcional para más información acerca del proyecto.

- Compilar con: gcc -o sistema_facturacion sistema_facturacion.c
- Ejecutar con: ./sistema_facturacion

## ÍNDICE SEMÁFOROS
A: SEM_DISPONIBLE_PEDIDOS
B: SEM_PENDIENTES_PEDIDOS
C: SEM_MUTEX_PEDIDOS

D: SEM_DISPONIBLE_FACTURAS 
E: SEM_PENDIENTES_FACTURAS
F:SEM_MUTEX_FACTURAS 

G: SEM_DISPONIBLE_STOCKBAJO
H: SEM_PENDIENTES_STOCKBAJO
I: SEM_MUTEX_STOCKBAJO

J: SEM_MUTEX_INVENTARIO


## MONITOREO BÁSICO 
htop 
ipcs
ipcs -m -i <id> 
ipcs -s -i <id>

(Cambiar <id> por el respectivo numero observado al ejecutar ipcs)


## PRUEBAS CAMBIANDO PARÁMETROS 

El objetivo es cambiar los parámetros de intervalo de generación de pedidos y de restock con el objetivo de observar como se comportan los buffers en memoria y los
diferentes semáforos.

### PRUEBA 1: Estado inicial

- INTERVALO_PEDIDOS = 1
- INTERVALO_RESTOCK = 5

ipcs -s <id>
tail -f pedidos_aprobados.txt pedidos_rechazados.txt

### PRUEBA 2: Visualización de stock y restock
-------------------------------------
- INTERVALO_PEDIDOS = 0.5
- INTERVALO_RESTOCK = 3

tail -f stock_bajo.log
tail -f restock.log
ipcs -m -i  <id>


### PRUEBA 3: Sobrecarga de pedidos.
-------------------------------------
- INTERVALO_PEDIDOS = 0

watch -n 0.1 ipcs -s -i <id>
tail -f pedidos_rechazados.txt
htop



