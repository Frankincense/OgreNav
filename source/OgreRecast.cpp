/*
    OgreCrowd
    ---------

    Copyright (c) 2012 Jonas Hauquier

    Additional contributions by:

    - mkultra333
    - Paul Wilson

    Sincere thanks and to:

    - Mikko Mononen (developer of Recast navigation libraries)

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.

*/
#include "OgreRecast.h"
#include "InputGeom.h"
#include "DetourTileCacheBuilder.h"
#include "PlayerFlagQueryFilter.h"
#include "OgreRecastConfigParams.h"
#include "NavMeshDebug.h"

OgreRecast::
OgreRecast ( const OgreRecastConfigParams &config_params ) :
   BuildContext ( false )
{
   // Set default size of box around points to look for nav polygons
   PolySearchBox [ 0 ] = 32.0f ;
   PolySearchBox [ 1 ] = 32.0f ;
   PolySearchBox [ 2 ] = 32.0f ;

   // Setup the default query filter
   QueryFilter.setIncludeFlags ( POLYFLAGS_ALL ) ;
   QueryFilter.setExcludeFlags ( 0 ) ;
   QueryFilter.setAreaCost ( POLYAREA_GRASS, 2.0f  ) ;
   QueryFilter.setAreaCost ( POLYAREA_WATER, 10.0f ) ;
   QueryFilter.setAreaCost ( POLYAREA_ROAD,  1.0f ) ;
   QueryFilter.setAreaCost ( POLYAREA_SAND,  4.0f  ) ;
   QueryFilter.setAreaCost ( POLYAREA_GATE,  1.8f  ) ; // Slgihtly less than normal Grass

   // Set configuration
   ConfigureBuildParameters ( config_params ) ;
}

void
OgreRecast::
Update ( const float delta_time,
         const bool  until_up_to_date )
{
   TileCache->HandleUpdate ( delta_time, until_up_to_date ) ;
}

bool
OgreRecast::
Generate ( const unsigned int         max_num_obstacles,
           const int                  tile_size,
           std::vector<Ogre::Entity*> source_meshes,
           const TerrainAreaVector    &area_list )
{
   TileCache = std::make_unique <OgreDetourTileCache> ( *this, BuildContext, RecastConfig, NavQuery, max_num_obstacles, tile_size ) ;

   return TileCache->TileCacheBuild ( std::move ( source_meshes ), area_list ) ;
}

bool
OgreRecast::
Load ( const Ogre::String         &filename,
       const unsigned int         max_num_obstacles,
       const int                  tile_size,
       std::vector<Ogre::Entity*> source_meshes )
{
   TileCache = std::make_unique <OgreDetourTileCache> ( *this, BuildContext, RecastConfig, NavQuery, max_num_obstacles, tile_size ) ;

   return TileCache->LoadAll ( filename, std::move ( source_meshes ) ) ;
}

bool
OgreRecast::
Save ( const Ogre::String &filename )
{
   assert ( TileCache ) ;

   if ( TileCache )
   {
      return TileCache->SaveAll ( filename ) ;
   }

   return false ;
}

std::unique_ptr <NavMeshDebug>
OgreRecast::
CreateNavMeshDebugger ()
{
   assert ( TileCache ) ;

   if ( TileCache )
   {
      return std::move ( std::unique_ptr <NavMeshDebug> ( TileCache->CreateDebugger () ) ) ;
   }

   return std::unique_ptr <NavMeshDebug> () ;
}

dtObstacleRef
OgreRecast::
AddObstacle ( const Ogre::Vector3  &min,
              const Ogre::Vector3  &max,
              const unsigned char  area_id,
              const unsigned short flags )
{
   return TileCache->AddObstacle ( min, max, area_id, flags ) ;
}

dtObstacleRef
OgreRecast::
AddObstacle ( const Ogre::Vector3  &centre,
              const float          width,
              const float          depth,
              const float          height,
              const float          y_rotation, // radians
              const unsigned char  area_id,
              const unsigned short flags )
{
   return TileCache->AddObstacle ( centre, width, depth, height, y_rotation, area_id, flags ) ;
}

const dtTileCacheObstacle *
OgreRecast::
GetObstacleByRef ( dtObstacleRef ref )
{
   return TileCache->GetObstacleByRef ( ref ) ;
}

