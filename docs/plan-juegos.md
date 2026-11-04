# Plan: Smash, Mario Party y los juegos que siguen

Próximos pasos para optimizar, mejorar y reutilizar lo hecho en Super Smash Bros. Ultimate y Mario Party Superstars en otros juegos de Switch. MK8D tiene su propio plan en [next-steps.md](next-steps.md); esta lista no lo repite. Detalles de cada juego: [smash-ultimate.md](smash-ultimate.md), [mario-party-superstars.md](mario-party-superstars.md).

## 1. Estado

| Juego | Llega a | Sin resolver |
|---|---|---|
| Smash Ultimate 1.0.0 (NSZ + 99 DLC) | título, menús, selección de reglas y de luchadores, pelea | retratos de resultados en silueta (la tarjeta se dibuja, el retrato sale plano, sin capa de color); "Challenger approaching" sin verificar; audio de las películas en silencio; escenas 3D parciales |
| Mario Party Superstars 1.0.0 | título, menú, selección de jugadores, cinemática con escenario | suelo gris (fix de arrays implementado, sin verificar; ver B); sonido sin probar; juegos de tablero sin probar |

Medido con la misma prueba en Smash (2 minutos headless, pulsaciones de A, `NX_SKIP_LOG=1`):
- Dibujos saltados por "shaders not in the registry": 540 antes de reconstruir la librería, 0 después (commit actual `734ec50`). La causa fue cobertura: la captura no había visitado algunas escenas.
- Dibujos saltados en Mario Party: 146 shaders con `NVN_NEED_TEX`, todos del suelo.

Medido en MPS: un frame entero tarda 90-100 ms en la prueba headless con capturas. Esa cifra no sirve para medir velocidad (las capturas la frenan); está pendiente medirla sin capturas.

## 2. Lo que se reutiliza

Estas piezas ya no son de un solo juego. Cualquier juego nuevo las usa:

- **Pipeline de shaders:** `NX_SHADER_DUMP` → `tools/shader_capture_build.py` → librería `.nxsl`. Se corre sobre cada juego.
- **Medición de cobertura:** `NX_SKIP_LOG=1` cuenta los motivos de dibujo saltado. Es la primera prueba de cada juego nuevo.
- **Arranque:** `tools/run_title.py` (extraer NSP/NSZ), más un `tools/run_<juego>.sh` con el mismo esquema que `run_smash.sh` y `run_mps.sh`.
- **Lanzador de KDE:** un `.desktop` por juego en `~/.local/share/applications`, con `StartupWMClass` igual a `NX_APP_ID`.
- **Soporte de servicios:** `ipc_irs.c`, `ipc_nfp.c`, `ipc_hwopus.c`, `ldr:ro`, `aoc` (DLC con licencia sin datos).
- **Descriptores bindless:** `NVN_SB_TEX_UBO` y `NVN_SB_IMG` (imágenes de almacenamiento) ya están en `shader_pack.py` y el draw path.

Regla para cada juego nuevo: primero se corre con dump y `NX_SKIP_LOG`, y solo después se decide qué código tocar. Así se sabe si el problema es de cobertura de la librería o de soporte del dibujo.

## 3. Próximos pasos (en orden)

### A. Cerrar Smash (verificación)
1. Partida completa hasta resultados y "Challenger approaching", con capturas a tamaño completo. Confirmar si las letras faltantes y los retratos siguen mal.
2. Si siguen mal: mirar el atlas de glifos y la textura del retrato con `NX_TEX_LOG`. Son los dos problemas que el usuario pidió arreglar.
3. Error de carga de batalla: localizar la llamada nula con `NX_TRAP` y `NX_BREAK_STACK`.
4. Actualizar la memoria del proyecto y `smash-ultimate.md` con el resultado verificado.

Resultado de la verificación headless (sesión con `NX_HID_TAP=0:5` / `NX_HID_PIPE`, `NX_TEX_LOG=1`, capturas a tiempo completo):

- Las letras no faltan ya: el fix de los atlas lineales (`NX_TEX_DIRTY_UPLOAD=1 NX_TEX_LINEAR=1`, `run_smash.sh`) está verificado en las pantallas de resultados y de texto (texto completo, sin huecos).
- Los retratos de resultados siguen en silueta: la tarjeta se dibuja entera (fondo, retrato, stats, texto) pero el retrato sale plano, sin la capa de color del personaje. No está resuelto; falta mirar la textura del retrato con `NX_TEX_LOG` / `NX_TEX_RAW`.
- El error de carga de batalla no era una llamada nula: sin pelea no hay llamada de carga. La partida completa (Fox con stick arriba + A) llegó a resultados sin el fallback.
- Única respuesta de servicio distinta de cero en una partida entera (`NX_IPC_FAIL`, debug nuevo en `src/host/ipc.c`): `mii.db cmd 4 -> 0x1d0a02 args: 0x1` (el título pide la base de datos Mii y no la hay). Sin otro `ErrorResultVariant` visible.
- "Challenger approaching" sin verificar: falta una pelea contra un retador aleatorio.

### B. Mario Party: arrays de texturas (implementado, sin verificar)
El intento de llenar los 32 elementos en push descriptors tumbó el proceso porque el GPU tiene `maxPushDescriptors = 32` (ver [mario-party-superstars.md](mario-party-superstars.md)). Opciones, de menor a mayor costo:

