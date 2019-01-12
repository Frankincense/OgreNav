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

#include "OgreDetourTileCache.h"
#include "NavMeshDebug.h"
#include "DetourTileCache.h"
#include <float.h>


///// Static config parameters //////

// Max number of layers a tile can have
const int OgreDetourTileCache::EXPECTED_LAYERS_PER_TILE = 1;

// Extra padding added to the border size of tiles (together with agent radius)
const float OgreDetourTileCache::BORDER_PADDING = 3;

// Set to false to disable debug drawing. Improves performance.
//bool OgreDetourTileCache::DEBUG_DRAW = true;

/////////////////////////////////////

OgreDetourTileCache::OgreDetourTileCache(OgreRecast *recast, unsigned int max_num_obstacles, int tileSize)
    : m_recast(recast),
      m_tileSize(tileSize - (tileSize%8)),  // Make sure tilesize is a multiple of 8
      MaxNumObstacles(max_num_obstacles),
      m_keepInterResults(false),
      m_tileCache(0),
      m_cacheBuildTimeMs(0),
      m_cacheCompressedSize(0),
      m_cacheRawSize(0),
      m_cacheLayerCount(0),
      m_cacheBuildMemUsage(0),
      m_maxTiles(0),
      m_maxPolysPerTile(0),
      m_cellSize(0),
      m_tcomp(0),
      m_geom(0),
      m_th(0),
      m_tw(0)
{
    m_ctx = nullptr;

    m_talloc = new LinearAllocator(32000);
    m_tcomp = new FastLZCompressor;
    m_tmproc = new MeshProcess;

    // Sanity check on tilesize
    if(m_tileSize < 16 || m_tileSize > 128)
        m_tileSize = 48;

    TempObstacleAdded    = false ;
    NavMeshDebugInstance = nullptr ;
}

OgreDetourTileCache::~OgreDetourTileCache()
{
    //    dtFreeNavMesh(m_navMesh);
    //    m_navMesh = 0;
    dtFreeTileCache(m_tileCache);
}

dtTileCache &
OgreDetourTileCache::
GetTileCache ()  const
{
   return *m_tileCache ;
}


bool OgreDetourTileCache::initTileCache()
{
    // BUILD TileCache
    dtFreeTileCache(m_tileCache);

    dtStatus status;

    m_tileCache = dtAllocTileCache();
    if (!m_tileCache)
    {
        m_recast->m_pLog->logMessage("ERROR: buildTiledNavigation: Could not allocate tile cache.");
        return false;
    }
    status = m_tileCache->init(&m_tcparams, m_talloc, m_tcomp, m_tmproc);
    if (dtStatusFailed(status))
    {
        m_recast->m_pLog->logMessage("ERROR: buildTiledNavigation: Could not init tile cache.");
        return false;
    }

    dtFreeNavMesh(m_recast->m_navMesh);

    m_recast->m_navMesh = dtAllocNavMesh();
    if (!m_recast->m_navMesh)
    {
        m_recast->m_pLog->logMessage("ERROR: buildTiledNavigation: Could not allocate navmesh.");
        return false;
    }


    // Init multi-tile navmesh parameters
    dtNavMeshParams params;
    memset(&params, 0, sizeof(params));
    rcVcopy(params.orig, m_tcparams.orig);   // Set world-space origin of tile grid
    params.tileWidth = m_tileSize*m_tcparams.cs;
    params.tileHeight = m_tileSize*m_tcparams.cs;
    params.maxTiles = m_maxTiles;
    params.maxPolys = m_maxPolysPerTile;

    status = m_recast->m_navMesh->init(&params);
    if (dtStatusFailed(status))
    {
        m_recast->m_pLog->logMessage("ERROR: buildTiledNavigation: Could not init navmesh.");
        return false;
    }

    // Init recast navmeshquery with created navmesh (in OgreRecast component)
    m_recast->m_navQuery = dtAllocNavMeshQuery();
    status = m_recast->m_navQuery->init(m_recast->m_navMesh, 2048);
    if (dtStatusFailed(status))
    {
        m_recast->m_pLog->logMessage("ERROR: buildTiledNavigation: Could not init Detour navmesh query");
        return false;
    }

    return true;
}

bool OgreDetourTileCache::TileCacheBuild(std::vector<Ogre::Entity*> srcMeshes,
                                         const TerrainAreaVector    &area_list )
{
    InputGeom *inputGeom = new InputGeom(srcMeshes);

    // Setup the terrain area volumes before the tile cache is built.
    // This will cause all of the areas marked to have the area id specified by AreaId.
    // The AreaId will then be used later to determine the area flags (such as walkability).
    // If this step is done after the tile cache is built then each tile will need to be rebuilt again and iterate over all of the
    // volumes again, taking a lot of time.
    for ( const auto &area : area_list )
    {
       const Ogre::Vector3 half_size = Ogre::Vector3 ( area.Width / 2.0f, 50.0f, area.Depth / 2.0f ) ;
       const Ogre::Vector3 min       = area.Centre - half_size ;
       const Ogre::Vector3 max       = area.Centre + half_size ;

       inputGeom->addConvexVolume ( new ConvexVolume ( Ogre::AxisAlignedBox ( min, max ), area.AreaId ) ) ;
    }

    // Init configuration for specified geometry
    configure(inputGeom);

    dtStatus status;

    // Preprocess tiles.
    // Prepares navmesh tiles in a 2D intermediary format that allows quick conversion to a 3D navmesh

//    ctx->resetTimers();

    m_cacheLayerCount = 0;
    m_cacheCompressedSize = 0;
    m_cacheRawSize = 0;

    for (int y = 0; y < m_th; ++y)
    {
        for (int x = 0; x < m_tw; ++x)
        {
            TileCacheData tiles[MAX_LAYERS];
            memset(tiles, 0, sizeof(tiles));
            int ntiles = rasterizeTileLayers(m_geom, x, y, m_cfg, tiles, MAX_LAYERS);  // This is where the tile is built

            for (int i = 0; i < ntiles; ++i)
            {
                TileCacheData* tile = &tiles[i];
                status = m_tileCache->addTile(tile->data, tile->dataSize, DT_COMPRESSEDTILE_FREE_DATA, 0);  // Add compressed tiles to tileCache
                if (dtStatusFailed(status))
                {
                    dtFree(tile->data);
                    tile->data = 0;
                    continue;
                }

                m_cacheLayerCount++;
                m_cacheCompressedSize += tile->dataSize;
                m_cacheRawSize += calcLayerBufferSize(m_tcparams.width, m_tcparams.height);
            }
        }
    }

    // Build initial meshes
    // Builds detour compatible navmesh from all tiles.
    // A tile will have to be rebuilt if something changes, eg. a temporary obstacle is placed on it.
//    ctx->startTimer(RC_TIMER_TOTAL);
    for (int y = 0; y < m_th; ++y)
        for (int x = 0; x < m_tw; ++x)
            m_tileCache->buildNavMeshTilesAt(x,y, m_recast->m_navMesh); // This immediately builds the tile, without the need of a dtTileCache::update()
//    ctx->stopTimer(RC_TIMER_TOTAL);

//    m_cacheBuildTimeMs = ctx->getAccumulatedTime(RC_TIMER_TOTAL)/1000.0f;
    m_cacheBuildMemUsage = m_talloc->high;


    // Count the total size of all generated tiles of the tiled navmesh
    const dtNavMesh* nav = m_recast->m_navMesh;
    int navmeshMemUsage = 0;
    for (int i = 0; i < nav->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = nav->getTile(i);
        if (tile->header)
            navmeshMemUsage += tile->dataSize;
    }



//    printf("navmeshMemUsage = %.1f kB\n", navmeshMemUsage/1024.0f);
    Ogre::LogManager::getSingletonPtr()->logMessage("Navmesh Mem Usage = "+ Ogre::StringConverter::toString(navmeshMemUsage/1024.0f) +" kB");
    Ogre::LogManager::getSingletonPtr()->logMessage("Tilecache Mem Usage = " +Ogre::StringConverter::toString(m_cacheCompressedSize/1024.0f) +" kB");


    return true;
}


