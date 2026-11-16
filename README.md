# Mansion

Un programa para jugar **The Addams Family** de Super Nintendo (Ocean, 1992) en PC, con menús nuevos, trucos, mapa, boss rush, modo fácil y más. Linux y Android están en camino.

El juego no se modifica: Mansion corre **tu propia ROM** con un núcleo de emulación (libretro) y le agrega todo lo nuevo leyendo y escribiendo la memoria del juego. Los menús usan la fuente, el marco de piedra y la mano-cursor **del propio juego**, que el programa saca de tu ROM al arrancar. Por eso este repositorio no contiene nada del juego.

*English summary at the end.*

---

## Qué tiene

**Menús**
- **Menú de inicio propio**: Jugar / Continuar / Partida nueva, Ir a nivel, Boss rush, Códigos, Cargar estado, Opciones, Salir. Jugar entra directo a la partida (el programa recorre solo el menú viejo del juego, que solo aceptaba Start).
- **Pausa en cualquier momento** con F1 o Esc (también Select/Back o Select + Start en el mando): Continuar, Ir a nivel, Trucos, Mapa, Progreso, Boss rush, Guardar/Cargar estado, Códigos, Opciones, Menú de inicio, Salir.
- **Español e inglés**, se cambia al instante. La primera vez toma el idioma del sistema.

**Trucos** (se aplican sin reiniciar)
- **Modo fácil**: cada golpe quita un cuarto de corazón; el corazón se va oscureciendo de a cuartos en el HUD.
- **Modo dios**: no recibís daño.
- Vida infinita, vidas infinitas, sombrero volador (fez-copter sin límite), espada, pelotas de golf.
- **Sin slowdown**: más velocidad al procesador emulado para que el juego no se ponga lento.

**Para jugar a tu manera**
- **Ir a nivel**: las 15 zonas del juego. Conserva corazones, objetos, vidas y familia rescatada; no te da nada que no tuvieras.
- **Códigos**: elegís un password de la lista y el programa lo escribe solo en la pantalla de passwords del juego. Todos los de la lista fueron probados en la ROM USA.
- **Mapa**: se dibuja solo con lo que vas viendo de cada zona; el punto rojo es Gomez.
- **Progreso**: tiempo total, familia rescatada, corazones y tiempo por zona.
- **Timer en pantalla** opcional.
- **Boss rush**: se desbloquea al terminar el juego. Mientras jugás, la primera vez que llegás a cada jefe el programa guarda el comienzo de la pelea. Ya desbloqueado elegís las reglas (corazones 2-5, vida infinita, vidas 1/3/5/9/infinitas, espada, pelotas de golf) y los jefes se encadenan con tiempo total.

**Lo de siempre**
- Guardar / cargar estado: F5 / F7, o LT·RT / L3·R3 en el mando.
- Avance rápido (Tab), pantalla completa (F11 o Alt+Enter).
- **Controles remapeables** (Opciones → Controles), teclado y mando.

## Cómo usarlo (Windows)

1. Descargá o compilá `Mansion.exe` y poné junto a él `SDL2.dll` y `snes9x_libretro.dll` (o cualquier núcleo libretro de SNES con ese nombre).
2. Poné tu ROM al lado: **`Addams Family, The (USA).sfc`** (también podés arrastrarla sobre el programa). Tiene que ser la versión USA sin modificar (CRC32 `2E8034AB`).
3. Abrí `Mansion.exe`. La primera vez tarda un par de segundos mientras saca el arte de los menús de tu ROM.

No se incluye ni se enlaza ninguna ROM. Usá la tuya.

### Controles por defecto

| SNES | Teclado | Mando |
|---|---|---|
| Cruz | Flechas | Cruceta / stick izquierdo |
| B (saltar) | Z | A (abajo) |
| A | X | B (derecha) |
| Y / X | A / S | X / Y |
| L / R | Q / W | LB / RB |
| Start | Enter | Start |
| Select | Shift derecho | Select / Back |

Siempre: **F1 o Esc** pausa · **F5 / F7** guardar / cargar · **Tab** avance rápido · **F11** pantalla completa.

## Compilar

```sh
# Linux
sudo apt install libsdl2-dev
make

# Windows desde Linux (MinGW-w64 + el paquete "SDL2-devel-…-mingw")
make windows SDL2_MINGW=ruta/a/SDL2-2.30.9/x86_64-w64-mingw32
```

