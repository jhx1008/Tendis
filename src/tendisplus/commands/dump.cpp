#include <algorithm>
#include <numeric>
#include <string>
#include <utility>
#include "tendisplus/commands/dump.h"
#include "tendisplus/commands/command.h"
#include "tendisplus/storage/skiplist.h"
#include "tendisplus/utils/string.h"
#include "tendisplus/utils/redis_port.h"

namespace tendisplus {
template <typename T>
size_t easyCopy(std::vector<byte> *buf, size_t *pos, T element) {
    if (*pos + sizeof(T) > buf->size()) {
        buf->resize(*pos + sizeof(T));
    }
    auto* ptr = reinterpret_cast<byte*>(&element);
    std::copy(ptr, (ptr + sizeof(T)), buf->begin() + *pos);
    *pos += sizeof(T);
    return sizeof(T);
}

template <typename T>
size_t easyCopy(std::vector<byte> *buf, size_t *pos,
                const T *array, size_t len) {
    if (*pos + len > buf->size()) {
        buf->resize(*pos + len * sizeof(T));
    }
    auto* ptr = const_cast<byte*>(reinterpret_cast<const byte*>(array));
    std::copy(ptr, (ptr + len * sizeof(T)), buf->begin() + *pos);
    *pos += len * sizeof(T);
    return len * sizeof(T);
}

template <typename T>
size_t easyCopy(T *dest, const std::string &buf, size_t *pos) {
    if (buf.size() < *pos) {
        return 0;
    }
    byte *ptr = reinterpret_cast<byte*>(dest);
    size_t end = *pos + sizeof(T);
    std::copy(&buf[*pos], &buf[end], ptr);
    *pos += sizeof(T);
    return sizeof(T);
}

// dump
// Base class
Serializer::Serializer(Session *sess,
        const std::string& key,
        DumpType type,
        RecordValue&& rv)
        : _sess(sess), _key(key), _type(type), _pos(0), _rv(rv) {
}

Expected<size_t> Serializer::saveObjectType(
        std::vector<byte> *payload, size_t *pos, DumpType type) {
    return easyCopy(payload, pos, type);
}

Expected<size_t> Serializer::saveLen(
        std::vector<byte> *payload, size_t *pos, size_t len) {
    byte header[2];

    if (len < (1 << 6)) {
        header[0] = (len & 0xff)|(RDB_6BITLEN << 6);
        return easyCopy(payload, pos, header, 1);
    } else if (len < (1 << 14)) {
        header[0] = ((len >> 8) & 0xff) | (RDB_14BITLEN << 6);
        header[1] = len & 0xff;
        return easyCopy(payload, pos, header, 2);
    } else if (len <= UINT32_MAX) {
        header[0] = RDB_32BITLEN;
        if (1 != easyCopy(payload, pos, header, 1)) {
            return { ErrorCodes::ERR_INTERNAL, "copy len to buffer failed" };
        }
        uint32_t len32 = htonl(len);
        return (1 + easyCopy(payload, pos, len32));
    } else {
        header[0] = RDB_64BITLEN;
        if (1 != easyCopy(payload, pos, header, 1)) {
            return { ErrorCodes::ERR_INTERNAL, "copy len to buffer failed"};
        }
        uint64_t len64 = redis_port::htonll(static_cast<uint64_t>(len));
        return (1 + easyCopy(payload, pos, len64));
    }
}

size_t Serializer::saveString(std::vector<byte> *payload,
        size_t *pos, const std::string &str) {
    size_t written(0);
    auto wr = Serializer::saveLen(payload, pos, str.size());
    INVARIANT(wr.value() > 0);
    written += wr.value();
    written += easyCopy(payload, pos, str.c_str(), str.size());
    return written;
}

Expected<std::vector<byte>> Serializer::dump(bool prefixVer) {
    std::vector<byte> payload;

    if (prefixVer) {
        Serializer::saveLen(&payload, &_pos, _rv.getVersionEP());
    }
    Serializer::saveObjectType(&payload, &_pos, _type);
    INVARIANT(_pos);

    auto expRet = dumpObject(payload);
    if (!expRet.ok()) {
        return expRet.status();
    }

    /* Write the footer, this is how it looks like:
     * ----------------+---------------------+---------------+
     * ... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
     * ----------------+---------------------+---------------+
     * RDB version and CRC are both in little endian.
     */
    byte version[2];
    version[0] = RDB_VERSION & 0xff;
    version[1] = (RDB_VERSION >> 8) & 0xff;
    easyCopy(&payload, &_pos, version, 2);

    uint64_t crc = redis_port::crc64(0, &payload[_begin], _pos - _begin);
    easyCopy(&payload, &_pos, crc);
    _end = _pos;
    return payload;
}

// Command who can only see base class Serializer.
class DumpCommand: public Command {
 public:
    DumpCommand()
        :Command("dump", "r") {
    }

    ssize_t arity() const {
        return 2;
    }

    int32_t firstkey() const {
        return 1;
    }
    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    Expected<std::string> run(Session *sess) final {
        const std::string& key = sess->getArgs()[1];
        std::vector<byte> buf;

        auto server = sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbWithKeyLock(
                sess, key, Command::RdLock());
        auto exps = getSerializer(sess, key);
        if (!exps.ok()) {
            if (exps.status().code() == ErrorCodes::ERR_EXPIRED ||
                exps.status().code() == ErrorCodes::ERR_NOTFOUND) {
                return Command::fmtNull();
            }
            return exps.status();
        }

        auto expBuf = exps.value()->dump();
        if (!expBuf.ok()) {
            return expBuf.status();
        }
        buf = std::move(expBuf.value());

        // expect our g++ compiler will do cow in this ctor
        std::string output(buf.begin() + exps.value()->_begin,
                           buf.begin() + exps.value()->_end);
        return Command::fmtBulk(output);
    }
} dumpCommand;

class DumpXCommand: public Command {
 public:
    DumpXCommand()
        :Command("dumpx", "r") {
    }