//bool OgreDetourTileCache::buildTile(const int tx, const int ty, InputGeom *inputGeom)
//{
//    if (! isWithinBounds(tx, ty))
//        return false;
//
////TODO maybe I want to keep these values up to date
//    /*
//    m_cacheLayerCount = 0;
//    m_cacheCompressedSize = 0;
//    m_cacheRawSize = 0;
//    */
//
//    TileCacheData tiles[MAX_LAYERS];
//    memset(tiles, 0, sizeof(tiles));
//    int ntiles = rasterizeTileLayers(inputGeom, tx, ty, m_cfg, tiles, MAX_LAYERS);  // This is where the tile is built
//
//    dtStatus status;
//
//    // I don't know exactly why this can still be multiple tiles (??) (maybe because there could be tiles on multiple layers)
//    for (int i = 0; i < ntiles; ++i)
//    {
//        TileCacheData* tile = &tiles[i];
//
//        dtTileCacheLayerHeader* header = (dtTileCacheLayerHeader*)tile->data;
//        // Important: if a tile already exists at this position, first remove the old one or it will not be updated!
//        removeTile( m_tileCache->getTileRef(m_tileCache->getTileAt(header->tx, header->ty,header->tlayer)) );
//        status = m_tileCache->addTile(tile->data, tile->dataSize, DT_COMPRESSEDTILE_FREE_DATA, 0);  // Add compressed tiles to tileCache
//        if (dtStatusFailed(status))
//        {
//            dtFree(tile->data);
//            tile->data = 0;
//            continue;       // TODO maybe return false here?
//        }
//
//// TODO this has to be recalculated differently when rebuilding a tile
//        /*
//        m_cacheLayerCount++;
//        m_cacheCompressedSize += tile->dataSize;
//        m_cacheRawSize += calcLayerBufferSize(m_tcparams.width, m_tcparams.height);
//        */
//    }
//
////TODO add a deferred command for this?
//    // Build navmesh tile from cached tile
//    m_tileCache->buildNavMeshTilesAt(tx,ty, m_recast->m_navMesh); // This immediately builds the tile, without the need of a dtTileCache::update()
//
////TODO update this value?
//    //m_cacheBuildMemUsage = m_talloc->high;
//
//// TODO extract debug drawing to a separate class
//    //drawDetail(tx, ty);
//    if ( NavMeshDebugInstance )
//    {
//       NavMeshDebugInstance->RedrawTile ( tx, ty ) ;
//    }
//
//    return true;
//}

//bool
//OgreDetourTileCache::
//buildTile ( const dtCompressedTileRef &tile_ref, InputGeom *inputGeom )
//{
//   TileCacheData tiles[MAX_LAYERS];
//   memset(tiles, 0, sizeof(tiles));
//   int ntiles = 0;//rasterizeTileLayers(inputGeom, tx, ty, m_cfg, tiles, MAX_LAYERS);  // This is where the tile is built
//
//   dtStatus status;
//
//   // I don't know exactly why this can still be multiple tiles (??) (maybe because there could be tiles on multiple layers)
//   for (int i = 0; i < ntiles; ++i)
//   {
//      TileCacheData* tile = &tiles[i];
//
//      dtTileCacheLayerHeader* header = (dtTileCacheLayerHeader*)tile->data;
//      // Important: if a tile already exists at this position, first remove the old one or it will not be updated!
//      removeTile( m_tileCache->getTileRef(m_tileCache->getTileAt(header->tx, header->ty,header->tlayer)) );
//      status = m_tileCache->addTile(tile->data, tile->dataSize, DT_COMPRESSEDTILE_FREE_DATA, 0);  // Add compressed tiles to tileCache
//      if (dtStatusFailed(status))
//      {
//         dtFree(tile->data);
//         tile->data = 0;
//         continue ;       // TODO maybe return false here?
//      }
//   }
//
//   // Build navmesh tile from cached tile
//   //m_tileCache->buildNavMeshTilesAt(tx,ty, m_recast->m_navMesh); // This immediately builds the tile, without the need of a dtTileCache::update()
//
//   //drawDetail(tx, ty);
//
//   return true ;
//}

int OgreDetourTileCache::rasterizeTileLayers(InputGeom* geom, const int tx, const int ty, const rcConfig& cfg, TileCacheData* tiles, const int maxTiles)
{
    if (!geom || geom->isEmpty()) {
        m_recast->m_pLog->logMessage("ERROR: buildTile: Input mesh is not specified.");
        return 0;
    }

    if (!geom->getChunkyMesh()) {
        m_recast->m_pLog->logMessage("ERROR: buildTile: Input mesh has no chunkyTriMesh built.");
        return 0;
    }

//TODO make these member variables?
    FastLZCompressor comp;
    RasterizationContext rc;

    const float* verts = geom->getVerts();
    const int nverts = geom->getVertCount();

    // The chunky tri mesh in the inputgeom is a simple spatial subdivision structure that allows to
    // process the vertices in the geometry relevant to this part of the tile.
    // The chunky tri mesh is a grid of axis aligned boxes that store indices to the vertices in verts
    // that are positioned in that box.
    const rcChunkyTriMesh* chunkyMesh = geom->getChunkyMesh();

    // Tile bounds.
    const float tcs = m_tileSize * m_cellSize;

    rcConfig tcfg;
    memcpy(&tcfg, &m_cfg, sizeof(tcfg));

    tcfg.bmin[0] = m_cfg.bmin[0] + tx*tcs;
    tcfg.bmin[1] = m_cfg.bmin[1];
    tcfg.bmin[2] = m_cfg.bmin[2] + ty*tcs;
    tcfg.bmax[0] = m_cfg.bmin[0] + (tx+1)*tcs;
    tcfg.bmax[1] = m_cfg.bmax[1];
    tcfg.bmax[2] = m_cfg.bmin[2] + (ty+1)*tcs;
    tcfg.bmin[0] -= tcfg.borderSize*tcfg.cs;
    tcfg.bmin[2] -= tcfg.borderSize*tcfg.cs;
    tcfg.bmax[0] += tcfg.borderSize*tcfg.cs;
    tcfg.bmax[2] += tcfg.borderSize*tcfg.cs;


    // This is part of the regular recast navmesh generation pipeline as in OgreRecast::NavMeshBuild()
    // but only up till step 4 and slightly modified.


    // Allocate voxel heightfield where we rasterize our input data to.
    rc.solid = rcAllocHeightfield();
    if (!rc.solid)
    {
        m_recast->m_pLog->logMessage("ERROR: buildNavigation: Out of memory 'solid'.");
        return 0;
    }
    if (!rcCreateHeightfield(m_ctx, *rc.solid, tcfg.width, tcfg.height, tcfg.bmin, tcfg.bmax, tcfg.cs, tcfg.ch))
    {
        m_recast->m_pLog->logMessage("ERROR: buildNavigation: Could not create solid heightfield.");
        return 0;
    }

    // Allocate array that can hold triangle flags.
    // If you have multiple meshes you need to process, allocate
    // an array which can hold the max number of triangles you need to process.
    rc.triareas = new unsigned char[chunkyMesh->maxTrisPerChunk];
    if (!rc.triareas)
    {
        m_recast->m_pLog->logMessage("ERROR: buildNavigation: Out of memory 'm_triareas' ("+Ogre::StringConverter::toString(chunkyMesh->maxTrisPerChunk)+").");
        return 0;
    }

    float tbmin[2], tbmax[2];
    tbmin[0] = tcfg.bmin[0];
    tbmin[1] = tcfg.bmin[2];
    tbmax[0] = tcfg.bmax[0];
    tbmax[1] = tcfg.bmax[2];
    int cid[512];// TODO: Make grow when returning too many items.
    const int ncid = rcGetChunksOverlappingRect(chunkyMesh, tbmin, tbmax, cid, 512);
    if (!ncid)
    {
        return 0; // empty
    }

    for (int i = 0; i < ncid; ++i)
    {
        const rcChunkyTriMeshNode& node = chunkyMesh->nodes[cid[i]];
        const int* tris = &chunkyMesh->tris[node.i*3];
        const int ntris = node.n;

        memset(rc.triareas, 0, ntris*sizeof(unsigned char));
        rcMarkWalkableTriangles(m_ctx, tcfg.walkableSlopeAngle,
                                verts, nverts, tris, ntris, rc.triareas);

        rcRasterizeTriangles(m_ctx, verts, nverts, tris, rc.triareas, ntris, *rc.solid, tcfg.walkableClimb);
    }

    // Once all geometry is rasterized, we do initial pass of filtering to
    // remove unwanted overhangs caused by the conservative rasterization
    // as well as filter spans where the character cannot possibly stand.
    rcFilterLowHangingWalkableObstacles(m_ctx, tcfg.walkableClimb, *rc.solid);
    rcFilterLedgeSpans(m_ctx, tcfg.walkableHeight, tcfg.walkableClimb, *rc.solid);
    rcFilterWalkableLowHeightSpans(m_ctx, tcfg.walkableHeight, *rc.solid);


    rc.chf = rcAllocCompactHeightfield();
    if (!rc.chf)
    {
        m_recast->m_pLog->logMessage("ERROR: buildNavigation: Out of memory 'chf'.");
        return 0;
    }
    if (!rcBuildCompactHeightfield(m_ctx, tcfg.walkableHeight, tcfg.walkableClimb, *rc.solid, *rc.chf))
    {
        m_recast->m_pLog->logMessage("ERROR: buildNavigation: Could not build compact data.");
        return 0;
    }

    // Erode the walkable area by agent radius.
    if (!rcErodeWalkableArea(m_ctx, tcfg.walkableRadius, *rc.chf))
    {
        m_recast->m_pLog->logMessage("ERROR: buildNavigation: Could not erode.");
        return 0;
    }

    // Mark areas of dynamically added convex polygons
    const ConvexVolume* const* vols = geom->getConvexVolumes();
    for (int i  = 0; i < geom->getConvexVolumeCount(); ++i)
    {
       // TODO: Check if this is actually used, i.e. are there ever any convex volumes at this point?
       //       This causes the recast height map to be marked instead of the tile cache which would be done using dtMark...
       //       This may only affect the 'standard' navigation mesh, i.e. not used for a tiled navigation mesh.
        rcMarkConvexPolyArea(m_ctx, vols[i]->verts, vols[i]->nverts,
                             vols[i]->hmin, vols[i]->hmax,
                             (unsigned char)vols[i]->area, *rc.chf);
    }



    // Up till this part was more or less the same as OgreRecast::NavMeshBuild()
    // The following part is specific for creating a 2D intermediary navmesh tile.

    rc.lset = rcAllocHeightfieldLayerSet();
    if (!rc.lset)
    {
        m_recast->m_pLog->logMessage("ERROR: buildNavigation: Out of memory 'lset'.");
        return 0;
    }
    if (!rcBuildHeightfieldLayers(m_ctx, *rc.chf, tcfg.borderSize, tcfg.walkableHeight, *rc.lset))
    {
        m_recast->m_pLog->logMessage("ERROR: buildNavigation: Could not build heightfield layers.");
        return 0;
    }

    rc.ntiles = 0;
    for (int i = 0; i < rcMin(rc.lset->nlayers, MAX_LAYERS); ++i)
    {
        TileCacheData* tile = &rc.tiles[rc.ntiles++];
        const rcHeightfieldLayer* layer = &rc.lset->layers[i];

        // Store header
        dtTileCacheLayerHeader header;
        header.magic = DT_TILECACHE_MAGIC;
        header.version = DT_TILECACHE_VERSION;

        // Tile layer location in the navmesh.
        header.tx = tx;
        header.ty = ty;
        header.tlayer = i;
        dtVcopy(header.bmin, layer->bmin);
        dtVcopy(header.bmax, layer->bmax);

        // Tile info.
        header.width = (unsigned char)layer->width;
        header.height = (unsigned char)layer->height;
        header.minx = (unsigned char)layer->minx;
        header.maxx = (unsigned char)layer->maxx;
        header.miny = (unsigned char)layer->miny;
        header.maxy = (unsigned char)layer->maxy;
        header.hmin = (unsigned short)layer->hmin;
        header.hmax = (unsigned short)layer->hmax;

        dtStatus status = dtBuildTileCacheLayer(&comp, &header, layer->heights, layer->areas, layer->cons,
                                                &tile->data, &tile->dataSize);
        if (dtStatusFailed(status))
        {
            return 0;
        }
    }

    // Transfer ownsership of tile data from build context to the caller.
    int n = 0;
    for (int i = 0; i < rcMin(rc.ntiles, maxTiles); ++i)
    {
        tiles[n++] = rc.tiles[i];
        rc.tiles[i].data = 0;
        rc.tiles[i].dataSize = 0;
    }

    return n;
}



