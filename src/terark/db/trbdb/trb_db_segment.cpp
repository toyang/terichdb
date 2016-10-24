#include "trb_db_segment.hpp"
#include "trb_db_index.hpp"
#include "trb_db_store.hpp"
#include "trb_db_context.hpp"
#include <terark/db/fixed_len_store.hpp>
#include <terark/num_to_str.hpp>
#include <terark/util/crc.hpp>
#include <boost/scope_exit.hpp>
#include <terark/io/FileStream.hpp>
#include <terark/io/StreamBuffer.hpp>
#include <terark/io/DataOutput.hpp>
#include <terark/io/DataIO.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/optional.hpp>
#include <thread>

#undef min
#undef max


namespace terark { namespace db { namespace trbdb {

TERARK_DB_REGISTER_SEGMENT(TrbColgroupSegment, "trbdb", "trb");

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

enum class LogAction : unsigned char
{
    Store,      //Id, Data
    Emplace,    //Id, Remove, Data
    Modify,     //[Id, Remove]
};

struct CrcUpdate
{
    template<class T>
    static uint32_t update(uint32_t c, ...)
    {
        static_assert(sizeof(T) != 0, "wtf ?");
        return c;
    }
    template<class T>
    static uint32_t update(uint32_t c, valvec<byte> &b, fstring v)
    {
        b.append(v.begin(), v.end());
        return Crc32c_update(c, v.data(), v.size());
    }
    template<class T>
    static uint32_t update(uint32_t c, valvec<byte> &b, valvec<byte> const &v)
    {
        b.append(v.begin(), v.end());
        return Crc32c_update(c, v.data(), v.size());
    }
    template<class T, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
    static uint32_t update(uint32_t c, valvec<byte> &b, T const &v)
    {
        byte const *v_ptr = reinterpret_cast<byte const *>(&v);
        b.append(v_ptr, v_ptr + sizeof(T));
        return Crc32c_update(c, &v, sizeof(T));
    }
};

template<class ...args_t>
void WriteLog(NativeDataOutput<OutputBuffer> &out, valvec<byte> &buffer, LogAction action, args_t const &...args)
{
    buffer.risk_set_size(0);
    uint32_t crc;
    std::initializer_list<uint32_t>{crc = CrcUpdate::update(0, buffer, uint8_t(action)), (crc = CrcUpdate::update<args_t>(crc, buffer, args))...};
    out << crc << buffer;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////


TrbSegmentRWLock::~TrbSegmentRWLock()
{
    for(auto pair : row_lock)
    {
        delete pair.second;
    }
    for(auto ptr : lock_pool)
    {
        delete ptr;
    }
}

TrbSegmentRWLock::scoped_lock::scoped_lock(TrbSegmentRWLock &m, size_t i, bool w)
    : parent(&m)
{
    {
        spin_lock_t::scoped_lock l(parent->g_lock);
        auto ib = parent->row_lock.emplace(uint32_t(i), nullptr);
        if(ib.second)
        {
            if(parent->lock_pool.empty())
            {
                ib.first->second = item = new map_item;
            }
            else
            {
                ib.first->second = item = parent->lock_pool.pop_val();
            }
        }
        else
        {
            item = ib.first->second;
        }
        item->id = uint32_t(i);
        ++item->count;
    }
    lock.acquire(item->lock, w);
}

TrbSegmentRWLock::scoped_lock::~scoped_lock()
{
    lock.release();
    if(--item->count == 0)
    {
        spin_lock_t::scoped_lock l(parent->g_lock);
        if(item->count == 0)
        {
            parent->row_lock.erase(item->id);
            parent->lock_pool.emplace_back(item);
        }
    }
}

bool TrbSegmentRWLock::scoped_lock::upgrade()
{
    return lock.upgrade_to_writer();
}

bool TrbSegmentRWLock::scoped_lock::downgrade()
{
    return lock.downgrade_to_reader();
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

class MutexLockTransaction : public DbTransaction
{
    const SchemaConfig& m_sconf;
    TrbColgroupSegment *m_seg;
    DbContext          *m_ctx;
    boost::optional<TrbSegmentRWLock::scoped_lock> m_lock;

public:
    explicit MutexLockTransaction(TrbColgroupSegment* seg, DbContext* ctx)
        : m_sconf(*seg->m_schema)
    {
        m_seg = seg;
        m_ctx = ctx;
    }
    ~MutexLockTransaction()
    {
    }
    void indexRemove(size_t indexId, fstring key) override
    {
        assert(started == m_status);
        m_seg->m_indices[indexId]->getWritableIndex()->remove(key, m_recId, m_ctx);
    }
    bool indexInsert(size_t indexId, fstring key) override
    {
        assert(started == m_status);
        return m_seg->m_indices[indexId]->getWritableIndex()->insert(key, m_recId, m_ctx);
    }
    void storeRemove() override
    {
        assert(started == m_status);
        size_t const colgroups_size = m_seg->m_colgroups.size();
        for(size_t i = m_seg->m_indices.size(); i < colgroups_size; ++i)
        {
            auto &store = m_seg->m_colgroups[i];
            store->getWritableStore()->remove(m_recId, m_ctx);
        }
        WriteLog(m_seg->m_out, m_ctx->trbBuf, LogAction::Modify, uint32_t(m_recId), uint32_t(0));
    }
    void storeUpdate(fstring row) override
    {
        assert(started == m_status);
        m_sconf.m_rowSchema->parseRow(row, &m_ctx->trbCols);
        size_t const colgroups_size = m_seg->m_colgroups.size();
        for(size_t i = m_seg->m_indices.size(); i < colgroups_size; ++i)
        {
            auto &store = m_seg->m_colgroups[i];
            auto &schema = m_sconf.getColgroupSchema(i);
            schema.selectParent(m_ctx->trbCols, &m_ctx->trbBuf);
            store->getUpdatableStore()->update(m_recId, m_ctx->trbBuf, m_ctx);
        }
        WriteLog(m_seg->m_out, m_ctx->trbBuf, LogAction::Store, uint32_t(m_recId), row);
    }
    void storeGetRow(valvec<byte>* row) override
    {
        assert(started == m_status);
        row->risk_set_size(0);
        m_seg->ColgroupWritableSegment::getValueAppend(m_recId, row, m_ctx);
    }
    void do_startTransaction() override
    {
        m_lock.emplace(m_seg->m_lock, m_recId);
    }
    bool do_commit() override
    {
        m_lock.reset();
        return true;
    }
    void do_rollback() override
    {
        m_lock.reset();
    }
    const std::string& strError() const override
    {
        return m_strError;
    }

    valvec<byte> m_wrtBuf;
    ColumnVec    m_cols1;
    ColumnVec    m_cols2;
    std::string  m_strError;
};

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

DbTransaction *TrbColgroupSegment::createTransaction(DbContext* ctx)
{
    auto txn = new MutexLockTransaction(this, ctx);
    return txn;
}

TrbColgroupSegment::TrbColgroupSegment()
{
    m_hasLockFreePointSearch = true;
}

TrbColgroupSegment::~TrbColgroupSegment()
{
    if(m_fp.isOpen())
    {
        fclose(m_fp.detach());
    }
}

void TrbColgroupSegment::load(PathRef path)
{
    assert(m_segDir == path);
    FILE *f = fopen(fixFilePath(m_segDir).c_str(), "rb+");
    if(f == nullptr)
    {
        assert(f != nullptr);
    }
    m_fp.attach(f);

    initIndicesColgroups();

    m_fp.disbuf();
    NativeDataInput<InputBuffer> in; in.attach(&m_fp);
    LogAction action;
    uint32_t crc;
    uint32_t index;
    uint32_t remove;
    valvec<byte> data;

    valvec<byte> buf;
    ColumnVec cols;

    struct BadLog{};

    try
    {
        size_t const colgroups_size = m_colgroups.size();
        while(true)
        {
            in >> crc >> data;
            if(crc != Crc32c_update(0, data.data(), data.size()))
            {
                //TODO crc32 check error !!!
                throw BadLog();
            }
            assert(data.size() >= 1);
            std::memcpy(&action, data.data(), 1);
            switch(action)
            {
            case LogAction::Store:
                assert(data.size() >= 5);
                std::memcpy(&index, data.data() + 1, 4);
                m_schema->m_rowSchema->parseRow({data.begin() + 5, data.end()}, &cols);
                for(size_t i = 0; i < colgroups_size; ++i)
                {
                    auto &store = m_colgroups[i];
                    auto &schema = m_schema->getColgroupSchema(i);
                    schema.selectParent(cols, &buf);
                    store->getWritableStore()->update(index, buf, nullptr); //TODO ... how to get context ?
                }
                break;
            case LogAction::Emplace:
                assert(data.size() >= 9);
                std::memcpy(&index, data.data() + 1, 4);
                std::memcpy(&remove, data.data() + 5, 4);
                m_schema->m_rowSchema->parseRow({data.begin() + 9, data.end()}, &cols);
                for(size_t i = 0; i < colgroups_size; ++i)
                {
                    auto &store = m_colgroups[i];
                    auto &schema = m_schema->getColgroupSchema(i);
                    schema.selectParent(cols, &buf);
                    store->getWritableStore()->update(index, buf, nullptr); //TODO ... context & remove ...
                }
                break;
            case LogAction::Modify:

                break;
            default:
                //TODO WTF ??
                throw BadLog();
            }
        }
    }
    catch(BadLog)
    {
        //TODO ... BadLog BadLog BadLog
    }
    catch(EndOfFileException const &)
    {
    }

    m_out.attach(&m_fp);
}

void TrbColgroupSegment::save(PathRef path) const
{
    m_fp.flush();
}

std::string TrbColgroupSegment::fixFilePath(PathRef path)
{
    return path.has_filename() && path.filename().string() == fstring("trb.log")
        ? path.string()
        : (path / "trb.log").string();
}

void TrbColgroupSegment::initIndicesColgroups()
{
    size_t const indices_size = m_schema->getIndexNum();
    size_t const colgroups_size = m_schema->getColgroupNum();
    m_indices.resize(indices_size);
    m_colgroups.resize(colgroups_size);
    for(size_t i = 0; i < indices_size; ++i)
    {
        const Schema& schema = m_schema->getIndexSchema(i);
        m_indices[i] = createIndex(schema, m_segDir);
        auto store = m_indices[i]->getReadableStore();
        assert(store);
        m_colgroups[i] = store;
    }
    for(size_t i = indices_size; i < colgroups_size; ++i)
    {
        const Schema& schema = m_schema->getColgroupSchema(i);
        auto store = createStore(schema, m_segDir);
        assert(store);
        m_colgroups[i] = store;
    }
}

void TrbColgroupSegment::initEmptySegment()
{
    //ColgroupWritableSegment::initEmptySegment();
    initIndicesColgroups();

    FILE *f = fopen(fixFilePath(m_segDir).c_str(), "wb");
    m_fp.attach(f);
    m_out.attach(&m_fp);
}

ReadableIndex *TrbColgroupSegment::openIndex(const Schema &schema, PathRef) const
{
    return TrbWritableIndex::createIndex(schema, this);
}
ReadableIndex *TrbColgroupSegment::createIndex(const Schema &schema, PathRef) const
{
    return TrbWritableIndex::createIndex(schema, this);
}
ReadableStore *TrbColgroupSegment::createStore(const Schema &schema, PathRef) const
{
    if(schema.getFixedRowLen() > 0)
    {
        return new MemoryFixedLenStore(schema, this);
    }
    else
    {
        return new TrbWritableStore(schema, this);
    }
}

void TrbColgroupSegment::indexSearchExactAppend(size_t mySegIdx, size_t indexId, fstring key, valvec<llong>* recIdvec, DbContext *ctx) const
{
    if(m_isFreezed)
    {
        ColgroupWritableSegment::indexSearchExactAppend(mySegIdx, indexId, key, recIdvec, ctx);
    }
    else
    {
        TrbSegmentRWLock::scoped_lock l(m_lock, indexId, false);
        ColgroupWritableSegment::indexSearchExactAppend(mySegIdx, indexId, key, recIdvec, ctx);
    }
}

void TrbColgroupSegment::getValueAppend(llong id, valvec<byte>* val, DbContext *ctx) const
{
    if(m_isFreezed)
    {
        ColgroupWritableSegment::getValueAppend(id, val, ctx);
    }
    else
    {
        TrbSegmentRWLock::scoped_lock l(m_lock, id, false);
        ColgroupWritableSegment::getValueAppend(id, val, ctx);
    }
}

llong TrbColgroupSegment::append(fstring row, DbContext* ctx)
{
    assert(false);
    return llong(-1);
}

void TrbColgroupSegment::update(llong id, fstring row, DbContext* ctx)
{
    assert(false);
}

void TrbColgroupSegment::remove(llong id, DbContext* ctx)
{
    assert(false);
}

void TrbColgroupSegment::shrinkToFit()
{
    for(auto &store : m_colgroups)
    {
        auto appendable_store = store->getAppendableStore();
        if(nullptr != appendable_store)
        {
            appendable_store->shrinkToFit();
        }
    }
}

void TrbColgroupSegment::saveRecordStore(PathRef segDir) const
{
    //for(size_t colgroupId : m_schema->m_updatableColgroups)
    //{
    //    const Schema& schema = m_schema->getColgroupSchema(colgroupId);
    //    assert(schema.m_isInplaceUpdatable);
    //    assert(schema.getFixedRowLen() > 0);
    //    auto store = m_colgroups[colgroupId];
    //    assert(nullptr != store);
    //    store->save(segDir / "colgroup-" + schema.m_name);
    //}
}

void TrbColgroupSegment::loadRecordStore(PathRef segDir)
{
    //assert(m_colgroups.size() == 0);
    //m_colgroups.resize(m_schema->getColgroupNum());
    //for(size_t colgroupId : m_schema->m_updatableColgroups)
    //{
    //    const Schema& schema = m_schema->getColgroupSchema(colgroupId);
    //    assert(schema.m_isInplaceUpdatable);
    //    assert(schema.getFixedRowLen() > 0);
    //    std::unique_ptr<FixedLenStore> store(new FixedLenStore(segDir, schema));
    //    store->openStore();
    //    m_colgroups[colgroupId] = store.release();
    //}
    //size_t const colgroups_size = m_colgroups.size();
    //for(size_t i = 0; i < colgroups_size; ++i)
    //{
    //    auto &store = m_colgroups[i];
    //    if(store)
    //    {
    //        continue;
    //    }
    //    if(i < m_indices.size())
    //    {
    //        store = m_indices[i]->getReadableStore();
    //    }
    //    if(!store)
    //    {
    //        const Schema& schema = m_schema->getColgroupSchema(i);
    //        store = new TrbWritableStore(segDir / "colgroup-" + schema.m_name);
    //    }
    //}
}

llong TrbColgroupSegment::dataStorageSize() const
{
    return totalStorageSize();
}
llong TrbColgroupSegment::totalStorageSize() const
{
    llong size = 0;
    for(auto &store : m_colgroups)
    {
        size += store->dataStorageSize();
    }
    return size;
}

}}} // namespace terark::db::trbdb