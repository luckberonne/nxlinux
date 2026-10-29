# Plan: port de nxnative a Linux x86_64 (Mario Kart 8 Deluxe primero)

## Context

nxnative corre juegos de Switch ejecutando el código aarch64 del juego directo en el chip de Apple. Reescribe solo unas pocas instrucciones (`svc`, `x18`, `tpidr_el0`) con `dm/tools/patch_nso.py`, y un host Horizon (`dm/host`, ~760 hooks `nn::*`) responde a las llamadas. Los gráficos son NVN sobre Metal (`dm/gfx/nvn/nvn_mtl.m`, 11k líneas).

Esta máquina es un Ryzen 5 7535HS (x86_64) con una RTX 2050 y una Radeon 680M, sobre CachyOS. Ese código aarch64 no corre en ella. El usuario aceptó reescribir el proyecto y pidió reciclar lo máximo posible, empezando por el juego mejor soportado.

**Juego objetivo: Mario Kart 8 Deluxe** (`0100152000022000`, updates 4.0.0 y 4.0.1, SDK 4.4.0, NVN).
- Su perfil `dm/games/0100152000022000/` no tiene `patches.py`, solo el recorte SDK común.
- En Mac corre a 60 fps con sonido y carreras completas.
- Brecha conocida: las cabezas Mii no se dibujan. Los otros candidatos sin `patches.py` para después son Captain Toad (`01009BF0072D4000`) y Super Mario Odyssey (`0100000000010000`).

**Enfoque:** mantener el host HLE y la cadena de shaders. Reemplazar tres piezas:
1. Ejecución nativa → emulador/JIT ARM64 (dynarmic).
2. Metal → backend Vulkan.
3. Cocoa, Core Audio, VideoToolbox y GameController → SDL3, miniaudio o SDL audio, y ffmpeg.

**Expectativa honesta:** son meses de trabajo, no días. El mayor riesgo es el rendimiento del JIT y la memoria de guest compartida con el host. Por eso el plan arranca con spikes que pueden cambiar el diseño.

## Qué se recicla y qué se reescribe

| Pieza | Acción |
|---|---|
| `dm/host/hle_*.c` portables (account, friends, mii, misc, net, offline, ro, sf, codec, oodle, fs, time, touch, swkbd, sdkinit, `hle_table.c`) | Reciclar. Arreglos pequeños: `mach_absolute_time`, `clonefile`, `strlcpy`, `_np`, libdispatch |
| `hle_hid.c`, `hle_audren.c`, `hle_nvdrv.c`, `gfx/hle_nv.c`, `gfx/hle_vi.c` | Reciclar. HID cambia su fuente de entrada a SDL |
| `hle_svc.c` (`svc_dispatch`), `hle_thread.c` (planificador sobre pthreads) | Reciclar. Se engancha al callback `CallSVC` de dynarmic |
| `loader.c`, `thread.c`, `entry.S`, `crash.c`, `probe.c`, `hle_bigstack.c`, `hle.c` (`resolve_import`) | Reescribir. Es la frontera guest/host |
| `patch_nso.py`, gates, `gates.tsv` | No se usa (el emulador tiene x18/tpidr reales). Los recortes de MK8D hay que revisarlos uno a uno |
| `dm/gfx/nvn/hle_nvn.c`, `nvn_obj.h`, `nvn_rec.h`, `nvn_pso_skey.h`, `nvn_tex.c`, `tex/nvn_deswizzle.c`, `nvn_hmap.h`, `nvn_pace.h`, `nvn_fstep.h`, `nvn_capture.c`, `gen_protos.py` | Reciclar (API-neutro) |
| `nvn_mtl.m`, `nvn_fmt_mtl.c`, `nvn_deswizzle_gpu.m`, `nvn_refl.m` | Reescribir como `nvn_vk.c`, tabla `VkFormat` y reflexión desde SPIR-V |
| `dmsh_xlate` (Ryujinx Maxwell→SPIR-V, .NET), `shader_split.py`, `dmsh_facts.py` | Reciclar. La cadena se corta en SPIR-V |
| `dmsh_build.py` (paso MSL/`xcrun metal`) | Reescribir: salida `.spv` + reflexión |
| `dm/tools/nx_extract` (LibHac), `nso.py`, `romfs.py`, `nacp.py`, `nsz` | Reciclar tal cual (multiplataforma) |
| `scripts/build_ipa_from_nsp.py` | Usar su estructura (survey → extraer → shaders). Nuevo `scripts/build_linux.py` sin Xcode |
| `front_mac.m`, `hle_audio.c` (AudioUnit), `hle_movie_avc.c` (VideoToolbox), `memstat.m`, haptics, motion Apple | Reescribir con SDL3 / ffmpeg |
| Tests C puros (`t_tex`, `t_fstep`, `t_hmap`, `t_dirty_*`, `t_capture`...) | Reciclar como base de CI en Linux |

