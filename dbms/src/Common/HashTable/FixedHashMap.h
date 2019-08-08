#pragma once

#include <Common/HashTable/FixedHashTable.h>
#include <Common/HashTable/HashMap.h>


template <typename KeyType, typename MappedType, typename StateType = HashTableNoState>
struct FixedHashMapCell
{
    using Mapped = MappedType;
    using State = StateType;
    using Key = KeyType;
    using value_type = PairNoInit<KeyType, MappedType>;

    Mapped mapped;
    bool full;

    FixedHashMapCell() {}
    FixedHashMapCell(const Key &, const State &) : full(true) {}
    FixedHashMapCell(const value_type & value_, const State &) : full(true), mapped(value_.second) {}

    Mapped & getSecond() { return mapped; }
    const Mapped & getSecond() const { return mapped; }
    bool isZero(const State &) const { return !full; }
    void setZero() { full = false; }
    static constexpr bool need_zero_value_storage = false;
    void setMapped(const value_type & value) { mapped = value.getSecond(); }
};

/**
  * A specialization of IteratorCellWrapper that supports separate methods
  * for reading key and value.
  */
template <typename KeyType, typename MappedType, typename StateType>
struct IteratorCellWrapper<FixedHashMapCell<KeyType, MappedType, StateType>>
{
    using Cell = FixedHashMapCell<KeyType, MappedType, StateType>;
    using Key = typename Cell::Key;
    using Mapped = typename Cell::Mapped;
    using Value = typename Cell::value_type;

    // We use const cell pointer here for both constant and non-constant
    // wrapper, to avoid adding another template parameter. Const-correctness
    // is guaranteed by that the const iterator always returns a constant cell
    // wrapper.
    const Cell * cell;
    Key key;

    const Key & getFirst() const { return key; }

    // FIXME it should be possible to remove this method altogether and use
    // getFirst() and getSecond() instead.
    // Also remove things like FixedHashMapCell::value_type = PairNoInit<KeyType, MappedType>;
    const Value getValue() const { return {getFirst(), getSecond()}; }

    const Mapped & getSecond() const { return cell->mapped; }
    Mapped & getSecond() { return const_cast<Cell *>(cell)->mapped; }
};


template <typename Key, typename Mapped, typename Allocator = HashTableAllocator>
class FixedHashMap : public FixedHashTable<Key, FixedHashMapCell<Key, Mapped>, Allocator>
{
public:
    using Base = FixedHashTable<Key, FixedHashMapCell<Key, Mapped>, Allocator>;
    using key_type = Key;
    using mapped_type = Mapped;
    using value_type = typename Base::cell_type::value_type;

    using Base::Base;

    mapped_type & ALWAYS_INLINE operator[](Key x)
    {
        typename Base::iterator it;
        bool inserted;
        this->emplace(x, it, inserted);
        if (inserted)
            new (&it->getSecond()) mapped_type();

        return it->getSecond();
    }
};
