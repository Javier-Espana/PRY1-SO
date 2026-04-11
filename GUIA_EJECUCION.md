# Guia de ejecucion

Esta guia explica como compilar y ejecutar el chat (servidor y cliente) en Linux, tanto en localhost como en red local Ethernet.

## 1. Requisitos

- Sistema Linux (Ubuntu/Debian recomendado)
- gcc, make, pthreads
- ncurses para el cliente

Instalacion de dependencias:

```bash
sudo apt update
sudo apt install -y build-essential libncurses-dev
```

## 2. Compilar el proyecto

Desde la raiz del proyecto:

```bash
make clean
make
```

Binarios esperados:

- bin/chat_server
- bin/chat_client

## 3. Ejecucion local rapida (misma computadora)

Abrir 3 terminales en la carpeta del proyecto.

Terminal 1 (servidor):

```bash
./bin/chat_server 9090

Terminal 2 (cliente 1):

```bash
./bin/chat_client alice 127.0.0.1 9090
```

Terminal 3 (cliente 2):

```bash
./bin/chat_client bob 127.0.0.1 9090
```

Nota: usa un puerto libre (9090 suele ser buena opcion). Puedes verificar antes con:

```bash
ss -ltn | grep ':9090' || echo '9090 libre'
```

## 4. Comandos utiles del cliente

- Escribir texto sin prefijo: envia mensaje global
- /dm <usuario> <mensaje>: mensaje directo
- /list: lista usuarios conectados
- /info <usuario>: muestra IP y estado
- /status ACTIVO|OCUPADO|INACTIVO: cambia estado
- /help: muestra ayuda
- /exit: salir del cliente

## 5. Prueba por red Ethernet (equipos distintos)

### 5.1 Elegir IP del servidor

Ejemplo: servidor 192.168.50.10 y puerto 8080.

### 5.2 Iniciar servidor en equipo servidor

```bash
./bin/chat_server 8080
```

### 5.3 Iniciar clientes en otros equipos

```bash
./bin/chat_client alice 192.168.50.10 8080
./bin/chat_client bob   192.168.50.10 8080
```

### 5.4 Verificar conectividad

Desde cada cliente:

```bash
ping -c 3 192.168.50.10
```

## 6. Checklist de verificacion

- Ambos clientes se conectan sin error.
- Un mensaje global de alice aparece en bob.
- /dm bob hola funciona.
- /list muestra usuarios y estados.
- /info alice muestra IP y estado.
- /status OCUPADO cambia estado correctamente.

## 7. Problemas comunes y solucion

### Error: "ncurses.h: No existe el archivo o el directorio"

Instalar dependencia y recompilar:

```bash
sudo apt install -y libncurses-dev
make clean && make
```

### Error: "bind: Address already in use"

El puerto ya esta ocupado. Usar otro:

```bash
./bin/chat_server 9090
./bin/chat_client alice 127.0.0.1 9090
```

### El cliente no conecta desde otra maquina

- Verificar IP del servidor.
- Verificar que ambos equipos esten en la misma red.
- Revisar firewall del servidor (permitir puerto TCP usado).

## 8. Cerrar la aplicacion

- En cada cliente: /exit
- En el servidor: Ctrl + C