    ssize_t arity() const {
        return -3;
    }

    int32_t firstkey() const {
        return 2;
    }
    int32_t lastkey() const {
        return -1;
    }
    int32_t keystep() const {
        return 2;
    }

    Expected<std::string> run(Session *sess) final {
        const auto& args = sess->getArgs();
        auto server = sess->getServerEntry();
        std::vector<int> index((args.size() - 1)/2);
        std::generate(index.begin(), index.end(), [n=0]() mutable { return n+=2; });
        auto locklist = server->getSegmentMgr()->getAllKeysLocked(
                sess, args, index, Command::RdLock());
        if (!locklist.ok()) {
            return locklist.status();
        }
        std::stringstream ss;
        std::vector<std::unique_ptr<std::string>> bufferlist;
        INVARIANT(!((args.size() - 1) % 2));
        bufferlist.reserve(3 * (args.size() - 1) / 2);
        size_t cnt(0);
        for (const auto& i : index) {
            auto expDbid = tendisplus::stoul(args[i-1]);
            if (!expDbid.ok()) {
                return expDbid.status();
            }
            auto dbid = static_cast<uint32_t>(expDbid.value());
            if (sess->getCtx()->getDbId() != dbid) {
                sess->getCtx()->setDbId(dbid);
            }
            auto expdb = server->getSegmentMgr()->getDbHasLocked(sess, args[i]);
            if (!expdb.ok()) {
                return expdb.status();
            }
            auto exps = getSerializer(sess, args[i]);
            if (!exps.ok()) {
                if (exps.status().code() != ErrorCodes::ERR_EXPIRED ||
                    exps.status().code() != ErrorCodes::ERR_NOTFOUND) {
                    return exps.status();
                }
            }

            auto expBuf = exps.value()->dump(true);
            if (!expBuf.ok()) {
                return expBuf.status();
            }
            bufferlist.emplace_back(std::make_unique<std::string>(args[i-1]));
            bufferlist.emplace_back(std::make_unique<std::string>(
                    args[i]));
            bufferlist.emplace_back(std::make_unique<std::string>(
                    expBuf.value().begin() + exps.value()->_begin,
                    expBuf.value().begin() + exps.value()->_end));
            cnt++;
        }
        Command::fmtMultiBulkLen(ss, 3 * cnt + 1);
        Command::fmtBulk(ss, "RESTOREX");
        INVARIANT(bufferlist.size() == 3 * cnt);
        for (size_t i = 0; i < 3 * cnt; i++) {
            Command::fmtBulk(ss, *bufferlist[i]);
        }
        return ss.str();
    }
} dumpxCommand;

// derived classes, each of them should handle the dump of different object.
class KvSerializer: public Serializer {
 public:
    explicit KvSerializer(Session *sess,
                          const std::string& key,
                          RecordValue&& rv)
        :Serializer(sess, key,
                DumpType::RDB_TYPE_STRING,
                std::forward<RecordValue>(rv)) {
    }

    Expected<size_t> dumpObject(std::vector<byte>& payload) {
        Serializer::saveString(&payload, &_pos, _rv.getValue());
        _begin = 0;
        return _pos - _begin;
    }
};

class ListSerializer: public Serializer {
 private:
    Expected<uint32_t> formatZiplist(std::vector<byte>& payload,
            size_t& pos,
            std::vector<std::string>& zl,
            uint32_t byteSz) {
        std::vector<byte> ziplist;
        ziplist.reserve(byteSz);
        size_t tmpPos(0);
        size_t zlInitPos(0);
        tmpPos += 8;
        uint32_t zlbytes(10);
        uint64_t prevlen(0);
        uint16_t zllen = zl.size();
        easyCopy(&ziplist, &tmpPos, zllen);
        for (size_t i = 0; i < zl.size(); i++) {
            size_t written(0);
            if (prevlen > 254) {
                written += easyCopy(&ziplist, &tmpPos,
                        static_cast<unsigned char>(0xfe));
                written += easyCopy(&ziplist, &tmpPos, prevlen);
            } else {
                written += easyCopy(&ziplist, &tmpPos,
                        static_cast<unsigned char>(prevlen));
            }
            written += saveString(&ziplist, &tmpPos, zl[i]);

            prevlen = written;
            zlbytes += written;
        }
        zlbytes += easyCopy(&ziplist, &tmpPos,
                static_cast<unsigned char>(0xff));
        uint32_t zltail(zlbytes - 1 - prevlen);
        easyCopy(&ziplist, &zlInitPos, zlbytes);
        easyCopy(&ziplist, &zlInitPos, zltail);

        size_t written(0);
        auto wr = Serializer::saveLen(&payload, &pos, ziplist.size());
        INVARIANT(wr.value() > 0);
        written += wr.value();
        written += easyCopy(&payload, &pos, ziplist.data(), ziplist.size());
        return written;
    }

 public:
    explicit ListSerializer(Session *sess,
                            const std::string& key,
                            RecordValue&& rv)
        :Serializer(sess, key,
                DumpType::RDB_TYPE_QUICKLIST,
                std::forward<RecordValue>(rv)) {
    }

