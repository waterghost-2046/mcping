# 🧭 MCPing - Minecraft Server List Query Tool

`MCPing` is a lightweight CLI tool that queries Minecraft servers using the [Server List Ping (SLP)](https://minecraft.wiki/w/Minecraft_Wiki:Projects/wiki.vg_merge/Server_List_Ping) protocol to retrieve server details such as:

- Server version
- MOTD (Message of the Day)
- Online player count
- Maximum player slots
- Mod information (if modded)
- Server icon
- More...

---

## 📦 Usage

```
mcping <host> [port] [--socks ip:port] [--ha-protocol]
```

- `host`: IP address or hostname (must resolve to A record)
- `port`: Minecraft server port (default: 25565)
- `--socks`: (optional) Use a SOCKS5 proxy, e.g. `--socks 127.0.0.1:1080`
- `--ha-protocol`: (optional) Prepend an [HAProxy PROXY protocol v1](https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt) header for servers behind HAProxy

📝 The tool prints raw JSON output returned by the server to `stdout`.

---

## 💡 Example

Query a Minecraft server directly:

```
mcping tor.forgeban.xyz 25565
```

Use with a SOCKS5 proxy:

```
mcping tor.forgeban.xyz 25565 --socks 127.0.0.1:1080
```

Use with HAProxy protocol:

```
mcping tor.forgeban.xyz 25565 --ha-protocol
```

Combined usage:

```
mcping tor.forgeban.xyz 25565 --socks 127.0.0.1:1080 --ha-protocol
```

Example output:

```json
{
  "version": {
    "protocol": 769,
    "name": "Paper 1.21.4"
  },
  "players": {
    "online": 1,
    "max": 40,
    "sample": [{"name": "Name", "id": "00000000-de71-4081-bd78-70db460e8d59"}]
  },
  "description": {
    "text": "A Minecraft Server"
  },
  "favicon": "data:image/png;base64,iV..."
}
```

---

## 🧠 SRV Record Support

SRV record resolution is not yet implemented, but you can look up SRV entries manually using `dig`:

```
dig _minecraft._tcp.forgeban.xyz SRV
```

Example output:

```
_minecraft._tcp.forgeban.xyz. 60 IN SRV 0 0 25565 tor.forgeban.xyz.
```

Then query directly using the resolved host and port:

```
mcping tor.forgeban.xyz 25565
```

---

## 🌐 MCPing API (Proof of Concept)

A web API backend is also available that wraps the MCPing query via HTTP.

### Endpoint

- **URL:** `https://forgeban.xyz/api/slp`
- **Method:** `POST`

### Request body

```json
{
  "address": "forgeban.xyz",
  "port": "25565" // optional if port is 25565 or using SRV
}
```

### Response

```json
{
  "version": {
    "name": "Paper 1.21.4",
    "protocol": 769
  },
  "favicon": "data:image/png;base64,...",
  "description": {"text": "A Minecraft Server"},
  "players": {
    "online": 1,
    "max": 40,
    "sample": [{"name": "Name", "id": "00000000-de71-4081-bd78-70db460e8d59"}]
  },
  "ip": "152.70.89.255",
  "port": "25565",
  "hostname": "tor.forgeban.xyz"
}
```

---

## ⚙️ Installation

### Download binary

Download a precompiled binary (coming soon).

### Build from source

#### Linux / macOS

```
gcc main.c -o mcping
```

#### Windows (Visual Studio Build Tools)

```
cl main.c
```

---

## 🚧 TODO / Planned Features

- Automatic SRV record resolution
- SOCKS proxy authentication support
- SOCKS4 proxy support
- HAProxy protocol v2 support

---

## 📄 License

**AGPL License**

```
Copyright (c) 2025 waterghost-2046
```
