/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/MaybeOwned.h>
#include <AK/Stream.h>
#include <LibCompress/GenericZlib.h>

namespace Compress {

// Decompresses raw DEFLATE data (RFC 1951), with neither a zlib nor a gzip wrapper, as found in ZIP archives.
class DeflateDecompressor final : public GenericZlibDecompressor {
public:
    static ErrorOr<NonnullOwnPtr<DeflateDecompressor>> create(MaybeOwned<Stream>);
    static ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes);

private:
    DeflateDecompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
        : GenericZlibDecompressor(move(buffer), move(stream), zstream)
    {
    }
};

}
