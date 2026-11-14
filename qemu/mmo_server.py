"""mmo_server.py -- a shared overworld server for the tests (doc/MMO.md,
"Netcode"): boards connect, each is given a player id, and each hears where
the other players on its map are.

Interest is the map: a board hears MN_PLAYER for every player on its own
player's map when they move, all of them when it arrives on a map, and
MN_GONE when one leaves it or stops playing. (Chunks, a screen across, are
the plan for maps with many players; one map is enough to test the path.)

The server is the one in the tests, not a deployment: one thread, polling.
It talks to each board over a byte stream: a QEMU UART's TCP socket here
(port/esp32s3/mmo_uart_esp32s3.c's tunnel).
"""
import select
import socket
import time

import mmo_net as N


class Board:
    def __init__(self, sock, pid):
        self.sock, self.pid = sock, pid
        self.reader = N.Reader()
        self.state = None
        self.hello = False
        self.welcomed = False

    def send(self, data):
        try:
            self.sock.sendall(data)
        except OSError:
            pass


class Server:
    def __init__(self, maxp=16, log=None):
        self.boards = []
        self.maxp = maxp
        self.log = log or (lambda *a: None)
        self.received = {N.HELLO: 0, N.STATE: 0}
        self.sent = {N.WELCOME: 0, N.PLAYER: 0, N.GONE: 0}

    def connect(self, host, port, timeout=30):
        """A board, by its tunnel's TCP port; it is given the next player id."""
        end = time.time() + timeout
        while True:
            try:
                s = socket.create_connection((host, port), timeout=2)
                break
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.2)
        s.setblocking(False)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        b = Board(s, len(self.boards))
        self.boards.append(b)
        return b

    def _tell(self, b, mtype, **kw):
        self.sent[mtype] += 1
        b.send(N.encode(mtype, **kw))

    def _on_state(self, b, st):
        before = b.state
        b.state = st
        for o in self.boards:
            if o is b or o.state is None:
                continue
            here_now = o.state.map() == st.map()
            here_before = before is not None and o.state.map() == before.map()
            if here_now:
                self._tell(o, N.PLAYER, pid=b.pid, state=st)
            elif here_before:
                self._tell(o, N.GONE, pid=b.pid)
        if before is None or before.map() != st.map():
            # On a new map: everyone already there.
            for o in self.boards:
                if o is not b and o.state is not None and o.state.map() == st.map():
                    self._tell(b, N.PLAYER, pid=o.pid, state=o.state)

    def poll(self, timeout=0.0):
        socks = [b.sock for b in self.boards]
        if not socks:
            return
        ready, _, _ = select.select(socks, [], [], timeout)
        for b in self.boards:
            if b.sock not in ready:
                continue
            try:
                data = b.sock.recv(4096)
            except BlockingIOError:
                continue
            if not data:
                continue
            for mtype, f in b.reader.feed(data):
                self.received[mtype] = self.received.get(mtype, 0) + 1
                if mtype == N.HELLO:
                    b.hello = True
                    self._tell(b, N.WELCOME, pid=b.pid, maxp=self.maxp)
                    b.welcomed = True
                    self.log('board %d: hello, welcomed as player %d' % (b.pid, b.pid))
                elif mtype == N.STATE:
                    self._on_state(b, f['state'])

    def bad_frames(self):
        return sum(b.reader.bad for b in self.boards)
