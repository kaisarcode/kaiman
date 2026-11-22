# libnetl.c - Persistent bidirectional networking

`libnetl.c` creates, operates, and closes persistent TCP and UDP sockets.
Inbound listeners and outbound connections use the same `peer, data` receive
callback and `kc_netl_send(peer, bytes, size)` operation.

It is transport infrastructure, not an application protocol. STUN, ICE,
rendezvous and P2P remain independent compositions.

## Native C API

```c
#include "libnetl.h"

static void receive(kc_netl_peer_t *peer,const void *data,size_t size,
    void *userdata)
{
    static const char answer[]="Received OK";
    (void)data;
    (void)size;
    (void)userdata;
    (void)kc_netl_send(peer,answer,sizeof(answer)-1U);
}

int main(void)
{
    kc_netl_options_t options={0};
    kc_netl_t *server=NULL;
    options.host="0.0.0.0";
    options.port=5000;
    options.protocol=KC_NETL_UDP;
    if(kc_netl_open(&server,&options,receive,NULL,NULL,NULL)!=KC_NETL_OK)
        return 1;

    /* Keep the instance active while the application does other work. */
    kc_netl_close(server);
    return 0;
}
```

An interface is prepared once with `kc_netl_open()` and then reused for sends
and receives. For TCP, the interface first tries to listen on the configured
address. When that address is not locally bindable, it keeps the address as the
remote target and the first `kc_netl_send()` with a NULL destination creates
the persistent connection. Later sends reuse the same peer. UDP reuses its bound
socket from `kc_netl_open()` and may select a per-datagram destination.
Disconnect and error callbacks are optional.

TCP peers identify live connections. An incoming UDP peer is valid only within
the receive callback; its `send()` sends back through the same bound socket.
Received bytes are independent from bytes chosen for the reply.

## CLI

```sh
netl servidor 0.0.0.0:5000 handler -t udp
netl cliente 192.168.1.20:6000 -t udp
echo "one" | netl cliente
echo "two" | netl cliente
netl -l
netl cliente -d
netl servidor -d
```

Creating the named endpoint starts a resident process holding the socket.
Subsequent calls with only its ID transfer binary stdin via IPC without
creating a new socket.

## Build and tests

```sh
make
make test
```

Use `make all` for other platforms with installed toolchains.

## Documentation

- [Interface behavior](doc/interface-behavior.md) - endpoint selection, persistent sockets, and UDP/TCP behavior.
## License

[![LGPLv3](https://www.gnu.org/graphics/lgplv3-147x51.png)](https://www.gnu.org/licenses/lgpl-3.0.html)

This project is distributed under the **GNU Lesser General Public License version 3 (LGPLv3)**.