El núcleo snes9x se compila desde [libretro/snes9x](https://github.com/libretro/snes9x) (`make -C libretro`, o `platform=win` con MinGW).

`make test` corre todas las pantallas sin ventana y guarda capturas en `test-out/`; también comprueba trucos, modo fácil, passwords, modo dios, ir a nivel, mapa y boss rush (necesita la ROM y el núcleo al lado).

## Cómo funciona

- `src/mansion.c` es todo el programa: carga el núcleo libretro, video y sonido con SDL2, menús, trucos y herramientas.
- Al arrancar, el programa juega solo hasta la pantalla de passwords sin mostrarla, y de la memoria de video copia el marco de piedra (capa BG1), la fuente de 8x8 y la mano del menú.
- Los menús viejos del juego se recorren con secuencias fijas de botones desde el reinicio: la emulación es determinista, así que siempre caen igual.
- **Modo fácil**: el juego solo cuenta corazones enteros. Antes de cada cuadro el programa guarda un estado; si un golpe llega y al corazón le quedan cuartos, vuelve a ese estado, le da a Gomez la invulnerabilidad que el juego da tras un golpe y juega el cuadro de nuevo, así el golpe no ocurre. Mientras un corazón está gastado a medias, el juego cree tener uno menos, para que un corazón recogido siempre cure.
- **Ir a nivel**: escribir la zona mientras la partida carga la hace empezar ahí; después se copia el progreso que tenías.

### Memoria del juego (7E:00xx, versión USA)

| Dirección | Qué es |
|---|---|
| 64 | Fez-copter (FF = sin límite) |
| 8A | Zona (00–0E); escribirla mientras carga la partida arranca en esa zona |
| A1 / A2 | Posición de Gomez en bloques de 16 px |
| A9 / AB / AC | Puntos / dinero / vidas |
| C3 / C4 | Corazones actuales / máximos |
| C6 | Invulnerabilidad tras un golpe (80, baja 1 por cuadro) |
| EE / EF | Espada / pelotas de golf (1 = las tiene) |
| F6 | Vida del jefe (FF sin jefe) |
| 100–10B | Progreso: 100 Giant Budgie vencido, 107/109/10B familia rescatada |

Zonas: 00 Hall of Stairs, 01 Addams Residence, 02 The Old Tree, 03 The Conservatory, 04 The Chain Room, 05 The Kitchen, 06 Portrait Gallery, 07 The Games Room, 08 The Underground, 09 The Furnace, 0A The Armoury, 0B Spooky Drop, 0C The Caves, 0D The Library, 0E Addams' Vault.

`tools/` tiene las herramientas en Python que se usaron para investigar (un frontend libretro sin ventana, el extractor del arte y una vista previa de los menús).

## Estado

Probado con pruebas automáticas y jugando. Falta comprobar con jefes reales la detección del boss rush y el desbloqueo al terminar el juego, y confirmar qué familiar es cada marca de progreso. Pendiente: versión Android con controles táctiles, versión Linux empaquetada, pantalla ancha.

## Créditos

- Direcciones de posición y velocidad de Gomez: [TASVideos](https://tasvideos.org/GameResources/SNES/AddamsFamily).
- Direcciones de vida, vidas, objetos y zonas: códigos Pro Action Replay publicados en [Almar's Guides](https://almarsguides.com/retro/walkthroughs/snes/games/addamsfamily,the); verificadas en la ROM.
- [snes9x](https://github.com/libretro/snes9x) (núcleo libretro), [SDL2](https://www.libsdl.org/), `libretro.h` del equipo de RetroArch.

The Addams Family es una marca de sus respectivos dueños. Este proyecto no está afiliado a ellos ni a Ocean.

---

## English

**Mansion** runs your own SNES **The Addams Family (USA)** ROM through a libretro core and adds: its own start menu, pause menu at any time (F1/Esc), Spanish/English, cheats (easy mode where a hit costs a quarter heart, god mode, infinite health/lives, flying hat, sword, golf balls, no slowdown), level select that keeps your progress, passwords typed in for you, an automap, progress screen, on-screen timer, a boss rush unlocked by finishing the game with its own rules, save states and remappable controls. The menus use the game's own font, frame and cursor, taken from your ROM at start-up, so this repository contains nothing from the game. Put `Addams Family, The (USA).sfc` (CRC32 `2E8034AB`) next to `Mansion.exe`, `SDL2.dll` and `snes9x_libretro.dll`. Build with `make` (Linux) or `make windows` (MinGW). MIT license.
