# CC3064 — Proyecto 01: Chat Multithread
**Grupo:** José Mérida · Javier España · Angel Esquit

---

## Estructura del proyecto

```text
PRY1-SO/
├── Makefile
├── README.md
├── annotated-Proyecto01-Protocolo.pdf
├── src/
│   ├── common/
│   │   ├── chat_protocol.h     # constantes del protocolo (tipos, estados, errores)
│   │   ├── protocol.h
│   │   └── protocol.c          # proto_tokenize, proto_sanitize
│   ├── client/
│   │   ├── main.c              # argparse -> session
│   │   ├── net.h / net.c       # conectar, enviar linea
│   │   ├── ui.h / ui.c         # ncurses: ventanas, log, input loop
│   │   └── session.h / session.c   # estado del cliente + logica del protocolo
│   └── server/
│       ├── main.c              # accept loop + spawn de hilos
│       ├── net.h / net.c       # listen/bind
│       └── session.h / session.c   # tabla de clientes + handlers + inactividad
├── bin/
│   ├── chat_server
│   └── chat_client
└── scripts/
```

---

## Compilación

```bash
# Dependencias (Ubuntu/Debian)
sudo apt update
sudo apt install -y build-essential libncurses-dev

# Fedora
sudo dnf install gcc make ncurses-devel

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
./bin/chat_server 9090

# Terminal 2 (cliente)
./bin/chat_client alice 127.0.0.1 9090

# Terminal 3 (cliente)
./bin/chat_client bob 127.0.0.1 9090
```

Si el puerto está ocupado:

```bash
ss -ltn | grep ':9090' || echo '9090 libre'
```

Nota: el servidor rechaza IPs duplicadas (error 102). Para probar dos clientes en una misma máquina sobre loopback solo funciona uno a la vez; para varios clientes simultáneos se necesita la configuración por Ethernet (al final del documento).

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

## Arquitectura por capas

La descomposición por módulos sigue una jerarquía de capas:

```
main → session → net, ui, common/protocol
```

Las capas bajas (`net`, `protocol`, `ui`) no dependen de `session`. `session` es el único módulo que conoce el protocolo y el I/O al mismo tiempo, y orquesta todo.

### Responsabilidades

**`common/protocol`** — Formato de mensaje. `proto_tokenize` parte una línea por `|` in-place; `proto_sanitize` reemplaza `|` por `::` en texto de usuario (§3.1 del PDF del protocolo). No toca sockets.

**`client/net`** — Sockets del cliente. `net_connect(ip, port)` → fd. `net_send_line(fd, msg)` apendea `\n` y envía.

**`client/ui`** — Toda la capa ncurses: ventanas (`status`, `chat`, `input`), colores, log circular (500 líneas), mutex de pantalla. `ui_input_loop(on_line, &running)` lee teclas y entrega cada línea al callback. No conoce el protocolo ni los sockets.

**`client/session`** — "Main lógico" del cliente. Mantiene `sock_fd`, `username`, `my_status`, `running`. El hilo `recv_thread` lee del socket, framea por `\n` y llama `process_server_msg`. `process_input` traduce comandos a mensajes del protocolo.

**`server/net`** — `net_listen(port)`: socket + `SO_REUSEADDR` + bind + listen.

**`server/session`** — Dueño de la tabla `clients[MAX_CLIENTS]` y el `list_lock` (rwlock). Contiene todos los handlers (`handle_register`, `handle_broadcast`, etc.), el hilo de inactividad y el hilo por cliente.

**`server/main`** — Argparse, `signal(SIGPIPE, SIG_IGN)`, `net_listen`, spawn de inactividad, loop de `accept()`.

---

## Concurrencia y sincronización

### Servidor
- **Hilo principal:** `accept()` en loop.
- **Un hilo por cliente** (`session_client_thread`): vive hasta `DISCONNECT` o `recv()==0`.
- **Hilo global de inactividad** (`session_inactivity_thread`): revisa cada 10s, marca `INACTIVO` a quienes superen `INACTIVITY_SEC` (60s).

**Locks:**
- `list_lock` (`pthread_rwlock_t`): protege la tabla de clientes. Rwlock porque la mayoría de operaciones solo leen (broadcast, DM lookup, list, info). Write-lock en register, disconnect, change_status, timeout.
- `sock_mutex` (`pthread_mutex_t`) por cliente: cuando un hilo escribe al socket de otro cliente toma su mutex antes de `send()` para no mezclar bytes.

