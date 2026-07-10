import random
import codecs

# List of Zawgyi character codes
chars = [0x106A, 0x106B, 0x108F, 0x1090, 0x1086, 0x1031, 0x103A, 0x105A, 0x1060, 0x1061]

# Generate 10,000 random characters
text = ''.join(chr(random.choice(chars)) for _ in range(10000))

# Write to file as UTF-8
with open('test_10KB.zawgyi', 'wb') as f:
    f.write(codecs.encode(text, 'utf-8'))

print('Created test_10KB.zawgyi')