    Expected<size_t> dumpObject(std::vector<byte>& payload) {
        size_t qlbytes(0);
        size_t notAligned = _pos;
        size_t qlEnd = notAligned + 9;

        {
            // make room for quicklist length first,
            // remember to move first several bytes to align after.
            payload.resize(payload.size() + 9);
            _pos += 9;
        }

        auto expListMeta = ListMetaValue::decode(_rv.getValue());
        if (!expListMeta.ok()) {
            return expListMeta.status();
        }
        uint64_t tail = expListMeta.value().getTail();
        uint64_t head = expListMeta.value().getHead();
        uint64_t len = tail - head;
        INVARIANT(len > 0);

        auto server = _sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbHasLocked(_sess, _key);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;

        auto ptxn = kvstore->createTransaction(_sess);
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());

        /* in this loop we should emulate to build a quicklist(or to say, many ziplists)
         * then compress it using lzf(not implemented), or just write raw to buffer, both can work.*/
        if (len > UINT16_MAX) {
            return { ErrorCodes::ERR_INTERNAL, "Currently not support" };
        }

        uint32_t byteSz(0);
        std::vector<std::string> ziplist;
        size_t zlCnt(0);
        for (size_t i = head; i != tail; i++) {
            RecordKey nodeKey(expdb.value().chunkId, _sess->getCtx()->getDbId(),
                    RecordType::RT_LIST_ELE, _key, std::to_string(i));
            auto expNodeVal = kvstore->getKV(nodeKey, txn.get());
            if (!expNodeVal.ok()) {
                return expNodeVal.status();
            }
            byteSz += expNodeVal.value().getValue().size();
            ziplist.emplace_back(std::move(expNodeVal.value().getValue()));
            if (byteSz > ZLBYTE_LIMIT || i == tail - 1) {
                ++zlCnt;
                auto ezlBytes = formatZiplist(payload, _pos,
                        ziplist, byteSz);
                if (!ezlBytes.ok()) {
                    return ezlBytes.status();
                }
                qlbytes += ezlBytes.value();
                ziplist.clear();
                byteSz = 0;
            }
        }

       auto expQlUsed = saveLen(&payload, &notAligned, zlCnt);
        if (!expQlUsed.ok()) {
            return expQlUsed.status();
        }
        if (expQlUsed.value() < 9) {
            std::copy_backward(payload.begin(),
                    payload.begin() + notAligned,
                    payload.begin() + qlEnd);
        }
        _begin = 9 - expQlUsed.value();
        _end = payload.size() - _begin;
        return qlbytes + expQlUsed.value();
    }
};

class SetSerializer: public Serializer {
 public:
    explicit SetSerializer(Session *sess,
                           const std::string &key,
                           RecordValue&& rv)
        :Serializer(sess, key,
                DumpType::RDB_TYPE_SET,
                std::forward<RecordValue>(rv)) {
    }

    Expected<size_t> dumpObject(std::vector<byte>& payload) {
        Expected<SetMetaValue> expMeta = SetMetaValue::decode(_rv.getValue());
        size_t len = expMeta.value().getCount();
        INVARIANT(len > 0);

        auto expwr = saveLen(&payload, &_pos, len);
        if (!expwr.ok()) {
            return expwr.status();
        }

        auto server = _sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbHasLocked(_sess, _key);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;
        auto ptxn = kvstore->createTransaction(_sess);
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());

        auto cursor = txn->createCursor();
        RecordKey fakeRk(expdb.value().chunkId,
                _sess->getCtx()->getDbId(),
                RecordType::RT_SET_ELE,
                _key, "");
        cursor->seek(fakeRk.prefixPk());
        while (true) {
            Expected<Record> eRcd = cursor->next();
            if (eRcd.status().code() == ErrorCodes::ERR_EXHAUST) {
                break;
            }
            if (!eRcd.ok()) {
                return eRcd.status();
            }
            Record &rcd = eRcd.value();
            const RecordKey &rcdKey = rcd.getRecordKey();
            if (rcdKey.prefixPk() != fakeRk.prefixPk()) {
                break;
            }

            const std::string &subk = rcdKey.getSecondaryKey();
            Serializer::saveString(&payload, &_pos, subk);
        }

        _begin = 0;
        return _pos - _begin;
    }
};

class ZsetSerializer: public Serializer {
 public:
    explicit ZsetSerializer(Session *sess,
                            const std::string& key,
                            RecordValue&& rv)
        :Serializer(sess, key,
                DumpType::RDB_TYPE_ZSET,
                std::forward<RecordValue>(rv)) {
    }

    Expected<size_t> dumpObject(std::vector<byte>& payload) {
        auto server = _sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbHasLocked(_sess, _key);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;
        auto ptxn = kvstore->createTransaction(_sess);
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());

        auto eMeta = ZSlMetaValue::decode(_rv.getValue());
        if (!eMeta.ok()) {
            return eMeta.status();
        }
        ZSlMetaValue meta = eMeta.value();
        SkipList zsl(expdb.value().chunkId,
                _sess->getCtx()->getDbId(),
                _key, meta, kvstore);

        auto expwr = saveLen(&payload, &_pos, zsl.getCount() - 1);
        if (!expwr.ok()) {
            return expwr.status();
        }

        auto rev = zsl.scanByRank(0, zsl.getCount() - 1, true, txn.get());
        if (!rev.ok()) {
            return rev.status();
        }
        for (auto& ele : rev.value()) {
            Serializer::saveString(
                    &payload, &_pos, std::forward<std::string>(ele.second));
            // save binary double score
            double score = ele.first;
            easyCopy(&payload, &_pos, score);
        }
        _begin = 0;
        return _pos - _begin;
    }
};

