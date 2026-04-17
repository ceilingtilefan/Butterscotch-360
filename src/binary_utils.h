#pragma once

#include <stdint.h>
#include <string.h>

// Little-endian reads/writes from a raw byte buffer.
// These are portable and work regardless of host endianness.

static inline uint8_t BinaryUtils_readUint8(const uint8_t* data) {
    return data[0];
}

static inline uint16_t BinaryUtils_readUint16(const uint8_t* data) {
    return (uint16_t) data[0] | ((uint16_t) data[1] << 8);
}

static inline int16_t BinaryUtils_readInt16(const uint8_t* data) {
    return (int16_t) BinaryUtils_readUint16(data);
}

static inline uint32_t BinaryUtils_readUint32(const uint8_t* data) {
    return (uint32_t) data[0] | ((uint32_t) data[1] << 8) | ((uint32_t) data[2] << 16) | ((uint32_t) data[3] << 24);
}

static inline int32_t BinaryUtils_readInt32(const uint8_t* data) {
    return (int32_t) BinaryUtils_readUint32(data);
}

static inline int64_t BinaryUtils_readInt64(const uint8_t* data) {
    uint64_t val = (uint64_t) data[0] | ((uint64_t) data[1] << 8) | ((uint64_t) data[2] << 16) | ((uint64_t) data[3] << 24) |
                   ((uint64_t) data[4] << 32) | ((uint64_t) data[5] << 40) | ((uint64_t) data[6] << 48) | ((uint64_t) data[7] << 56);
    return (int64_t) val;
}

static inline float BinaryUtils_readFloat32(const uint8_t* data) {
    // Read as little-endian uint32, then reinterpret as float.
    // This is portable across both little-endian and big-endian hosts.
    uint32_t bits = BinaryUtils_readUint32(data);
    float val;
    memcpy(&val, &bits, 4);
    return val;
}

static inline double BinaryUtils_readFloat64(const uint8_t* data) {
    // Read as little-endian uint64, then reinterpret as double.
    uint64_t bits = (uint64_t) data[0] | ((uint64_t) data[1] << 8) | ((uint64_t) data[2] << 16) | ((uint64_t) data[3] << 24) |
                    ((uint64_t) data[4] << 32) | ((uint64_t) data[5] << 40) | ((uint64_t) data[6] << 48) | ((uint64_t) data[7] << 56);
    double val;
    memcpy(&val, &bits, 8);
    return val;
}

static inline void BinaryUtils_writeUint32(uint8_t* data, uint32_t val) {
    // Write as little-endian (portable).
    data[0] = (uint8_t)(val);
    data[1] = (uint8_t)(val >> 8);
    data[2] = (uint8_t)(val >> 16);
    data[3] = (uint8_t)(val >> 24);
}