void
OgreDetourTileCache::
handleUpdate ( const float dt,
               const bool  until_up_to_date ) // Continue processing the tile cache obstacles until the entire navmesh is up-to-date
{
   if ( ! m_recast->m_navMesh )
   {
      return ;
   }

   if ( ! m_tileCache )
   {
      return ;
   }

   if ( ! until_up_to_date )
   {
      m_tileCache->update ( dt, m_recast->m_navMesh ) ;
   }
   else
   {
      bool up_to_date = false ;

      while ( ! up_to_date )
      {
         m_tileCache->update ( dt, m_recast->m_navMesh, &up_to_date ) ;
      }
   }

   if ( TempObstacleAdded &&
        NavMeshDebugInstance )
   {
      for ( int obstacle_index = 0 ; obstacle_index < m_tileCache->getObstacleCount () ; ++obstacle_index )
      {
         const dtTileCacheObstacle *obstacle = m_tileCache->getObstacle ( obstacle_index ) ;

         for ( int tile_index = 0 ; tile_index < obstacle->ntouched ; ++tile_index )
         {
            const dtCompressedTile *tile = m_tileCache->getTileByRef ( obstacle->touched [ tile_index ] ) ;

            if ( tile )
            {
               NavMeshDebugInstance->RedrawTile ( tile->header->tx, tile->header->ty ) ;
               //drawDetail ( tile->header->tx, tile->header->ty ) ;
            }
         }
      }

      TempObstacleAdded = false ;
   }
}

//void OgreDetourTileCache::getTileAtPos(const float* pos, int& tx, int& ty)
//{
////    if (!m_geom) return;
//// TODO is it correct to read from OgreRecast cfg here?
//    const float* bmin = m_recast->m_cfg.bmin;
//
//    const float ts = getTileSize();
//    tx = (int)((pos[0] - bmin[0]) / ts);
//    ty = (int)((pos[2] - bmin[2]) / ts);
//}
//
//Ogre::Vector2 OgreDetourTileCache::getTileAtPos(const Ogre::Vector3 pos)
//{
//    float position[3];
//    OgreRecast::OgreVect3ToFloatA(pos, position);
//    int tx; int ty;
//
//    getTileAtPos(position, tx, ty);
//
//    return Ogre::Vector2(tx, ty);
//}

//bool OgreDetourTileCache::tileExists(int tx, int ty)
//{
//    if(!isWithinBounds(tx,ty))
//        return false;
//
//    dtCompressedTileRef tiles;
//    int nTiles = m_tileCache->getTilesAt(tx, ty, &tiles, 1);
//
//    // Return wheter a tile exists on some layer at grid position (tx,ty)
//    return nTiles > 0;
//}

//bool OgreDetourTileCache::isWithinBounds(int tx, int ty)
//{
//    if (tx < 0 || tx >= m_tw)
//        return false;
//
//    if (ty < 0 || ty >= m_th)
//        return false;
//
//    return true;
//}
//
//bool OgreDetourTileCache::isWithinBounds(Ogre::Vector3 pos)
//{
//    Ogre::Vector2 tpos = getTileAtPos(pos);
//    return isWithinBounds((int)tpos.x, (int)tpos.y);
//}

//TileSelection OgreDetourTileCache::getBounds()
//{
//    TileSelection result;
//
//    result.minTx = 0;
//    result.minTy = 0;
//    result.maxTx = m_tw-1;    // TODO off by one?
//    result.maxTy = m_th-1;
//
//    result.bounds = getWorldSpaceBounds();
//
//    return result;
//}
//
//Ogre::AxisAlignedBox OgreDetourTileCache::getWorldSpaceBounds()
//{
//    Ogre::AxisAlignedBox result;
//    result.setExtents( Ogre::Vector3 ( m_cfg.bmin[0], m_cfg.bmin[1], m_cfg.bmin[2] ),
//                       Ogre::Vector3 ( m_cfg.bmax[0], m_cfg.bmax[1], m_cfg.bmax[2] ) ) ;
//
//    return result;
//}

//void OgreDetourTileCache::clearAllTempObstacles()
//{
//    if (!m_tileCache)
//        return;
//    for (int i = 0; i < m_tileCache->getObstacleCount(); ++i)
//    {
//        const dtTileCacheObstacle* ob = m_tileCache->getObstacle(i);
//        if (ob->state == DT_OBSTACLE_EMPTY) continue;
//        m_tileCache->removeObstacle(m_tileCache->getObstacleRef(ob));
//    }
//}

// TODO it's also possible, like with convex obstacles, to draw the changed navmesh (as cylindrical obstacles also involve re-rasterizing the navmesh tiles)
//dtObstacleRef OgreDetourTileCache::addTempObstacle(Ogre::Vector3 pos)
//{
//    if (!m_tileCache)
//        return 0;
//
//    float p[3];
//    OgreRecast::OgreVect3ToFloatA(pos, p);
//    p[1] -= 0.5f;
//    dtObstacleRef result;
//    dtStatus rval = m_tileCache->addObstacle(p, 45.0f / 2.0f, 20.0f, &result);
//    if(rval != DT_SUCCESS)
//        return 0;
//
//    TempObstacleAdded = true ;
//
//    return result;
//}
//
//dtObstacleRef
//OgreDetourTileCache::
//addTempObstacle ( const float                       &height,
//                  const std::vector <Ogre::Vector3> &verts )
//{
//   if ( ! m_tileCache )
//   {
//      return 0 ;
//   }
//
//   dtObstacleRef result ;
//   int           num_verticies = verts.size () ;
//
//   float *verts_array = new float [ 3 * num_verticies ] () ;
//
//   for ( int vert_index = 0 ; vert_index < num_verticies ; ++vert_index )
//   {
//      verts_array [ ( vert_index * 3 ) ]     = verts [ vert_index ].x ;
//      verts_array [ ( vert_index * 3 ) + 1 ] = verts [ vert_index ].y ;
//      verts_array [ ( vert_index * 3 ) + 2 ] = verts [ vert_index ].z ;
//   }
//
//   dtStatus rval = m_tileCache->addPolygonObstacle ( verts_array, num_verticies, height, &result ) ;
//
//   if ( rval != DT_SUCCESS )
//   {
//      return 0 ;
//   }
//
//   TempObstacleAdded = true ;
//
//   return result ;
//}