bool
OgreRecast::
RemoveObstacle ( dtObstacleRef ref )
{
   return TileCache->RemoveObstacle ( ref ) ;
}

int
OgreRecast::
AddConvexVolume ( ConvexVolume *vol )
{
   return TileCache->AddConvexVolume ( vol ) ;
}

bool
OgreRecast::
DeleteConvexVolume ( int volume_index )
{
   return TileCache->DeleteConvexVolume ( volume_index ) ;
}

FindPathReturnCode
OgreRecast::
FindPath ( float                      *start_pos,
           float                      *end_pos,
           const unsigned int         include_flags,
           const unsigned int         exclude_flags,
           std::vector<Ogre::Vector3> &path )
{
   dtStatus  status ;
   dtPolyRef start_poly ;
   dtPolyRef end_poly ;
   int       path_poly_count = 0 ;
   int       vertex_count    = 0 ;
   float     start_nearest_point [ 3 ] ;
   float     end_nearest_point   [ 3 ] ;
   dtPolyRef poly_path     [ MAX_PATHPOLY ] ;
   float     straight_path [ MAX_PATHVERT * 3 ] ;

   QueryFilter.setIncludeFlags ( include_flags ) ;
   QueryFilter.setExcludeFlags ( exclude_flags ) ;

   // Find the start polygon
   status = NavQuery.findNearestPoly ( start_pos, PolySearchBox, &QueryFilter, &start_poly, start_nearest_point ) ;

   if ( ( status & DT_FAILURE ) ||
        ( status & DT_STATUS_DETAIL_MASK ) )
   {
      return FindPathReturnCode::CANNOT_FIND_START ; // couldn't find a polygon
   }

   // Find the end polygon
   status = NavQuery.findNearestPoly ( end_pos, PolySearchBox, &QueryFilter, &end_poly, end_nearest_point ) ;

   if ( ( status & DT_FAILURE ) ||
        ( status & DT_STATUS_DETAIL_MASK ) )
   {
      return FindPathReturnCode::CANNOT_FIND_END ; // couldn't find a polygon
   }

   status = NavQuery.findPath ( start_poly, end_poly, start_nearest_point, end_nearest_point, &QueryFilter, poly_path, &path_poly_count, MAX_PATHPOLY ) ;

   if ( ( status & DT_PARTIAL_RESULT ) &&
        ( path_poly_count > 0 ) )
   {
      auto new_start = poly_path [ path_poly_count -1 ] ;

      status = NavQuery.findPath ( new_start, end_poly, start_nearest_point, end_nearest_point, &QueryFilter, poly_path, &path_poly_count, MAX_PATHPOLY ) ;
   }

   if ( ( status & DT_FAILURE ) ||
        ( status & DT_STATUS_DETAIL_MASK ) )
   {
      return FindPathReturnCode::CANNOT_CREATE_PATH ; // couldn't create a path
   }

   if ( path_poly_count == 0 )
   {
      return FindPathReturnCode::CANNOT_FIND_PATH ; // couldn't find a path
   }

   status = NavQuery.findStraightPath ( start_nearest_point, end_nearest_point, poly_path, path_poly_count, straight_path, nullptr, nullptr, &vertex_count, MAX_PATHVERT, DT_STRAIGHTPATH_AREA_CROSSINGS ) ;

   if ( ( status & DT_FAILURE ) ||
        ( status & DT_STATUS_DETAIL_MASK ) )
   {
      return FindPathReturnCode::CANNOT_CREATE_STRAIGHT_PATH ; // couldn't create a path
   }

   if ( vertex_count == 0 )
   {
      return FindPathReturnCode::CANNOT_FIND_STRAIGHT_PATH ; // couldn't find a path
   }
   else
   {
      // At this point we have our path
      std::size_t path_poly_index = 0U ;

      for ( auto vertex_index = 0 ; vertex_index < vertex_count ; ++vertex_index )
      {
         path.push_back ( Ogre::Vector3 ( straight_path [ path_poly_index + 0 ],
                                          straight_path [ path_poly_index + 1 ],
                                          straight_path [ path_poly_index + 2 ] ) ) ;

         path_poly_index += 3 ;
      }

      return FindPathReturnCode::PATH_FOUND ;
   }
}