## Fases

### Fase 0 — Repo nuevo y spikes (decide el diseño)
**Repo nuevo:** `/home/lucas/Proyectos/nxlinux` (`git init`, rama `main`), separado del clon de referencia `/home/lucas/Proyectos/nxnative-src/nxnative`, que queda de solo lectura.
- El código reciclado se copia al repo nuevo conservando la licencia MIT de nxnative (`LICENSE` + aviso de origen en un `NOTICE`), y lo de terceros con sus propias licencias (Ryujinx, LibHac, libvpx, stb_vorbis).
- **Un commit por paso verificado**, con mensaje simple de una línea y sin mencionar a Claude ni a ningún agente (regla del usuario; prevalece sobre el trailer por defecto). Cada commit sale con sus pruebas pasando (tests C/Python en Linux, o el programa de prueba de la fase).
- Compilar y probar de verdad en cada paso. Solo hago `git push` si me lo pides y hay un remoto.
- Puedo buscar en internet (documentación de dynarmic, Vulkan, formatos de Switch) cuando falte información.

1. **Disponibilidad de dynarmic.** Upstream `merryhime/dynarmic` (BSD-0); hay forks vivos como `azahar-emu/dynarmic`. Confirmar commit, licencia y que compile con CMake en CachyOS, y fijar la versión en el repo (submódulo o `FetchContent`). Unicorn se descarta por lento. Como sus mantenedores han cambiado, verificar la fuente antes de depender de ella.
2. **Modelo de direcciones.** El host hoy desreferencia punteros del guest directamente (`h_sf_Connect(uint32_t *out, const char *name)`, `memcpy`, `objmap_*` por dirección). Probar si se puede reservar la memoria del guest en una ventana fija con `mmap(MAP_FIXED_NOREPLACE)` y que guest == host, usando fastmem/page table de dynarmic A64. Si funciona, casi todo el HLE queda intacto.
   - Plan B: macros `G2H()` en cada desreferencia, un cambio mecánico pero enorme.
3. **Trampolines HLE.** Un programa aarch64 mínimo ejecuta un `svc` y llama a una función HLE. Las funciones HLE son C x86-64 con firma AAPCS64, así que hace falta un shim genérico: lee `x0..x7` y `d0..d7` y llama a la función con la convención SysV. El macro `HLE_FORCE_BIG` ya limita a 8 argumentos sin variádicos.
4. **Seguimiento de páginas sucias.** `nvn_dirty.c` usa `mprotect` + SIGSEGV. Verificar que conviva con el manejador de fallos de fastmem de dynarmic.

**Puerta de decisión:** si 2 o 4 no funcionan, replantear antes de seguir.

