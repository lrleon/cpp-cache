# Promela model for cpp-cache

Este directorio contiene un modelo Promela acotado del comportamiento actual
del cache concurrente.

## Que modela

- `capacity = 2`
- `keys = 3`
- single-flight por clave
- resultado explicito `Saturated` cuando todas las entradas estan en `Computing`
- invalidacion lazy
- LRU
- expiracion abstracta como un bit `expired`

No es una traduccion linea por linea de `include/cache/cache.H`. Es un modelo
de comportamiento para buscar violaciones de seguridad y progreso.

## Safety cubierta por `assert`

Las aserciones del modelo verifican en cada transicion:

- nunca hay mas entradas que la capacidad
- `computing_count` coincide exactamente con los slots en `Computing`
- nunca existen dos slots activos para la misma clave
- nunca hay dos computaciones simultaneas para la misma clave
- nunca se deja una clave viva en un slot `Empty`
- una entrada `Computing` nunca aparece expirada
- la estructura LRU siempre queda bien formada
- la eviccion solo reutiliza slots no `Computing`

Estas aserciones se comprueban en cada corrida de `pan` que aparece abajo.

## Liveness cubierta por LTL

Las formulas LTL comprueban:

- toda clave pedida eventualmente llega a materializarse en el cache
- no hay hambruna por clave (`no_starvation_key*`)
- toda computacion admitida eventualmente deja de estar en `Computing`
- una saturacion no puede durar para siempre si las computaciones terminan

En el modelo, "materializarse" significa que la clave entra en `Ready` o
`Failed`. Si quieres exigir solo inserciones positivas, cambia `Resolver()`
para que solo complete en `Ready` o ajusta las formulas.

## Como ejecutarlo

Para correr todo:

```bash
./formal/run_spin.sh
```

Para una corrida base que chequea assertions y la propiedad principal de
no-hambruna:

```bash
cd formal
spin -a cache_model.pml
cc -O2 -DNFAIR=8 -o pan pan.c
./pan -n -E -a -f -N every_requested_key_eventually_materializes
```

Para una propiedad de liveness concreta, con weak fairness:

```bash
cd formal
./pan -n -E -a -f -N no_starvation_key2
```

## Notas

- El supuesto de liveness importante es el mismo que pediste: todo miss en
  `Computing` eventualmente se resuelve. Por eso las propiedades de progreso
  se ejecutan con `-f`.
- Se usa `-E` porque el modelo mantiene procesos de fondo (`Resolver` y
  `Background`) que quedan vivos a proposito; no queremos que SPIN reporte
  eso como un problema del protocolo.
- El modelo esta acotado a 2 slots y 3 claves para mantener el espacio de
  estados verificable. Si subes esos limites, la explosion de estados crece
  rapido.
