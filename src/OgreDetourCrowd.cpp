#include "Ogre.h"
#include "OgreDetourCrowd.h"
#include "Detour/DetourCommon.h"


// TODO voor highlighten toch m_agentDebug gebruiken?

OgreDetourCrowd::OgreDetourCrowd(OgreRecastDemo *recastDemo)
    : m_crowd(0),
    m_highlightedAgent(0),
    m_recastDemo(recastDemo),
    m_targetRef(0)
{
    m_crowd = dtAllocCrowd();
    if(!m_crowd)
        Ogre::LogManager::getSingletonPtr()->logMessage("Error: Could not allocate crowd instance.");


    // Set default agent parameters
    m_anticipateTurns = true;
    m_optimizeVis = true;
    m_optimizeTopo = true;
    m_obstacleAvoidance = true;
    m_separation = false;

    m_obstacleAvoidanceType = 3.0f;
    m_separationWeight = 2.0f;


    memset(m_trails, 0, sizeof(m_trails));

    m_vod = dtAllocObstacleAvoidanceDebugData();
    m_vod->init(2048);

    memset(&m_agentDebug, 0, sizeof(m_agentDebug));
    m_agentDebug.idx = -1;
    m_agentDebug.vod = m_vod;



    dtNavMesh* nav = recastDemo->m_navMesh;
    dtCrowd* crowd = m_crowd;
            if (nav && crowd && crowd->getAgentCount() == 0)
            {
                    crowd->init(MAX_AGENTS, m_recastDemo->m_agentRadius, nav);

                    // Make polygons with 'disabled' flag invalid.
                    crowd->getEditableFilter()->setExcludeFlags(SAMPLE_POLYFLAGS_DISABLED);


                    // Create different avoidance settings presets. The crowd object can store multiple, identified by an index number.
                    // Setup local avoidance params to different qualities.
                    dtObstacleAvoidanceParams params;
                    // Use mostly default settings, copy from dtCrowd.
                    memcpy(&params, crowd->getObstacleAvoidanceParams(0), sizeof(dtObstacleAvoidanceParams));

                    // Low (11)
                    params.velBias = 0.5f;
                    params.adaptiveDivs = 5;
                    params.adaptiveRings = 2;
                    params.adaptiveDepth = 1;
                    crowd->setObstacleAvoidanceParams(0, &params);

                    // Medium (22)
                    params.velBias = 0.5f;
                    params.adaptiveDivs = 5;
                    params.adaptiveRings = 2;
                    params.adaptiveDepth = 2;
                    crowd->setObstacleAvoidanceParams(1, &params);

                    // Good (45)
                    params.velBias = 0.5f;
                    params.adaptiveDivs = 7;
                    params.adaptiveRings = 2;
                    params.adaptiveDepth = 3;
                    crowd->setObstacleAvoidanceParams(2, &params);

                    // High (66)
                    params.velBias = 0.5f;
                    params.adaptiveDivs = 7;
                    params.adaptiveRings = 3;
                    params.adaptiveDepth = 3;

                    crowd->setObstacleAvoidanceParams(3, &params);
            }
}

OgreDetourCrowd::~OgreDetourCrowd()
{
    dtFreeCrowd(m_crowd);
    dtFreeObstacleAvoidanceDebugData(m_vod);
}


void OgreDetourCrowd::updateTick(const float dt)
{
        dtNavMesh* nav = m_recastDemo->m_navMesh;
        dtCrowd* crowd = m_crowd;
        if (!nav || !crowd) return;

//        TimeVal startTime = getPerfTime();

        crowd->update(dt, &m_agentDebug);

//        TimeVal endTime = getPerfTime();

        // Update agent trails
        for (int i = 0; i < crowd->getAgentCount(); ++i)
        {
                const dtCrowdAgent* ag = crowd->getAgent(i);
                AgentTrail* trail = &m_trails[i];
                if (!ag->active)
                        continue;
                // Update agent movement trail.
                trail->htrail = (trail->htrail + 1) % AGENT_MAX_TRAIL;
                dtVcopy(&trail->trail[trail->htrail*3], ag->npos);
        }

        m_agentDebug.vod->normalizeSamples();

        //m_crowdSampleCount.addSample((float)crowd->getVelocitySampleCount());
        //m_crowdTotalTime.addSample(getPerfDeltaTimeUsec(startTime, endTime) / 1000.0f);
}


