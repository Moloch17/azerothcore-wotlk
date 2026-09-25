/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __UPDATEMASK_H
#define __UPDATEMASK_H

#include "ByteBuffer.h"
#include "Errors.h"
#include <algorithm>

/// Which update fields of an object changed. Stored as the 32-bit words the client reads, one bit per
/// field: a Player's 1326 fields are 42 words, clearing is a short fill, and the packet form is the
/// storage form. (Stock kept one byte per field and transposed it into words for every packet.)
class UpdateMask
{
public:
    /// Type representing how client reads update mask
    typedef uint32 ClientUpdateMaskType;

    enum UpdateMaskCount
    {
        CLIENT_UPDATE_MASK_BITS = sizeof(ClientUpdateMaskType) * 8,
    };

    UpdateMask() = default;

    UpdateMask(UpdateMask const& right)
    {
        SetCount(right.GetCount());
        std::copy_n(right._blocks, _blockCount, _blocks);
    }

    ~UpdateMask() { delete[] _blocks; }

    void SetBit(uint32 index) { _blocks[index / CLIENT_UPDATE_MASK_BITS] |= ClientUpdateMaskType(1) << (index % CLIENT_UPDATE_MASK_BITS); }
    void UnsetBit(uint32 index) { _blocks[index / CLIENT_UPDATE_MASK_BITS] &= ~(ClientUpdateMaskType(1) << (index % CLIENT_UPDATE_MASK_BITS)); }
    [[nodiscard]] bool GetBit(uint32 index) const { return (_blocks[index / CLIENT_UPDATE_MASK_BITS] >> (index % CLIENT_UPDATE_MASK_BITS)) & 1; }

    void AppendToPacket(ByteBuffer* data)
    {
        for (uint32 i = 0; i < _blockCount; ++i)
            *data << _blocks[i];
    }

    [[nodiscard]] uint32 GetBlockCount() const { return _blockCount; }
    [[nodiscard]] uint32 GetCount() const { return _fieldCount; }

    void SetCount(uint32 valuesCount)
    {
        delete[] _blocks;

        _fieldCount = valuesCount;
        _blockCount = (valuesCount + CLIENT_UPDATE_MASK_BITS - 1) / CLIENT_UPDATE_MASK_BITS;

        _blocks = new ClientUpdateMaskType[_blockCount]();
    }

    void Clear()
    {
        if (_blocks)
            std::fill_n(_blocks, _blockCount, ClientUpdateMaskType(0));
    }

    UpdateMask& operator=(UpdateMask const& right)
    {
        if (this == &right)
            return *this;

        SetCount(right.GetCount());
        std::copy_n(right._blocks, _blockCount, _blocks);
        return *this;
    }

    /// Fields beyond right's count are left as they are, as stock did.
    UpdateMask& operator&=(UpdateMask const& right)
    {
        ASSERT(right.GetCount() <= GetCount());
        uint32 const fullBlocks = right._fieldCount / CLIENT_UPDATE_MASK_BITS;
        for (uint32 i = 0; i < fullBlocks; ++i)
            _blocks[i] &= right._blocks[i];

        if (uint32 const rest = right._fieldCount % CLIENT_UPDATE_MASK_BITS)
        {
            ClientUpdateMaskType const low = (ClientUpdateMaskType(1) << rest) - 1;
            _blocks[fullBlocks] = (_blocks[fullBlocks] & ~low) | (_blocks[fullBlocks] & right._blocks[fullBlocks] & low);
        }

        return *this;
    }

    UpdateMask& operator|=(UpdateMask const& right)
    {
        ASSERT(right.GetCount() <= GetCount());
        uint32 const fullBlocks = right._fieldCount / CLIENT_UPDATE_MASK_BITS;
        for (uint32 i = 0; i < fullBlocks; ++i)
            _blocks[i] |= right._blocks[i];

        if (uint32 const rest = right._fieldCount % CLIENT_UPDATE_MASK_BITS)
        {
            ClientUpdateMaskType const low = (ClientUpdateMaskType(1) << rest) - 1;
            _blocks[fullBlocks] |= right._blocks[fullBlocks] & low;
        }

        return *this;
    }

    UpdateMask operator|(UpdateMask const& right)
    {
        UpdateMask ret(*this);
        ret |= right;
        return ret;
    }

private:
    uint32 _fieldCount{0};
    uint32 _blockCount{0};
    ClientUpdateMaskType* _blocks{nullptr};
};

#endif