dtObstacleRef
OgreDetourTileCache::
AddObstacle ( const Ogre::Vector3  &min,
              const Ogre::Vector3  &max,
              const unsigned char  area_id,
              const unsigned short flags )
{
   dtObstacleRef result = 0 ;

   if ( m_tileCache )
   {
      float bmin [ 3 ] ;
      float bmax [ 3 ] ;
      OgreRecast::OgreVect3ToFloatA ( min, bmin ) ;
      OgreRecast::OgreVect3ToFloatA ( max, bmax ) ;

      if ( m_tileCache->addBoxObstacle ( bmin, bmax, &result, area_id, flags ) == DT_SUCCESS ) // No rotation
      {
         TempObstacleAdded = true ;
      }
   }

   return result ;
}

dtObstacleRef
OgreDetourTileCache::
AddObstacle ( const Ogre::Vector3  &centre,
              const float          width,
              const float          depth,
              const float          height,
              const float          y_rotation, // radians
              const unsigned char  area_id,
              const unsigned short flags )
{
   dtObstacleRef result = 0 ;

   if ( m_tileCache )
   {
      float centre_position [ 3 ] ;
      float half_extents [ 3 ] ;
      OgreRecast::OgreVect3ToFloatA ( centre, centre_position ) ;
      OgreRecast::OgreVect3ToFloatA ( Ogre::Vector3 ( width, height, depth ) / 2.0f, half_extents ) ;

      if ( m_tileCache->addBoxObstacle ( centre_position, half_extents, y_rotation, &result, area_id, flags ) == DT_SUCCESS )
      {
         TempObstacleAdded = true ;
      }
   }

   return result ;
}

const dtTileCacheObstacle *
OgreDetourTileCache::
getObstacleByRef ( dtObstacleRef ref )
{
   return m_tileCache->getObstacleByRef ( ref ) ;
}

//dtObstacleRef OgreDetourTileCache::removeTempObstacle(Ogre::Vector3 raySource, Ogre::Vector3 rayHit)
//{
//    if (!m_tileCache)
//        return 0;
//
//    float sp[3]; float sq[3];
//    OgreRecast::OgreVect3ToFloatA(raySource, sp);
//    OgreRecast::OgreVect3ToFloatA(rayHit, sq);
//
//    dtObstacleRef ref = hitTestObstacle(m_tileCache, sp, sq);
//    m_tileCache->removeObstacle(ref);
//
//    return ref;
//}

bool OgreDetourTileCache::RemoveObstacle(dtObstacleRef obstacleRef)
{
    if(m_tileCache->removeObstacle(obstacleRef) == DT_SUCCESS)
        return true;
    else
        return false;
}


//static bool isectSegAABB(const float* sp, const float* sq,
//                         const float* amin, const float* amax,
//                         float& tmin, float& tmax)
//{
//    static const float EPS = 1e-6f;
//
//    float d[3];
//    rcVsub(d, sq, sp);
//    tmin = 0;  // set to -FLT_MAX to get first hit on line
//    tmax = FLT_MAX;		// set to max distance ray can travel (for segment)
//
//    // For all three slabs
//    for (int i = 0; i < 3; i++)
//    {
//        if (fabsf(d[i]) < EPS)
//        {
//            // Ray is parallel to slab. No hit if origin not within slab
//            if (sp[i] < amin[i] || sp[i] > amax[i])
//                return false;
//        }
//        else
//        {
//            // Compute intersection t value of ray with near and far plane of slab
//            const float ood = 1.0f / d[i];
//            float t1 = (amin[i] - sp[i]) * ood;
//            float t2 = (amax[i] - sp[i]) * ood;
//            // Make t1 be intersection with near plane, t2 with far plane
//            if (t1 > t2) rcSwap(t1, t2);
//            // Compute the intersection of slab intersections intervals
//            if (t1 > tmin) tmin = t1;
//            if (t2 < tmax) tmax = t2;
//            // Exit with no collision as soon as slab intersection becomes empty
//            if (tmin > tmax) return false;
//        }
//    }
//
//    return true;
//}

//dtObstacleRef OgreDetourTileCache::hitTestObstacle(const dtTileCache* tc, const float* sp, const float* sq)
//{
//    float tmin = FLT_MAX;
//    const dtTileCacheObstacle* obmin = 0;
//    for (int i = 0; i < tc->getObstacleCount(); ++i)
//    {
//        const dtTileCacheObstacle* ob = tc->getObstacle(i);
//        if (ob->state == DT_OBSTACLE_EMPTY)
//            continue;
//
//        float bmin[3], bmax[3], t0,t1;
//        tc->getObstacleBounds(ob, bmin,bmax);
//
//        if (isectSegAABB(sp,sq, bmin,bmax, t0,t1))
//        {
//            if (t0 < tmin)
//            {
//                tmin = t0;
//                obmin = ob;
//            }
//        }
//    }
//    return tc->getObstacleRef(obmin);
//}

//void OgreDetourTileCache::drawNavMesh()
//{
//    for (int y = 0; y < m_th; ++y)
//    {
//        for (int x = 0; x < m_tw; ++x)
//        {
//            drawDetail(x, y);
//        }
//    }
//}
//
//void OgreDetourTileCache::drawDetail(const int tx, const int ty)
//{
//    if (!DEBUG_DRAW)
//        return; // Don't debug draw for huge performance gain!
//
//    struct TileCacheBuildContext
//    {
//        inline TileCacheBuildContext(struct dtTileCacheAlloc* a) : layer(0), lcset(0), lmesh(0), alloc(a) {}
//        inline ~TileCacheBuildContext() { purge(); }
//        void purge()
//        {
//            dtFreeTileCacheLayer(alloc, layer);
//            layer = 0;
//            dtFreeTileCacheContourSet(alloc, lcset);
//            lcset = 0;
//            dtFreeTileCachePolyMesh(alloc, lmesh);
//            lmesh = 0;
//        }
//        struct dtTileCacheLayer* layer;
//        struct dtTileCacheContourSet* lcset;
//        struct dtTileCachePolyMesh* lmesh;
//        struct dtTileCacheAlloc* alloc;
//    };
//
//    dtCompressedTileRef tiles[MAX_LAYERS];
//    const int ntiles = m_tileCache->getTilesAt(tx,ty,tiles,MAX_LAYERS);
//
//    dtTileCacheAlloc* talloc = m_tileCache->getAlloc();
//    dtTileCacheCompressor* tcomp = m_tileCache->getCompressor();
//    const dtTileCacheParams* params = m_tileCache->getParams();
//
//    for (int i = 0; i < ntiles; ++i)
//    {
//        const dtCompressedTile* tile = m_tileCache->getTileByRef(tiles[i]);
//
//        talloc->reset();
//
//        TileCacheBuildContext bc(talloc);
//        const int walkableClimbVx = (int)(params->walkableClimb / params->ch);
//        dtStatus status;
//
//        // Decompress tile layer data.
//        status = dtDecompressTileCacheLayer(talloc, tcomp, tile->data, tile->dataSize, &bc.layer);
//        if (dtStatusFailed(status))
//            return;
//
//        // Build navmesh
//        status = dtBuildTileCacheRegions(talloc, *bc.layer, walkableClimbVx);
//        if (dtStatusFailed(status))
//            return;
//
//        //TODO this part is replicated from navmesh tile building in DetourTileCache. Maybe that can be reused.
//        // Also is it really necessary to do an extra navmesh rebuild from compressed tile just to draw it?
//        // Can't I just draw it somewhere where the navmesh is rebuilt?
//        bc.lcset = dtAllocTileCacheContourSet(talloc);
//        if (!bc.lcset)
//            return;
//        status = dtBuildTileCacheContours(talloc, *bc.layer, walkableClimbVx,
//                                          params->maxSimplificationError, *bc.lcset);
//        if (dtStatusFailed(status))
//            return;
//
//        bc.lmesh = dtAllocTileCachePolyMesh(talloc);
//        if (!bc.lmesh)
//            return;
//        status = dtBuildTileCachePolyMesh(talloc, *bc.lcset, *bc.lmesh);
//        if (dtStatusFailed(status))
//            return;
//
//        // Draw navmesh
//        Ogre::String tileName = Ogre::StringConverter::toString(tiles[i]);
//        // Ogre::LogManager::getSingletonPtr()->logMessage("Drawing tile: "+tileName);
//
//        // TODO this is a dirty quickfix that should be gone as soon as there is a rebuildTile(tileref) method
//        drawPolyMesh ( tileName, *bc.lmesh, tile->header->bmin, params->cs, params->ch, *bc.layer ) ;
//    }
//}
//
//void OgreDetourTileCache::drawPolyMesh(const Ogre::String tileName, const struct dtTileCachePolyMesh &mesh, const float *orig, const float cs, const float ch, const struct dtTileCacheLayer &regionLayers, bool colorRegions)
//{
//   const int nvp = mesh.nvp;
//
//   const unsigned short* verts = mesh.verts;
//   const unsigned short* polys = mesh.polys;
//   const unsigned char* areas = mesh.areas;
//   const unsigned char *regs = regionLayers.regs;
//   const int nverts = mesh.nverts;
//   const int npolys = mesh.npolys;
//   const int maxpolys = m_maxPolysPerTile;
//
//   unsigned short *regions = new unsigned short[npolys];
//   for (int i =0; i< npolys; i++) {
//         regions[i] = (const unsigned short)regs[i];
//   }
//
//   m_recast->CreateRecastPolyMesh(tileName, verts, nverts, polys, npolys, areas, maxpolys, regions, nvp, cs, ch, orig, colorRegions);
//
//   delete[] regions;
//}