class HashSerializer: public Serializer {
 public:
    explicit HashSerializer(Session *sess,
                            const std::string &key,
                            RecordValue&& rv)
        :Serializer(sess, key,
                DumpType::RDB_TYPE_HASH,
                std::forward<RecordValue>(rv)) {
    }

    Expected<size_t> dumpObject(std::vector<byte> &payload) {
        Expected<HashMetaValue> expHashMeta =
                HashMetaValue::decode(_rv.getValue());
        if (!expHashMeta.ok()) {
            return expHashMeta.status();
        }
        auto expwr = saveLen(&payload, &_pos, expHashMeta.value().getCount());
        if (!expwr.ok()) {
            return expwr.status();
        }

        auto server = _sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbHasLocked(_sess, _key);
        if (!expdb.ok()) {
            return expdb.status();
        }

        PStore kvstore = expdb.value().store;
        auto ptxn = kvstore->createTransaction(_sess);
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());

        RecordKey fakeRk(
                expdb.value().chunkId,
                _sess->getCtx()->getDbId(),
                RecordType::RT_HASH_ELE, _key, "");
        auto cursor = txn->createCursor();
        cursor->seek(fakeRk.prefixPk());
        while (true) {
            Expected<Record> expRcd = cursor->next();
            if (expRcd.status().code() == ErrorCodes::ERR_EXHAUST) {
                break;
            }
            if (!expRcd.ok()) {
                return expRcd.status();
            }
            if (expRcd.value().getRecordKey().prefixPk() != fakeRk.prefixPk()) {
                break;
            }
            const std::string &field =
                    expRcd.value().getRecordKey().getSecondaryKey();
            const std::string &value =
                    expRcd.value().getRecordValue().getValue();
            Serializer::saveString(&payload, &_pos, field);
            Serializer::saveString(&payload, &_pos, value);
        }
        _begin = 0;
        return _pos - _begin;
    }
};

// outlier function
Expected<std::unique_ptr<Serializer>> getSerializer(Session *sess,
        const std::string &key) {
    Expected<RecordValue> rv =
            Command::expireKeyIfNeeded(sess, key, RecordType::RT_DATA_META);
    if (!rv.ok()) {
        return rv.status();
    }

    std::unique_ptr<Serializer> ptr;
    auto type = rv.value().getRecordType();
    switch (type) {
        case RecordType::RT_KV:
            ptr = std::move(std::unique_ptr<Serializer>(
                    new KvSerializer(sess, key, std::move(rv.value()))));
            break;
        case RecordType::RT_LIST_META:
            ptr = std::move(std::unique_ptr<Serializer>(
                    new ListSerializer(sess, key, std::move(rv.value()))));
            break;
        case RecordType::RT_HASH_META:
            ptr = std::move(std::unique_ptr<Serializer>(
                    new HashSerializer(sess, key, std::move(rv.value()))));
            break;
        case RecordType::RT_SET_META:
            ptr = std::move(std::unique_ptr<Serializer>(
                    new SetSerializer(sess, key, std::move(rv.value()))));
            break;
        case RecordType::RT_ZSET_META:
            ptr = std::move(std::unique_ptr<Serializer>(
                    new ZsetSerializer(sess, key, std::move(rv.value()))));
            break;
        default:
            return {ErrorCodes::ERR_WRONG_TYPE, "type can not be dumped"};
    }

    return std::move(ptr);
}

// restore
Deserializer::Deserializer(
        Session *sess,
        const std::string &payload,
        const std::string &key,
        const uint64_t ttl)
    : _sess(sess), _payload(payload), _key(key), _ttl(ttl), _pos(1) {}

Expected<DumpType> Deserializer::loadObjectType(
        const std::string &payload,
        size_t &&pos) {
    uint8_t t;
    easyCopy(&t, payload, &pos);
    return static_cast<DumpType>(t);
}

Expected<size_t> Deserializer::loadLen(
        const std::string &payload,
        size_t *pos,
        bool *isencoded) {
    byte buf[2];
    size_t ret;
    INVARIANT(easyCopy(&buf[0], payload, pos) == 1);
    uint8_t encType = static_cast<uint8_t>((buf[0]&0xC0) >> 6);
    if (isencoded) {
        *isencoded = false;
    }
    /* Support ENCODING_INT and lzf(compressed) */
    if (encType == RDB_ENCVAL) {
        ret = buf[0] & 0x3F;
        if (isencoded) {
            *isencoded = true;
        }
        LOG(INFO) << "return encType " << (int)ret;
    } else if (encType == RDB_6BITLEN) {
        ret = buf[0] & 0x3F;
    } else if (encType == RDB_14BITLEN) {
        INVARIANT(easyCopy(&buf[1], payload, pos) == 1);
        ret = ((buf[0]&0x3F) << 8) | buf[1];
    } else if (buf[0] == RDB_32BITLEN) {
        uint32_t len32;
        INVARIANT(easyCopy(&len32, payload, pos) == 4);
        ret = ntohl(len32);
    } else if (buf[0] == RDB_64BITLEN) {
        uint64_t len64;
        INVARIANT(easyCopy(&len64, payload, pos) == 8);
        ret = redis_port::ntohll(len64);
    } else {
        return {ErrorCodes::ERR_INTERNAL, "Unknown length encoding"};
    }
    return ret;
}

