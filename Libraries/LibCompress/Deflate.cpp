/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Deflate.h>

#include <zlib.h>

namespace Compress {

ErrorOr<NonnullOwnPtr<DeflateDecompressor>> DeflateDecompressor::create(MaybeOwned<Stream> stream)
{
    auto buffer = TRY(AK::FixedArray<u8>::create(16 * 1024));
    // A negative window size tells zlib that the data has no header.
    auto zstream = TRY(GenericZlibDecompressor::new_z_stream(-MAX_WBITS));
    return adopt_nonnull_own_or_enomem(new (nothrow) DeflateDecompressor(move(buffer), move(stream), zstream));
}

ErrorOr<ByteBuffer> DeflateDecompressor::decompress_all(ReadonlyBytes bytes)
{
    return ::Compress::decompress_all<DeflateDecompressor>(bytes);
}

}
