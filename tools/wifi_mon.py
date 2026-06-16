#!/usr/bin/env python3
"""Drive the devicesim virtual WiFi controller over its pty, the way an OS would.

Run the app first, then:  python3 tools/wifi_mon.py [channel]

Speaks the wifi.h protocol on /tmp/tty.wifi:
  frame = [plane(1)][op(1)][len(2 LE)][payload]
  CTRL(0): set_mode(0x01)/set_channel(0x0b)   RAW80211(3): rx hdr + 802.11 frame / TX = raw frame
"""
import os, sys, struct, time, select, termios

PATH = "/tmp/tty.wifi"
PLANE_CTRL, PLANE_RAW = 0, 3
CMD_SET_MODE, CMD_SET_CHANNEL = 0x01, 0x0b
MODE_MONITOR = 3

def set_raw(fd):  # termios.cfmakeraw is 3.11+; do it by hand for older pythons
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
    iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
               termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
    oflag &= ~termios.OPOST
    lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    cflag &= ~(termios.CSIZE | termios.PARENB)
    cflag |= termios.CS8
    cc = list(cc); cc[termios.VMIN] = 1; cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])

def frame(plane, op, payload=b""):
    return bytes([plane, op, len(payload) & 0xff, (len(payload) >> 8) & 0xff]) + payload

def probe_req(sa=b"\x20\xe2\x17\x00\x67\x4d"):
    bcast = b"\xff" * 6
    return (b"\x40\x00\x00\x00" + bcast + sa + bcast + b"\x00\x00"
            + b"\x00\x00" + b"\x01\x04\x02\x04\x0b\x16")  # probe-req, wildcard SSID, rates

def ssid_of(f):  # beacon(0x80)/probe-resp(0x50): SSID IE at offset 36
    if len(f) >= 38 and f[0] in (0x80, 0x50) and f[36] == 0:
        n = f[37]
        return f[38:38+n].decode("ascii", "replace")
    return None

def main():
    chan = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    fd = os.open(PATH, os.O_RDWR | os.O_NOCTTY)
    set_raw(fd)

    os.write(fd, frame(PLANE_CTRL, CMD_SET_MODE, bytes([MODE_MONITOR])))
    os.write(fd, frame(PLANE_CTRL, CMD_SET_CHANNEL, bytes([chan])))
    print(f"[client] monitor + channel {chan}; injecting probe-req every 2s, decoding frames...")

    buf, seen, last_tx = b"", {}, 0.0
    while True:
        now = time.time()
        if now - last_tx > 2.0:
            os.write(fd, frame(PLANE_RAW, 0, probe_req())); last_tx = now
            print("[client] -> injected probe-req")
        if select.select([fd], [], [], 0.5)[0]:
            buf += os.read(fd, 8192)
        while len(buf) >= 4:
            ln = buf[2] | (buf[3] << 8)
            if len(buf) < 4 + ln: break
            plane, body, buf = buf[0], buf[4:4+ln], buf[4+ln:]
            if plane == PLANE_RAW and ln >= 5:
                ch, rssi = body[0], struct.unpack("b", body[1:2])[0]
                f = body[5:]
                s = ssid_of(f)
                if s is not None:
                    kind = "BEACON" if f[0] == 0x80 else "PROBE-RESP"
                    key = (kind, s)
                    if key not in seen or now - seen[key] > 5:
                        seen[key] = now
                        print(f"[client] <- {kind:10} ch{ch} {rssi:>4}dBm len{len(f):<4} ssid={s!r}")

if __name__ == "__main__":
    try: main()
    except KeyboardInterrupt: print("\n[client] bye")