std::string Deserializer::loadString(const std::string &payload, size_t *pos) {
    bool isencoded(false);
    auto expLen = Deserializer::loadLen(payload, pos, &isencoded);
    if (!expLen.ok()) {
        return std::string("");
    }
    size_t len = expLen.value();
    if (isencoded) {
        LOG(INFO) << "is encoded";
        switch (static_cast<uint8_t>(len)) {
            case RDB_ENC_INT8:
            case RDB_ENC_INT16:
            case RDB_ENC_INT32: {
                /* transfer integer to string */
                auto eVal = loadIntegerString(payload, pos, len);
                if (!eVal.ok()) {
                    return std::string();
                }
                return std::to_string(eVal.value());
                break;
            }
            case RDB_ENC_LZF: {
                LOG(INFO) << "is encoded LZF";
                auto expLzf = loadLzfString(payload, pos);
                if (!expLzf.ok()) {
                    return std::string();
                }
                return expLzf.value();
                break;
            }
            default:
                LOG(INFO) << "Unknown encoding " << (int)len;
        }
    }

    *pos += len;
    if (payload.begin() + *pos > payload.end()) {
        LOG(INFO) << "pos over limit";
        return std::string("");
    }
    return std::string(payload.begin() + *pos - len, payload.begin() + *pos);
}

Expected<int64_t> Deserializer::loadIntegerString(
        const std::string &payload,
        size_t *pos,
        uint8_t encType) {
    int64_t val;

    switch (encType) {
        case RDB_ENC_INT8: {
            int8_t enc;
            easyCopy(&enc, payload, pos);
            val = static_cast<int64_t>(enc);
            break;
        }
        case RDB_ENC_INT16: {
            int16_t enc;
            easyCopy(&enc, payload, pos);
            val = static_cast<int64_t>(enc);
            break;
        }
        case RDB_ENC_INT32: {
            int32_t enc;
            easyCopy(&enc, payload, pos);
            val = static_cast<int64_t>(enc);
            break;
        }
        default:
            LOG(ERROR) << "Unknown ENCODING_INT type " << encType;
            return { ErrorCodes::ERR_PARSEPKT,
                     "Unknown RDB integer encoding type" };
    }
    return val;
}

Expected<std::string> Deserializer::loadLzfString(
        const std::string &payload,
        size_t *pos) {
    auto expClen = loadLen(payload, pos);
    if (!expClen.ok()) {
        return expClen.status();
    }
    LOG(INFO) << "load clen " << expClen.value();
    auto expLen = loadLen(payload, pos);
    if (!expLen.ok()) {
        return expLen.status();
    }
    LOG(INFO) << "load len " << expLen.value();
    const auto lzfEnd = payload.begin() + *pos + expClen.value();
    if (lzfEnd > payload.cend()) {
        return {ErrorCodes::ERR_PARSEOPT, "Wrong lzf buffer length"};
    }
    std::string lzfBuf(payload.begin() + *pos, lzfEnd);
    *pos += expClen.value();
    std::vector<char>outBuf(expLen.value(), '\0');

    if (redis_port::lzf_decompress(lzfBuf.c_str(), lzfBuf.size(),
            outBuf.data(), outBuf.size()) == 0) {
        LOG(INFO) << "Invalid LZF";
        return {ErrorCodes::ERR_PARSEPKT, "Invalid LZF compressed string"};
    }

    return std::string(outBuf.begin(), outBuf.end());
}

class RestoreCommand: public Command {
 public:
    RestoreCommand()
        :Command("restore", "wm") {
    }

    ssize_t arity() const {
        return -4;
    }

    int32_t firstkey() const {
        return 1;
    }

    int32_t lastkey() const {
        return 1;
    }

    int32_t keystep() const {
        return 1;
    }

    static Status verifyDumpPayload(const std::string &payload) {
        uint8_t buf[2];
        uint64_t crc;

        size_t len = payload.size();
        if (len < 10) {
            return { ErrorCodes::ERR_INTERNAL, "len cannot be lt 10" };
        }
        size_t chkpos = len - 10;
        INVARIANT(easyCopy(&buf[0], payload, &chkpos) == 1 &&
                  easyCopy(&buf[1], payload, &chkpos) == 1);

        uint16_t rdbver = (buf[1] << 8) | buf[0];
        if (rdbver > RDB_VERSION) {
            return { ErrorCodes::ERR_INTERNAL, "rdb version not match" };
        }

        crc = redis_port::crc64(0,
                reinterpret_cast<const byte*>(payload.c_str()),
                chkpos);
        if (memcmp(&crc, payload.c_str() + chkpos, 8) == 0) {
            return { ErrorCodes::ERR_OK, "OK" };
        }
        return { ErrorCodes::ERR_INTERNAL, "crc not match" };
    }

    Expected<std::string> run(Session *sess) final {
        const auto& args = sess->getArgs();
        const std::string &key = args[1];
        const std::string &sttl = args[2];
        const std::string &payload = args[3];
        bool replace(false);

        {
            for (size_t i = 4; i < args.size(); i++) {
                if (!::strcasecmp(args[i].c_str(), "replace")) {
                    replace = true;
                } else {
                    return {ErrorCodes::ERR_PARSEOPT, ""};
                }
            }
        }

        auto lock = sess->getServerEntry()->getSegmentMgr()->getAllKeysLocked(sess,
                args, {1}, mgl::LockMode::LOCK_X);
        // check if key exists
        Expected<RecordValue> rv =
                Command::expireKeyIfNeeded(sess, key, RecordType::RT_DATA_META);
        if (rv.status().code() != ErrorCodes::ERR_EXPIRED &&
            rv.status().code() != ErrorCodes::ERR_NOTFOUND) {
            if (!rv.ok()) {
                return rv.status();
            }
            if (replace) {
                Status s = delKey(sess, key, RecordType::RT_DATA_META);
                if (!s.ok()) {
                    return s;
                }
            } else {
                return Command::fmtBusyKey();
            }
        }

        Expected<int64_t> expttl = tendisplus::stoll(sttl);
        if (!expttl.ok()) {
            return expttl.status();
        }
        if (expttl.value() < 0) {
            return {ErrorCodes::ERR_PARSEPKT,
                    "Invalid TTL value, must be >= 0"};
        }
        uint64_t ts = 0;
        if (expttl.value() != 0)
            ts = msSinceEpoch() + expttl.value();

        Status chk = RestoreCommand::verifyDumpPayload(payload);
        if (!chk.ok()) {
            return {ErrorCodes::ERR_PARSEPKT,
                    "DUMP payload version or checksum are wrong"};
        }

        // do restore
        auto expds = getDeserializer(sess, payload, key, ts);
        if (!expds.ok()) {
            return expds.status();
        }

        Status res = expds.value()->restore();
        if (res.ok()) {
            return Command::fmtOK();
        }
        return res;
    }
} restoreCommand;

