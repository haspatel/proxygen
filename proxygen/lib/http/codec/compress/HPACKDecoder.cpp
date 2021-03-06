/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/codec/compress/HPACKDecoder.h>

#include <algorithm>
#include <folly/Memory.h>
#include <proxygen/lib/http/codec/compress/HeaderCodec.h>
#include <proxygen/lib/http/codec/compress/Huffman.h>

using folly::IOBuf;
using folly::io::Cursor;
using std::list;
using std::string;
using std::unique_ptr;
using std::vector;

namespace proxygen {

unique_ptr<HPACKDecoder::headers_t> HPACKDecoder::decode(const IOBuf* buffer) {
  auto headers = folly::make_unique<headers_t>();
  Cursor cursor(buffer);
  uint32_t totalBytes = buffer ? cursor.totalLength() : 0;
  decode(cursor, totalBytes, *headers);
  // release ownership of the set of headers
  return std::move(headers);
}

const huffman::HuffTree& HPACKDecoder::getHuffmanTree() const {
  return (msgType_ == HPACK::MessageType::REQ) ?
    huffman::reqHuffTree05() : huffman::respHuffTree05();
}

uint32_t HPACKDecoder::decode(Cursor& cursor,
                              uint32_t totalBytes,
                              headers_t& headers) {
  uint32_t emittedSize = 0;
  HPACKDecodeBuffer dbuf(getHuffmanTree(), cursor, totalBytes);
  while (!hasError() && !dbuf.empty()) {
    emittedSize += decodeHeader(dbuf, headers);
    if (emittedSize > HeaderCodec::kMaxUncompressed) {
      LOG(ERROR) << "exceeded uncompressed size limit of "
                 << HeaderCodec::kMaxUncompressed << " bytes";
      err_ = Error::HEADERS_TOO_LARGE;
      return dbuf.consumedBytes();
    }
  }
  emittedSize += emitRefset(headers);
  // the emitted bytes from the refset are bounded by the size of the table,
  // but adding the check just for uniformity
  if (emittedSize > HeaderCodec::kMaxUncompressed) {
    LOG(ERROR) << "exceeded uncompressed size limit of "
               << HeaderCodec::kMaxUncompressed << " bytes";
    err_ = Error::HEADERS_TOO_LARGE;
  }
  return dbuf.consumedBytes();
}

uint32_t HPACKDecoder::emitRefset(headers_t& emitted) {
  // emit the reference set
  std::sort(emitted.begin(), emitted.end());
  list<uint32_t> refset = table_.referenceSet();
  // remove the refset entries that have already been emitted
  list<uint32_t>::iterator refit = refset.begin();
  while (refit != refset.end()) {
    const HPACKHeader& header = getDynamicHeader(dynamicToGlobalIndex(*refit));
    if (std::binary_search(emitted.begin(), emitted.end(), header)) {
      refit = refset.erase(refit);
    } else {
      refit++;
    }
  }
  // try to avoid multiple resizing of the headers vector
  emitted.reserve(emitted.size() + refset.size());
  uint32_t emittedSize = 0;
  for (const auto& index : refset) {
    emittedSize += emit(getDynamicHeader(dynamicToGlobalIndex(index)), emitted);
  }
  return emittedSize;
}

uint32_t HPACKDecoder::decodeLiteralHeader(HPACKDecodeBuffer& dbuf,
                                           headers_t& emitted) {
  uint8_t byte = dbuf.peek();
  bool indexing = !(byte & HPACK::HeaderEncoding::LITERAL_NO_INDEXING);
  HPACKHeader header;
  // check for indexed name
  const uint8_t indexMask = 0x3F;  // 0011 1111
  if (byte & indexMask) {
    uint32_t index;
    if (!dbuf.decodeInteger(6, index)) {
      LOG(ERROR) << "buffer overflow decoding index";
      err_ = Error::BUFFER_OVERFLOW;
      return 0;
    }
    // validate the index
    if (!isValid(index)) {
      LOG(ERROR) << "received invalid index: " << index;
      err_ = Error::INVALID_INDEX;
      return 0;
    }
    header.name = getHeader(index).name;
  } else {
    // skip current byte
    dbuf.next();
    if (!dbuf.decodeLiteral(header.name)) {
      LOG(ERROR) << "buffer overflow decoding header name";
      err_ = Error::BUFFER_OVERFLOW;
      return 0;
    }
  }
  // value
  if (!dbuf.decodeLiteral(header.value)) {
    LOG(ERROR) << "buffer overflow decoding header value";
    err_ = Error::BUFFER_OVERFLOW;
    return 0;
  }

  uint32_t emittedSize = emit(header, emitted);

  if (indexing && table_.add(header)) {
    // only add it to the refset if the header fit in the table
    table_.addReference(1);
  }
  return emittedSize;
}

uint32_t HPACKDecoder::decodeIndexedHeader(HPACKDecodeBuffer& dbuf,
                                           headers_t& emitted) {
  uint32_t index;
  if (!dbuf.decodeInteger(7, index)) {
    LOG(ERROR) << "buffer overflow decoding index";
    err_ = Error::BUFFER_OVERFLOW;
    return 0;
  }
  if (index == 0) {
    table_.clearReferenceSet();
    return 0;
  }
  // validate the index
  if (!isValid(index)) {
    LOG(ERROR) << "received invalid index: " << index;
    err_ = Error::INVALID_INDEX;
    return 0;
  }
  uint32_t emittedSize = 0;
  // a static index cannot be part of the reference set
  if (isStatic(index)) {
    auto& header = getStaticHeader(index);
    emittedSize = emit(header, emitted);
    if (table_.add(header)) {
      table_.addReference(1);
    }
  } else if (table_.inReferenceSet(globalToDynamicIndex(index))) {
    // index remove operation
    table_.removeReference(globalToDynamicIndex(index));
  } else {
    auto& header = getDynamicHeader(index);
    emittedSize = emit(header, emitted);
    table_.addReference(globalToDynamicIndex(index));
  }
  return emittedSize;
}

bool HPACKDecoder::isValid(uint32_t index) {
  if (!isStatic(index)) {
    return table_.isValid(globalToDynamicIndex(index));
  }
  return getStaticTable().isValid(globalToStaticIndex(index));
}

uint32_t HPACKDecoder::decodeHeader(HPACKDecodeBuffer& dbuf,
                                    headers_t& emitted) {
  uint8_t byte = dbuf.peek();
  if (byte & HPACK::HeaderEncoding::INDEXED) {
    return decodeIndexedHeader(dbuf, emitted);
  } else  {
    // LITERAL_NO_INDEXING or LITERAL_INCR_INDEXING
    return decodeLiteralHeader(dbuf, emitted);
  }
}

uint32_t HPACKDecoder::emit(const HPACKHeader& header,
                        headers_t& emitted) {
  emitted.push_back(header);
  return header.bytes();
}

}
