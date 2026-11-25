/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Checked.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/MemoryStream.h>
#include <AK/StringBuilder.h>
#include <LibCompress/Deflate.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibWebView/ExtensionPackage.h>

namespace WebView {

// Anything larger than this is far beyond what any real extension packs, and only makes us run out of memory.
static constexpr size_t MAX_UNPACKED_SIZE = 512 * MiB;

static constexpr u32 ZIP_END_OF_CENTRAL_DIRECTORY_SIGNATURE = 0x06054b50;
static constexpr u32 ZIP_CENTRAL_DIRECTORY_FILE_HEADER_SIGNATURE = 0x02014b50;
static constexpr u32 ZIP_LOCAL_FILE_HEADER_SIGNATURE = 0x04034b50;

static constexpr u16 ZIP_METHOD_STORE = 0;
static constexpr u16 ZIP_METHOD_DEFLATE = 8;

static u16 read_u16(ReadonlyBytes bytes, size_t offset)
{
    return static_cast<u16>(bytes[offset]) | (static_cast<u16>(bytes[offset + 1]) << 8);
}

static u32 read_u32(ReadonlyBytes bytes, size_t offset)
{
    return static_cast<u32>(read_u16(bytes, offset)) | (static_cast<u32>(read_u16(bytes, offset + 2)) << 16);
}

static ErrorOr<ReadonlyBytes> checked_slice(ReadonlyBytes bytes, size_t offset, size_t length)
{
    Checked<size_t> end = offset;
    end += length;
    if (end.has_overflow() || end.value() > bytes.size())
        return Error::from_string_literal("Archive is truncated");
    return bytes.slice(offset, length);
}

static ErrorOr<String> normalized_archive_path(StringView name)
{
    auto path = name.replace("\\"sv, "/"sv, ReplaceMode::All);
    auto canonical = LexicalPath::canonicalized_path(ByteString::formatted("/{}", path));
    if (canonical == "/"sv)
        return Error::from_string_literal("Archive has a file without a name");
    return String::from_utf8(canonical.substring_view(1));
}

static ErrorOr<ByteBuffer> inflate(ReadonlyBytes compressed, size_t uncompressed_size)
{
    auto stream = TRY(Compress::DeflateDecompressor::create(MaybeOwned<Stream>(make<FixedMemoryStream>(compressed))));
    auto buffer = TRY(ByteBuffer::create_uninitialized(uncompressed_size));
    TRY(stream->read_until_filled(buffer));

    // The entry has to be exactly as large as the archive says.
    u8 extra_byte = 0;
    if (!stream->is_eof() && !TRY(stream->read_some({ &extra_byte, 1 })).is_empty())
        return Error::from_string_literal("Archive entry is larger than it claims to be");
    return buffer;
}

ErrorOr<HashMap<String, ByteBuffer>> read_zip_archive(ReadonlyBytes data)
{
    static constexpr size_t end_of_central_directory_size = 22;
    if (data.size() < end_of_central_directory_size)
        return Error::from_string_literal("Archive is too small");

    // The end of central directory record is followed by a comment of at most 65535 bytes.
    Optional<size_t> end_of_central_directory;
    auto search_end = data.size() - end_of_central_directory_size;
    auto search_start = search_end > 0xffff ? search_end - 0xffff : 0;
    for (size_t offset = search_end + 1; offset-- > search_start;) {
        if (read_u32(data, offset) == ZIP_END_OF_CENTRAL_DIRECTORY_SIGNATURE) {
            end_of_central_directory = offset;
            break;
        }
    }
    if (!end_of_central_directory.has_value())
        return Error::from_string_literal("Archive has no central directory");

    auto entry_count = read_u16(data, *end_of_central_directory + 10);
    auto central_directory_size = read_u32(data, *end_of_central_directory + 12);
    auto central_directory_offset = read_u32(data, *end_of_central_directory + 16);
    auto central_directory = TRY(checked_slice(data, central_directory_offset, central_directory_size));

    HashMap<String, ByteBuffer> files;
    size_t total_size = 0;
    size_t offset = 0;

    for (size_t i = 0; i < entry_count; ++i) {
        auto header = TRY(checked_slice(central_directory, offset, 46));
        if (read_u32(header, 0) != ZIP_CENTRAL_DIRECTORY_FILE_HEADER_SIGNATURE)
            return Error::from_string_literal("Archive has a malformed central directory");

        auto flags = read_u16(header, 8);
        auto method = read_u16(header, 10);
        auto compressed_size = read_u32(header, 20);
        auto uncompressed_size = read_u32(header, 24);
        auto name_length = read_u16(header, 28);
        auto extra_length = read_u16(header, 30);
        auto comment_length = read_u16(header, 32);
        auto local_header_offset = read_u32(header, 42);

        auto name = StringView { TRY(checked_slice(central_directory, offset + 46, name_length)) };
        offset += 46 + name_length + extra_length + comment_length;

        if (name.ends_with('/'))
            continue;
        if (flags & 1)
            return Error::from_string_literal("Archive has an encrypted file");
        if (compressed_size == NumericLimits<u32>::max() || uncompressed_size == NumericLimits<u32>::max() || local_header_offset == NumericLimits<u32>::max())
            return Error::from_string_literal("ZIP64 archives are not supported");

        total_size += uncompressed_size;
        if (total_size > MAX_UNPACKED_SIZE)
            return Error::from_string_literal("Archive is too large");

        auto local_header = TRY(checked_slice(data, local_header_offset, 30));
        if (read_u32(local_header, 0) != ZIP_LOCAL_FILE_HEADER_SIGNATURE)
            return Error::from_string_literal("Archive has a malformed file header");
        auto data_offset = static_cast<size_t>(local_header_offset) + 30 + read_u16(local_header, 26) + read_u16(local_header, 28);
        auto compressed = TRY(checked_slice(data, data_offset, compressed_size));

        ByteBuffer contents;
        if (method == ZIP_METHOD_STORE) {
            if (compressed_size != uncompressed_size)
                return Error::from_string_literal("Archive has a stored file of the wrong size");
            contents = TRY(ByteBuffer::copy(compressed));
        } else if (method == ZIP_METHOD_DEFLATE) {
            contents = TRY(inflate(compressed, uncompressed_size));
        } else {
            return Error::from_string_literal("Archive uses an unsupported compression method");
        }

        files.set(TRY(normalized_archive_path(name)), move(contents));
    }

    return files;
}

String extension_id_from_hash(ReadonlyBytes hash)
{
    StringBuilder builder;
    for (auto byte : hash.trim(16)) {
        builder.append(static_cast<char>('a' + (byte >> 4)));
        builder.append(static_cast<char>('a' + (byte & 0xf)));
    }
    return MUST(builder.to_string());
}

static String extension_id_from_data(ReadonlyBytes data)
{
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, data);
    return extension_id_from_hash(digest.bytes());
}