FindPathReturnCode
OgreRecast::
FindPath ( const Ogre::Vector3        &start_pos,
           const Ogre::Vector3        &end_pos,
           const unsigned int         include_flags,
           const unsigned int         exclude_flags,
           std::vector<Ogre::Vector3> &path )
{
   float start [ 3 ] ;
   float end   [ 3 ] ;

   OgreVect3ToFloatA ( start_pos, start ) ;
   OgreVect3ToFloatA ( end_pos,   end ) ;

   return FindPath ( start, end, include_flags, exclude_flags, path ) ;
}

void
OgreRecast::
OgreVect3ToFloatA ( const Ogre::Vector3 &vect,
                    float               *result )
{
   result [ 0 ] = vect.x ;
   result [ 1 ] = vect.y ;
   result [ 2 ] = vect.z ;
}

void
OgreRecast::
FloatAToOgreVect3 ( const float   *vect,
                    Ogre::Vector3 &result )
{
   result.x = vect [ 0 ] ;
   result.y = vect [ 1 ] ;
   result.z = vect [ 2 ] ;
}

bool
OgreRecast::
FindNearestPointOnNavmesh ( const Ogre::Vector3 &position,
                            const unsigned int  include_flags,
                            const unsigned int  exclude_flags,
                            Ogre::Vector3       &result_point )
{
   dtPolyRef navmeshPoly ;
   return FindNearestPolyOnNavmesh ( position, include_flags, exclude_flags, result_point, navmeshPoly ) ;
}

bool
OgreRecast::
FindNearestPolyOnNavmesh ( const Ogre::Vector3 &position,
                           const unsigned int  include_flags,
                           const unsigned int  exclude_flags,
                           Ogre::Vector3       &result_point,
                           dtPolyRef           &result_poly )
{
   QueryFilter.setIncludeFlags ( include_flags ) ;
   QueryFilter.setExcludeFlags ( exclude_flags ) ;

   float point [ 3 ] ;
   float found_point [ 3 ] ;

   OgreVect3ToFloatA ( position, point ) ;

   dtStatus status = NavQuery.findNearestPoly ( point, PolySearchBox, &QueryFilter, &result_poly, found_point ) ;

   if ( ( status & DT_FAILURE ) ||
        ( status & DT_STATUS_DETAIL_MASK ) )
   {
      return false ; // couldn't find a polygon
   }
   else
   {
      FloatAToOgreVect3 ( found_point, result_point ) ;

      return true ;
   }
}

void
OgreRecast::
ConfigureBuildParameters ( const OgreRecastConfigParams &config_params )
{
   // NOTE: this is one of the most important parts to get it right!!
   // Perhaps the most important part of the above is setting the agent size with m_agentHeight and m_agentRadius,
   // and the voxel cell size used, m_cellSize and m_cellHeight. In my project 1 units is a little less than 1 meter,
   // so I've set the agent to 2.5 units high, and the cell sizes to sub-meter size.
   // This is about the same as in the original cell sizes in the Recast/Detour demo.

   // Smaller cellsizes are the most accurate at finding all the places we could go, but are also slow to generate.
   // Might be suitable for pre-generated meshes. Though it also produces a lot more polygons.

   // Init cfg object
   memset ( &RecastConfig, 0, sizeof ( RecastConfig ) ) ;

   RecastConfig.cs                     = config_params.getCellSize () ;
   RecastConfig.ch                     = config_params.getCellHeight () ;
   RecastConfig.walkableSlopeAngle     = config_params.getAgentMaxSlope () ;
   RecastConfig.walkableHeight         = config_params._getWalkableheight () ;
   RecastConfig.walkableClimb          = config_params._getWalkableClimb () ;
   RecastConfig.walkableRadius         = config_params._getWalkableRadius () ;
   RecastConfig.maxEdgeLen             = config_params._getMaxEdgeLen () ;
   RecastConfig.maxSimplificationError = config_params.getEdgeMaxError () ;
   RecastConfig.minRegionArea          = config_params._getMinRegionArea () ;
   RecastConfig.mergeRegionArea        = config_params._getMergeRegionArea () ;
   RecastConfig.maxVertsPerPoly        = config_params.getVertsPerPoly () ;
   RecastConfig.detailSampleDist       = static_cast <float> ( config_params._getDetailSampleDist () ) ;
   RecastConfig.detailSampleMaxError   = static_cast <float> ( config_params._getDetailSampleMaxError () ) ;
}