// TODO I need to redraw the changed tiles!!
//int
//OgreDetourTileCache::
//addConvexShapeObstacle ( ConvexVolume     *obstacle,
//                         Ogre::Quaternion orientation,
//                         Ogre::Vector3    pivot_point )
//{
//    // Add convex shape to input geometry
//   int result = m_geom->addConvexVolume ( obstacle ) ;
//
//   if ( orientation != Ogre::Quaternion::IDENTITY )
//   {
//      m_geom->applyOrientation ( orientation, pivot_point ) ;
//   }
//
//   if ( result == -1 )
//   {
//      return result ;
//   }
//
//   // Determine which navmesh tiles have to be updated
//   // Borrowed from detourTileCache::update()
//   // Find touched tiles using obstacle bounds.
//   int                 ntouched = 0 ;
//   dtCompressedTileRef touched [ DT_MAX_TOUCHED_TILES ] ;
//
//   m_tileCache->queryTiles ( obstacle->bmin, obstacle->bmax, touched, &ntouched, DT_MAX_TOUCHED_TILES ) ;
//
//   // Rebuild affected tiles
//   // TODO maybe defer this and timeslice it, like happend in dtTileCache with tempObstacle updates
//   for ( int i = 0 ; i < ntouched ; ++i )
//   {
//      // TODO when you do deffered commands, make sure you issue a rebuild for a tile only once per update, so remove doubles from the request queue (this is what contains() is for in dtTileCache)
//      // Retrieve coordinates of tile that has to be rebuilt
//      const dtCompressedTile* tile = m_tileCache->getTileByRef ( touched [ i ] ) ;
//
//      // If it is null, tile is already rebuilt (and has a new ref ID)
//      if ( tile )
//      {
//         int tx = tile->header->tx ;
//         int ty = tile->header->ty ;
//         tile = NULL ;
//
//         // TODO we actually want a buildTile method with a tileRef as input param.
//         // As this method does a bounding box intersection with tiles again, which might result in multiple tiles being rebuilt (which will lead to nothing because only one tile is removed..),
//         // and we determined which tiles to bebuild already, anyway (using queryTiles)
//         buildTile ( tx, ty, m_geom ) ;  // Might rebuild multiple tiles
//      }
//
//   }
//
//   return result;
//}
//
//bool
//OgreDetourTileCache::
//addConvexShapeObstacles ( const std::vector <ConvexVolume *> &obstacle_list,
//                          std::vector <int>                  &obstacle_ref_list )
//{
//   bool all_added = true ;
//   std::vector <std::vector <dtCompressedTileRef>> touched_tile_list ;
//
//   // Add convex shape to input geometry
//   for ( unsigned int obstacle_index = 0 ; obstacle_index < obstacle_list.size () ; ++obstacle_index )
//   {
//      int result = m_geom->addConvexVolume ( obstacle_list [ obstacle_index ] ) ;
//
//      if ( result != -1 )
//      {
//         int                               ntouched = 0 ;
//         dtCompressedTileRef               touched [ DT_MAX_TOUCHED_TILES ] ;
//         std::vector <dtCompressedTileRef> touched_list ;
//
//         m_tileCache->queryTiles ( obstacle_list [ obstacle_index ]->bmin, obstacle_list [ obstacle_index ]->bmax, touched, &ntouched, DT_MAX_TOUCHED_TILES ) ;
//
//         for ( int touched_index = 0 ; touched_index < ntouched ; ++touched_index )
//         {
//            touched_list.push_back ( touched [ touched_index ] ) ;
//         }
//
//         if ( touched_list.size () > 0 )
//         {
//            touched_tile_list.push_back ( touched_list ) ;
//         }
//
//         obstacle_ref_list.push_back ( result ) ;
//      }
//      else
//      {
//         all_added = false ;
//      }
//
//      //if ( orientation != Ogre::Quaternion::IDENTITY )
//      //{
//         //m_geom->applyOrientation ( orientation, pivot_point ) ;
//      //}
//   }
//
//   // Rebuild affected tiles
//   for ( unsigned int touched_list_index = 0 ; touched_list_index < touched_tile_list.size () ; ++touched_list_index )
//   {
//      std::vector <dtCompressedTileRef> tile_list = touched_tile_list [ touched_list_index ] ;
//
//      for ( unsigned int tile_index = 0 ; tile_index < tile_list.size () ; ++tile_index )
//      {
//         const dtCompressedTile *tile = m_tileCache->getTileByRef ( tile_list [ tile_index ] ) ;
//
//         // If it is null, tile is already rebuilt (and has a new ref ID)
//         if ( tile )
//         {
//            int tx = tile->header->tx ;
//            int ty = tile->header->ty ;
//            tile = NULL ;
//
//            // TODO we actually want a buildTile method with a tileRef as input param.
//            // As this method does a bounding box intersection with tiles again, which might result in multiple tiles being rebuilt (which will lead to nothing because only one tile is removed..),
//            // and we determined which tiles to bebuild already, anyway (using queryTiles)
//            buildTile ( tx, ty, m_geom ) ; // Might rebuild multiple tiles
//         }
//      }
//   }
//
//   return all_added ;
//}