class RestoreXCommand: public Command {
 public:
    RestoreXCommand()
        :Command("restorex", "wm") {
    }

    ssize_t arity() const {
        return -4;
    }

    int32_t firstkey() const {
        return 2;
    }

    int32_t lastkey() const {
        return -1;
    }

    int32_t keystep() const {
        return 3;
    }

    Expected<std::string> run(Session *sess) final {
        return Command::fmtOK();
    }
} restorexCmd;

class KvDeserializer: public Deserializer {
 public:
    explicit KvDeserializer(
            Session *sess,
            const std::string &payload,
            const std::string &key,
            const uint64_t ttl )
        : Deserializer(sess, payload, key, ttl) {
    }

    virtual Status restore() {
        auto ret = Deserializer::loadString(_payload, &_pos);
        auto server = _sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbHasLocked(_sess, _key);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;
        auto ptxn = kvstore->createTransaction(_sess);
        if (!ptxn.ok()) {
            return ptxn.status();
        }

        std::unique_ptr<Transaction> txn = std::move(ptxn.value());

        SessionCtx *pCtx = _sess->getCtx();
        INVARIANT(pCtx != nullptr);

        RecordKey rk(expdb.value().chunkId,
                pCtx->getDbId(),
                RecordType::RT_KV, _key, "");
        RecordValue rv(ret, RecordType::RT_KV, pCtx->getVersionEP(), _ttl);
        for (int32_t i = 0; i < Command::RETRY_CNT; ++i) {
            Status s = kvstore->setKV(rk, rv, txn.get());
            if (!s.ok()) {
                return s;
            }
            Expected<uint64_t> expCmt = txn->commit();
            if (expCmt.ok()) {
                return { ErrorCodes::ERR_OK, "OK"};
            } else if (expCmt.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return expCmt.status();
            }

            if (i == Command::RETRY_CNT - 1) {
                return expCmt.status();
            }

            ptxn = kvstore->createTransaction(_sess);
            if (!ptxn.ok()) {
                return ptxn.status();
            }
            txn = std::move(ptxn.value());
        }

        return { ErrorCodes::ERR_INTERNAL, "not reachable"};
    }
};

class SetDeserializer: public Deserializer {
 public:
    explicit SetDeserializer(
            Session *sess,
            const std::string &payload,
            const std::string &key,
            const uint64_t ttl)
        : Deserializer(sess, payload, key, ttl) {
    }

    virtual Status restore() {
        auto expLen = Deserializer::loadLen(_payload, &_pos);
        if (!expLen.ok()) {
            return expLen.status();
        }

        size_t len = expLen.value();
        // std::vector<std::string> set(2);

        auto server = _sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbHasLocked(_sess, _key);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;
        auto ptxn = kvstore->createTransaction(_sess);
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());

        RecordKey metaRk(expdb.value().chunkId,
                         _sess->getCtx()->getDbId(),
                         RecordType::RT_SET_META, _key, "");
        SetMetaValue sm;

        for (size_t i = 0; i < len; i++) {
            std::string ele = loadString(_payload, &_pos);
            RecordKey rk(metaRk.getChunkId(),
                    metaRk.getDbId(),
                    RecordType::RT_SET_ELE,
                    metaRk.getPrimaryKey(), std::move(ele));
            RecordValue rv("", RecordType::RT_SET_ELE, -1);
            Status s = kvstore->setKV(rk, rv, txn.get());
            if (!s.ok()) {
                return s;
            }
        }
        sm.setCount(len);
        Status s = kvstore->setKV(metaRk,
                RecordValue(sm.encode(), RecordType::RT_SET_META,
                        _sess->getCtx()->getVersionEP(), _ttl),
                txn.get());
        if (!s.ok()) {
            return s;
        }
        Expected<uint64_t> expCmt = txn->commit();
        if (!expCmt.ok()) {
            return expCmt.status();
        }
        return { ErrorCodes::ERR_OK, "OK" };
    }
};

class ZsetDeserializer: public Deserializer {
 public:
    explicit ZsetDeserializer(
            Session *sess,
            const std::string &payload,
            const std::string &key,
            const uint64_t ttl)
        : Deserializer(sess, payload, key, ttl) {
    }

