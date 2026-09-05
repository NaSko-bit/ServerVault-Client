# ServerVault-Client
The client program for ServerVault. ANDROID GUI builded on top of C/Bash back-end. The client program, must check if the Server is online. If not will store the data in buffer memory, the moment the Server is available it will transfer the data through TCP/IP Connection.

## Build and run

```bash
gcc -Wall -Wextra -std=c11 main.c -o client
./client [SERVER_IP]
```

The default server address is `127.0.0.1`. For a server on a private or
Tailscale network, pass its address at runtime, for example:

```bash
./client <SERVER_IP>
```

The server address is intentionally not stored in the source code.