### Fase 1 — CPU, loader y SVCs (sin gráficos)
- Nuevo `dm/host/cpu_dynarmic.cpp`: un `Jit` por hilo de guest, con TLS real (`tpidr_el0`, `tpidrro_el0`), `CallSVC` → `svc_dispatch`, contador `cntpct` a 19.2 MHz.
- `loader.c`: mapear NSO sin reescribir (`nso.py` los descomprime) y resolver imports a páginas de trampolines (reemplaza `resolve_import`, `unimpl_stubs` y los thunks de `nvn_thunks.S`).
- `hle_thread.c`: `dm_thread_enter` pasa a fijar `sp/pc/x0` y llamar a `Run()`. Pausa con `HaltExecution`.
- Bucles de espera activa (antes "spin gates"): tratarlos con `yield` en `wfe/yield` o en saltos calientes.
- Sistema de build: CMake en lugar de Makefile con frameworks.
- **Hito:** MK8D llega a su primera llamada a `nvnBootstrapLoader` y el log lista los imports sin implementar.

### Fase 2 — Servicios y plataforma
- Compilar los `hle_*.c` portables bajo Linux y quitar las dependencias de Mach.
- Ventana, entrada y audio con SDL3. Audio: `hle_audren.c` sigue produciendo PCM y SDL lo reproduce.
- Películas (previews H.264 de MK8D): stub al inicio y ffmpeg después. VP9 ya usa libvpx de `third_party`.
- **Hito:** el juego llega al bucle de frames con la GPU como no-op.

### Fase 3 — Backend Vulkan (la fase más grande)
- Nuevo `dm/gfx/nvn/nvn_vk.c` sobre las estructuras de `nvn_obj.h`/`nvn_rec.h`. Se conserva el diseño de grabar y reproducir.
- Mapeo: argument buffers → descriptor indexing (`VK_EXT_descriptor_indexing`, ~38% de los shaders de Mario usan texturas bindless); pools de memoria NVN → `VK_KHR_buffer_device_address` con memoria host-visible; `MTL4Compiler` → `VkPipelineCache`; geometry shaders → nativos; transform feedback → `VK_EXT_transform_feedback`.
- Convención de profundidad `vs_depth` → `VK_EXT_depth_clip_control` o parche SPIR-V.
- Apagar lo que no se necesita: MetalFX y el camino TBDR (ya está apagado por defecto).
- Orden: limpiar pantalla → triángulo → texturas (deswizzle en CPU primero) → menús de MK8D → pista.
- Empezar por la Radeon 680M (memoria compartida, más simple) y luego la RTX 2050 (hay que copiar memoria).

### Fase 4 — Shaders
- `dmsh_build.py` se queda en `dmsh_xlate` → `.spv` + `.res.json`; sin `spirv-cross` ni `xcrun`.
- Reescribir `reflect()` (hoy parsea texto MSL) con SPIRV-Reflect.
- Portar a SPIR-V los parches que hoy se aplican sobre MSL: `vs_pad`, `vs_depth`, `vs_layer`, `vs_tf`, `vs_discard`, `fs_dual`, `fs_uint`, `vs_base_instance`.
- 29,785 shaders de MK8D: compilar offline una vez; crear pipelines en runtime con caché.

### Fase 5 — Pulido y rendimiento
- Perfilar el JIT, los pipelines y la sincronización de hilos. Meta: 30 fps estables y luego 60.
- Cabezas Mii (brecha conocida).
- Segundo juego: Captain Toad u Odyssey, para comprobar que el port no es específico de MK8D.

## Archivos clave
- Host: `dm/host/{loader,hle_svc,hle_thread,thread,hle,entry,crash,main}.c`, `hle_table.c`, `hle_audio.c`, `hle_movie*.c`, `front_mac.m`
- Gráficos: `dm/gfx/nvn/{hle_nvn.c,nvn_obj.h,nvn_rec.h,nvn_mtl.m,nvn_dirty.c,nvn_thunks.S}`, `dm/gfx/tex/`, `dm/gfx/tools/{dmsh_build.py,dmsh_xlate/Program.cs,shader_split.py}`
- Perfil: `dm/games/0100152000022000/{game.tsv,README.md}`
- Orquestación: `scripts/build_ipa_from_nsp.py` (referencia para `scripts/build_linux.py`)

