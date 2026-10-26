#include <liblha/lha.h>

#include <algorithm>
#include <cassert>
#include <string_view>

// messy tests, needs rework

int main(int argc, char** argv) {
    using namespace lha;

    assert(argc == 2);
    Codec codec = Codec::Lh0;
    assert(codecFromMethod(argv[1], codec));
    assert(methodName(codec) == argv[1]);

    Bytes original(32768);
    for (std::size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<Byte>((i / 29) % 17);
    }

    // TODO: use some kind of selector
    const Result packed = compress(codec, original);
    if (codec != Codec::Lh0
    &&  codec != Codec::Lh1
    &&  codec != Codec::Lh2
    &&  codec != Codec::Lh3
    &&  codec != Codec::Lh4
    &&  codec != Codec::Lh5
    &&  codec != Codec::Lh6
    &&  codec != Codec::Lh7
    &&  codec != Codec::Lz4
    &&  codec != Codec::Lz5
    &&  codec != Codec::Lzs
    &&  codec != Codec::Lhx
    &&  codec != Codec::Lk7
    &&  codec != Codec::Pm0
    &&  codec != Codec::Pm1
    &&  codec != Codec::Pm2
    ) {
        assert(!packed);
        assert(packed.error == Error::UnsupportedCodec);
        const Result unpacked = decompress(codec, {}, original.size());
        assert(!unpacked);
        assert(unpacked.error == Error::UnsupportedCodec);
        return 0;
    }

    assert(packed);
    if (codec == Codec::Lh1
    ||  codec == Codec::Lh2
    ||  codec == Codec::Lh3
    ||  codec == Codec::Lh4
    ||  codec == Codec::Lh5
    ||  codec == Codec::Lh6
    ||  codec == Codec::Lh7
    ||  codec == Codec::Lz5
    ||  codec == Codec::Lzs
    ||  codec == Codec::Lhx
    ||  codec == Codec::Lk7
    ) {
        assert(packed.data.size() < original.size());
    } else if (codec == Codec::Lh0 || codec == Codec::Lz4 || codec == Codec::Pm0) {
        assert(packed.data == original);
    }
    
    const Result unpacked = decompress(codec, packed.data, original.size());
    assert(unpacked);
    assert(unpacked.data == original);

    if (codec == Codec::Lz4) {
        Bytes repetitive(65536);
        for (std::size_t i = 0; i < repetitive.size(); ++i) {
            repetitive[i] = static_cast<Byte>((i / 17) % 9);
        }

        const Result repetitivePacked = compress(Codec::Lz4, repetitive);
        assert(repetitivePacked);
        assert(repetitivePacked.data == repetitive);
        const Result repetitiveUnpacked = decompress(Codec::Lz4, repetitivePacked.data, repetitive.size());
        assert(repetitiveUnpacked);
        assert(repetitiveUnpacked.data == repetitive);

        const Result mismatched = decompress(Codec::Lz4, std::span<const Byte>(repetitivePacked.data).first(repetitivePacked.data.size() - 1), repetitive.size());
        assert(!mismatched);
        assert(mismatched.error == Error::SizeMismatch);

        const Result empty = compress(Codec::Lz4, {});
        assert(empty);
        const Result emptyUnpacked = decompress(Codec::Lz4, empty.data, 0);
        assert(emptyUnpacked);

        const Entry entry{"lz4.bin", repetitive, Codec::Lz4};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lz4);
        assert(archiveRead.entries[0].data == repetitive);
    }

    if (codec == Codec::Lz5) {
        const Bytes initialDictionaryMatch{0x00, 0x4d, 0x31};
        const Result knownVector = decompress(Codec::Lz5, initialDictionaryMatch, 4);
        assert(knownVector);
        assert((knownVector.data == Bytes{'A', 'A', 'A', 'A'}));

        const Result repetitivePacked = compress(Codec::Lz5, original);
        assert(repetitivePacked);
        assert(repetitivePacked.data.size() < original.size());
        const Result repetitiveUnpacked = decompress(Codec::Lz5, repetitivePacked.data, original.size());
        assert(repetitiveUnpacked);
        assert(repetitiveUnpacked.data == original);

        const Result truncated = decompress(Codec::Lz5, Bytes{0x00, 0x4d}, 4);
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);

        const Result empty = compress(Codec::Lz5, {});
        assert(empty);
        const Result emptyUnpacked = decompress(Codec::Lz5, empty.data, 0);
        assert(emptyUnpacked);

        const Entry entry{"lz5.bin", original, Codec::Lz5};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lz5);
        assert(archiveRead.entries[0].data == original);
    }

    if (codec == Codec::Lzs) {
        const Bytes literalA{0xa0, 0x80};
        const Result knownLiteral = decompress(Codec::Lzs, literalA, 1);
        assert(knownLiteral);
        assert((knownLiteral.data == Bytes{'A'}));
        const Result encodedLiteral = compress(Codec::Lzs, Bytes{'A'});
        assert(encodedLiteral);
        assert(encodedLiteral.data == literalA);

        const Bytes initialDictionaryMatch{0x7e, 0xf0};
        const Result knownMatch = decompress(Codec::Lzs, initialDictionaryMatch, 2);
        assert(knownMatch);
        assert((knownMatch.data == Bytes{' ', ' '}));

        const Result repetitivePacked = compress(Codec::Lzs, original);
        assert(repetitivePacked);
        assert(repetitivePacked.data.size() < original.size());
        const Result repetitiveUnpacked = decompress(Codec::Lzs, repetitivePacked.data, original.size());
        assert(repetitiveUnpacked);
        assert(repetitiveUnpacked.data == original);

        // missing MA byte sometimes?

        const Result truncated = decompress(Codec::Lzs, Bytes{0x80}, 1);
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);
        const Result invalidPadding = decompress(Codec::Lzs, Bytes{0xa0, 0x81}, 1);
        assert(!invalidPadding);
        assert(invalidPadding.error == Error::InvalidInput);

        const Result empty = compress(Codec::Lzs, {});
        assert(empty);
        const Result emptyUnpacked = decompress(Codec::Lzs, empty.data, 0);
        assert(emptyUnpacked);

        const Entry entry{"lzs.bin", original, Codec::Lzs};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lzs);
        assert(archiveRead.entries[0].data == original);
    }

    if (codec == Codec::Lhx) {
        const Result repetitivePacked = compress(Codec::Lhx, original);
        assert(repetitivePacked);
        assert(repetitivePacked.data.size() < original.size());
        const Result repetitiveUnpacked = decompress(Codec::Lhx, repetitivePacked.data, original.size());
        assert(repetitiveUnpacked);
        assert(repetitiveUnpacked.data == original);

        Bytes beyondLh7Window(100512);
        std::uint32_t state = 0x12345678;
        for (std::size_t i = 0; i < 100000; ++i) {
            state = state * 1664525u + 1013904223u;
            beyondLh7Window[i] = static_cast<Byte>(state >> 24);
        }
        
        std::copy_n(beyondLh7Window.begin(), 512, beyondLh7Window.begin() + 100000);
        const Result lhxPacked = compress(Codec::Lhx, beyondLh7Window);
        assert(lhxPacked);
        const Result lh7Packed = compress(Codec::Lh7, beyondLh7Window);
        assert(lh7Packed);
        assert(lhxPacked.data.size() < lh7Packed.data.size());
        const Result extendedUnpacked = decompress(Codec::Lhx, lhxPacked.data, beyondLh7Window.size());
        assert(extendedUnpacked);
        assert(extendedUnpacked.data == beyondLh7Window);

        const Result truncated = decompress(Codec::Lhx, std::span<const Byte>(repetitivePacked.data).first(repetitivePacked.data.size() - 1), original.size());
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);

        const Result empty = compress(Codec::Lhx, {});
        assert(empty);
        const Result emptyUnpacked = decompress(Codec::Lhx, empty.data, 0);
        assert(emptyUnpacked);

        const Entry entry{"lhx.bin", original, Codec::Lhx};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lhx);
        assert(archiveRead.entries[0].data == original);
    }

    if (codec == Codec::Lk7) {
        Bytes longMatches(4096, 'Q');
        const Result longPacked = compress(Codec::Lk7, longMatches);
        assert(longPacked);
        assert(longPacked.data.size() < longMatches.size());
        const Result longUnpacked = decompress(Codec::Lk7, longPacked.data, longMatches.size());
        assert(longUnpacked);
        assert(longUnpacked.data == longMatches);

        Bytes distantMatch(40032);
        std::uint32_t state = 0x76543210;
        for (std::size_t i = 0; i < 40000; ++i) {
            state = state * 1664525u + 1013904223u;
            distantMatch[i] = static_cast<Byte>(state >> 24);
        }

        std::copy_n(distantMatch.begin(), 32, distantMatch.begin() + 40000);
        const Result distantPacked = compress(Codec::Lk7, distantMatch);
        assert(distantPacked);
        const Result distantUnpacked = decompress(Codec::Lk7, distantPacked.data, distantMatch.size());
        assert(distantUnpacked);
        assert(distantUnpacked.data == distantMatch);

        const Result truncated = decompress(Codec::Lk7, std::span<const Byte>(longPacked.data).first(longPacked.data.size() - 1), longMatches.size());
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);

        const Result empty = compress(Codec::Lk7, {});
        assert(empty);
        const Result emptyUnpacked = decompress(Codec::Lk7, empty.data, 0);
        assert(emptyUnpacked);

        const Entry entry{"lk7.bin", original, Codec::Lk7};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lk7);
        assert(archiveRead.entries[0].data == original);
    }

    if (codec == Codec::Lh1) {
        Bytes varied(12000);
        std::uint32_t state = 0x12345678;
        for (Byte& value : varied) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<Byte>(state >> 24);
        }

        const Result variedPacked = compress(Codec::Lh1, varied);
        assert(variedPacked);
        const Result variedUnpacked = decompress(Codec::Lh1, variedPacked.data, varied.size());
        assert(variedUnpacked);
        assert(variedUnpacked.data == varied);

        Bytes reconstructed(100000);
        state = 0x87654321;
        for (Byte& value : reconstructed) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<Byte>(state >> 24);
        }

        const Result reconstructedPacked = compress(Codec::Lh1, reconstructed);
        assert(reconstructedPacked);
        const Result reconstructedUnpacked = decompress(Codec::Lh1, reconstructedPacked.data, reconstructed.size());
        assert(reconstructedUnpacked);
        assert(reconstructedUnpacked.data == reconstructed);

        const Result truncated = decompress(Codec::Lh1, std::span<const Byte>(packed.data).first(packed.data.size() - 1), original.size());
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);

        const Result empty = compress(Codec::Lh1, {});
        assert(empty);
        const Result emptyUnpacked = decompress(Codec::Lh1, empty.data, 0);
        assert(emptyUnpacked);

        const Entry entry{"lh1.bin", original, Codec::Lh1};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lh1);
        assert(archiveRead.entries[0].data == original);
    }

    if (codec == Codec::Lh2) {
        Bytes beyondLh1Window(6512);
        std::uint32_t state = 0xa341316c;
        for (std::size_t i = 0; i < 6000; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            beyondLh1Window[i] = static_cast<Byte>(state >> 24);
        }

        for (std::size_t i = 0; i < 512; ++i) {
            beyondLh1Window[6000 + i] = beyondLh1Window[i];
        }

        const Result lh2Packed = compress(Codec::Lh2, beyondLh1Window);
        const Result lh1Packed = compress(Codec::Lh1, beyondLh1Window);
        assert(lh2Packed);
        assert(lh1Packed);
        assert(lh2Packed.data.size() < lh1Packed.data.size());
        const Result lh2Unpacked = decompress(Codec::Lh2, lh2Packed.data, beyondLh1Window.size());
        assert(lh2Unpacked);
        assert(lh2Unpacked.data == beyondLh1Window);

        const Result truncated = decompress(Codec::Lh2, std::span<const Byte>(lh2Packed.data).first(lh2Packed.data.size() - 1), beyondLh1Window.size());
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);

        const Entry entry{"lh2.bin", beyondLh1Window, Codec::Lh2};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lh2);
        assert(archiveRead.entries[0].data == beyondLh1Window);

        Bytes longMatch(900);
        for (std::size_t i = 0; i < longMatch.size(); ++i) {
            longMatch[i] = static_cast<Byte>((i * 73 + i / 11) & 0xff);
        }

        const Bytes repeatedPart = longMatch;
        longMatch.insert(longMatch.end(), repeatedPart.begin(), repeatedPart.end());
        const Result longMatchPacked = compress(Codec::Lh2, longMatch);
        assert(longMatchPacked);
        const Result longMatchUnpacked = decompress(Codec::Lh2, longMatchPacked.data, longMatch.size());
        assert(longMatchUnpacked);
        assert(longMatchUnpacked.data == longMatch);
    }

    if (codec == Codec::Lh3) {
        Bytes varied(16000);
        std::uint32_t state = 0x510e527f;
        for (Byte& value : varied) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<Byte>(state >> 24);
        }

        const Result variedPacked = compress(Codec::Lh3, varied);
        assert(variedPacked);
        const Result variedUnpacked = decompress(Codec::Lh3, variedPacked.data, varied.size());
        assert(variedUnpacked);
        assert(variedUnpacked.data == varied);

        const Result truncated = decompress(Codec::Lh3, std::span<const Byte>(packed.data).first(packed.data.size() - 1), original.size());
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);

        const Result empty = compress(Codec::Lh3, {});
        assert(empty);
        const Result emptyUnpacked = decompress(Codec::Lh3, empty.data, 0);
        assert(emptyUnpacked);

        const Entry entry{"lh3.bin", original, Codec::Lh3};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lh3);
        assert(archiveRead.entries[0].data == original);
    }

    if (codec == Codec::Lh4) {
        Bytes beyondLh4Window(6512);
        std::uint32_t state = 0x3c6ef372;
        for (std::size_t i = 0; i < 6000; ++i) {
            state = state * 1664525u + 1013904223u;
            beyondLh4Window[i] = static_cast<Byte>(state >> 24);
        }

        for (std::size_t i = 0; i < 512; ++i) {
            beyondLh4Window[6000 + i] = beyondLh4Window[i];
        }

        const Result lh4Packed = compress(Codec::Lh4, beyondLh4Window);
        const Result lh5Packed = compress(Codec::Lh5, beyondLh4Window);
        assert(lh4Packed);
        assert(lh5Packed);
        assert(lh5Packed.data.size() < lh4Packed.data.size());
        const Result lh4Unpacked = decompress(Codec::Lh4, lh4Packed.data, beyondLh4Window.size());
        assert(lh4Unpacked);
        assert(lh4Unpacked.data == beyondLh4Window);

        const Result truncated = decompress(Codec::Lh4, std::span<const Byte>(lh4Packed.data).first(lh4Packed.data.size() - 1), beyondLh4Window.size());
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);

        const Entry entry{"lh4.bin", beyondLh4Window, Codec::Lh4};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lh4);
        assert(archiveRead.entries[0].data == beyondLh4Window);
    }

    if (codec == Codec::Lh6) {
        Bytes beyondLh5Window(20000);
        std::uint32_t state = 0x91e10da5;
        for (std::size_t i = 0; i < 17000; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            beyondLh5Window[i] = static_cast<Byte>(state >> 24);
        }

        for (std::size_t i = 0; i < 512; ++i) {
            beyondLh5Window[17000 + i] = beyondLh5Window[i];
        }

        const Result lh6Packed = compress(Codec::Lh6, beyondLh5Window);
        const Result lh5Packed = compress(Codec::Lh5, beyondLh5Window);
        assert(lh6Packed);
        assert(lh5Packed);
        assert(lh6Packed.data.size() < lh5Packed.data.size());
        const Result lh6Unpacked = decompress(Codec::Lh6, lh6Packed.data, beyondLh5Window.size());
        assert(lh6Unpacked);
        assert(lh6Unpacked.data == beyondLh5Window);

        const Result truncated = decompress(Codec::Lh6, std::span<const Byte>(lh6Packed.data).first(lh6Packed.data.size() - 1), beyondLh5Window.size());
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);

        const Entry entry{"lh6.bin", beyondLh5Window, Codec::Lh6};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lh6);
        assert(archiveRead.entries[0].data == beyondLh5Window);
    }

    if (codec == Codec::Lh7) {
        Bytes beyondLh6Window(41024);
        std::uint32_t state = 0x6d2b79f5;
        for (std::size_t i = 0; i < 40000; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            beyondLh6Window[i] = static_cast<Byte>(state >> 24);
        }

        for (std::size_t i = 0; i < 1024; ++i) {
            beyondLh6Window[40000 + i] = beyondLh6Window[i];
        }

        const Result lh7Packed = compress(Codec::Lh7, beyondLh6Window);
        const Result lh6Packed = compress(Codec::Lh6, beyondLh6Window);
        assert(lh7Packed);
        assert(lh6Packed);
        assert(lh7Packed.data.size() < lh6Packed.data.size());
        const Result lh7Unpacked = decompress(Codec::Lh7, lh7Packed.data, beyondLh6Window.size());
        assert(lh7Unpacked);
        assert(lh7Unpacked.data == beyondLh6Window);

        const Result truncated = decompress(Codec::Lh7, std::span<const Byte>(lh7Packed.data).first(lh7Packed.data.size() - 1), beyondLh6Window.size());
        assert(!truncated);
        assert(truncated.error == Error::InvalidInput);

        const Entry entry{"lh7.bin", beyondLh6Window, Codec::Lh7};
        const Result archive = writeArchive(std::span<const Entry>(&entry, 1));
        assert(archive);
        const ArchiveResult archiveRead = readArchive(archive.data);
        assert(archiveRead);
        assert(archiveRead.entries.size() == 1);
        assert(archiveRead.entries[0].codec == Codec::Lh7);
        assert(archiveRead.entries[0].data == beyondLh6Window);
    }

    return 0;
}
