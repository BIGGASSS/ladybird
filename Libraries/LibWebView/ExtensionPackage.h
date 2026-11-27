/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibWebView/Export.h>

namespace WebView {

// The files of a Chrome extension, as packed into a .crx file, or a plain .zip archive.
struct ExtensionPackage {
    String id;
    HashMap<String, ByteBuffer> files;
};

// Reads a CRX2 or CRX3 file, or a ZIP archive. The extension's ID is the one Chrome would give it: from the CRX
// header, or from the "key" of its manifest. Without either, it comes from the contents of the archive. The signatures
// of a CRX file are not verified.
WEBVIEW_API ErrorOr<ExtensionPackage> read_extension_package(ReadonlyBytes);

// Reads the files of a ZIP archive that stores them, or compresses them with DEFLATE.
WEBVIEW_API ErrorOr<HashMap<String, ByteBuffer>> read_zip_archive(ReadonlyBytes);

// Chrome's extension IDs are the first 128 bits of a SHA-256 hash, written with the letters a to p.
WEBVIEW_API String extension_id_from_hash(ReadonlyBytes);

}