bool OgreDetourTileCache::configure(InputGeom *inputGeom)
{
    m_geom = inputGeom;

    // Reuse OgreRecast context for tiled navmesh building
    m_ctx = m_recast->m_ctx;

    if (!m_geom || m_geom->isEmpty()) {
        m_recast->m_pLog->logMessage("ERROR: OgreDetourTileCache::configure: No vertices and triangles.");
        return false;
    }

    if (!m_geom->getChunkyMesh()) {
        m_recast->m_pLog->logMessage("ERROR: OgreDetourTileCache::configure: Input mesh has no chunkyTriMesh built.");
        return false;
    }

    m_tmproc->init(m_geom);


    // Init cache bounding box
    const float* bmin = m_geom->getMeshBoundsMin();
    const float* bmax = m_geom->getMeshBoundsMax();

    // Navmesh generation params.
    // Use config from recast module
    m_cfg = m_recast->m_cfg;

    // Most params are taken from OgreRecast::configure, except for these:
    m_cfg.tileSize = m_tileSize;
    m_cfg.borderSize = (int) (m_cfg.walkableRadius + BORDER_PADDING); // Reserve enough padding.
    m_cfg.width = m_cfg.tileSize + m_cfg.borderSize*2;
    m_cfg.height = m_cfg.tileSize + m_cfg.borderSize*2;

    // Set mesh bounds
    rcVcopy(m_cfg.bmin, bmin);
    rcVcopy(m_cfg.bmax, bmax);
    // Also define navmesh bounds in recast component
    rcVcopy(m_recast->m_cfg.bmin, bmin);
    rcVcopy(m_recast->m_cfg.bmax, bmax);

    // Cell size navmesh generation property is copied from OgreRecast config
    m_cellSize = m_cfg.cs;

    // Determine grid size (number of tiles) based on bounding box and grid cell size
    int gw = 0, gh = 0;
    rcCalcGridSize(bmin, bmax, m_cellSize, &gw, &gh);   // Calculates total size of voxel grid
    const int ts = m_tileSize;
    const int tw = (gw + ts-1) / ts;    // Tile width
    const int th = (gh + ts-1) / ts;    // Tile height
    m_tw = tw;
    m_th = th;
    Ogre::LogManager::getSingletonPtr()->logMessage("Total Voxels: "+Ogre::StringConverter::toString(gw) + " x " + Ogre::StringConverter::toString(gh));
    Ogre::LogManager::getSingletonPtr()->logMessage("Tilesize: "+Ogre::StringConverter::toString(m_tileSize)+"  Cellsize: "+Ogre::StringConverter::toString(m_cellSize));
    Ogre::LogManager::getSingletonPtr()->logMessage("Tiles: "+Ogre::StringConverter::toString(m_tw)+" x "+Ogre::StringConverter::toString(m_th));


    // Max tiles and max polys affect how the tile IDs are caculated.
    // There are 22 bits available for identifying a tile and a polygon.
    int tileBits = rcMin((int)dtIlog2(dtNextPow2(tw*th*EXPECTED_LAYERS_PER_TILE)), 14);
    if (tileBits > 14) tileBits = 14;
    int polyBits = 22 - tileBits;
    m_maxTiles = 1 << tileBits;
    m_maxPolysPerTile = 1 << polyBits;
    Ogre::LogManager::getSingletonPtr()->logMessage("Max Tiles: " + Ogre::StringConverter::toString(m_maxTiles));
    Ogre::LogManager::getSingletonPtr()->logMessage("Max Polys: " + Ogre::StringConverter::toString(m_maxPolysPerTile));


    // Tile cache params.
    memset(&m_tcparams, 0, sizeof(m_tcparams));
    rcVcopy(m_tcparams.orig, bmin);
    m_tcparams.width = m_tileSize;
    m_tcparams.height = m_tileSize;
    m_tcparams.maxTiles = tw*th*EXPECTED_LAYERS_PER_TILE;
    m_tcparams.maxObstacles = MaxNumObstacles;    // Max number of temp obstacles that can be added to or removed from navmesh

    // Copy the rest of the parameters from OgreRecast config
    m_tcparams.cs = m_cfg.cs;
    m_tcparams.ch = m_cfg.ch;
    m_tcparams.walkableHeight = (float) m_cfg.walkableHeight;
    m_tcparams.walkableRadius = (float) m_cfg.walkableRadius;
    m_tcparams.walkableClimb = (float) m_cfg.walkableClimb;
    m_tcparams.maxSimplificationError = m_cfg.maxSimplificationError;

    return initTileCache();
}

TileSelection OgreDetourTileCache::getTileSelection(const Ogre::AxisAlignedBox &selectionArea)
{
// TODO Account for origin? Have a look at dtTileCache::queryTiles()
    TileSelection result;

    // Verify whether area to select falls within tilecache bounds, otherwise clip
    Ogre::Vector3 min = selectionArea.getMinimum();
    if (min.x < m_cfg.bmin[0])
        min.x = m_cfg.bmin[0];
    if (min.z < m_cfg.bmin[2])
        min.z = m_cfg.bmin[2];
    if (min.x > m_cfg.bmax[0])
        min.x = m_cfg.bmax[0];
    if (min.z > m_cfg.bmax[2])
        min.z = m_cfg.bmax[2];

    Ogre::Vector3 max = selectionArea.getMaximum();
    if (max.x < m_cfg.bmin[0])
        max.x = m_cfg.bmin[0];
    if (max.z < m_cfg.bmin[2])
        max.z = m_cfg.bmin[2];
    if (max.x > m_cfg.bmax[0])
        max.x = m_cfg.bmax[0];
    if (max.z > m_cfg.bmax[2])
        max.z = m_cfg.bmax[2];


    // Width of one tile in world units
    float tileWidth = getTileSize();

    // Calculate tile index range that falls within bounding box
    result.minTx = (min.x - m_cfg.bmin[0]) / tileWidth;
    result.maxTx = (max.x - m_cfg.bmin[0]) / tileWidth;
    result.minTy = (min.z - m_cfg.bmin[2]) / tileWidth;
    result.maxTy = (max.z - m_cfg.bmin[2]) / tileWidth;
        // TODO you can also go the other route: using cellsize and tilesize

// Let's assume these will be correct for a small performance gain
/*
    // Assert tx and ty are within index bounds, otherwise clip
    if (result.minTx < 0)
        result.minTx = 0;
    if (result.maxTx < 0)
        result.maxTx = 0;
    if (result.minTx > m_tw)
        result.minTx = m_tw;
    if (result.maxTx > m_tw)
        result.maxTx = m_tw;

    if (result.minTy < 0)
        result.minTy = 0;
    if (result.maxTy < 0)
        result.maxTy = 0;
    if (result.minTy > m_th)
        result.minTy = m_th;
    if (result.maxTy > m_th)
        result.maxTy = m_th;
*/

    // Calculate proper bounds aligned to tile bounds
    min.x = m_cfg.bmin[0] + (result.minTx * tileWidth);
    min.y = m_cfg.bmin[1];
    min.z = m_cfg.bmin[2] + (result.minTy * tileWidth);

    max.x = m_cfg.bmin[0] + ((result.maxTx+1) * tileWidth);
    max.y = m_cfg.bmax[1];
    max.z = m_cfg.bmin[2] + ((result.maxTy+1) * tileWidth);
// TODO does this box need to be offset a little further with borders?


    // Return result
    result.bounds.setExtents(min,
                             max);

    return result;
}

//Ogre::AxisAlignedBox OgreDetourTileCache::getTileBounds(int tx, int ty)
//{
//    const float* bmin = m_recast->m_cfg.bmin;
//    const float ts = getTileSize();
//
//
//    Ogre::AxisAlignedBox result;
//
//
//    result.setExtents( Ogre::Vector3 (
//                bmin[0] + tx * ts,
//                bmin[1],
//                bmin[2] + ty * ts
//                  ),
//                Ogre::Vector3 (
//                   bmin[0] + (tx+1) * ts,
//                   m_recast->m_cfg.bmax[1],
//                   bmin[2] + (ty+1) * ts
//                ) ) ;
//
//    return result;
//}

bool OgreDetourTileCache::removeTile(dtCompressedTileRef tileRef)
{
    if(!tileRef)
        return false;

    Ogre::LogManager::getSingletonPtr()->logMessage("Removed tile "+Ogre::StringConverter::toString(tileRef));

    dtStatus status = m_tileCache->removeTile(tileRef, NULL, NULL);
        // RemoveTile also returns the data of the removed tile if you supply the second and third parameter

    if (status != DT_SUCCESS)
        return false;

    return true;
}

//Ogre::AxisAlignedBox OgreDetourTileCache::getTileAlignedBox(const Ogre::AxisAlignedBox &selectionArea)
//{
//    return getTileSelection(selectionArea).bounds;
//}

//void OgreDetourTileCache::buildTiles(InputGeom *inputGeom, const Ogre::AxisAlignedBox *areaToUpdate)
//{
//    // Use bounding box from inputgeom if no area was explicitly specified
//    Ogre::AxisAlignedBox updateArea;
//    if(!areaToUpdate)
//        updateArea = inputGeom->getBoundingBox();
//    else
//        updateArea = *areaToUpdate;
//
//    // Reduce bounding area a little with one cell in size, to be sure that if it was already tile-aligned, we don't select an extra tile
//    updateArea.setExtents( updateArea.getMinimum() + Ogre::Vector3(m_cellSize, 0, m_cellSize),
//                           updateArea.getMaximum() - Ogre::Vector3(m_cellSize, 0, m_cellSize) );
//
//    // Select tiles to build or rebuild (builds a tile-aligned BB)
//    TileSelection selection = getTileSelection(updateArea);
//
//    int tilesToBuildX = (selection.maxTx - selection.minTx)+1;  // Tile ranges are inclusive
//    int tilesToBuildY = (selection.maxTy - selection.minTy)+1;
//    if(tilesToBuildX * tilesToBuildY > 5)
//        Ogre::LogManager::getSingletonPtr()->logMessage("Building "+Ogre::StringConverter::toString(tilesToBuildX)+" x "+Ogre::StringConverter::toString(tilesToBuildY)+" navmesh tiles.");
//
//    // Build tiles
//    for (int ty = selection.minTy; ty <= selection.maxTy; ty++) {
//        for (int tx = selection.minTx; tx <= selection.maxTx; tx++) {
//            buildTile(tx, ty, inputGeom);
//        }
//    }
//}

//void OgreDetourTileCache::buildTiles(std::vector<Ogre::Entity*> srcEntities, const Ogre::AxisAlignedBox *areaToUpdate)
//{
//    InputGeom geom = InputGeom(srcEntities, getTileAlignedBox(*areaToUpdate));
//    buildTiles(&geom);
//}