    virtual Status restore() {
        auto expLen = loadLen(_payload, &_pos);
        if (!expLen.ok()) {
            return expLen.status();
        }

        size_t len = expLen.value();
        std::map<std::string, double> scoreMap;
        while (len--) {
            std::string ele = loadString(_payload, &_pos);
            double score;
            INVARIANT(easyCopy(&score, _payload, &_pos) == 8);
            scoreMap[ele] = score;
        }
        auto server = _sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbHasLocked(_sess, _key);
        if (!expdb.ok()) {
            return expdb.status();
        }

        RecordKey rk(expdb.value().chunkId,
                _sess->getCtx()->getDbId(),
                RecordType::RT_ZSET_META, _key, "");
        PStore kvstore = expdb.value().store;
        // set ttl first
        auto ptxn = kvstore->createTransaction(_sess);
        if (!ptxn.ok()) {
            return ptxn.status();
        }

        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
        Expected<RecordValue> eMeta =
                kvstore->getKV(rk, txn.get());
        if (!eMeta.ok() && eMeta.status().code() != ErrorCodes::ERR_NOTFOUND) {
            return eMeta.status();
        }
        INVARIANT(eMeta.status().code() == ErrorCodes::ERR_NOTFOUND);
        ZSlMetaValue meta(1, 1, 0);
        RecordValue rv(meta.encode(), RecordType::RT_ZSET_META,
                _sess->getCtx()->getVersionEP(), _ttl);
        Status s = kvstore->setKV(rk, rv, txn.get());
        if (!s.ok()) {
            return s;
        }
        RecordKey headRk(rk.getChunkId(),
                rk.getDbId(),
                RecordType::RT_ZSET_S_ELE,
                rk.getPrimaryKey(),
                std::to_string(ZSlMetaValue::HEAD_ID));
        ZSlEleValue headVal;
        RecordValue headRv(headVal.encode(), RecordType::RT_ZSET_S_ELE, -1);
        s = kvstore->setKV(headRk, headRv, txn.get());
        if (!s.ok()) {
            return s;
        }
        Expected<uint64_t> expCmt = txn->commit();
        if (!expCmt.ok()) {
            return expCmt.status();
        }

        for (int32_t i = 0; i < Command::RETRY_CNT; ++i) {
            // maybe very slow
            Expected<std::string> res =
                    genericZadd(_sess, kvstore, rk, rv, scoreMap, ZADD_NX);
            if (res.ok()) {
                return { ErrorCodes::ERR_OK, "OK" };
            }
            if (res.status().code() != ErrorCodes::ERR_COMMIT_RETRY) {
                return res.status();
            }
            if (i == Command::RETRY_CNT - 1) {
                return res.status();
            } else {
                continue;
            }
        }

        return { ErrorCodes::ERR_INTERNAL, "not reachable" };
    }
};

class HashDeserializer: public Deserializer {
 public:
    explicit HashDeserializer(
            Session *sess,
            const std::string &payload,
            const std::string &key,
            const uint64_t ttl)
        : Deserializer(sess, payload, key, ttl) {
    }

    virtual Status restore() {
        auto expLen = loadLen(_payload, &_pos);
        if (!expLen.ok()) {
            return expLen.status();
        }

        size_t len = expLen.value();
        auto server = _sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbHasLocked(_sess, _key);
        if (!expdb.ok()) {
            return expdb.status();
        }
        PStore kvstore = expdb.value().store;
        auto ptxn = kvstore->createTransaction(_sess);
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
        for (size_t i = 0; i < len; i++) {
            std::string field = loadString(_payload, &_pos);
            std::string value = loadString(_payload, &_pos);
            // need existence check ?
            RecordKey rk(expdb.value().chunkId, _sess->getCtx()->getDbId(),
                    RecordType::RT_HASH_ELE, _key, field);
            RecordValue rv(value, RecordType::RT_HASH_ELE, -1);
            Status s = kvstore->setKV(rk, rv, txn.get());
            if (!s.ok()) {
                return s;
            }
        }

        RecordKey metaRk(expdb.value().chunkId, _sess->getCtx()->getDbId(),
                RecordType::RT_HASH_META, _key, "");
        HashMetaValue hashMeta;
        hashMeta.setCount(len);
        RecordValue metaRv(std::move(hashMeta.encode()), RecordType::RT_HASH_META,
                    _sess->getCtx()->getVersionEP(), _ttl);
        Status s = kvstore->setKV(metaRk, metaRv, txn.get());
        if (!s.ok()) {
            return s;
        }
        Expected<uint64_t> expCmt = txn->commit();
        if (!expCmt.ok()) {
            return expCmt.status();
        }
        return { ErrorCodes::ERR_OK, "OK" };
    }
};