## Verificación
1. **Unitarios:** compilar y ejecutar en Linux los tests C puros de `dm/gfx/tests` y `dm/gfx/tex/tests` (en particular los 1,029 layouts diferenciales de `t_tex`).
2. **Fase 0:** programa aarch64 de prueba con `svc` y llamadas HLE, con resultados conocidos.
3. **Fase 1:** `nx_extract` sobre las NSP de MK8D (más ambos updates) y comparar el log con el flujo de arranque de `docs/agents/03-bringup.md`.
4. **Fases 3–4:** validation layers de Vulkan sin errores, y capturas con RenderDoc comparadas con las capturas del README de MK8D.
5. **Aceptación:** menús, carrera de demostración y Gran Premio completo con sonido, medido en fps.
6. **Ensayo sintético (hecho):** `tools/synth_nsp.py` genera un `prod.keys` desechable y NSP de un juego falso (base + update, ExeFS con NSOs y RomFS con IVFC en la sección Data); el `nx_extract` vendido de `nxnative-src` los acepta de punta a punta: extrae el ExeFS, recorre el RomFS (copia en carpetas, `--manifest` y `--romfs-image` con la conversión DMROMFS1), combina base + update y detecta un byte corrupto (`[RED] hash check failed` + exit 3, `--allow-damaged` para leerlo tal cual). En `tests/test_synth_nsp.py` (21 pruebas): referencias XTS/CTR de OpenSSL, contenedores a mano e integración si hay `work/nx_extract`. Con archivos reales el punto 3 reutiliza el mismo camino. Detalles del RomFS sintético: árbol IVFC con región 0 (tabla SHA-256 de la región 1 en bloques de 0x1000), región 1 (tabla del RomFS en bloques de 0x4000) y master hash de una sola pieza sobre la región 0; los digests son SHA-256 puro de cada bloque rellenado con ceros (el lector de `Tools/FsSystem` rellena y compara a saco; el bit de validación es cosa de save data); dentro, header de 0x50 con las nueve posiciones en u64 desde 0x08, cubetas como `RomfsEntryKey.GetRomHashCode % primo`, entradas de 24/32 bytes con cadena de hermanos por directorio y cadena de cubetas por nombre. Extras para probar la ruta completa hasta el kernel: `--main-elf` mete al base un NSO aarch64 real embalado desde un ELF del huésped (el de clang+lld: hasta 3 PT_LOAD en orden texto→ro→data, cada vaddr en página de 0x1000, texto en 0, huecos de página permitidos, bss sólo al final; los vaddr quedan como memory offsets del NSO para que las direcciones preenlazadas sigan resolviendo, y el build id sale de la nota GNU del ELF); y el RomFS lleva `control.nacp` (0x4000, idioma/nombre/editor en la entrada 0, versión en 0x3000). El guion de link que respeta esa forma (lld no pagina sin `PHDRS` declarados): `PHDRS { text PT_LOAD FLAGS(5); ro PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); }` con `SECTIONS { . = 0; .text : {...} :text  . = ALIGN(0x1000); .rodata : {...} :ro  . = ALIGN(0x1000); .data : {...} :data }` y `-Wl,-T,script,-z,max-page-size=0x1000`; un `.so` viejo (4 LOADs, código a 0x10eac) lo rechaza con ese instructivo. Y para que el propio `main` del juego sea código compilado: `tools/synth_mod0.S` + `tools/synth_guest.ld` dan a un guest de clang+lld el MOD0, la sección dinámica y .dynsym/.dynstr/.rela.dyn dentro de la imagen (el bloque MOD0 va clavado por el guion al inicio del segmento data en 0x2000, la dinámica justo detrás; el huésped enlaza `-shared -fPIC -nostdlib -nostartfiles -fuse-ld=lld -Wl,-T,tools/synth_guest.ld,-z,max-page-size=0x1000`), y `--main-elf` lo embala. Probado de punta a punta por primera vez: `nx_extract` → `module_map`/`module_relocate` (MOD0, dinámica, relocations, 0 imports sueltos) → la CPU corre `main` de verdad (devuelve 42; deja en .bss `0x5A5A5A5A` y el puntero relocatado llega al texto de `hello[]`). Uso: `python tools/synth_nsp.py <dir> [--seed N] [--no-romfs] [--main-elf guest.elf]`. El bloque y su dinámica son posicionales: con huéspedes grandes lld mete .got/.got.plt en el segmento data, así que el guion toma *(.got) ANTES del bloque y deja el orden determinista (.got -> bloque MOD0 -> .dynamic en bloque+8; commit 42fa66b). Smoke de arranque de juego (30-10): un guest con la API NVN completa (device, pool, buffers, queue, command buffer, clear naranja sobre RADV, CopyTextureToBuffer y verificación de píxel, salida por svc) compilado plano, embalado con --main-elf y corrido entero desde el NSP contra el backend Vulkan del port: PASS (guest y driver quedan fuera del repo, /tmp).

