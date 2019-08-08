#pragma once

#include <Common/HashTable/HashTable.h>

template <typename KeyType, typename TState = HashTableNoState>
struct FixedHashTableCell
{
    using State = TState;
    using Key = KeyType;
    using value_type = Key;

    bool full;

    FixedHashTableCell() {}
    FixedHashTableCell(const Key &, const State &) : full(true) {}

    bool isZero(const State &) const { return !full; }
    void setZero() { full = false; }
    static constexpr bool need_zero_value_storage = false;
    void setMapped(const value_type & /*value*/) {}
};

/**
  * The fixed hash table has to provide the same iterator interface as
  * normal hash table. In particular, its iterators should dereference to an
  * object that can return the key of the corresponding cell. The iterators of
  * HashTable can just return its cells. Howewer, the cells of the fixed hash
  * table do not contain the key and cannot provide this interface. This is why
  * its iterator returns a wrapper object that is different from the cell itself.
  * IteratorCellWrapper is a generic implementation of such a wrapper.
  * More complex extensions of FixedHashTable such as FixedHashMap implement
  * their own specialization of this wrapper to support different cell structure
  * and additional required methods.
  */
template <typename Cell>
struct IteratorCellWrapper
{
    using Key = typename Cell::Key;
    using Value = typename Cell::value_type;

    // We use const cell pointer here for both constant and non-constant
    // wrapper, to avoid adding another template parameter. Const-correctness
    // is guaranteed by that the const iterator always returns a constant
    // cell wrapper.
    const Cell * cell;
    Key key;

    const Key & getFirst() const { return key; }
    const Value & getValue() const { return key; }
};

/** Used as a lookup table for small keys such as UInt8, UInt16. It's different
  *  than a HashTable in that keys are not stored in the Cell buf, but inferred
  *  inside each iterator. There are a bunch of to make it faster than using
  *  HashTable: a) It doesn't have a conflict chain; b) There is no key
  *  comparision; c) The number of cycles for checking cell empty is halved; d)
  *  Memory layout is tighter, especially the Clearable variants.
  *
  * NOTE: For Set variants this should always be better. For Map variants
  *  however, as we need to assemble the real cell inside each iterator, there
  *  might be some cases we fall short.
  *
  * TODO: Deprecate the cell API so that end users don't rely on the structure
  *  of cell. Instead iterator should be used for operations such as cell
  *  transfer, key updates (f.g. StringRef) and serde. This will allow
  *  TwoLevelHashSet(Map) to contain different type of sets(maps).
  */
template <typename Key, typename Cell, typename Allocator>
class FixedHashTable : private boost::noncopyable, protected Allocator, protected Cell::State
{
    static constexpr size_t BUFFER_SIZE = 1ULL << (sizeof(Key) * 8);

protected:
    friend class const_iterator;
    friend class iterator;
    friend class Reader;

    using Self = FixedHashTable;
    using cell_type = Cell;

    size_t m_size = 0; /// Amount of elements
    Cell * buf; /// A piece of memory for all elements except the element with zero key.

    void alloc() { buf = reinterpret_cast<Cell *>(Allocator::alloc(BUFFER_SIZE * sizeof(Cell))); }

    void free()
    {
        if (buf)
        {
            Allocator::free(buf, getBufferSizeInBytes());
            buf = nullptr;
        }
    }

    void destroyElements()
    {
        if (!std::is_trivially_destructible_v<Cell>)
            for (iterator it = begin(), it_end = end(); it != it_end; ++it)
                it.getPtr()->~Cell();
    }



    template <typename Derived, bool is_const>
    class iterator_base
    {
        using Container = std::conditional_t<is_const, const Self, Self>;
        using cell_type = std::conditional_t<is_const, const Cell, Cell>;

        Container * container;
        IteratorCellWrapper<Cell> cell_wrapper;

    public:
        iterator_base() {}
        iterator_base(Container * container_, cell_type * cell) :
            container(container_),
            cell_wrapper{cell, static_cast<Key>(cell - container->buf)}
        {
        }

        bool operator==(const iterator_base & rhs) const
        {
            return cell_wrapper.cell == rhs.cell_wrapper.cell;
        }

        bool operator!=(const iterator_base & rhs) const { return !(*this == rhs); }

