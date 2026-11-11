#ifndef REDIS_SIMPLE_SERVER_RDB_RDB_FORMAT_H_
#define REDIS_SIMPLE_SERVER_RDB_RDB_FORMAT_H_

#include <cstdint>
#include <string_view>

// Constants of Redis' RDB file format (src/rdb.h).
namespace redis_simple::rdb {
inline constexpr std::string_view kMagic = "REDIS";
// The newest format version this server loads: Redis 8.10's.
inline constexpr int kMaxLoadVersion = 15;
// The version this server writes: the oldest that covers every type it
// writes (SET_LISTPACK came in version 11, with Redis 7.2), so Redis 7.2 and
// later can load its files. Redis writes its own newest version instead.
inline constexpr int kWriteVersion = 11;

// Length encodings: the top two bits of the first byte.
inline constexpr unsigned kLength6Bit = 0;
inline constexpr unsigned kLength14Bit = 1;
inline constexpr unsigned kEncodedValue = 3;
inline constexpr unsigned char kLength32Bit = 0x80;
inline constexpr unsigned char kLength64Bit = 0x81;

// Special string encodings, after kEncodedValue's two bits.
inline constexpr unsigned kEncodingInt8 = 0;
inline constexpr unsigned kEncodingInt16 = 1;
inline constexpr unsigned kEncodingInt32 = 2;
inline constexpr unsigned kEncodingLzf = 3;

// Value types.
enum ObjectType : uint8_t {
  kTypeString = 0,
  kTypeList = 1,
  kTypeSet = 2,
  kTypeZSet = 3,
  kTypeHash = 4,
  kTypeZSet2 = 5,
  kTypeModulePreGa = 6,
  kTypeModule2 = 7,
  kTypeHashZipmap = 9,
  kTypeListZiplist = 10,
  kTypeSetIntSet = 11,
  kTypeZSetZiplist = 12,
  kTypeHashZiplist = 13,
  kTypeListQuickList = 14,
  kTypeStreamListPacks = 15,
  kTypeHashListPack = 16,
  kTypeZSetListPack = 17,
  kTypeListQuickList2 = 18,
  kTypeStreamListPacks2 = 19,
  kTypeSetListPack = 20,
  kTypeStreamListPacks3 = 21,
  kTypeHashMetadataPreGa = 22,
  kTypeHashListPackExPreGa = 23,
  kTypeHashMetadata = 24,
  kTypeHashListPackEx = 25,
  kTypeStreamListPacks4 = 26,
  kTypeStreamListPacks5 = 27,
  kTypeArray = 28,
  kTypeHashTemplateListPack = 29,
  kTypeHashTemplateListPackRef = 30,
  kTypeHashTemplateArray = 31,
  kTypeHashTemplateArrayRef = 32,
  kTypeGcra = 33,
};

// Opcodes, which share the type byte's space.
enum Opcode : uint8_t {
  kOpcodeHashTemplate = 242,
  kOpcodeKeyMeta = 243,
  kOpcodeSlotInfo = 244,
  kOpcodeFunction2 = 245,
  kOpcodeFunctionPreGa = 246,
  kOpcodeModuleAux = 247,
  kOpcodeIdle = 248,
  kOpcodeFreq = 249,
  kOpcodeAux = 250,
  kOpcodeResizeDb = 251,
  kOpcodeExpireTimeMs = 252,
  kOpcodeExpireTime = 253,
  kOpcodeSelectDb = 254,
  kOpcodeEof = 255,
};

// A quicklist node's container in LIST_QUICKLIST_2: a listpack, or one
// element too large for one (Redis' QUICKLIST_NODE_CONTAINER_*).
inline constexpr uint64_t kContainerPlain = 1;
inline constexpr uint64_t kContainerPacked = 2;
}  // namespace redis_simple::rdb

#endif  // REDIS_SIMPLE_SERVER_RDB_RDB_FORMAT_H_