### Cliente
- **Hilo principal:** `ui_input_loop` (bloqueado en `wgetch`).
- **`recv_thread`:** lee del socket, decodifica, actualiza UI.
- **Locks UI:** `log_mutex` (buffer de log), `screen_mutex` (ncurses no es thread-safe).

### Race conditions atendidas

- **Registro concurrente del mismo usuario:** `wrlock` rodea validación e inserción en `handle_register`.
- **Broadcast concurrente con desconexión:** `rdlock` durante la iteración; `wrlock` para remover. Además `signal(SIGPIPE, SIG_IGN)` evita caída del servidor si un socket cerró mientras escribimos.

---

## Protocolo (resumen)

TCP, mensajes de texto delimitados por `\n`, campos separados por `|`. Máximo 4096 bytes.

### Cliente → Servidor
```text
REGISTER|usuario
MSG_BROADCAST|remitente|mensaje
MSG_DIRECT|remitente|destino|mensaje
LIST_USERS|remitente
GET_USER_INFO|remitente|usuario
CHANGE_STATUS|remitente|ACTIVO|OCUPADO|INACTIVO
DISCONNECT|remitente
```

### Servidor → Cliente
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

### Códigos de error

| Código | Contexto | Significado |
|---|---|---|
| 101 | REGISTER | Nombre duplicado. |
| 102 | REGISTER | IP con sesión activa. |
| 103 | REGISTER | Nombre inválido. |
| 201 | DM / GET_USER_INFO | Destino no existe / no conectado. |
| 301 | CHANGE_STATUS | Estado inválido. |
| 401 | General | Formato desconocido/malformado. |
| 501 | BROADCAST / DIRECT | Cliente INACTIVO debe cambiar estado. |
| 999 | REGISTER | Servidor lleno (extensión, no documentado en el PDF). |

---

## Diferencias entre el PDF del protocolo y la implementación

### Se respeta tal cual
- Formato de mensaje (`TIPO|CAMPO1|...|CAMPON\n`), tope 4096 bytes.
- Todos los tipos de mensaje y códigos de error listados arriba.
- Modelo de concurrencia (un hilo por cliente, `pthread_rwlock_t` para la lista, `pthread_mutex_t` por socket).
- Flujos de §4 del PDF (registro, broadcast, DM, list, info, status, inactividad, desconexión).
- Comando: `./chat_server <puerto>`, `./chat_client <usuario> <IP> <puerto>`.

### Extensiones por implementación
- `INACTIVITY_SEC = 60` (el PDF lo menciona solo como variable). En `src/server/session.h`.
- `MAX_CLIENTS = 50` (tabla estática). Si se llena se responde `ERROR|999|Servidor lleno`.
- Comandos UX del cliente (`/dm`, `/list`, `/info`, `/status`, `/help`, `/exit`) — el PDF define mensajes, no comandos.
- UI ncurses con tres ventanas, colores semánticos, log circular de 500 líneas.
- `OK` del servidor no se muestra en el chat (reduce ruido, ya que broadcasts y DMs se reflejan por otro mensaje). Los `ERROR` sí se muestran.

### Limitaciones conocidas
- **Framing del servidor:** `session_client_thread` corta en el primer `\n` y descarta el resto del `recv`. Si un cliente bien portado llegara a pipelinear dos mensajes en un `send`, el segundo se pierde. En la práctica no ocurre porque el cliente manda uno a la vez. El cliente sí usa un buffer de acumulación correcto.
- `INACTIVITY_SEC` hardcoded (podría ser argumento del servidor).
- `MAX_CLIENTS` fijo (lista enlazada o vector dinámico si se quiere escalar).

---

## Prueba por Ethernet (Linux a Linux)

Escenario recomendado: 3 computadoras conectadas por switch/cable en la misma red local.

### 1) Configurar IP estática temporal

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

### 2) Verificar conectividad

```bash
ping -c 3 192.168.50.10
```

### 3) Ejecutar

```bash
./bin/chat_server 9090
./bin/chat_client alice 192.168.50.10 9090
./bin/chat_client bob   192.168.50.10 9090
```

### 4) Checklist de demo

- Registro exitoso de ambos clientes.
- Broadcast de `alice` visible en `bob`.
- `/dm bob hola` desde `alice`.
- `/list` y `/info alice`.
- Cambio de `/status OCUPADO`.
- Timeout de inactividad (si aplica para la demo).

---

## Parámetro de inactividad

En `src/server/session.h`:

```c
#define INACTIVITY_SEC 60
```

Para demo corta, cambiar a `30` y recompilar con `make`.
