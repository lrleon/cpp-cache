# Promela model for cpp-cache

Este directorio contiene un modelo Promela acotado del comportamiento actual
del cache concurrente (`cache_model.pml`, version v5).

## Que modela

- `capacity = 2`
- `keys = 3`
- 6 hilos de peticion: 4 `Requester` (`get_or_compute()`) y 2 `FindRequester` (`find()`)
- 5 procesos de fondo: `Background` (envejecimiento y expiracion), `Invalidator`,
  `TouchRequester`, `PeekRequester` y `HasRequester`; 11 procesos en total, mas `init`
- single-flight por clave
- resultado explicito `Saturated` cuando todas las entradas estan en `Computing`
- invalidacion lazy
- LRU
- expiracion abstracta como un bit `expired`
- refcount explicito (`slot_rc`), que modela `CacheEntry::_refcount` y `EntryGuard`

No es una traduccion linea por linea de `include/cache/cache.H`. Es un modelo
de comportamiento para buscar violaciones de seguridad y progreso.

## Safety cubierta por `assert`

El modelo tiene 30 sentencias `assert`. Verifican en cada transicion:

- nunca hay mas entradas que la capacidad
- `computing_count` coincide exactamente con los slots en `Computing`
- nunca existen dos slots activos para la misma clave
- nunca hay dos computaciones simultaneas para la misma clave
- nunca se deja una clave viva en un slot `Empty`
- una entrada `Computing` nunca aparece expirada
- la estructura LRU siempre queda bien formada
- la eviccion solo reutiliza slots no `Computing` y sin referencias externas
- el refcount es coherente: 0 en un slot vacio, al menos 1 en uno ocupado y al
  menos 2 en uno en `Computing`

La busqueda de aserciones se compila sin never claims (`-DNOCLAIM`). Con un
claim activo, `pan` solo comprueba las aserciones dentro del alcance del claim.

## Liveness cubierta por LTL

Diez formulas LTL, verificadas bajo weak fairness (`-f`):

- `every_requested_key_eventually_materializes`: toda clave pedida llega a materializarse
- `no_starvation_thread0` a `no_starvation_thread5`: ningun hilo de peticion sufre hambruna
- `all_terminate`: todos los hilos de peticion terminan
- `no_orphaned_computing`: toda computacion iniciada deja de estar en `Computing`
- `saturation_transient`: una saturacion no puede durar para siempre si las
  computaciones terminan

En el modelo, "materializarse" significa que la clave entra en `Ready` o
`Failed`. Si quieres exigir solo inserciones positivas, cambia la fase 3 de
`get_or_compute_api()`, donde el dueno de la computacion elige `Ready` o
`Failed`, para que solo complete en `Ready`, o ajusta las formulas.

## Como ejecutarlo

```bash
./formal/run_spin.sh
```

El script corre la busqueda de aserciones y las diez propiedades LTL, y
clasifica cada busqueda:

- `PASS`: `pan` exploro todo el espacio de estados y no encontro errores.
- `FAIL`: `pan` encontro errores; la traza queda en el directorio de salida.
- `INCOMPLETE`: `pan` se detuvo antes de agotar el espacio de estados por el
  limite de memoria, de profundidad o de tiempo. Su `errors: 0` solo cubre la
  parte explorada.

`pan` termina con codigo 0 en los tres casos, asi que el script lee su salida.
El codigo de salida del script es 0 si todas las busquedas son `PASS`, 1 si
alguna es `FAIL` y 2 si alguna quedo `INCOMPLETE`.

| Variable | Por defecto | Uso |
|---|---|---|
| `MEMLIM` | `4096` | limite de memoria de `pan`, en MB |
| `DEPTH` | `100000` | profundidad maxima de busqueda |
| `NFAIR` | `8` | cota de weak fairness de las busquedas LTL |
| `COLLAPSE` | `1` | compresion sin perdida `-DCOLLAPSE`; `0` para desactivarla |
| `TIMEOUT` | `0` | limite por busqueda, en segundos; `0` es sin limite |
| `SAFETY` | `1` | `0` para omitir la busqueda de aserciones |
| `PROPS` | `all` | `all`, `none` o una lista de propiedades separadas por espacios |
| `OUT_DIR` | temporal | salida completa de `pan`, trazas y `summary.tsv` |

```bash
MEMLIM=16384 ./formal/run_spin.sh                        # mas memoria
SAFETY=0 PROPS="all_terminate" ./formal/run_spin.sh      # una sola propiedad
./formal/spin_verify.sh ltl no_starvation_thread2        # atajo equivalente
```

`spin_verify.sh` delega en `run_spin.sh`; `verify_full.sh` y `spin_verify.rb`
aplican el mismo criterio `PASS`/`FAIL`/`INCOMPLETE` y los mismos codigos de salida.

## Cuanta memoria hace falta

Con el modelo v5 ninguna busqueda termina dentro de 4 GB. En una corrida del
19 de octubre de 2026 (SPIN 6.5.2, AMD Ryzen 7 PRO 7840U, commit `5bfaf38`),
las diez propiedades LTL alcanzaron el limite de 4 GB con entre 11,5 y 27
millones de estados almacenados. Una busqueda de aserciones sin claim, con
`-DCOLLAPSE` y un limite de 40 GB, almaceno 537 millones de estados en 38
minutos antes de que el sistema operativo la detuviera por falta de memoria.
Ninguna encontro errores en la parte explorada. La salida completa de `pan` esta en
<https://github.com/simylresearch/simyl-benchmarks/tree/main/formal/cpp-cache-5bfaf38>.

Para una verificacion completa hace falta mas memoria que la de esa corrida, o
reducir los limites del modelo (hilos, claves o capacidad).

## Corrida manual

```bash
cd formal
spin -a cache_model.pml

# aserciones, sin never claim
cc -O2 -DSAFETY -DNOCLAIM -DCOLLAPSE -DMEMLIM=4096 -o pan_safety pan.c
./pan_safety -m100000 -n -E

# una propiedad LTL, con weak fairness
cc -O2 -DNFAIR=8 -DCOLLAPSE -DMEMLIM=4096 -o pan_ltl pan.c
./pan_ltl -m100000 -n -E -a -f -N no_starvation_thread2
```

Lee el final de cada salida: `reached -DMEMLIM bound`, `max search depth too small`
o `Search not completed` indican que la busqueda se trunco, diga lo que diga `errors:`.

## Notas

- El supuesto de liveness importante es que todo miss en `Computing`
  eventualmente se resuelve. Por eso las propiedades de progreso se ejecutan
  con `-f`.
- Se usa `-E` porque el modelo mantiene procesos de fondo (`Background`,
  `Invalidator`, `TouchRequester`, `PeekRequester` y `HasRequester`) que quedan
  vivos a proposito; no queremos que SPIN reporte eso como un problema del
  protocolo.
- El modelo esta acotado a 2 slots y 3 claves para mantener el espacio de
  estados manejable. Si subes esos limites, la explosion de estados crece
  rapido.
