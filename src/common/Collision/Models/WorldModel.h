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

#ifndef _WORLDMODEL_H
#define _WORLDMODEL_H

#include "BoundingIntervalHierarchy.h"
#include "Define.h"
#include <G3D/AABox.h>
#include <G3D/Ray.h>
#include <G3D/Vector3.h>

namespace VMAP
{
    class TreeNode;
    struct AreaInfo;
    struct LocationInfo;
    struct GroupLocationInfo;
    enum class ModelIgnoreFlags : uint32;

    class MeshTriangle
    {
    public:
        MeshTriangle()  { }
        MeshTriangle(uint32 na, uint32 nb, uint32 nc): idx0(na), idx1(nb), idx2(nc) { }

        uint32 idx0{0};
        uint32 idx1{0};
        uint32 idx2{0};
    };

    class WmoLiquid
    {
    public:
        WmoLiquid(uint32 width, uint32 height, G3D::Vector3 const& corner, uint32 type);
        WmoLiquid(WmoLiquid const& other);
        ~WmoLiquid();
        WmoLiquid& operator=(WmoLiquid const& other);
        bool GetLiquidHeight(G3D::Vector3 const& pos, float& liqHeight) const;
        //! The nearest crossing of the ray with the liquid's surface (each used tile's two triangles, as
        //! GetLiquidHeight tessellates it; the single level of a liquid with no tiles) closer than `distance`, in
        //! model space: sets `distance` and returns true. Read-only; nothing else calls it (the bots' camera does).
        bool IntersectRay(G3D::Ray const& ray, float& distance) const;
        [[nodiscard]] uint32 GetType() const { return iType; }
        //! Read-only views for the bots' camera's GPU copy (Animus/Gpu/VisionScene): null heights is no surface,
        //! null flags a single level over the whole liquid.
        [[nodiscard]] float const* GetHeights() const { return iHeight; }
        [[nodiscard]] uint8 const* GetFlags() const { return iFlags; }
        float* GetHeightStorage() { return iHeight; }
        uint8* GetFlagsStorage() { return iFlags; }
        uint32 GetFileSize();
        bool writeToFile(FILE* wf);
        static bool readFromFile(FILE* rf, WmoLiquid*& liquid);
        void GetPosInfo(uint32& tilesX, uint32& tilesY, G3D::Vector3& corner) const;
    private:
        WmoLiquid() { }
        uint32 iTilesX{0};       //!< number of tiles in x direction, each
        uint32 iTilesY{0};
        G3D::Vector3 iCorner;    //!< the lower corner
        uint32 iType{0};         //!< liquid type
        float* iHeight{nullptr}; //!< (tilesX + 1)*(tilesY + 1) height values
        uint8* iFlags{nullptr};  //!< info if liquid tile is used
    };

    /*! holding additional info for WMO group files */
    class GroupModel
    {
    public:
        GroupModel() { }
        GroupModel(GroupModel const& other);
        GroupModel(uint32 mogpFlags, uint32 groupWMOID, G3D::AABox const& bound):
            iBound(bound), iMogpFlags(mogpFlags), iGroupWMOID(groupWMOID), iLiquid(nullptr) { }
        ~GroupModel() { delete iLiquid; }

        //! pass mesh data to object and create BIH. Passed vectors get get swapped with old geometry!
        void setMeshData(std::vector<G3D::Vector3>& vert, std::vector<MeshTriangle>& tri);
        void setLiquidData(WmoLiquid*& liquid) { iLiquid = liquid; liquid = nullptr; }
        /// `normal`, when given, takes the nearest hit triangle's normal (unnormalised, in this model's space, either
        /// side), and is left alone without a hit.
        bool IntersectRay(G3D::Ray const& ray, float& distance, bool stopAtFirstHit, G3D::Vector3* normal = nullptr) const;
        enum InsideResult { INSIDE = 0, MAYBE_INSIDE = 1, ABOVE = 2, OUT_OF_BOUNDS = -1 };
        InsideResult IsInsideObject(G3D::Ray const& ray, float& z_dist) const;
        bool GetLiquidLevel(G3D::Vector3 const& pos, float& liqHeight) const;
        //! The ray's nearest crossing of the group's liquid within the group's bound, closer than `distance`.
        bool IntersectLiquid(G3D::Ray const& ray, float& distance, uint32& liquidType) const;
        [[nodiscard]] uint32 GetLiquidType() const;
        bool writeToFile(FILE* wf);
        bool readFromFile(FILE* rf);
        [[nodiscard]] G3D::AABox const& GetBound() const { return iBound; }
        [[nodiscard]] G3D::AABox const& GetMeshTreeBound() const { return meshTree.bound(); }
        [[nodiscard]] uint32 GetMogpFlags() const { return iMogpFlags; }
        [[nodiscard]] uint32 GetWmoID() const { return iGroupWMOID; }
        void GetMeshData(std::vector<G3D::Vector3>& outVertices, std::vector<MeshTriangle>& outTriangles, WmoLiquid*& liquid);
        //! Read-only views of what IntersectRay and IntersectLiquid read (the bots' camera's GPU copy).
        [[nodiscard]] std::vector<G3D::Vector3> const& GetVertices() const { return vertices; }
        [[nodiscard]] std::vector<MeshTriangle> const& GetTriangles() const { return triangles; }
        [[nodiscard]] BIH const& GetMeshTree() const { return meshTree; }
        [[nodiscard]] WmoLiquid const* GetLiquid() const { return iLiquid; }
    protected:
        G3D::AABox iBound;
        uint32 iMogpFlags{0};// 0x8 outdor; 0x2000 indoor
        uint32 iGroupWMOID{0};
        std::vector<G3D::Vector3> vertices;
        std::vector<MeshTriangle> triangles;
        BIH meshTree;
        WmoLiquid* iLiquid{nullptr};
    };
    /*! Holds a model (converted M2 or WMO) in its original coordinate space */
    class WorldModel
    {
    public:
        WorldModel() { }

        //! pass group models to WorldModel and create BIH. Passed vector is swapped with old geometry!
        void setGroupModels(std::vector<GroupModel>& models);
        void setRootWmoID(uint32 id) { RootWMOID = id; }
        /// `normal` as GroupModel::IntersectRay's.
        bool IntersectRay(G3D::Ray const& ray, float& distance, bool stopAtFirstHit, ModelIgnoreFlags ignoreFlags,
            G3D::Vector3* normal = nullptr) const;
        //! The ray's nearest crossing of any group's liquid closer than `distance` (opt-in: only the camera asks).
        bool IntersectLiquid(G3D::Ray const& ray, float& distance, uint32& liquidType) const;
        bool GetLocationInfo(G3D::Vector3 const& p, G3D::Vector3 const& down, float& dist, GroupLocationInfo& info) const;
        bool writeFile(std::string const& filename);
        bool readFile(std::string const& filename);
        void GetGroupModels(std::vector<GroupModel>& outGroupModels);
        //! Read-only views of what IntersectRay and IntersectLiquid read (the bots' camera's GPU copy).
        [[nodiscard]] std::vector<GroupModel> const& GetGroups() const { return groupModels; }
        [[nodiscard]] BIH const& GetGroupTree() const { return groupTree; }
        uint32 Flags;
    protected:
        uint32 RootWMOID{0};
        std::vector<GroupModel> groupModels;
        BIH groupTree;
    };
} // namespace VMAP

#endif // _WORLDMODEL_H