## Estado y hoja de ruta (actualizado el 2026-10-29)

### Dónde estamos (todo probado sin material de ningún juego)
| Fase | Estado |
|---|---|
| 0. Spikes | **Hecha.** dynarmic con fastmem de identidad (guest == host), trampolines HLE, seguimiento de páginas sucias. |
| 1. CPU, loader, SVC | **Hecha con sintéticos.** Un `Jit` por hilo, cargador de NSO/ELF con relocaciones, HLE por `svc`, planificador de hilos Horizon, SVC de memoria, hilos, sincronización, eventos. Falta probarla con un NSO retail. |
| 2. Servicios y plataforma | **No empezada** (salvo lo que NVN necesita). ~760 hooks `nn::*`, SDL3, audio, entrada, ffmpeg. |
| 3. Vulkan | **Avanzada.** Pools importados con host-pointer, colas y fences, repetición de ~45 operaciones, texturas (subida por subrecurso), limpiezas, dibujos (estado, pipelines, blending, profundidad, índices, UBO) y muestreo de texturas 2D. Validación de Vulkan limpia. |
| 4. Shaders | **Cadena montada.** Maxwell → SPIR-V (traductor Ryujinx vendido) → biblioteca NXSL v2 → registro por clave → dibujo. Probada con shaders ensamblados a mano (`tools/maxwell_asm.py`). |
| 5. Pulido | No empezada. |

17 etapas de dibujo, 16 tests en CTest. Los controles negativos con `NX_NULL_GPU=1` fallan como deben.

### Lo que se puede avanzar sin juego, en este orden
1. **Guest como NSO en un NSP sintético** (corto). Regla CMake que compile `tests/guest/nvn_draw.c` con `tools/synth_guest.ld` y `tools/synth_mod0.S`, que `synth_nsp.py` lo empaquete, y un test que recorra `nx_extract` → `nso.py` → `module` → kernel → NVN. Cierra el camino de carga real salvo las claves de verdad.
2. **Segmentos de driver c0/c2 y `support_buffer`** (medio). Casi todo shader real los lee: tablas de handles de texturas, tablas de storage buffers, tamaño de punto, instancia base, y el buffer de soporte del traductor (escala de render, tamaño de viewport, inversión en y). Formato en `src/gfx/nvn/dmsh_abi.h`. Sin esto la mayoría de los shaders se salta por `needs`.
3. **Resto de recursos de shader** (medio): textura ficticia para slots sin registrar, texturas separadas y samplers separados, cubos, arrays, 3D, sombras, storage buffers, imágenes, handles bindless por UBO.
4. **Etapas que faltan** (medio a grande): geometry, tessellation, compute y dispatch; draws indirectos; transform feedback; MSAA; viewports múltiples; primitivas sin equivalente (line loop, quads, índices de 8 bits); logic op y alpha test.
5. **Formatos que RADV no tiene** (medio): decodificar ASTC, ETC2 y D24S8 por compute o convertir a BC7 / D32S8.
6. **Memoria virtual de NVN** (pequeño a medio): pools virtuales y dispersos necesitan una ventana de guest compartida (memfd); hoy es un caso abierto de `vm_remap`.
7. **Presentación** (medio): ventana SDL3, swapchain para `NVNwindow`, pacing de frames. Se puede probar con un guest que dibuje en bucle.
8. **Servicios HLE** (el trabajo más grande, ~760 hooks): portar en lotes los `hle_*.c` de nxnative en el orden en que los toque el arranque: `sm`/`sf`, `fs`, `time`, `account`, `hid`, `nvdrv`, `vi`, `audren`, resto. Cada lote se prueba con un guest sintético que llama al servicio. Entrada con SDL, audio con SDL.
9. **Película y audio** (medio): H.264 con ffmpeg, VP9 con libvpx, `audren` hacia SDL.

