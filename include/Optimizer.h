/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "FrameId.h"
#include "Sim3.h"

namespace ORB_SLAM2
{

class Map;
class MapPoint;
class Frame;
class KeyFrame;

using KeyFrameAndPose = std::map<KeyFrame*, Sim3>;
using LoopConnections = std::map<KeyFrame*, std::set<KeyFrame*>>;

namespace Optimizer
{

void BundleAdjustment(const std::vector<KeyFrame*>& keyframes, const std::vector<MapPoint*>& mappoints,
	int niterations = 5, bool* stopFlag = nullptr, frameid_t loopKFId = 0, bool robust = true);

void GlobalBundleAdjustemnt(Map* map, int niterations, bool* stopFlag = nullptr, frameid_t loopKFId = 0,
	bool robust = true);

void LocalBundleAdjustment(KeyFrame* currKeyFrame, bool* stopFlag, Map* map);

int PoseOptimization(Frame* pFrame);

// if bFixScale is true, 6DoF optimization (stereo,rgbd), 7DoF otherwise (mono)
void OptimizeEssentialGraph(Map* map, KeyFrame* loopKF, KeyFrame* currKF,
	const KeyFrameAndPose& nonCorrectedSim3, const KeyFrameAndPose& correctedSim3,
	const LoopConnections& loopConnections, bool fixScale);

// if bFixScale is true, optimize SE3 (stereo,rgbd), Sim3 otherwise (mono)
int OptimizeSim3(KeyFrame* keyframe1, KeyFrame* keyframe2, std::vector<MapPoint*>& matches1, Sim3& S12,
	float maxChi2, bool fixScale);

} //namespace Optimizer

} //namespace ORB_SLAM

#endif // OPTIMIZER_H