        Derived & operator++()
        {
            ++cell_wrapper.cell;

            /// Skip empty cells in the main buffer.
            auto buf_end = container->buf + container->BUFFER_SIZE;
            while (cell_wrapper.cell < buf_end
                   && cell_wrapper.cell->isZero(*container))
            {
                ++cell_wrapper.cell;
            }

            // Have to cast explicitly here, because the key type is more narrow
            // than pointer type, e.g., UInt8.
            cell_wrapper.key = static_cast<Key>(cell_wrapper.cell - container->buf);

            return static_cast<Derived &>(*this);
        }

        auto & operator*()
        {
            return cell_wrapper;
        }

        auto * operator-> ()
        {
            return &cell_wrapper;
        }

        const cell_type * getPtr() const { return cell_wrapper.cell; }

        Key getKey() const { return cell_wrapper.key; }
        size_t getHash() const { return cell_wrapper.key; }
        size_t getCollisionChainLength() const { return 0; }
    };


public:
    using key_type = Key;
    using value_type = typename Cell::value_type;

    size_t hash(const Key & x) const { return x; }

    FixedHashTable() { alloc(); }

    FixedHashTable(FixedHashTable && rhs) : buf(nullptr) { *this = std::move(rhs); }

    ~FixedHashTable()
    {
        destroyElements();
        free();
    }

    FixedHashTable & operator=(FixedHashTable && rhs)
    {
        destroyElements();
        free();

        std::swap(buf, rhs.buf);
        std::swap(m_size, rhs.m_size);

        Allocator::operator=(std::move(rhs));
        Cell::State::operator=(std::move(rhs));

        return *this;
    }

    class Reader final : private Cell::State
    {
    public:
        Reader(DB::ReadBuffer & in_) : in(in_) {}

        Reader(const Reader &) = delete;
        Reader & operator=(const Reader &) = delete;

        bool next()
        {
            if (!is_initialized)
            {
                Cell::State::read(in);
                DB::readVarUInt(size, in);
                is_initialized = true;
            }

            if (read_count == size)
            {
                is_eof = true;
                return false;
            }

            cell.read(in);
            ++read_count;

            return true;
        }

        inline const value_type & get() const
        {
            if (!is_initialized || is_eof)
                throw DB::Exception("No available data", DB::ErrorCodes::NO_AVAILABLE_DATA);

            return cell.getValue();
        }

    private:
        DB::ReadBuffer & in;
        Cell cell;
        size_t read_count = 0;
        size_t size;
        bool is_eof = false;
        bool is_initialized = false;
    };


    class iterator : public iterator_base<iterator, false>
    {
    public:
        using iterator_base<iterator, false>::iterator_base;
    };

    class const_iterator : public iterator_base<const_iterator, true>
    {
    public:
        using iterator_base<const_iterator, true>::iterator_base;
    };


    const_iterator begin() const
    {
        if (!buf)
            return end();

        const Cell * ptr = buf;
        auto buf_end = buf + BUFFER_SIZE;
        while (ptr < buf_end && ptr->isZero(*this))
            ++ptr;

        return const_iterator(this, ptr);
    }

    const_iterator cbegin() const { return begin(); }

    iterator begin()
    {
        if (!buf)
            return end();

        Cell * ptr = buf;
        auto buf_end = buf + BUFFER_SIZE;
        while (ptr < buf_end && ptr->isZero(*this))
            ++ptr;

        return iterator(this, ptr);
    }

    const_iterator end() const { return const_iterator(this, buf + BUFFER_SIZE); }
    const_iterator cend() const { return end(); }
    iterator end() { return iterator(this, buf + BUFFER_SIZE); }


protected:
    void ALWAYS_INLINE emplaceImpl(Key x, iterator & it, bool & inserted)
    {
        it = iterator(this, &buf[x]);

        if (!buf[x].isZero(*this))
        {
            inserted = false;
            return;
        }

        new (&buf[x]) Cell(x, *this);
        inserted = true;
        ++m_size;
    }


public:
    std::pair<iterator, bool> ALWAYS_INLINE insert(const value_type & x)
    {
        std::pair<iterator, bool> res;
        emplaceImpl(Cell::getKey(x), res.first, res.second);
        if (res.second)
            res.first.ptr->setMapped(x);

        return res;
    }


    void ALWAYS_INLINE emplace(Key x, iterator & it, bool & inserted) { emplaceImpl(x, it, inserted); }
    void ALWAYS_INLINE emplace(Key x, iterator & it, bool & inserted, size_t) { emplaceImpl(x, it, inserted); }