//void OgreDetourTileCache::unloadTiles(const Ogre::AxisAlignedBox &areaToUpdate)
//{
//    // Determine which navmesh tiles have to be removed
//    float bmin[3], bmax[3];
//    OgreRecast::OgreVect3ToFloatA(areaToUpdate.getMinimum(), bmin);
//    OgreRecast::OgreVect3ToFloatA(areaToUpdate.getMaximum(), bmax);
//    dtCompressedTileRef touched[DT_MAX_TOUCHED_TILES];
//    int ntouched = 0;
//    m_tileCache->queryTiles(bmin, bmax, touched, &ntouched, DT_MAX_TOUCHED_TILES);
//
//    // Remove tiles
//    for (int i = 0; i < ntouched; ++i)
//    {
//        removeTile(touched[i]);
//    }
//}

//ConvexVolume* OgreDetourTileCache::getConvexShapeObstacle(int obstacleIndex)
//{
//    return m_geom->getConvexVolume(obstacleIndex);
//}

//int OgreDetourTileCache::getConvexShapeObstacleId(ConvexVolume *convexHull)
//{
//    return m_geom->getConvexVolumeId(convexHull);
//}

//bool OgreDetourTileCache::removeConvexShapeObstacle(ConvexVolume* convexHull)
//{
//    int id = getConvexShapeObstacleId(convexHull);
//    return removeConvexShapeObstacleById(id);
//}

//bool OgreDetourTileCache::removeConvexShapeObstacleById(int obstacleIndex, ConvexVolume** removedVolume)
//{
//    ConvexVolume* obstacle;
//    if(! m_geom->deleteConvexVolume(obstacleIndex, &obstacle))
//        return false;
//
//    if(removedVolume != NULL)
//        *removedVolume = obstacle;
//
////TODO For removing a convex volume again, store a reference to the impacted tiles in the convex volume so they can be found quickly
//    int ntouched = 0;
//    dtCompressedTileRef touched[DT_MAX_TOUCHED_TILES];
//    m_tileCache->queryTiles(obstacle->bmin, obstacle->bmax, touched, &ntouched, DT_MAX_TOUCHED_TILES);
//
//    // Rebuild affected tiles
//// TODO maybe defer this and timeslice it, like happens in dtTileCache with tempObstacle updates
//    for (int i = 0; i < ntouched; ++i)
//    {
//// TODO when you do deffered commands, make sure you issue a rebuild for a tile only once per update, so remove doubles from the request queue (this is what contains() is for in dtTileCache)
//        // Retrieve coordinates of tile that has to be rebuilt
//        const dtCompressedTile* tile = m_tileCache->getTileByRef(touched[i]);
//
//        // If it is null, tile is already rebuilt (and has a new ref ID)
//        if(tile) {
//            int tx = tile->header->tx;
//            int ty = tile->header->ty;
//            tile = NULL;
//
//            // Issue full rebuild from inputGeom, with the specified convex shape removed, for this tile
//            buildTile(tx, ty, m_geom);
//        }
//    }
//
//    return true;
//}

//bool
//OgreDetourTileCache::
//removeConvexShapeObstaclesById ( const std::vector <int> &obstacleIndexList )
//{
//   bool                                            all_removed = true ;
//   std::vector <std::vector <dtCompressedTileRef>> touched_tile_list ;
//
//   for ( auto obstacle_index : obstacleIndexList )
//   {
//      ConvexVolume *obstacle ;
//
//      int result = m_geom->deleteConvexVolume ( obstacle_index, &obstacle ) ;
//
//      if ( result > 0 )
//      {
//         int                               ntouched = 0 ;
//         dtCompressedTileRef               touched [ DT_MAX_TOUCHED_TILES ] ;
//         std::vector <dtCompressedTileRef> touched_list ;
//
//         m_tileCache->queryTiles ( obstacle->bmin, obstacle->bmax, touched, &ntouched, DT_MAX_TOUCHED_TILES ) ;
//
//         for ( int touched_index = 0 ; touched_index < ntouched ; ++touched_index )
//         {
//            touched_list.push_back ( touched [ touched_index ] ) ;
//         }
//
//         if ( touched_list.size () > 0 )
//         {
//            touched_tile_list.push_back ( touched_list ) ;
//         }
//      }
//      else
//      {
//         all_removed = false ;
//      }
//   }
//
//   // Rebuild affected tiles
//   for ( unsigned int touched_list_index = 0 ; touched_list_index < touched_tile_list.size () ; ++touched_list_index )
//   {
//      std::vector <dtCompressedTileRef> tile_list = touched_tile_list [ touched_list_index ] ;
//
//      for ( unsigned int tile_index = 0 ; tile_index < tile_list.size () ; ++tile_index )
//      {
//         const dtCompressedTile *tile = m_tileCache->getTileByRef ( tile_list [ tile_index ] ) ;
//
//         // If it is null, tile is already rebuilt (and has a new ref ID)
//         if ( tile )
//         {
//            int tx = tile->header->tx ;
//            int ty = tile->header->ty ;
//            tile = NULL ;
//
//            // TODO we actually want a buildTile method with a tileRef as input param.
//            // As this method does a bounding box intersection with tiles again, which might result in multiple tiles being rebuilt (which will lead to nothing because only one tile is removed..),
//            // and we determined which tiles to bebuild already, anyway (using queryTiles)
//            buildTile ( tx, ty, m_geom ) ; // Might rebuild multiple tiles
//         }
//      }
//   }
//
//   return all_removed ;
//}

//int OgreDetourTileCache::hitTestConvexShapeObstacle(Ogre::Vector3 raySource, Ogre::Vector3 rayHit)
//{
//    float sp[3]; float sq[3];
//    OgreRecast::OgreVect3ToFloatA(raySource, sp);
//    OgreRecast::OgreVect3ToFloatA(rayHit, sq);
//
//    int shapeIdx = m_geom->hitTestConvexVolume(sp, sq);
//
//    return shapeIdx;
//}

//int OgreDetourTileCache::removeConvexShapeObstacle(Ogre::Vector3 raySource, Ogre::Vector3 rayHit, ConvexVolume** removedVolume)
//{
//    int shapeIdx = hitTestConvexShapeObstacle(raySource, rayHit);
//
//    if (shapeIdx == -1)
//        return -1;
//
//    removeConvexShapeObstacleById(shapeIdx, removedVolume);
//    return shapeIdx;
//}


//std::vector<dtCompressedTileRef> OgreDetourTileCache::getTilesAroundPoint(Ogre::Vector3 point, Ogre::Real radius)
//{
//    std::vector<dtCompressedTileRef> result;
//
//    // calculate bounds
//    float bmin[3]; float bmax[3];
//    bmin[0] = point.x - radius;
//    bmin[1] = point.y - radius;
//    bmin[2] = point.z - radius;
//    bmax[0] = point.x + radius;
//    bmax[1] = point.y + radius;
//    bmax[2] = point.z + radius;
//
//    dtCompressedTileRef results[DT_MAX_TOUCHED_TILES];
//    int resultCount = 0;
//    m_tileCache->queryTiles(bmin, bmax, results, &resultCount, DT_MAX_TOUCHED_TILES);
//
//    if (resultCount > 0)
//        result.assign(results, results + resultCount);
//
//    return result;
//}

//std::vector<dtCompressedTileRef> OgreDetourTileCache::getTilesContainingBox(Ogre::Vector3 boxMin, Ogre::Vector3 boxMax)
//{
//    std::vector<dtCompressedTileRef> result;
//
//    // calculate bounds
//    float bmin[3]; float bmax[3];
//    OgreRecast::OgreVect3ToFloatA(boxMin, bmin);
//    OgreRecast::OgreVect3ToFloatA(boxMax, bmax);
//
//    dtCompressedTileRef results[DT_MAX_TOUCHED_TILES];
//    int resultCount = 0;
//    m_tileCache->queryTiles(bmin, bmax, results, &resultCount, DT_MAX_TOUCHED_TILES);
//
//    if (resultCount > 0)
//        result.assign(results, results + resultCount);
//
//    return result;
//}

