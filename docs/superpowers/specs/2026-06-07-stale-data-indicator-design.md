# Indicador de "datos viejos / sin conexión" — diseño

Fecha: 2026-06-07. Branch: `feat/sonos-control`. Board objetivo: `waveshare_amoled_216`.

## Problema

El equipo (ESP32-S3 AMOLED 2.16) muestra el consumo de tokens de Claude que le
entrega el daemon de la Mac por BLE. Cuando el token OAuth de Claude Code expira,
el daemon deja de entregar datos frescos, pero:

- El enlace BLE sigue `Connected` (confirmado por el usuario — la pantalla BT
  sigue diciendo "Connected").
- La animación del spinner en la pantalla de uso (`ui_tick_anim()`, `ui.cpp:557`)
  **corre siempre** que la pantalla de uso está activa, sin importar si llegan
  datos nuevos.

Resultado: la pantalla "parece viva" con números viejos. El usuario solo se entera
mirando el consumo real en Claude, y lo arregla reabriendo `claude` en la terminal
(eso refresca el token en el Keychain → el daemon retoma).

El firmware **no guarda ningún timestamp** del último dato fresco (`main.cpp:332`
parsea y actualiza, pero no registra cuándo). No hay forma de detectar staleness.

## Objetivo

Que el equipo avise de forma imposible de ignorar cuando los datos están viejos o
el enlace cayó, para que el usuario tome acción (reabrir claude / revisar el daemon).

## Enfoque elegido (Opción A — firmware-only)

Toda la lógica vive en el **firmware**. Sin cambios en el daemon, el protocolo BLE
ni `data.h`. El firmware ya conoce el estado BLE (`ble_get_state()`), así que puede
dar un mensaje preciso según la causa:

- BLE `Disconnected` → "SIN CONEXIÓN" (cayó el enlace ESP32↔Mac).
- BLE `Connected` + datos viejos > 3 min → "DATOS VIEJOS — reabrí claude"
  (caso típico de token expirado).

El timeout del firmware es la red de seguridad: cubre **todas** las causas (token
expirado, daemon caído, Mac dormida, caída de red, outage de Anthropic), no solo el
401.

Descartado (YAGNI): que el daemon señalice el 401 con un payload especial (Opción B).
Más trabajo, toca el protocolo, y **igual** requiere el timeout del firmware como
respaldo. Se puede sumar después si se quiere el wording exacto "TOKEN EXPIRADO".

Fuera de scope: el cambio sin commitear del daemon (anti-spam de reintentos 401) es
complementario pero independiente; se decide por separado.

## Componentes

### API pública nueva (`ui.h`)

```c
void ui_note_data_fresh(void);  // main.cpp la llama cuando un payload parsea OK
void ui_tick_status(void);      // main.cpp la llama cada loop; evalúa y pinta el banner
```

### Estado privado (`ui.cpp`)

- `static lv_obj_t* status_banner` — label creado en `ui_init()` sobre
  `lv_layer_top()`. Franja de ancho completo arriba, alto ~36px, oculta por defecto
  (`LV_OBJ_FLAG_HIDDEN`). Al vivir en el top layer de LVGL, flota sobre **cualquier**
  pantalla (uso, BT, sonos, splash) y persiste a través de los cambios de pantalla
  sin recrearse.
- `static uint32_t last_fresh_ms` — `lv_tick_get()` del último dato fresco.
- `static bool ever_received` — "armado". La staleness solo se evalúa después del
  primer dato fresco; un boot recién flasheado (sin datos aún) no dispara falsa
  alarma.
- `static int last_banner_status` — cache del último estado pintado, para no llamar
  a LVGL en cada loop (solo cuando el estado cambia).

### Constante

```c
#define STALE_THRESHOLD_MS 180000   // 3 min = 3 polls del daemon (poll cada ~60s)
```

## Lógica de estado (`ui_tick_status`, cada loop)

Estados: `STATUS_OK = 0`, `STATUS_STALE = 1`, `STATUS_DISCONNECTED = 2`.

