# JC4827W543C_ESP32S3_NV3041A_RainMatrix
Matrix Rain para JC4827W543C ESP32-S3 + NV3041A (480×272 QSPI) con soporte de doble buffer en PSRAM y render paralelo en Core1 usando Arduino_GFX.
<img width="1536" height="1024" alt="2ea2f74f-e150-4509-85f3-3b39c89f8246" src="https://github.com/user-attachments/assets/28e0d78c-3118-48d3-85a6-f661ec84b28e" />

El JC4827W543C es un módulo de pantalla TFT LCD de 4.3 pulgadas con resolución 480×272 píxeles

🔹 Características principales

Controlador: NV3041A, que maneja el refresco de la pantalla.

Interfaz: normalmente trabaja con QSPI (4 líneas de datos + reloj + chip select), lo que permite un alto ancho de banda con el ESP32-S3.

Panel IPS/TFT: ofrece buen ángulo de visión y colores vivos.

Retroiluminación: controlada por pin dedicado 

Touch capacitivo GT911 (conexión por I²C).

Tamaño visible: 4.3” (diagonal).

Resolución nativa: 480 (ancho) × 272 (alto).

Este proyecto implementa un efecto Matrix Rain optimizado para el ESP32-S3 con pantalla JC4827W543C (NV3041A, 480×272, interfaz QSPI).
El render se ejecuta en una tarea dedicada en Core1 y utiliza framebuffers en PSRAM (cuando está disponible) para alcanzar un refresco fluido.

🛠️ Requisitos

JC4827W543C

🚀 Uso

Abre el proyecto en Arduino IDE.

Configura la placa:

ESP32S3 Dev Module o ESP32S3 DevKitC-1

USB CDC On Boot: Enabled

PSRAM: Enabled (si tu módulo lo soporta).



Compila y sube.

📷 Resultado esperado

Una lluvia estilo Matrix, con caracteres blancos/verde cayendo en columnas independientes y dejando un rastro degradado.
En dispositivos con PSRAM se habilita doble buffer, evitando tearing y logrando animación más suave.