// Finds the first length-delimited field with the given number in a protocol buffer message.
static Optional<ReadonlyBytes> protobuf_bytes_field(ReadonlyBytes message, u64 field_number)
{
    size_t offset = 0;

    auto read_varint = [&]() -> Optional<u64> {
        u64 value = 0;
        for (size_t shift = 0; shift < 64; shift += 7) {
            if (offset >= message.size())
                return {};
            auto byte = message[offset++];
            value |= static_cast<u64>(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return value;
        }
        return {};
    };

    while (offset < message.size()) {
        auto key = read_varint();
        if (!key.has_value())
            return {};

        switch (*key & 7) {
        case 0:
            if (!read_varint().has_value())
                return {};
            break;
        case 1:
            offset += 8;
            break;
        case 5:
            offset += 4;
            break;
        case 2: {
            auto length = read_varint();
            if (!length.has_value() || *length > message.size() - offset)
                return {};
            if ((*key >> 3) == field_number)
                return message.slice(offset, *length);
            offset += *length;
            break;
        }
        default:
            return {};
        }
    }

    return {};
}

// An archive without a CRX header has the ID of the public key its manifest gives, as an unpacked extension does in
// Chrome. Without one, the ID comes from the archive itself, which stays the same when the file is renamed.
static ErrorOr<String> extension_id_from_manifest(HashMap<String, ByteBuffer> const& files, ReadonlyBytes archive)
{
    auto manifest = files.get("manifest.json"sv);
    if (!manifest.has_value())
        return Error::from_string_literal("Extension has no manifest.json");

    auto json = TRY(JsonValue::from_string(StringView { manifest->bytes() }.trim("\xEF\xBB\xBF"sv, TrimMode::Left)));
    if (!json.is_object())
        return Error::from_string_literal("Extension manifest is not an object");

    if (auto key = json.as_object().get_string("key"sv); key.has_value()) {
        if (auto public_key = decode_base64(*key); !public_key.is_error())
            return extension_id_from_data(public_key.value());
    }

    return extension_id_from_data(archive);
}

ErrorOr<ExtensionPackage> read_extension_package(ReadonlyBytes data)
{
    ReadonlyBytes archive = data;
    Optional<String> id;

    if (data.size() >= 4 && StringView { data.slice(0, 4) } == "Cr24"sv) {
        if (data.size() < 12)
            return Error::from_string_literal("CRX file is truncated");

        auto version = read_u32(data, 4);
        if (version == 3) {
            // CRX3: the magic number, the version, the size of a CrxFileHeader message, the message, and the archive.
            auto header_size = read_u32(data, 8);
            auto header = TRY(checked_slice(data, 12, header_size));
            archive = data.slice(12 + header_size);

            // CrxFileHeader.signed_header_data (10000) is a SignedData message, whose crx_id (1) is the ID.
            if (auto signed_header_data = protobuf_bytes_field(header, 10000); signed_header_data.has_value()) {
                if (auto crx_id = protobuf_bytes_field(*signed_header_data, 1); crx_id.has_value() && crx_id->size() == 16)
                    id = extension_id_from_hash(*crx_id);
            }
        } else if (version == 2) {
            // CRX2: the magic number, the version, the sizes of the public key and the signature, both of those, and
            // the archive. The ID is derived from the public key.
            if (data.size() < 16)
                return Error::from_string_literal("CRX file is truncated");
            auto public_key_size = read_u32(data, 8);
            auto signature_size = read_u32(data, 12);
            auto public_key = TRY(checked_slice(data, 16, public_key_size));
            TRY(checked_slice(data, 16 + static_cast<size_t>(public_key_size), signature_size));
            archive = data.slice(16 + static_cast<size_t>(public_key_size) + signature_size);
            id = extension_id_from_data(public_key);
        } else {
            return Error::from_string_literal("Unsupported CRX version");
        }
    }

    auto files = TRY(read_zip_archive(archive));
    if (!files.contains("manifest.json"sv))
        return Error::from_string_literal("Extension has no manifest.json");

    if (!id.has_value())
        id = TRY(extension_id_from_manifest(files, archive));

    return ExtensionPackage { id.release_value(), move(files) };
}

}
