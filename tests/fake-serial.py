#!/usr/bin/env python3
# A board on a serial port, for the tests (tests/serial.sh): a pseudo-
# terminal whose far end is the "device", linked at DEV/NAME as a USB
# serial port's node would be (BLIT_USB_DEV points blit's stand-in sysfs at
# DEV). What blit writes to the port is added to HEARD as it comes. It is
# told what to do through the fifo CTL, a line at a time:
#
#   say TEXT        TEXT written to the port (\r \n \e \xHH and \\ as those bytes)
#   ticks N         "tick 1\r\n" ... "tick N\r\n"
#   file PATH       the bytes of the file at PATH written to the port (a crash dump)
#   flood N         N coloured log lines as fast as the port takes them
#                   ("\e[0;32mI (N) main: line N of the flood, as a board logs it\e[0m\r\n",
#                   about 70 bytes, as ESP-IDF logs)
#   hangup          the port goes, as a board's USB serial does on a reset:
#                   the far end closed, the link and the interface's tty in
#                   the stand-in sysfs (IFACE/tty/NAME) taken away
#   back NAME       a new port, at DEV/NAME, its tty in IFACE again
#   tryopen FILE    the port opened as a flash tool would (pyserial's
#                   exclusive open, esptool's: the lock taken), "opened" or
#                   "busy" written to FILE
#   echo            from now on, what blit writes is written back too (a
#                   board's console echoing keys: a keystroke's round trip)
#   quit
#
#   tests/fake-serial.py DEV NAME HEARD CTL [IFACE]
import fcntl
import os
import pty
import select
import sys
import tty

dev, name, heard, ctl = sys.argv[1:5]
iface = sys.argv[5] if len(sys.argv) > 5 else ""


def bytes_of(text):
    return text.replace("\\e", "\\x1b").encode("latin-1").decode("unicode_escape").encode("latin-1")


class Port:
    def __init__(self, name):
        self.name = name
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)  # (nothing echoed back before blit sets it as it wants)
        os.set_blocking(self.master, False)
        self.link = os.path.join(dev, name)
        if os.path.lexists(self.link):
            os.unlink(self.link)
        if iface:
            os.makedirs(os.path.join(iface, "tty", name), exist_ok=True)
        os.symlink(os.ttyname(self.slave), self.link)

    def write(self, data):
        while data:
            try:
                k = os.write(self.master, data)
                data = data[k:]
            except BlockingIOError:
                select.select([], [self.master], [], 1.0)
                self.read()

    def read(self):
        try:
            data = os.read(self.master, 65536)
        except (BlockingIOError, OSError):
            return
        if data:
            with open(heard, "ab") as f:
                f.write(data)
            if echo:
                self.write(data)

    def close(self):
        os.unlink(self.link)
        if iface:
            os.rmdir(os.path.join(iface, "tty", self.name))
        os.close(self.master)
        os.close(self.slave)


echo = False
port = Port(name)
fifo = os.open(ctl, os.O_RDWR | os.O_NONBLOCK)
pending = b""
while True:
    fds = [fifo] + ([port.master] if port else [])
    ready, _, _ = select.select(fds, [], [], 1.0)
    if port and port.master in ready:
        port.read()
    if fifo not in ready:
        continue
    try:
        pending += os.read(fifo, 65536)
    except BlockingIOError:
        continue
    while b"\n" in pending:
        line, pending = pending.split(b"\n", 1)
        verb, _, arg = line.decode().partition(" ")
        if verb == "say" and port:
            port.write(bytes_of(arg))
        elif verb == "file" and port:
            with open(arg, "rb") as f:
                port.write(f.read())
        elif verb == "ticks" and port:
            port.write(b"".join(b"tick %d\r\n" % i for i in range(1, int(arg) + 1)))
        elif verb == "flood" and port:
            n = int(arg)
            port.write(b"".join(b"\x1b[0;32mI (%d) main: line %d of the flood, as a board logs it\x1b[0m\r\n" % (i, i) for i in range(1, n + 1)))
        elif verb == "hangup" and port:
            port.close()
            port = None
        elif verb == "back":
            port = Port(arg)
        elif verb == "tryopen":
            try:
                fd = os.open(os.path.join(dev, port.name if port else name), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    said = "opened"
                finally:
                    os.close(fd)
            except OSError:
                said = "busy"
            with open(arg, "w") as f:
                f.write(said + "\n")
        elif verb == "echo":
            echo = True
        elif verb == "quit":
            sys.exit(0)