### Lo que necesita material que yo no puedo conseguir
- **MK8D real**: `prod.keys` y las actualizaciones 4.0.0 y 4.0.1 de la propia consola del usuario (Lockpick_RCM, nxdumptool). Hoy solo existe la base 1.0.0, que no basta. Sin ellos no hay primer frame real, y no los busco ni descargo de terceros.
- **Cadena de shaders del juego** (`dmsh_chain.py`, `shader_split.py`, `dmsh_facts.py`, una versión Linux de `build_ipa_from_nsp.py`): se porta ya, pero solo se ejecuta contra los NCA descifrados del usuario. Los ~29 785 shaders de MK8D se traducen una vez, fuera de línea.

### Riesgos, por orden de daño
1. Rendimiento del JIT en el hilo de render y en los bucles de espera activa (se mide recién con un juego real).
2. Cantidad de servicios y su exactitud (un hook mal hecho cuelga el arranque; nxnative ya tiene los parches por juego como guía).
3. Diferencias de Vulkan que solo salen con shaders reales: profundidad, orientación, subgrupos, precisión.
4. Tamaño de página de 4 KiB frente a los supuestos de 16 KiB en `nvn_dirty.c`.
5. La RTX 2050 no aparece en Vulkan (solo la Radeon 660M); si se quisiera usar, hace falta resolver el driver.

### Cómo se mide el avance
Cada paso cierra con un test que falla con `NX_NULL_GPU=1` o sin la pieza nueva, validación de Vulkan limpia y un commit de una línea. Los hitos de aceptación de la sección Verificación siguen siendo los de arriba.

## Requisitos y notas
- **Sobre `prod.keys`:** revisé el repo (`README.md`, `docs/`, `scripts/build_ipa_from_nsp.py`) y la página que mandaste. Ninguno los incluye ni explica cómo obtenerlas; solo piden que las traigas (`-k prod.keys`). No se pueden evitar: los NCA del juego están cifrados y `nx_extract` (LibHac) las necesita para leer el RomFS y el ExeFS. Tampoco voy a buscar ni descargar claves de terceros. La vía legítima es sacarlas de tu propia consola con Lockpick_RCM (junto con el volcado de tus juegos con NXDumpTool). Si no tienes consola, la parte del juego real no se puede probar; el repo nuevo igual avanza con los programas de prueba.
- Necesitas tus propios dumps de MK8D (NSP base + update 4.0.0 y 4.0.1) y `prod.keys`. Quedan fuera del repo (carpeta de trabajo aparte, nunca en git). Hasta tenerlos, las fases 0 y 3 se prueban con programas aarch64 de prueba y tests sintéticos; las fases 1–2 y 4–5 necesitan los archivos reales.
- Commits sin mencionar a Claude ni a ningún agente, mensajes de una línea (regla del usuario, skill `commits-sin-claude`).
- Dependencias nuevas: dynarmic, SDL3, ffmpeg, SPIRV-Reflect, CMake, .NET (ya requerido por `nx_extract`); y `cryptography` para la venv de Python (kit sintético de `tools/synth_nsp.py`).
- El código de `dm/` seguirá en inglés, como pide `AGENTS.md`, y las líneas rojas de log empiezan con `[RED]`.
