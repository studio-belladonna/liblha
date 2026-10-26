### libLHA
library for compressing and decompressing LZH/LHa

This supports the Lempel-Ziv-Huffman written by Haruyasu Yoshizaki in 1988 
alongside multiple additional formats.

Currently supports compression and decompression of:
- lh0
- lh1
- lh2
- lh3
- lh4
- lh5
- lh6
- lh7
- lhx
- lk7
- lz4
- lz5
- lzs
- pma

Some of these are really old and obscure but the goal with this is to support literaly all known formats.

### Usage:

```cpp
#include <liblha/lha.h>

int main() {
    // choose the codec you want
    lha::Codec codec = lha::Codec::Lh7;

    lha::Bytes data(32768);

    // pass the codec and your data
    lha::Result packedData = lha::compress(codec, data);

    // pass the codec, your compressed data, and the expected size
    lha::Result unpackedData = lha::decompress(codec, packedData, data.size());

    return 0;
}

```

### building:

```bash
cmake -S . -B build -G ninja
cmake --build build

# test it with
ctest --test-dir build --output-on-failure
```