class ListDeserializer: public Deserializer {
    Expected<std::vector<std::string>> deserializeZiplist(
            const std::string &payload, size_t *pos) {
        uint32_t zlbytes(0), zltail(0);
        uint16_t zllen(0);
        std::vector<std::string> zl;
        INVARIANT(easyCopy(&zlbytes, payload, pos) == 4 &&
            easyCopy(&zltail, payload, pos) == 4 &&
            easyCopy(&zllen, payload, pos) == 2);

        zl.reserve(zllen);
        uint32_t prevlen(0);
        while (zllen--) {
            if (prevlen > 254) {
                *pos += 5;
            } else {
                *pos += 1;
            }
            std::string val;
            const unsigned char& encoding = payload.at(*(pos));
            if (static_cast<uint8_t>(encoding) < ZIP_STR_MASK) {
                val = loadString(payload, pos);
            } else {
                (*pos)++;
                switch (encoding) {
                    case ZIP_INT_8B: {
                        int8_t intEntry;
                        easyCopy(&intEntry, payload, pos);
                        val = std::to_string(intEntry);
                        break;
                    }
                    case ZIP_INT_16B: {
                        int16_t intEntry;
                        easyCopy(&intEntry, payload, pos);
                        val = std::to_string(intEntry);
                        break;
                    }
                    case ZIP_INT_24B: {
                        uint8_t data[3];
                        int32_t intEntry;
                        for (int i = 0; i < 3; i++) {
                            easyCopy(&data[i], payload, pos);
                        }
                        // Read sequence: low addr -> high addr, so data[2] gets left shift
                        intEntry = (data[2] << 16) | (data[1] << 8) | (data[0]);
                        val = std::to_string(intEntry);
                        break;
                    }
                    case ZIP_INT_32B: {
                        int32_t intEntry;
                        easyCopy(&intEntry, payload, pos);
                        val = std::to_string(intEntry);
                        break;
                    }
                    case ZIP_INT_64B: {
                        int64_t intEntry;
                        easyCopy(&intEntry, payload, pos);
                        val = std::to_string(intEntry);
                    }
                    default: {
                        if (encoding >= ZIP_INT_IMM_MIN &&
                            encoding <= ZIP_INT_IMM_MAX) {
                            uint8_t intEntry;
                            intEntry = (encoding & ZIP_INT_IMM_MASK) - 1;
                            val = std::to_string(intEntry);
                        } else {
                            LOG(INFO) << "Invalid integer encoding " << static_cast<uint8_t>(encoding);
                            return {ErrorCodes::ERR_PARSEPKT, "Invalid integer encoding"};
                        }
                        break;
                    }
                }
            }
            prevlen = val.size();
            zl.push_back(std::move(val));
        }
        uint8_t zlend(0);
        INVARIANT(easyCopy(&zlend, payload, pos) == 1);
        INVARIANT(zlend == 0xff);
        return zl;
    }

 public:
    explicit ListDeserializer(
            Session *sess,
            const std::string &payload,
            const std::string &key,
            const uint64_t ttl)
        : Deserializer(sess, payload, key, ttl) {
    }

    virtual Status restore() {
        auto qlExpLen = loadLen(_payload, &_pos);
        if (!qlExpLen.ok()) {
            return qlExpLen.status();
        }

        size_t qlLen = qlExpLen.value();
        auto server = _sess->getServerEntry();
        auto expdb = server->getSegmentMgr()->getDbHasLocked(_sess, _key);
        if (!expdb.ok()) {
            return expdb.status();
        }
        RecordKey metaRk(expdb.value().chunkId, _sess->getCtx()->getDbId(),
                RecordType::RT_LIST_META, _key, "");
        PStore kvstore = expdb.value().store;
        auto ptxn = kvstore->createTransaction(_sess);
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        std::unique_ptr<Transaction> txn = std::move(ptxn.value());
        ListMetaValue lm(INITSEQ, INITSEQ);

        uint64_t head = lm.getHead();
        uint64_t tail = lm.getTail();
        while (qlLen--) {
            auto zlist = loadString(_payload, &_pos);
            size_t pos = 0;
            auto expZl = deserializeZiplist(zlist, &pos);
            if (!expZl.ok()) {
                LOG(ERROR) << "Restore list failed, " << expZl.status().toString();
                return expZl.status();
            }
            const auto& zl = expZl.value();
            uint64_t idx;
            for (auto iter = zl.begin(); iter != zl.end(); iter++) {
                idx = tail++;
                RecordKey rk(metaRk.getChunkId(),
                             metaRk.getDbId(),
                             RecordType::RT_LIST_ELE,
                             metaRk.getPrimaryKey(),
                             std::to_string(idx));
                RecordValue rv(std::move(*iter), RecordType::RT_LIST_ELE, -1);
                Status s = kvstore->setKV(rk, rv, txn.get());
                if (!s.ok()) {
                    return s;
                }
            }
        }
        lm.setHead(head);
        lm.setTail(tail);
        RecordValue metaRv(lm.encode(), RecordType::RT_LIST_META,
                _sess->getCtx()->getVersionEP(), _ttl);
        Status s = kvstore->setKV(metaRk, metaRv, txn.get());
        if (!s.ok()) {
            return s;
        }
        Expected<uint64_t> expCmt = txn->commit();
        if (!expCmt.ok()) {
            return expCmt.status();
        }
        return { ErrorCodes::ERR_OK, "OK" };
    }
};

Expected<std::unique_ptr<Deserializer>> getDeserializer(
        Session *sess,
        const std::string &payload,
        const std::string &key, const uint64_t ttl) {
    Expected<DumpType> expType = Deserializer::loadObjectType(payload, 0);
    if (!expType.ok()) {
        return expType.status();
    }
    std::unique_ptr<Deserializer> ptr;
    DumpType type = std::move(expType.value());
    switch (type) {
        case DumpType::RDB_TYPE_STRING:
            ptr = std::move(std::unique_ptr<Deserializer>(
                    new KvDeserializer(sess, payload, key, ttl)));
            break;
        case DumpType::RDB_TYPE_SET:
            ptr = std::move(std::unique_ptr<Deserializer>(
                    new SetDeserializer(sess, payload, key, ttl)));
            break;
        case DumpType::RDB_TYPE_ZSET:
            ptr = std::move(std::unique_ptr<Deserializer>(
                    new ZsetDeserializer(sess, payload, key, ttl)));
            break;
        case DumpType::RDB_TYPE_HASH:
            ptr = std::move(std::unique_ptr<Deserializer>(
                    new HashDeserializer(sess, payload, key, ttl)));
            break;
        case DumpType::RDB_TYPE_QUICKLIST:
            ptr = std::move(std::unique_ptr<Deserializer>(
                    new ListDeserializer(sess, payload, key, ttl)));
            break;
        default:
            return {ErrorCodes::ERR_INTERNAL, "Not implemented"};
    }
    return std::move(ptr);
}

}  // namespace tendisplus