void OgreDetourCrowd::addAgent(const Ogre::Vector3 position)
{
        // Define parameters for agent in crowd
        dtCrowdAgentParams ap;
        memset(&ap, 0, sizeof(ap));
        ap.radius = m_recastDemo->m_agentRadius;    // TODO define getters for this
        ap.height = m_recastDemo->m_agentHeight;
        ap.maxAcceleration = 8.0f;
        ap.maxSpeed = 3.5f;
        ap.collisionQueryRange = ap.radius * 12.0f;
        ap.pathOptimizationRange = ap.radius * 30.0f;

        // Set update flags according to config
        ap.updateFlags = 0;
        if (m_anticipateTurns)
                ap.updateFlags |= DT_CROWD_ANTICIPATE_TURNS;
        if (m_optimizeVis)
                ap.updateFlags |= DT_CROWD_OPTIMIZE_VIS;
        if (m_optimizeTopo)
                ap.updateFlags |= DT_CROWD_OPTIMIZE_TOPO;
        if (m_obstacleAvoidance)
                ap.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
        if (m_separation)
                ap.updateFlags |= DT_CROWD_SEPARATION;
        ap.obstacleAvoidanceType = (unsigned char)m_obstacleAvoidanceType;
        ap.separationWeight = m_separationWeight;

        float p[3];
        m_recastDemo->OgreVect3ToFloatA(position, p);
        int idx = m_crowd->addAgent(p, &ap);
        if (idx != -1)
        {
            // If a move target is defined: move agent towards it
                if (m_targetRef)
                        m_crowd->requestMoveTarget(idx, m_targetRef, m_targetPos);

                // Init trail
                AgentTrail* trail = &m_trails[idx];
                for (int i = 0; i < AGENT_MAX_TRAIL; ++i)
                        dtVcopy(&trail->trail[i*3], p);
                trail->htrail = 0;
        }
}


void OgreDetourCrowd::removeAgent(const int idx)
{
        m_crowd->removeAgent(idx);

        // TODO
//        if (m_highlightedAgent == m_agentDebug.idx)
//                m_agentDebug.idx = -1;
}


void OgreDetourCrowd::hilightAgent(Ogre::Entity* agent)
{
    if(m_highlightedAgent != NULL) {
        m_highlightedAgent->setMaterialName("Agent");
    }

    agent->setMaterialName("AgentHilight");
}


void OgreDetourCrowd::setMoveTarget(Ogre::Vector3 position, bool adjust)
{
        // Find nearest point on navmesh and set move request to that location.
        dtNavMeshQuery* navquery = m_recastDemo->m_navQuery;
        dtCrowd* crowd = m_crowd;
        const dtQueryFilter* filter = crowd->getFilter();
        const float* ext = crowd->getQueryExtents();
        float p[3];
        m_recastDemo->OgreVect3ToFloatA(position, p);

        navquery->findNearestPoly(p, ext, filter, &m_targetRef, m_targetPos);

        // Adjust target using tiny local search. (instead of recalculating full path)
        if (adjust)
        {
            // if agent selected, only apply new target to that agent
            // TODO
/*                if (m_agentDebug.idx != -1)
                {
                        const dtCrowdAgent* ag = crowd->getAgent(m_agentDebug.idx);
                        if (ag && ag->active)
                                crowd->adjustMoveTarget(m_agentDebug.idx, m_targetRef, m_targetPos);
                }
                else    // apply to all agents
                {
                */
                        for (int i = 0; i < crowd->getAgentCount(); ++i)
                        {
                                const dtCrowdAgent* ag = crowd->getAgent(i);
                                if (!ag->active) continue;
                                crowd->adjustMoveTarget(i, m_targetRef, m_targetPos);
                        }
                //}
        }
        else
        {
                // Move target using path finder (recalculate a full new path)
            // TODO
              /*if (m_agentDebug.idx != -1)
                {
                        const dtCrowdAgent* ag = crowd->getAgent(m_agentDebug.idx);
                        if (ag && ag->active)
                                crowd->requestMoveTarget(m_agentDebug.idx, m_targetRef, m_targetPos);
                }
                else
                {
            */
                        for (int i = 0; i < crowd->getAgentCount(); ++i)
                        {
                                const dtCrowdAgent* ag = crowd->getAgent(i);
                                if (!ag->active) continue;
                                crowd->requestMoveTarget(i, m_targetRef, m_targetPos);
                        }
            //    }
        }
}


