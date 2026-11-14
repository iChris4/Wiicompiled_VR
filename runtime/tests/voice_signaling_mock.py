#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for MKW-VoiceChat's signaling Worker, for testing voice chat on one machine.

It speaks the Retro Rewind room subset the client uses, the way signaling_worker/src/index.js
does: RR_DEV_ADMIT puts every client in one room (there is no RWFC roster here), each member is
introduced to every other one with RR_DEV_PEER_INFO and PEER_READY, SIGNAL is relayed with the
sender's member id, and a closed connection becomes PEER_LEFT. ICE_SERVERS is sent empty, so the
peers connect over host candidates only and the test needs no internet.

Standard library only (asyncio and a minimal RFC 6455 server, ping/pong included: the client
closes a WebSocket whose pings go unanswered).

    python -I voice_signaling_mock.py [port]          # default 8787, like `wrangler dev`

Pair it with mkw_voice_session_probe (two processes, two profile ids) to see a whole session.
"""

import asyncio
import base64
import hashlib
import struct
import sys
import uuid

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ROOM_ID = "mock-room"
CREATED = "2026-01-01T00:00:00Z"


def sha256_hex(text):
    return hashlib.sha256(text.encode()).hexdigest().upper()


ROOM_INSTANCE = sha256_hex(ROOM_ID + "\n" + CREATED)


class Client:
    def __init__(self, writer):
        self.writer = writer
        self.session = uuid.uuid4().hex
        self.member = None
        self.pid = None

    async def send(self, text):
        payload = text.encode()
        header = bytes([0x81])
        if len(payload) < 126:
            header += bytes([len(payload)])
        elif len(payload) < 65536:
            header += bytes([126]) + struct.pack(">H", len(payload))
        else:
            header += bytes([127]) + struct.pack(">Q", len(payload))
        self.writer.write(header + payload)
        await self.writer.drain()

    def peer_info(self):
        name = ("Player " + self.pid[-3:]).encode().hex().upper()
        return "\n".join(["RR_DEV_PEER_INFO", self.member, self.pid, ROOM_INSTANCE, "XX", name, "", "0"])


clients = []


async def read_frame(reader):
    b1, b2 = await reader.readexactly(2)
    opcode, length = b1 & 0x0F, b2 & 0x7F
    if length == 126:
        length = struct.unpack(">H", await reader.readexactly(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", await reader.readexactly(8))[0]
    mask = await reader.readexactly(4) if b2 & 0x80 else b"\0\0\0\0"
    data = bytearray(await reader.readexactly(length))
    for i in range(length):
        data[i] ^= mask[i % 4]
    return bool(b1 & 0x80), opcode, bytes(data)


async def handle_text(client, message):
    if message.startswith("RR_DEV_ADMIT "):
        parts = message.split()
        client.pid = parts[1]
        client.member = sha256_hex("mkwvc-rr-dev-member\n" + client.pid + "\n" + client.session + "\n" + ROOM_INSTANCE)[:32]
        admitted = [c for c in clients if c.member]
        rows = []
        for c in admitted:
            name = ("Player " + c.pid[-3:]).encode().hex().upper()
            rows.append("\t".join([c.pid, "1", name, "", "0"]))
        await client.send("ICE_SERVERS\n")
        await client.send("RR_DEV_ADMITTED " + "\n".join([client.member, ROOM_INSTANCE, ROOM_ID, CREATED, str(len(rows))] + rows))
        print(f"admitted {client.pid} as {client.member[:8]}", flush=True)
        for other in admitted:
            if other is client:
                continue
            await client.send(other.peer_info())
            await other.send(client.peer_info())
            await client.send("PEER_READY " + other.member)
            await other.send("PEER_READY " + client.member)
    elif message.startswith("SIGNAL "):
        head, _, bundle = message.partition("\n")
        target = head[7:].strip().upper()
        for other in clients:
            if other.member == target:
                await other.send("SIGNAL " + client.member + "\n" + bundle)
                print(f"relayed a {len(bundle)}-byte bundle {client.member[:8]} -> {target[:8]}", flush=True)
    elif message == "ALIVE":
        pass
    else:
        print("ignored:", message.split("\n")[0][:60], flush=True)


async def serve(reader, writer):
    request = await reader.readuntil(b"\r\n\r\n")
    headers = {}
    for line in request.decode(errors="replace").split("\r\n")[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    accept = base64.b64encode(hashlib.sha1((headers.get("sec-websocket-key", "") + GUID).encode()).digest()).decode()
    writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
    await writer.drain()
    client = Client(writer)
    clients.append(client)
    fragments = b""
    try:
        while True:
            fin, opcode, data = await read_frame(reader)
            if opcode == 0x8:
                break
            if opcode == 0x9:
                writer.write(bytes([0x8A, len(data)]) + data)
                await writer.drain()
                continue
            if opcode in (0x1, 0x0):
                fragments += data
                if fin:
                    await handle_text(client, fragments.decode(errors="replace"))
                    fragments = b""
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        clients.remove(client)
        if client.member:
            print(f"{client.pid} left", flush=True)
            for other in clients:
                try:
                    await other.send("PEER_LEFT " + client.member)
                except ConnectionError:
                    pass
        writer.close()


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8787
    server = await asyncio.start_server(serve, "127.0.0.1", port)
    print(f"mock signaling on ws://127.0.0.1:{port}/", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