1. **Set de descriptores normal para los arrays.** Pool por submit: se reserva al grabar el comando, se libera cuando termina la cola. Hay que ver cómo `vkctx` devuelve los recursos a la cola (ya lo hacen `nvn_vk_defer_image` y los pools). Cambio en `make_layout` y en el enlace del draw.
2. **Un set bindless global** (PARTIALLY_BOUND, UPDATE_AFTER_BIND, ya habilitado en `vkctx.c`): se llena cuando cambia el registro de texturas, no por draw. Más trabajo inicial, mejor rendimiento. Sirve también para otros juegos con arrays bindless.
3. **Reescribir el SPIR-V** para que el índice no recorte a 31 y use el ID real. Solo si los dos anteriores no bastan.

Estado (opción 1 implementada, sin commit todavía):

- Se volvió a aplicar la parte buena del intento descartado (`work/run/mps/texarray_push_overflow.patch`): los tipos `NVN_SB_TEXA` / `NVN_SB_SAMPA` (`nvn_shader_vk.h`), el llenado por ID de pool (`array_fill` en el draw, `nvn_vk_desc_array_tex` / `nvn_vk_desc_sampler_id` en `nvn_desc_vk.c`) y `shader_pack.py` (`texture_array_kind`, `KIND_TEXA` / `KIND_SAMPA`).
- Opción 1: en `make_layout`, si algún binding es array (`TEXA` / `SAMPA`), el layout ya no es de push: se crea un layout de set normal, se reserva un `VkDescriptorSet` del pool nuevo de `vkctx` (`c->desc_pool`, creado ahí al primer uso) y en el draw `prepare` llena el set con `vkUpdateDescriptorSets` (cada array va en una escritura de 32 elementos) y lo enlaza con `vkCmdBindDescriptorSets`. Las canales sin arrays siguen por push descriptors, sin cambios en su código. Con esto deja de importar `maxPushDescriptors = 32`.
- Un dispatch de compute con array bindings se salta con su mensaje propio (igual que antes, no lo toca el cambio).
- Compila en `build-opencode`; los 21 tests de ctest pasan. MK8D título headless (`NX_PRESENT=null NX_FRAME_EVERY=60`): arranca y dibuja hasta el frame 6960 sin saltos nuevos ni errores.
- Pendiente para cerrar B: carrera de MK8D; correr MPS y comprobar el criterio de éxito; reconfirmar Smash. El `mps.nxsl` del 3 de nov se empaquetó con este parche puesto, así que sus shaders del suelo deberían salir ya como `TEXA` y no `NEED_TEX` (verificarlo al arrancar). Nada commiteado: los archivos con el cambio son `src/gfx/nvn/nvn_draw_vk.c`, `nvn_desc_vk.c`, `nvn_shader_vk.h`, `nvn_vk_priv.h`, `src/gfx/vk/vkctx.{c,h}` y `tools/shader_pack.py`.

Criterio de éxito: `NVN_NEED_TEX` = 0 en MPS, el suelo se dibuja y MK8D y Smash no cambian (21 tests de ctest y las capturas de título y carrera).

### C. Depth-stencil desde almacenamiento (formato 0x35)
Aparece en MPS y en la prueba de Smash. Hoy se salta la subida de la textura. Es necesario para sombras y efectos que leen profundidad. Medir primero cuántos dibujos dependen de esa textura con `NX_TEX_LOG`.

### D. Audio
- Smash: las películas (movie) van en silencio porque `hwopus`/VP9 solo da PCM silencioso. Falta decodificar el audio de la película (`hle_movie.c`, tipo 5).
- MPS: la revisión 8 del renderer solo se acepta en la cabecera. Falta leer las secciones y mezclar. Probar con `NX_AUDIO_LOG` (si no existe, agregarlo).
- Criterio: una corrida de 2 minutos con audio en vez de `NX_AUDIO=0`, y comparar la duración de los eventos de sonido con el audio original.

### E. Velocidad
Todavía no hay una medición de velocidad para Smash ni MPS. Antes de optimizar:
1. Medir sin `NX_SDL_READBACK` ni `NX_FRAME_DUMP` (esas dos capturas frenan la prueba, según `next-steps.md`).
2. Usar `NX_PROF` como en MK8D (ver `mk8d-bringup.md`) y ubicar si el tiempo está en el invitado, en el replay o en el submit.
3. Solo después, cambiar código. Los candidatos conocidos son el número de descriptores por draw y las vistas de textura creadas por draw (`nvn_vk_tex_sampled`).

### F. Reutilización para otros juegos
1. Elegir un tercer juego (que no sea Smash ni Mario Party) y correr la secuencia de la sección 2 sin cambiar código.
2. Anotar los motivos de salto que aparezcan. Si aparecen `NVN_NEED_TEX` o `NVN_NEED_IMG`, el trabajo de la sección B sirve también a ese juego.
3. Crear `docs/<juego>.md` con el mismo formato que los dos anteriores: estado, qué se arregló, qué falta, comandos.

## 4. Lo que no conviene hacer todavía

- Otro traductor de shaders: el de `dmsh_xlate` cubre lo que hace falta. Los casos sin soporte están marcados.
- Optimizar antes de medir sin capturas.
- Tocar el draw path de MK8D sin correr su regresión (título y carrera, 21 tests).

## 5. Reglas de trabajo

- Cada cambio en `src/gfx/nvn/` se verifica en MK8D (título y carrera) antes de subir.
- Los cambios de la librería de shaders se anotan en el documento del juego, con el número de shaders antes y después.
- Los archivos fuera de la propia área se reclaman con `tools/agentmsg.py claim` antes de editarlos.
- El dump de shaders se hace con la misma secuencia de escenas que la prueba, o la librería queda incompleta (ver la sección 1).