    // FixedHashTable doesn't store references to the keys, so it doesn't care
    // about key persistence.
    template <typename KeyPtr>
    void emplacePtr(KeyPtr && key_ptr, iterator & it, bool & inserted)
    {
        emplace(*key_ptr, it, inserted);
    }

    template <typename KeyPtr>
    void emplacePtr(KeyPtr && key_ptr, iterator & it, bool & inserted, size_t)
    {
        emplace(*key_ptr, it, inserted);
    }


    template <typename ObjectToCompareWith>
    iterator ALWAYS_INLINE find(ObjectToCompareWith x)
    {
        return !buf[x].isZero(*this) ? iterator(this, &buf[x]) : end();
    }

    template <typename ObjectToCompareWith>
    const_iterator ALWAYS_INLINE find(ObjectToCompareWith x) const
    {
        return !buf[x].isZero(*this) ? const_iterator(this, &buf[x]) : end();
    }

    template <typename ObjectToCompareWith>
    iterator ALWAYS_INLINE find(ObjectToCompareWith, size_t hash_value)
    {
        return !buf[hash_value].isZero(*this) ? iterator(this, &buf[hash_value]) : end();
    }

    template <typename ObjectToCompareWith>
    const_iterator ALWAYS_INLINE find(ObjectToCompareWith, size_t hash_value) const
    {
        return !buf[hash_value].isZero(*this) ? const_iterator(this, &buf[hash_value]) : end();
    }

    bool ALWAYS_INLINE has(Key x) const { return !buf[x].isZero(*this); }
    bool ALWAYS_INLINE has(Key, size_t hash_value) const { return !buf[hash_value].isZero(*this); }

    void write(DB::WriteBuffer & wb) const
    {
        Cell::State::write(wb);
        DB::writeVarUInt(m_size, wb);

        for (auto ptr = buf, buf_end = buf + BUFFER_SIZE; ptr < buf_end; ++ptr)
            if (!ptr->isZero(*this))
            {
                DB::writeVarUInt(ptr - buf);
                ptr->write(wb);
            }
    }

    void writeText(DB::WriteBuffer & wb) const
    {
        Cell::State::writeText(wb);
        DB::writeText(m_size, wb);

        for (auto ptr = buf, buf_end = buf + BUFFER_SIZE; ptr < buf_end; ++ptr)
        {
            if (!ptr->isZero(*this))
            {
                DB::writeChar(',', wb);
                DB::writeText(ptr - buf, wb);
                DB::writeChar(',', wb);
                ptr->writeText(wb);
            }
        }
    }

    void read(DB::ReadBuffer & rb)
    {
        Cell::State::read(rb);
        destroyElements();
        DB::readVarUInt(m_size, rb);
        free();
        alloc();

        for (size_t i = 0; i < m_size; ++i)
        {
            size_t place_value = 0;
            DB::readVarUInt(place_value, rb);
            Cell x;
            x.read(rb);
            new (&buf[place_value]) Cell(x, *this);
        }
    }

    void readText(DB::ReadBuffer & rb)
    {
        Cell::State::readText(rb);
        destroyElements();
        DB::readText(m_size, rb);
        free();
        alloc();

        for (size_t i = 0; i < m_size; ++i)
        {
            size_t place_value = 0;
            DB::assertChar(',', rb);
            DB::readText(place_value, rb);
            Cell x;
            DB::assertChar(',', rb);
            x.readText(rb);
            new (&buf[place_value]) Cell(x, *this);
        }
    }

    size_t size() const { return m_size; }

    bool empty() const { return 0 == m_size; }

    void clear()
    {
        destroyElements();
        m_size = 0;

        memset(static_cast<void *>(buf), 0, BUFFER_SIZE * sizeof(*buf));
    }

    /// After executing this function, the table can only be destroyed,
    ///  and also you can use the methods `size`, `empty`, `begin`, `end`.
    void clearAndShrink()
    {
        destroyElements();
        m_size = 0;
        free();
    }

    size_t getBufferSizeInBytes() const { return BUFFER_SIZE * sizeof(Cell); }

    size_t getBufferSizeInCells() const { return BUFFER_SIZE; }

#ifdef DBMS_HASH_MAP_COUNT_COLLISIONS
    size_t getCollisions() const { return 0; }
#endif
};
