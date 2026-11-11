# Digi 003 en Apple Silicon

Driver de usuario (sin kext, con SIP activo) para el **Digidesign 003 Rack** por FireWire en macOS 15 (Sequoia) con Apple Silicon. Sirve como interfaz de audio y MIDI y también como expansor de previos para una **Apollo x8p** vía ADAT.

> ⚠️ Funciona solo hasta macOS 15. macOS 26 (Tahoe) elimina FireWire.
> La alternativa sería portar el protocolo a [ASFireWire](https://www.asfw.net/) (DriverKit).

## Qué hace

- **Interfaz Core Audio:** "Digi 003 Rack", 18 entradas y 18 salidas a 44.1 o 48 kHz.
  Orden de canales: Analog 1-8, S/PDIF L/R, ADAT 1-8.
- **Reloj:** Interno, S/PDIF, ADAT o Word Clock. En reloj externo sigue sola la frecuencia que le llega.
- **Modo previos:** las analógicas 1-8 (o 1-4) salen por la salida ADAT del 003 aunque ninguna app lo use. La latencia es de ~7.5 ms (segura) o ~5.6 ms (baja).
- **MIDI IN/OUT:** puertos Core MIDI "Digi 003 MIDI In/Out", creados por la app de barra de menú.
- **Reposo:** detiene el stream al dormir y lo reanuda al despertar. Un vigilante reinicia el stream si se detiene, si llega con errores continuos o si cambia la frecuencia externa.
- **App de barra de menú "Digi 003":** estado, medidores, ruteo ADAT, puerto óptico, latencia, reloj, frecuencia, reinicio, avisos y la opción de evitar el reposo.

## Compensación de los previos en Logic

Los previos del 003 que llegan a la Apollo por ADAT van atrasados respecto a las entradas propias de la Apollo.
Medido con correlación cruzada (`tools/latxc`, con la misma señal entrando por los dos caminos) y estable:

| Frecuencia | Latencia Segura | Latencia Baja |
|---|---|---|
| 44.1 kHz | 363 muestras (8.23 ms) | 274 muestras (6.21 ms) |
| 48 kHz | 384 muestras (8.00 ms) | 288 muestras (6.00 ms) |

En Logic: Inspector de región → Retardo = −8.2 / −8.0 ms (Segura) o −6.2 / −6.0 ms (Baja) en las pistas de ADAT 1-8.

## Estructura

| Carpeta | Contenido |
|---|---|
| `engine/` | Motor de streaming isócrono (IOFireWireLib + NuDCL): CIP/AM824, codificado "double-oh-three", copia de la cadencia del 003, modelo de reloj, MIDI |
| `driver/` | AudioServerPlugIn (`Digi003.driver`) que usa el motor |
| `menubar/` | App de barra de menú en Swift; incluye el puente Core MIDI |
| `tools/` | Herramientas de prueba y diagnóstico (ver abajo) |
| `reference/linux-digi00x/` | Driver de Linux `snd-firewire-digi00x` (GPL-2.0), fuente del protocolo |

## Compilar e instalar

```bash
./build-driver.sh            # -> build/Digi003.driver
./install-driver.command     # pide contraseña; detiene el stream, reinicia coreaudiod, relanza UA Mixer Engine
./build-menubar.sh           # -> ~/Applications/Digi 003.app
```

Los ajustes del driver se guardan en el almacenamiento del HAL. CFPreferences no funciona dentro del sandbox.

## Herramientas (`tools/*.c`)

| Herramienta | Uso |
|---|---|
| `d003probe` | Lee los registros del 003 (solo si ningún driver lo tiene abierto) |
| `d003test` | Prueba del motor sin Core Audio: tono, grabación y estadísticas. `D003_CLOCK=2` = ADAT |
| `halharness` | Carga el driver como lo haría coreaudiod, con un host simulado |
| `d3ctl` | Lee y escribe las propiedades propias del driver: `status`, `levels`, `route 0/4/8`, `lowlat`, `optical`, `restart`, `midiout` |
| `caprop` | Frecuencia y reloj de cualquier dispositivo; `adat on/off` |
| `chpeak` | Picos por canal de un dispositivo (para verificar la ruta ADAT en la Apollo) |
| `clockcmp` | Compara la frecuencia real de dos dispositivos (sincronía por ADAT) |
| `mkaggregate` | Crea el dispositivo agregado "Apollo + 003" |
| `midimon` | Monitor de "Digi 003 MIDI In" y arpegio de prueba por Out |

Compilar una herramienta: `clang -O2 -o tools/d3ctl tools/d3ctl.c -framework CoreAudio -framework CoreFoundation`.
Las que usan el motor también necesitan `engine/d003engine.c -framework IOKit`.

## Problemas conocidos

- Reiniciar coreaudiod detiene **UA Mixer Engine**, y entonces la Apollo entrega silencio en todas sus entradas.
  `launchctl kickstart gui/$UID/com.uaudio.ua_mixer_engine` (el instalador ya lo hace).
- Matar coreaudiod con el stream activo dejó una vez colgado el firmware del 003: seguía en el bus pero no respondía. Se arregla apagándolo y prendiéndolo.
- El conector FW400 del 003 se afloja fácil. Mover cables atrás del 003 puede provocar ráfagas de errores de paquete.
- El adaptador Thunderbolt→FireWire tiene que ir en un puerto **trasero** del Mac Studio (los frontales son USB).

## Créditos

Protocolo: Takashi Sakamoto (driver `snd-firewire-digi00x` de Linux), Robin Gareus y Damien Zammit (algoritmo "double-oh-three").
