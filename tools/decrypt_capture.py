#!/usr/bin/env python3
"""Decrypt SwitchBot keypad BLE traffic from an Android btsnoop_hci.log.

The keypad's GATT commands are AES-128-CTR encrypted with a per-keypad
communication key (K14). Sniff the official app talking to your keypad
(Android → Developer options → "Enable Bluetooth HCI snoop log"), grab your K14
from this firmware (set `show_communication_key: true`, pair, read it in the web
console → Settings), then decrypt to see the plaintext commands.

    python decrypt_capture.py btsnoop_hci.log <K14-hex-32-chars>

Frame format (see docs/protocol.md):
    command  : 57 <key_id> <iv0> <iv1> <AES-128-CTR(K14, session_IV, plaintext)>
    response : 01 <key_id> <iv0> <iv1> <AES-128-CTR(K14, session_IV, plaintext)>
The 16-byte session IV (the CTR counter, reset per frame) comes from the IV
negotiation reply `01 00 00 00 <IV[16]>`.

Needs `cryptography` (pip install cryptography) or `pycryptodome`.
"""
import struct
import sys


def ctr_decrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

        d = Cipher(algorithms.AES(key), modes.CTR(iv)).decryptor()
        return d.update(data) + d.finalize()
    except ImportError:
        from Crypto.Cipher import AES
        from Crypto.Util import Counter

        ctr = Counter.new(128, initial_value=int.from_bytes(iv, "big"))
        return AES.new(key, AES.MODE_CTR, counter=ctr).decrypt(data)


# ATT opcodes and the keypad's lock GATT handles (from the same capture).
ATT_WRITE_REQ, ATT_WRITE_CMD, ATT_HVN, ATT_HVI = 0x12, 0x52, 0x1B, 0x1D
RX_HANDLE, TX_HANDLE = 0x0010, 0x0012  # cba20002 (write) / cba20003 (notify)

UNLOCK_METHOD = {0x04: "pin", 0x08: "nfc", 0x0C: "fingerprint", 0x18: "face"}


def decode(pt: bytes) -> str:
    """Best-effort decode of a decrypted plaintext command."""
    if pt[:4] == bytes.fromhex("0f4e0103") and len(pt) >= 8 and pt[5] == 0x80:
        idx = pt[6]
        return f"UNLOCK method={UNLOCK_METHOD.get(pt[4], f'0x{pt[4]:02x}')} index={idx}"
    if pt[:4] == bytes.fromhex("0f4e0103"):
        return "LOCK"
    if pt[:3] == bytes.fromhex("0f4f81"):
        return "STATE_POLL"
    if pt[:2] == bytes.fromhex("0103"):
        return "DOORBELL"
    if pt[:3] == bytes.fromhex("0f5201") and len(pt) >= 5:
        return f"SET   param=0x{pt[3]:02x} value={pt[4:].hex()}"
    if pt[:3] == bytes.fromhex("0f5301") and len(pt) >= 4:
        return f"GET   param=0x{pt[3]:02x}"
    if pt[:2] == bytes.fromhex("0005"):
        return "CREDENTIAL op (passcode?)"
    return "??? UNKNOWN - candidate new command"


def att_frames(path: str):
    """Yield ('CMD'|'RESP', value_bytes) for the lock RX/TX handles, in order."""
    data = open(path, "rb").read()
    if data[:8] != b"btsnoop\x00":
        sys.exit("not a btsnoop file")
    off = 16
    frag = {}
    while off + 24 <= len(data):
        _, incl, _flags, _drops, _ts = struct.unpack(">IIIIQ", data[off:off + 24])
        off += 24
        pkt = data[off:off + incl]
        off += incl
        if len(pkt) < 1 or pkt[0] != 0x02:  # ACL data only
            continue
        body = pkt[1:]
        if len(body) < 4:
            continue
        hf, alen = struct.unpack("<HH", body[:4])
        h, pb = hf & 0x0FFF, (hf >> 12) & 3
        payload = body[4:4 + alen]
        if pb == 1:  # continuation
            st = frag.get(h)
            if not st:
                continue
            st[2].extend(payload)
            st[0] -= len(payload)
            if st[0] > 0:
                continue
            l2, cid = bytes(st[2]), st[1]
            del frag[h]
        else:
            if len(payload) < 4:
                continue
            l2len, cid = struct.unpack("<HH", payload[:4])
            l2 = payload[4:]
            if len(l2) < l2len:
                frag[h] = [l2len - len(l2), cid, bytearray(l2)]
                continue
            l2 = l2[:l2len]
        if cid != 0x0004 or not l2:
            continue
        op = l2[0]
        if op in (ATT_WRITE_REQ, ATT_WRITE_CMD) and len(l2) >= 3:
            if struct.unpack("<H", l2[1:3])[0] == RX_HANDLE:
                yield "CMD", bytes(l2[3:])
        elif op in (ATT_HVN, ATT_HVI) and len(l2) >= 3:
            if struct.unpack("<H", l2[1:3])[0] == TX_HANDLE:
                yield "RESP", bytes(l2[3:])


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, k14 = sys.argv[1], bytes.fromhex(sys.argv[2].strip())
    if len(k14) != 16:
        sys.exit("K14 must be 16 bytes (32 hex chars)")

    iv = None
    seen = set()
    print("=== Decrypted keypad traffic ===\n")
    for kind, v in att_frames(path):
        hx = v.hex()
        if kind == "RESP" and hx.startswith("01000000") and len(v) == 20:
            iv = v[4:]
            print(f"--- session IV = {iv.hex()} ---")
            continue
        if kind == "CMD" and hx.startswith("570000000f2103"):
            print(f"IV-negotiate (key_id=0x{v[7]:02x})")
            continue
        if v and v[0] in (0x57, 0x01) and len(v) >= 4 and iv:
            ct = v[4:]
            if not ct:
                continue  # bare ack
            pt = ctr_decrypt(k14, iv, ct)
            label = decode(pt) if kind == "CMD" else "response"
            line = f"{kind:4} PT={pt.hex():<24} {label}"
            print(line)
            if kind == "CMD":
                seen.add(pt.hex())

    print("\n=== distinct commands seen ===")
    for h in sorted(seen):
        print(f"  {h:<24} {decode(bytes.fromhex(h))}")


if __name__ == "__main__":
    main()
