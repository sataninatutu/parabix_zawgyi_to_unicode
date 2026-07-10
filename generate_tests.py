import random
import codecs
import os

chars = [0x106A, 0x106B, 0x108F, 0x1090, 0x1086, 0x1031, 0x103A, 0x105A, 0x1060, 0x1061]

for size_mb in [1, 5, 10]:
    num_chars = size_mb * 1024 * 1024 // 3
    text = "".join(chr(random.choice(chars)) for _ in range(num_chars))
    filename = f"test_{size_mb}MB.zawgyi"
    with open(filename, "wb") as f:
        encoded = codecs.encode(text, "utf-8")
        f.write(encoded[:size_mb * 1024 * 1024])
    print(f"Created {filename} ({size_mb}MB)")

print("Done!")
