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

#ifndef _MODELINSTANCE_H_
#define _MODELINSTANCE_H_

#include "Define.h"
#include <G3D/AABox.h>
#include <G3D/Matrix3.h>
#include <G3D/Ray.h>
#include <G3D/Vector3.h>
#include <memory>

namespace VMAP
{
    class WorldModel;
    struct AreaInfo;
    struct LocationInfo;
    enum class ModelIgnoreFlags : uint32;

    enum ModelFlags
    {
        MOD_M2 = 1,
        MOD_WORLDSPAWN = 1 << 1,
        MOD_HAS_BOUND = 1 << 2
    };

    class ModelSpawn
    {
    public:
        //mapID, tileX, tileY, Flags, ID, Pos, Rot, Scale, Bound_lo, Bound_hi, name
        uint32 flags;
        uint16 adtId;
        uint32 ID;
        G3D::Vector3 iPos;
        G3D::Vector3 iRot;
        float iScale;
        G3D::AABox iBound;
        std::string name;
        bool operator==(ModelSpawn const& other) const { return ID == other.ID; }
        //uint32 hashCode() const { return ID; }
        // temp?
        [[nodiscard]] G3D::AABox const& GetBounds() const { return iBound; }

        static bool readFromFile(FILE* rf, ModelSpawn& spawn);
        static bool writeToFile(FILE* rw, ModelSpawn const& spawn);
    };

    class ModelInstance: public ModelSpawn
    {
    public:
        ModelInstance() { }
        ModelInstance(ModelSpawn const& spawn, std::shared_ptr<WorldModel> model);
        /// `normal`, when given, takes the nearest hit triangle's normal in the tree's space (unnormalised, either
        /// side), and is left alone without a hit.
        bool intersectRay(G3D::Ray const& pRay, float& pMaxDist, bool StopAtFirstHit, ModelIgnoreFlags ignoreFlags,
            G3D::Vector3* normal = nullptr) const;
        //! The ray's nearest crossing of the model's liquid closer than pMaxDist (WorldModel::IntersectLiquid).
        bool intersectLiquid(G3D::Ray const& pRay, float& pMaxDist, uint32& liquidType) const;
        bool GetLocationInfo(G3D::Vector3 const& p, LocationInfo& info) const;
        bool GetLiquidLevel(G3D::Vector3 const& p, LocationInfo& info, float& liqHeight) const;
        WorldModel* getWorldModel() { return iModel.get(); }
        //! What intersectRay reads, read-only (the bots' camera copies it to the GPU: Animus/Gpu/VisionScene).
        [[nodiscard]] WorldModel const* GetWorldModel() const { return iModel.get(); }
        [[nodiscard]] G3D::Matrix3 const& GetInvRot() const { return iInvRot; }
        [[nodiscard]] float GetInvScale() const { return iInvScale; }
    protected:
        G3D::Matrix3 iInvRot;
        float iInvScale{0.0f};
        std::shared_ptr<WorldModel> iModel;
    };
} // namespace VMAP

#endif // _MODELINSTANCE_H_