**Gate de armado:** si `!ever_received` → siempre `STATUS_OK` (sin banner),
sin importar el estado BLE. Esto evita la falsa alarma en cada arranque: al boot
el enlace está `ADVERTISING`/`INIT` hasta que el daemon conecta, y todavía no
llegó ningún dato. El indicador solo tiene sentido una vez que el sistema estuvo
sano al menos una vez. (El caso "nunca conectó en un boot nuevo" es de
primer-setup, no el problema recurrente que resolvemos.)

Una vez `ever_received == true`, evaluación por prioridad:

1. Si `ble_get_state() != BLE_STATE_CONNECTED` → `STATUS_DISCONNECTED`.
2. Si no, y `(lv_tick_get() - last_fresh_ms) > STALE_THRESHOLD_MS` → `STATUS_STALE`.
3. Si no → `STATUS_OK`.

Acción solo cuando el estado calculado != `last_banner_status`:

- `STATUS_DISCONNECTED` → banner visible, fondo rojo, texto `⚠ SIN CONEXIÓN`.
- `STATUS_STALE` → banner visible, fondo ámbar, texto `⚠ DATOS VIEJOS — reabrí claude`.
- `STATUS_OK` → banner oculto (`LV_OBJ_FLAG_HIDDEN`).

`ui_note_data_fresh()` setea `last_fresh_ms = lv_tick_get()` y `ever_received = true`.

El spinner de la animación y los números de uso quedan **como están** — el banner
flotante es la señal. Scope acotado a lo elegido (banner en todas las pantallas).

## Cableado (`main.cpp`)

- En el bloque `ble_has_data()` (~línea 333), cuando `parse_json(...)` devuelve true,
  agregar `ui_note_data_fresh();` (junto a `ui_update(&usage); ble_send_ack();`).
- En `loop()`, agregar una llamada `ui_tick_status();` por iteración (junto a las
  otras llamadas de tick). Costo trivial: solo toca LVGL cuando cambia el estado.

## Interacción con sleep

Cuando la pantalla duerme (`idle`), LVGL no renderiza. Al despertar, `ui_tick_status()`
vuelve a correr y refleja el estado actual. El banner es un objeto LVGL normal: si
sigue stale al despertar, sigue visible. Sin manejo especial requerido.

## Plan de pruebas (QA propio con `./screenshot.sh`)

1. **OK / sin banner**: con daemon corriendo y datos llegando, el banner debe estar
   oculto en todas las pantallas. Capturar PNG de la pantalla de uso → sin banner.
2. **Datos viejos** (BLE conectado, sin datos frescos): el escenario real
   (daemon vivo pero sin entregar) es difícil de forzar. Para testear el estado
   STALE de forma determinística, build temporal con (a) `STALE_THRESHOLD_MS`
   corto (ej. 15000) y (b) la llamada a `ui_note_data_fresh()` comentada en
   `main.cpp` para que `last_fresh_ms` nunca se refresque mientras el daemon
   mantiene vivo el enlace BLE. Tras ~15s con BLE `Connected`, capturar PNG →
   banner ámbar "DATOS VIEJOS — reabrí claude". Revertir ambos cambios antes de
   commitear.
3. **Sin conexión**: `launchctl unload` del daemon LaunchAgent, esperar el disconnect
   BLE, capturar PNG → banner rojo "SIN CONEXIÓN".
4. **Recuperación**: reabrir claude / recargar el daemon → al llegar el próximo dato
   fresco, `ui_note_data_fresh()` resetea el timer y el banner desaparece. Verificar.

## Criterios de aceptación

- [ ] Tras 3 min sin dato fresco con BLE conectado, aparece el banner ámbar en
      cualquier pantalla activa.
- [ ] Si el enlace BLE cae, aparece el banner rojo "SIN CONEXIÓN".
- [ ] Un boot recién flasheado (sin datos aún) NO muestra banner.
- [ ] Al volver a llegar datos frescos, el banner desaparece automáticamente.
- [ ] Cero regresión en el footprint de heap (banner es un solo label en top layer,
      costo despreciable).

## Archivos afectados

- `firmware/src/ui.h` — declarar `ui_note_data_fresh()`, `ui_tick_status()`.
- `firmware/src/ui.cpp` — banner en top layer, estado, lógica, constante.
- `firmware/src/main.cpp` — llamar `ui_note_data_fresh()` al parsear OK y
  `ui_tick_status()` en el loop.
