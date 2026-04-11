# CC3064 — Proyecto 01: Chat Multithread
**Grupo:** José Mérida · Javier España · Angel Esquit

---

## Estructura del proyecto (ordenada)

```text
PRY1-SO/
├── Makefile
├── README.md
├── src/
│   ├── common/
│   │   └── chat_protocol.h
│   ├── server/
│   │   ├── main.c
│   │   ├── server_app.c
│   │   └── server_app.h
│   └── client/
│       ├── main.c
│       ├── client_app.c
│       └── client_app.h
├── bin/
│   ├── chat_server
│   └── chat_client
└── scripts/
```

`src/` contiene código fuente modular, `bin/` los ejecutables compilados y `scripts/` queda para automatizaciones de demo/pruebas.

---

## Compilación

```bash
# Dependencias (Ubuntu/Debian)
sudo apt update
sudo apt install -y build-essential libncurses-dev

# Compilar servidor + cliente
make
```

Ejecutables generados:
- `bin/chat_server`
- `bin/chat_client`

---

## Ejecución rápida

```bash
# Terminal 1 (servidor)
./bin/chat_server 8080

# Terminal 2 (cliente)
./bin/chat_client alice 127.0.0.1 8080

# Terminal 3 (cliente)
./bin/chat_client bob 127.0.0.1 8080
```

---

## Comandos del cliente (ncurses)

- `<texto>`: mensaje global (broadcast)
- `/dm <usuario> <mensaje>`: mensaje directo
- `/list`: listar usuarios y estado
- `/info <usuario>`: consultar IP y estado
- `/status ACTIVO|OCUPADO|INACTIVO`: cambiar estado
- `/help`: ayuda
- `/exit`: salir del chat

---

## Protocolo (resumen)

TCP, mensajes de texto delimitados por `\n`, campos separados por `|`.

Cliente -> Servidor:
```text
REGISTER|usuario
MSG_BROADCAST|remitente|mensaje
MSG_DIRECT|remitente|destino|mensaje
LIST_USERS|remitente
GET_USER_INFO|remitente|usuario
CHANGE_STATUS|remitente|ACTIVO|OCUPADO|INACTIVO
DISCONNECT|remitente
```

Servidor -> Cliente:
```text
OK|mensaje
ERROR|codigo|mensaje
SERVER_BROADCAST|remitente|mensaje
SERVER_DIRECT|remitente|mensaje
USER_LIST|n|usuario:estado|...
USER_INFO|usuario|ip|estado
STATUS_UPDATE|usuario|estado
FORCED_STATUS|usuario|INACTIVO
USER_JOINED|usuario|ip
USER_LEFT|usuario
```

---

## Concurrencia y sincronización

- Servidor:
  - hilo principal `accept()`
  - un hilo por cliente (`client_thread`)
  - hilo de inactividad (`inactivity_thread`)
- Cliente:
  - hilo principal de UI/input
  - hilo receptor de mensajes (`recv_thread`)

Protecciones:
- `pthread_rwlock_t list_lock` para la tabla global de clientes.
- `pthread_mutex_t sock_mutex` por cliente para evitar intercalado de `send()`.
- `log_mutex` y `screen_mutex` en cliente para proteger log y ncurses.

---

## Race conditions documentadas

1. Registro concurrente del mismo usuario:
- Solución: `wrlock` alrededor de validación e inserción en `handle_register`.

2. Broadcast concurrente con desconexión:
- Solución: `rdlock` durante iteración + `wrlock` para remover cliente.
- Adicional: `signal(SIGPIPE, SIG_IGN)` para evitar caída del servidor en `send()` inválido.

---

## Propuesta simple para prueba por Ethernet (Linux a Linux)

Escenario recomendado: 3 computadoras conectadas por switch/cable en la misma red local.

### 1) Configurar IP estática temporal (una vez por equipo)

Equipo servidor:
```bash
ip -br link
sudo ip addr flush dev enp3s0
sudo ip addr add 192.168.50.10/24 dev enp3s0
sudo ip link set enp3s0 up
```

Cliente 1:
```bash
sudo ip addr flush dev enp3s0
sudo ip addr add 192.168.50.11/24 dev enp3s0
sudo ip link set enp3s0 up
```

Cliente 2:
```bash
sudo ip addr flush dev enp3s0
sudo ip addr add 192.168.50.12/24 dev enp3s0
sudo ip link set enp3s0 up
```

### 2) Verificar conectividad básica

```bash
ping -c 3 192.168.50.10
```

### 3) Ejecutar chat en red real

Servidor:
```bash
./bin/chat_server 8080
```

Clientes:
```bash
./bin/chat_client alice 192.168.50.10 8080
./bin/chat_client bob   192.168.50.10 8080
```

### 4) Checklist de demo rápida

- Registro exitoso de ambos clientes.
- Broadcast de `alice` visible en `bob`.
- `/dm bob hola` desde `alice`.
- `/list` y `/info alice`.
- Cambio de `/status OCUPADO`.
- Timeout de inactividad (si aplica para la demo).

---

## Parámetro de inactividad

En `src/server/server_app.c`:

```c
#define INACTIVITY_SEC 60
```

Para demo corta, pueden cambiarlo a `30` y recompilar con `make`.

---

## Estado final del proyecto

- Implementación servidor y cliente completa.
- Estructura de carpetas ordenada.
- Build configurado desde `src/` hacia `bin/`.
- Warning real de funciones no usadas eliminado en servidor.
- Documentación técnica actualizada para entrega y demo por Ethernet.