bool OgreDetourTileCache::saveAll(Ogre::String filename)
{
    if (!m_tileCache) {
        Ogre::LogManager::getSingletonPtr()->logMessage("Error: OgreDetourTileCache::saveAll("+filename+"). Could not save tilecache, no tilecache to save.");
        return false;
    }

       FILE* fp = fopen(filename.data(), "wb");
       if (!fp) {
           Ogre::LogManager::getSingletonPtr()->logMessage("Error: OgreDetourTileCache::saveAll("+filename+"). Could not save file.");
           return false;
       }

// Store header.
       TileCacheSetHeader header;
       header.magic = TILECACHESET_MAGIC;
       header.version = TILECACHESET_VERSION;
       header.numTiles = 0;
       for (int i = 0; i < m_tileCache->getTileCount(); ++i)
       {
               const dtCompressedTile* tile = m_tileCache->getTile(i);
               if (!tile || !tile->header || !tile->dataSize) continue;
               header.numTiles++;
       }
       memcpy(&header.cacheParams, m_tileCache->getParams(), sizeof(dtTileCacheParams));
       memcpy(&header.meshParams, m_recast->m_navMesh->getParams(), sizeof(dtNavMeshParams));
       memcpy(&header.recastConfig, &m_cfg, sizeof(rcConfig));
       fwrite(&header, sizeof(TileCacheSetHeader), 1, fp);

       // Store tiles.
       for (int i = 0; i < m_tileCache->getTileCount(); ++i)
       {
               const dtCompressedTile* tile = m_tileCache->getTile(i);
               if (!tile || !tile->header || !tile->dataSize) continue;

               TileCacheTileHeader tileHeader;
               tileHeader.tileRef = m_tileCache->getTileRef(tile);
               tileHeader.dataSize = tile->dataSize;
               fwrite(&tileHeader, sizeof(tileHeader), 1, fp);

               fwrite(tile->data, tile->dataSize, 1, fp);
       }

       fclose(fp);
       return true;
}

bool OgreDetourTileCache::loadAll(Ogre::String filename,
                                  std::vector<Ogre::Entity*> srcMeshes)
{
       FILE* fp = fopen(filename.data(), "rb");
       if (!fp) {
           Ogre::LogManager::getSingletonPtr()->logMessage("Error: OgreDetourTileCache::loadAll("+filename+"). Could not open file.");
           return false;
       }

       // Read header.
       TileCacheSetHeader header;
       fread(&header, sizeof(TileCacheSetHeader), 1, fp);
       if (header.magic != TILECACHESET_MAGIC)
       {
           fclose(fp);
           Ogre::LogManager::getSingletonPtr()->logMessage("Error: OgreDetourTileCache::loadAll("+filename+"). File does not appear to contain valid tilecache data.");
           return false;
       }
       if (header.version != TILECACHESET_VERSION)
       {
           fclose(fp);
           Ogre::LogManager::getSingletonPtr()->logMessage("Error: OgreDetourTileCache::loadAll("+filename+"). File contains a different version of the tilecache data format ("+Ogre::StringConverter::toString(header.version)+" instead of "+Ogre::StringConverter::toString(TILECACHESET_VERSION)+").");
           return false;
       }

       m_recast->m_navMesh = dtAllocNavMesh();
       if (!m_recast->m_navMesh)
       {
           fclose(fp);
           Ogre::LogManager::getSingletonPtr()->logMessage("Error: OgreDetourTileCache::loadAll("+filename+"). Could not allocate navmesh.");
           return false;
       }
       dtStatus status = m_recast->m_navMesh->init(&header.meshParams);
       if (dtStatusFailed(status))
       {
           fclose(fp);
           Ogre::LogManager::getSingletonPtr()->logMessage("Error: OgreDetourTileCache::loadAll("+filename+"). Could not init navmesh.");
           return false;
       }

       m_tileCache = dtAllocTileCache();
       if (!m_tileCache)
       {
           fclose(fp);
           Ogre::LogManager::getSingletonPtr()->logMessage("Error: OgreDetourTileCache::loadAll("+filename+"). Could not allocate tilecache.");
           return false;
       }
       status = m_tileCache->init(&header.cacheParams, m_talloc, m_tcomp, m_tmproc);
       if (dtStatusFailed(status))
       {
           fclose(fp);
           Ogre::LogManager::getSingletonPtr()->logMessage("Error: OgreDetourTileCache::loadAll("+filename+"). Could not init tilecache.");
           return false;
       }

       memcpy(&m_cfg, &header.recastConfig, sizeof(rcConfig));

       // Read tiles.
       for (int i = 0; i < header.numTiles; ++i)
       {
               TileCacheTileHeader tileHeader;
               fread(&tileHeader, sizeof(tileHeader), 1, fp);
               if (!tileHeader.tileRef || !tileHeader.dataSize)
                       break;

               unsigned char* data = (unsigned char*)dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM);
               if (!data) break;
               memset(data, 0, tileHeader.dataSize);
               fread(data, tileHeader.dataSize, 1, fp);

               dtCompressedTileRef tile = 0;
               m_tileCache->addTile(data, tileHeader.dataSize, DT_COMPRESSEDTILE_FREE_DATA, &tile);

               if (tile)
                       m_tileCache->buildNavMeshTile(tile, m_recast->m_navMesh);
       }

       fclose(fp);


       // Init recast navmeshquery with created navmesh (in OgreRecast component)
       m_recast->m_navQuery = dtAllocNavMeshQuery();
       m_recast->m_navQuery->init(m_recast->m_navMesh, 2048);


       // Config
       // TODO handle this nicer, also inputGeom is not inited, making some functions crash
       m_cellSize = m_cfg.cs;
       m_tileSize = m_cfg.tileSize;

       // cache bounding box
       const float* bmin = m_cfg.bmin;
       const float* bmax = m_cfg.bmax;

       // Copy loaded config back to recast module
       memcpy(&m_recast->m_cfg, &m_cfg, sizeof(rcConfig));

       m_tileSize = m_cfg.tileSize;
       m_cellSize = m_cfg.cs;
       m_tcparams = header.cacheParams;

       // Determine grid size (number of tiles) based on bounding box and grid cell size
       int gw = 0, gh = 0;
       rcCalcGridSize(bmin, bmax, m_cellSize, &gw, &gh);   // Calculates total size of voxel grid
       const int ts = m_tileSize;
       const int tw = (gw + ts-1) / ts;    // Tile width
       const int th = (gh + ts-1) / ts;    // Tile height
       m_tw = tw;
       m_th = th;


       Ogre::LogManager::getSingletonPtr()->logMessage("Total Voxels: "+Ogre::StringConverter::toString(gw) + " x " + Ogre::StringConverter::toString(gh));
       Ogre::LogManager::getSingletonPtr()->logMessage("Tilesize: "+Ogre::StringConverter::toString(m_tileSize)+"  Cellsize: "+Ogre::StringConverter::toString(m_cellSize));
       Ogre::LogManager::getSingletonPtr()->logMessage("Tiles: "+Ogre::StringConverter::toString(m_tw)+" x "+Ogre::StringConverter::toString(m_th));


       // Max tiles and max polys affect how the tile IDs are caculated.
       // There are 22 bits available for identifying a tile and a polygon.
       int tileBits = rcMin((int)dtIlog2(dtNextPow2(tw*th*EXPECTED_LAYERS_PER_TILE)), 14);
       if (tileBits > 14) tileBits = 14;
       int polyBits = 22 - tileBits;
       m_maxTiles = 1 << tileBits;
       m_maxPolysPerTile = 1 << polyBits;
       Ogre::LogManager::getSingletonPtr()->logMessage("Max Tiles: " + Ogre::StringConverter::toString(m_maxTiles));
       Ogre::LogManager::getSingletonPtr()->logMessage("Max Polys: " + Ogre::StringConverter::toString(m_maxPolysPerTile));
       // End config ////





       // Build initial meshes
       // Builds detour compatible navmesh from all tiles.
       // A tile will have to be rebuilt if something changes, eg. a temporary obstacle is placed on it.
       for (int y = 0; y < m_th; ++y)
           for (int x = 0; x < m_tw; ++x)
           {
               m_tileCache->buildNavMeshTilesAt(x,y, m_recast->m_navMesh); // This immediately builds the tile, without the need of a dtTileCache::update()

               //drawDetail(x, y);
           }

   //    m_cacheBuildTimeMs = ctx->getAccumulatedTime(RC_TIMER_TOTAL)/1000.0f;
       m_cacheBuildMemUsage = m_talloc->high;


       // Count the total size of all generated tiles of the tiled navmesh
       const dtNavMesh* nav = m_recast->m_navMesh;
       int navmeshMemUsage = 0;
       for (int i = 0; i < nav->getMaxTiles(); ++i)
       {
           const dtMeshTile* tile = nav->getTile(i);
           if (tile->header)
               navmeshMemUsage += tile->dataSize;
       }


       Ogre::LogManager::getSingletonPtr()->logMessage("Navmesh Mem Usage = "+ Ogre::StringConverter::toString(navmeshMemUsage/1024.0f) +" kB");
       Ogre::LogManager::getSingletonPtr()->logMessage("Tilecache Mem Usage = " +Ogre::StringConverter::toString(m_cacheCompressedSize/1024.0f) +" kB");

       // Set member objects ready which would usually be done if the tile cache was built from scratch
       {
         assert ( !m_geom && !m_ctx) ;

         m_geom = new InputGeom ( std::move ( srcMeshes ) ) ;
         m_ctx  = m_recast->m_ctx ;
       }

       return true;
}