#  MC Ping

Query minecraft server via [SLP (Server Listing Ping)](https://minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Server_List_Ping) to retrieve information (version, motd, player count, max players, mods(on moded), server icon...)

## Usage

```
mcping <host> <port>
```

Prints json data sent by server to stdout.

Host can be IP address or hostname(A record).


### Example

Support for SRV records has not been added yet, so use dns queries when using them.
```
dig _minecraft._tcp.forgeban.xyz SRV

; <<>> DiG 9.10.6 <<>> _minecraft._tcp.forgeban.xyz SRV
;; global options: +cmd
;; Got answer:
;; ->>HEADER<<- opcode: QUERY, status: NOERROR, id: 58413
;; flags: qr rd ra ad; QUERY: 1, ANSWER: 1, AUTHORITY: 0, ADDITIONAL: 1

;; OPT PSEUDOSECTION:
; EDNS: version: 0, flags:; udp: 512
;; QUESTION SECTION:
;_minecraft._tcp.forgeban.xyz.	IN	SRV

;; ANSWER SECTION:
_minecraft._tcp.forgeban.xyz. 60 IN	SRV	0 0 25565 tor.forgeban.xyz.

;; Query time: 100 msec
;; SERVER: 9.9.9.11#53(9.9.9.11)
;; WHEN: Mon Mar 24 KST 2025
;; MSG SIZE  rcvd: 93
```
In this case ``` mcping tor.forgeban.xyz 25565 ```

```
mcping tor.forgeban.xyz 25565
{
  "version": {
    "protocol": 769,
    "name": "Paper 1.21.4"
  },
  "players": {
    "online": 0,
    "max": 40
  },
  "description": {
    "text": "A Minecraft Server"
  },
  "favicon": "data:image/png;base64,iV...
  }%
```



## Installation

Download binary or build from source and run.

### Downloads

## Build from source

### Linux/Mac

```
gcc mcping.c -o mcping
```

### Windows

You can use [Visual C++ Build Tools](http://landinghub.visualstudio.com/visual-cpp-build-tools)

```
cl mcping.c
```

## Features to be added in the future

Resolve SRV records and... ㅇㅅㅇ

## License

AGPL License

Copyright (c) 2025 waterghost-2046