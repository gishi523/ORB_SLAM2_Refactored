/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Ra�Yl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
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

#include "LoopClosing.h"

#include <mutex>
#include <thread>

#include "Sim3Solver.h"
#include "Optimizer.h"
#include "ORBmatcher.h"
#include "Map.h"
#include "KeyFrame.h"
#include "KeyFrameDatabase.h"
#include "ORBVocabulary.h"
#include "Tracking.h"
#include "LocalMapping.h"
#include "Usleep.h"

#define LOCK_MUTEX_LOOP_QUEUE() std::unique_lock<std::mutex> lock1(mutexLoopQueue_);
#define LOCK_MUTEX_FINISH()     std::unique_lock<std::mutex> lock2(mutexFinish_);
#define LOCK_MUTEX_RESET()      std::unique_lock<std::mutex> lock3(mutexReset_);
#define LOCK_MUTEX_GLOBAL_BA()  std::unique_lock<std::mutex> lock4(mutexGBA_);
#define LOCK_MUTEX_MAP_UPDATE() std::unique_lock<std::mutex> lock5(map_->mutexMapUpdate);

namespace ORB_SLAM2
{

class LoopDetector
{

public:

	struct Loop
	{
		KeyFrame* matchedKF;
		Sim3 Scw;
		std::vector<MapPoint*> matchedPoints;
		std::vector<MapPoint*> loopMapPoints;
	};

	LoopDetector(KeyFrameDatabase* keyframeDB, ORBVocabulary* voc, bool fixScale)
		: keyFrameDB_(keyframeDB), voc_(voc), fixScale_(fixScale), minConsistency_(3) {}

	static bool FindLoopInCandidateKFs(KeyFrame* currentKF, std::vector<KeyFrame*>& candidateKFs, Loop& loop, bool fixScale)
	{
		// For each consistent loop candidate we try to compute a Sim3

		const int ninitialCandidates = static_cast<int>(candidateKFs.size());

		// We compute first ORB matches for each candidate
		// If enough matches are found, we setup a Sim3Solver
		ORBmatcher matcher(0.75f, true);

		std::vector<std::unique_ptr<Sim3Solver>> solvers(ninitialCandidates);
		std::vector<std::vector<MapPoint*>> vmatches(ninitialCandidates);
		std::vector<bool> discarded(ninitialCandidates);
		int ncandidates = 0; //candidates with enough matches

		for (int i = 0; i < ninitialCandidates; i++)
		{
			KeyFrame* candidateKF = candidateKFs[i];

			// avoid that local mapping erase it while it is being processed in this thread
			candidateKF->SetNotErase();

			if (candidateKF->isBad())
			{
				discarded[i] = true;
				continue;
			}

			const int nmatches = matcher.SearchByBoW(currentKF, candidateKF, vmatches[i]);
			if (nmatches < 20)
			{
				discarded[i] = true;
				continue;
			}
			else
			{
				auto solver = std::make_unique<Sim3Solver>(currentKF, candidateKF, vmatches[i], fixScale);
				solver->SetRansacParameters(0.99, 20, 300);
				solvers[i] = std::move(solver);
			}

			ncandidates++;
		}

		// Perform alternatively RANSAC iterations for each candidate
		// until one is succesful or all fail
		while (ncandidates > 0)
		{
			for (int i = 0; i < ninitialCandidates; i++)
			{
				if (discarded[i])
					continue;

				KeyFrame* candidateKF = candidateKFs[i];

				// Perform 5 Ransac Iterations
				std::vector<bool> isInlier;
				auto& solver = solvers[i];
				Sim3 Scm;
				const bool found = solver->iterate(5, Scm, isInlier);
				
				// If Ransac reachs max. iterations discard keyframe
				if (solver->terminate())
				{
					discarded[i] = true;
					ncandidates--;
				}

				// If RANSAC returns a Sim3, perform a guided matching and optimize with all correspondences
				if (found)
				{
					std::vector<MapPoint*> matches(vmatches[i].size());
					for (size_t j = 0; j < isInlier.size(); j++)
						matches[j] = isInlier[j] ? vmatches[i][j] : nullptr;

					matcher.SearchBySim3(currentKF, candidateKF, matches, Scm, 7.5f);

					const int nInliers = Optimizer::OptimizeSim3(currentKF, candidateKF, matches, Scm, 10, fixScale);

					// If optimization is succesful stop ransacs and continue
					if (nInliers >= 20)
					{
						Sim3 Smw(candidateKF->GetPose());
						loop.matchedKF = candidateKF;
						loop.Scw = Scm * Smw;
						loop.matchedPoints = matches;
						return true;
					}
				}
			}
		}

		return false;
	}

	bool Detect(KeyFrame* currentKF, Loop& loop, int lastLoopKFId)
	{
		///////////////////////////////////////////////////////////////////////////////////////////////////
		// DetectLoop
		///////////////////////////////////////////////////////////////////////////////////////////////////

		//If the map contains less than 10 KF or less than 10 KF have passed from last loop detection
		if ((int)currentKF->id < lastLoopKFId + 10)
			return false;

		// Compute reference BoW similarity score
		// This is the lowest score to a connected keyframe in the covisibility graph
		// We will impose loop candidates to have a higher similarity than this
		float minScore = 1.f;
		for (KeyFrame* neighborKF : currentKF->GetVectorCovisibleKeyFrames())
		{
			if (neighborKF->isBad())
				continue;

			const float score = static_cast<float>(voc_->score(currentKF->bowVector, neighborKF->bowVector));
			minScore = std::min(minScore, score);
		}

		// Query the database imposing the minimum score
		const std::vector<KeyFrame*> tmpCandidateKFs = keyFrameDB_->DetectLoopCandidates(currentKF, minScore);

		// If there are no loop candidates, just add new keyframe and return false
		if (tmpCandidateKFs.empty())
		{
			prevConsistentGroups_.clear();
			return false;
		}

		// For each loop candidate check consistency with previous loop candidates
		// Each candidate expands a covisibility group (keyframes connected to the loop candidate in the covisibility graph)
		// A group is consistent with a previous group if they share at least a keyframe
		// We must detect a consistent loop in several consecutive keyframes to accept it
		std::vector<KeyFrame*> candidateKFs;

		auto IsConsistent = [](const std::set<KeyFrame*>& prevGroup, const std::set<KeyFrame*>& currGroup)
		{
			for (KeyFrame* grounpKF : currGroup)
				if (prevGroup.count(grounpKF))
					return true;
			return false;
		};

		std::vector<ConsistentGroup> currConsistentGroups;
		std::vector<bool> consistentFound(prevConsistentGroups_.size(), false);
		for (KeyFrame* candidateKF : tmpCandidateKFs)
		{
			std::set<KeyFrame*> currGroup = candidateKF->GetConnectedKeyFrames();
			currGroup.insert(candidateKF);

			bool candidateFound = false;
			std::vector<size_t> consistentGroupsIds;
			for (size_t iG = 0; iG < prevConsistentGroups_.size(); iG++)
			{
				const std::set<KeyFrame*>& prevGroup = prevConsistentGroups_[iG].first;
				if (IsConsistent(prevGroup, currGroup))
					consistentGroupsIds.push_back(iG);
			}

			for (size_t iG : consistentGroupsIds)
			{
				const int currConsistency = prevConsistentGroups_[iG].second + 1;
				if (!consistentFound[iG])
				{
					currConsistentGroups.push_back(std::make_pair(currGroup, currConsistency));
					consistentFound[iG] = true; //this avoid to include the same group more than once
				}
				if (currConsistency >= minConsistency_ && !candidateFound)
				{
					candidateKFs.push_back(candidateKF);
					candidateFound = true; //this avoid to insert the same candidate more than once
				}
			}

			// If the group is not consistent with any previous group insert with consistency counter set to zero
			if (consistentGroupsIds.empty())
				currConsistentGroups.push_back(std::make_pair(currGroup, 0));
		}

		// Update Covisibility Consistent Groups
		prevConsistentGroups_ = currConsistentGroups;

		if (candidateKFs.empty())
			return false;

		///////////////////////////////////////////////////////////////////////////////////////////////////
		// ComputeSim3
		///////////////////////////////////////////////////////////////////////////////////////////////////

		const bool found = FindLoopInCandidateKFs(currentKF, candidateKFs, loop, fixScale_);
		if (!found)
		{
			for (KeyFrame* candidateKF : candidateKFs)
				candidateKF->SetErase();
			currentKF->SetErase();
			return false;
		}

		// Retrieve MapPoints seen in Loop Keyframe and neighbors
		std::vector<KeyFrame*> connectedKFs = loop.matchedKF->GetVectorCovisibleKeyFrames();
		connectedKFs.push_back(loop.matchedKF);
		loop.loopMapPoints.clear();
		for (KeyFrame* connectedKF : connectedKFs)
		{
			for (MapPoint* mappoint : connectedKF->GetMapPointMatches())
			{
				if (!mappoint || mappoint->isBad() || mappoint->loopPointForKF == currentKF->id)
					continue;

				loop.loopMapPoints.push_back(mappoint);
				mappoint->loopPointForKF = currentKF->id;
			}
		}

		// Find more matches projecting with the computed Sim3
		ORBmatcher matcher(0.75f, true);
		matcher.SearchByProjection(currentKF, loop.Scw, loop.loopMapPoints, loop.matchedPoints, 10);

		// If enough matches accept Loop
		const auto nmatches = std::count_if(std::begin(loop.matchedPoints), std::end(loop.matchedPoints),
			[](const MapPoint* mappoint) { return mappoint != nullptr; });

		if (nmatches >= 40)
		{
			for (KeyFrame* candidateKF : candidateKFs)
				if (candidateKF != loop.matchedKF)
					candidateKF->SetErase();
			return true;
		}
		else
		{
			for (KeyFrame* candidateKF : candidateKFs)
				candidateKF->SetErase();
			return false;
		}
	}

private:

	using ConsistentGroup = std::pair<std::set<KeyFrame*>, int>;

	KeyFrameDatabase* keyFrameDB_;
	ORBVocabulary* voc_;
	std::vector<ConsistentGroup> prevConsistentGroups_;
	bool fixScale_;
	int minConsistency_;
};

class ReusableThread
{
public:

	ReusableThread() : thread_(nullptr) {}
	~ReusableThread() { Join(); }

	template <class... Args>
	void Reset(Args&&... args)
	{
		Detach();
		thread_ = new std::thread(std::forward<Args>(args)...);
	}

	void Join()
	{
		if (!thread_ || !thread_->joinable())
			return;
		thread_->join();
		Clear();
	}

	void Detach()
	{
		if (!thread_ || !thread_->joinable())
			return;
		thread_->detach();
		Clear();
	}

	void Clear()
	{
		delete thread_;
		thread_ = nullptr;
	}

private:
	std::thread* thread_;
};

class GlobalBA
{
public:

	GlobalBA(Map* map) : map_(map), localMapper_(nullptr), running_(false), finished_(true), stop_(false), fullBAIdx_(0) {}

	void SetLocalMapper(LocalMapping* localMapper)
	{
		localMapper_ = localMapper;
	}

	// This function will run in a separate thread
	void _Run(frameid_t loopKFId)
	{
		std::cout << "Starting Global Bundle Adjustment" << std::endl;

		const int idx = fullBAIdx_;
		Optimizer::GlobalBundleAdjustemnt(map_, 10, &stop_, loopKFId, false);

		// Update all MapPoints and KeyFrames
		// Local Mapping was active during BA, that means that there might be new keyframes
		// not included in the Global BA and they are not consistent with the updated map.
		// We need to propagate the correction through the spanning tree
		{
			LOCK_MUTEX_GLOBAL_BA();
			if (idx != fullBAIdx_)
				return;

			if (!stop_)
			{
				std::cout << "Global Bundle Adjustment finished" << std::endl;
				std::cout << "Updating map ..." << std::endl;
				localMapper_->RequestStop();

				// Wait until Local Mapping has effectively stopped
				while (!localMapper_->isStopped() && !localMapper_->isFinished())
				{
					usleep(1000);
				}

				// Get Map Mutex
				LOCK_MUTEX_MAP_UPDATE();

				// Correct keyframes starting at map first keyframe
				std::list<KeyFrame*> toCheck(std::begin(map_->keyFrameOrigins), std::end(map_->keyFrameOrigins));
				while (!toCheck.empty())
				{
					KeyFrame* keyframe = toCheck.front();
					CameraPose Twc = keyframe->GetPose().Inverse();
					for (KeyFrame* child : keyframe->GetChildren())
					{
						if (child->BAGlobalForKF != loopKFId)
						{
							CameraPose Tchildc = child->GetPose() * Twc;
							child->TcwGBA = Tchildc * keyframe->TcwGBA;
							child->BAGlobalForKF = loopKFId;

						}
						toCheck.push_back(child);
					}

					keyframe->TcwBefGBA = keyframe->GetPose();
					keyframe->SetPose(keyframe->TcwGBA);
					toCheck.pop_front();
				}

				// Correct MapPoints
				for (MapPoint* mappoint : map_->GetAllMapPoints())
				{
					if (mappoint->isBad())
						continue;

					if (mappoint->BAGlobalForKF == loopKFId)
					{
						// If optimized by Global BA, just update
						mappoint->SetWorldPos(mappoint->posGBA);
					}
					else
					{
						// Update according to the correction of its reference keyframe
						KeyFrame* referenceKF = mappoint->GetReferenceKeyFrame();

						if (referenceKF->BAGlobalForKF != loopKFId)
							continue;

						// Map to non-corrected camera
						const auto Rcw = referenceKF->TcwBefGBA.R();
						const auto tcw = referenceKF->TcwBefGBA.t();
						const Point3D Xc = Rcw * mappoint->GetWorldPos() + tcw;

						// Backproject using corrected camera
						const auto Twc = referenceKF->GetPose().Inverse();
						const auto Rwc = Twc.R();
						const auto twc = Twc.t();

						mappoint->SetWorldPos(Rwc * Xc + twc);
					}
				}

				map_->InformNewBigChange();

				localMapper_->Release();

				std::cout << "Map updated!" << std::endl;
			}

			finished_ = true;
			running_ = false;
		}
	}

	void Run(frameid_t loopKFId)
	{
		running_ = true;
		finished_ = false;
		stop_ = false;
		thread_.Reset(&GlobalBA::_Run, this, loopKFId);
	}

	void Stop()
	{
		LOCK_MUTEX_GLOBAL_BA();
		stop_ = true;

		fullBAIdx_++;
		thread_.Detach();
	}

	bool Running() const
	{
		LOCK_MUTEX_GLOBAL_BA();
		return running_;
	}

	bool Finished() const
	{
		LOCK_MUTEX_GLOBAL_BA();
		return finished_;
	}

private:

	Map* map_;
	LocalMapping* localMapper_;
	bool running_;
	bool finished_;
	bool stop_;
	int fullBAIdx_;
	mutable std::mutex mutexGBA_;
	ReusableThread thread_;
};

class LoopCorrector
{

private:

	Map* map_;
	LocalMapping* localMapper_;
	GlobalBA* GBA_;
	// Fix scale in the stereo/RGB-D case
	bool fixScale_;

public:

	LoopCorrector(Map* map, GlobalBA* GBA, bool fixScale) : map_(map), GBA_(GBA), fixScale_(fixScale) {}

	void SetLocalMapper(LocalMapping *pLocalMapper)
	{
		localMapper_ = pLocalMapper;
	}

	void Correct(KeyFrame* currentKF, LoopDetector::Loop& loop)
	{
		std::cout << "Loop detected!" << std::endl;

		KeyFrame* matchedKF = loop.matchedKF;
		const Sim3& Scw = loop.Scw;
		std::vector<MapPoint*>& matchedPoints = loop.matchedPoints;
		std::vector<MapPoint*>& loopMapPoints = loop.loopMapPoints;

		// Send a stop signal to Local Mapping
		// Avoid new keyframes are inserted while correcting the loop
		localMapper_->RequestStop();

		// If a Global Bundle Adjustment is running, abort it
		if (GBA_->Running())
		{
			GBA_->Stop();
		}

		// Wait until Local Mapping has effectively stopped
		while (!localMapper_->isStopped())
		{
			usleep(1000);
		}

		// Ensure current keyframe is updated
		currentKF->UpdateConnections();

		// Retrive keyframes connected to the current keyframe and compute corrected Sim3 pose by propagation
		std::vector<KeyFrame*> connectedKFs;
		connectedKFs = currentKF->GetVectorCovisibleKeyFrames();
		connectedKFs.push_back(currentKF);

		KeyFrameAndPose CorrectedSim3, NonCorrectedSim3;
		CorrectedSim3[currentKF] = Scw;
		const CameraPose Twc = currentKF->GetPose().Inverse();


		{
			// Get Map Mutex
			LOCK_MUTEX_MAP_UPDATE();
			for (KeyFrame* connectedKF : connectedKFs)
			{
				const CameraPose Tiw = connectedKF->GetPose();
				if (connectedKF != currentKF)
				{
					const CameraPose Tic = Tiw * Twc;
					const Sim3 Sic(Tic);
					const Sim3 CorrectedSiw = Sic * Scw;
					//Pose corrected with the Sim3 of the loop closure
					CorrectedSim3[connectedKF] = CorrectedSiw;
				}

				//Pose without correction
				NonCorrectedSim3[connectedKF] = Sim3(Tiw);
			}

			// Correct all MapPoints obsrved by current keyframe and neighbors, so that they align with the other side of the loop
			for (const auto& v : CorrectedSim3)
			{
				KeyFrame* connectedKF = v.first;
				const Sim3& CorrectedSiw = v.second;
				const Sim3& CorrectedSwi = CorrectedSiw.Inverse();
				const Sim3& Siw = NonCorrectedSim3[connectedKF];
				const Sim3 correction = CorrectedSwi * Siw;

				for (MapPoint* mappiont : connectedKF->GetMapPointMatches())
				{
					if (!mappiont || mappiont->isBad() || mappiont->correctedByKF == currentKF->id)
						continue;
					
					// Project with non-corrected pose and project back with corrected pose
					const Point3D P3Dw = mappiont->GetWorldPos();
					const Point3D CorrectedP3Dw = correction.Map(P3Dw);

					mappiont->SetWorldPos(CorrectedP3Dw);
					mappiont->correctedByKF = currentKF->id;
					mappiont->correctedReference = connectedKF->id;
					mappiont->UpdateNormalAndDepth();
				}

				// Update keyframe pose with corrected Sim3. First transform Sim3 to SE3 (scale translation)
				
				// [R t/s;0 1]
				const auto R = CorrectedSiw.R();
				const auto t = CorrectedSiw.t();
				const double invs = 1. / CorrectedSiw.Scale();
				
				connectedKF->SetPose(CameraPose(R, invs * t));

				// Make sure connections are updated
				connectedKF->UpdateConnections();
			}

			// Start Loop Fusion
			// Update matched map points and replace if duplicated
			for (size_t i = 0; i < matchedPoints.size(); i++)
			{
				if (matchedPoints[i])
				{
					MapPoint* loopMP = matchedPoints[i];
					MapPoint* currMP = currentKF->GetMapPoint(i);
					if (currMP)
					{
						currMP->Replace(loopMP);
					}
					else
					{
						currentKF->AddMapPoint(loopMP, i);
						loopMP->AddObservation(currentKF, i);
						loopMP->ComputeDistinctiveDescriptors();
					}
				}
			}
		}

		// Project MapPoints observed in the neighborhood of the loop keyframe
		// into the current keyframe and neighbors using corrected poses.
		// Fuse duplications.
		ORBmatcher matcher(0.8f);
		for (const auto& v : CorrectedSim3)
		{
			KeyFrame* connectedKF = v.first;
			const Sim3& Scw = v.second;
			//cv::Mat cvScw = Converter::toCvMat(Scw);

			std::vector<MapPoint*> replacePoints(loopMapPoints.size(), nullptr);
			matcher.Fuse(connectedKF, Scw, loopMapPoints, 4, replacePoints);

			// Get Map Mutex
			LOCK_MUTEX_MAP_UPDATE();
			for (size_t i = 0; i < loopMapPoints.size(); i++)
			{
				MapPoint* mappoint = replacePoints[i];
				if (mappoint)
					mappoint->Replace(loopMapPoints[i]);
			}
		}

		// After the MapPoint fusion, new links in the covisibility graph will appear attaching both sides of the loop
		std::map<KeyFrame*, std::set<KeyFrame*>> LoopConnections;

		for (KeyFrame* connectedKF : connectedKFs)
		{
			std::vector<KeyFrame*> prevNeighbors = connectedKF->GetVectorCovisibleKeyFrames();

			// Update connections. Detect new links.
			connectedKF->UpdateConnections();
			LoopConnections[connectedKF] = connectedKF->GetConnectedKeyFrames();

			for (KeyFrame* neighborKF : prevNeighbors)
				LoopConnections[connectedKF].erase(neighborKF);

			for (KeyFrame* neighborKF : connectedKFs)
				LoopConnections[connectedKF].erase(neighborKF);
		}

		// Optimize graph
		Optimizer::OptimizeEssentialGraph(map_, matchedKF, currentKF, NonCorrectedSim3, CorrectedSim3, LoopConnections, fixScale_);

		map_->InformNewBigChange();

		// Add loop edge
		matchedKF->AddLoopEdge(currentKF);
		currentKF->AddLoopEdge(matchedKF);

		// Launch a new thread to perform Global Bundle Adjustment
		GBA_->Run(currentKF->id);

		// Loop closed. Release Local Mapping.
		localMapper_->Release();
	}
};

class LoopClosingImpl : public LoopClosing
{

public:

	LoopClosingImpl(Map *map, KeyFrameDatabase* keyframeDB, ORBVocabulary *voc, bool fixScale)
		: resetRequested_(false), finishRequested_(false), finished_(true), lastLoopKFId_(0),
		keyframeDB_(keyframeDB), detector_(keyframeDB, voc, fixScale), corrector_(map, &GBA_, fixScale), GBA_(map)
	{
	}

	void SetTracker(Tracking* tracker) override
	{
		tracker_ = tracker;
	}

	void SetLocalMapper(LocalMapping* localMapper) override
	{
		localMapper_ = localMapper;
		GBA_.SetLocalMapper(localMapper);
		corrector_.SetLocalMapper(localMapper);
	}

	// Main function
	void Run() override
	{
		finished_ = false;

		while (true)
		{
			// Check if there are keyframes in the queue
			if (CheckNewKeyFrames())
			{
				KeyFrame* currentKF = nullptr;
				{
					LOCK_MUTEX_LOOP_QUEUE();
					currentKF = keyFrameQueue_.front();
					keyFrameQueue_.pop_front();
					currentKF->SetNotErase();
				}

				// Detect loop candidates and check covisibility consistency
				// Compute similarity transformation [sR|t]
				// In the stereo/RGBD case s=1
				LoopDetector::Loop loop;
				const bool found = detector_.Detect(currentKF, loop, lastLoopKFId_);

				// Add Current Keyframe to database
				keyframeDB_->add(currentKF);

				if (found)
				{
					// Perform loop fusion and pose graph optimization
					corrector_.Correct(currentKF, loop);
					lastLoopKFId_ = currentKF->id;
				}
				else
				{
					currentKF->SetErase();
				}
			}

			ResetIfRequested();

			if (CheckFinish())
				break;

			usleep(5000);
		}

		SetFinish();
	}

	void InsertKeyFrame(KeyFrame* keyframe) override
	{
		LOCK_MUTEX_LOOP_QUEUE();
		if (keyframe->id != 0)
			keyFrameQueue_.push_back(keyframe);
	}

	void RequestReset() override
	{
		{
			LOCK_MUTEX_RESET();
			resetRequested_ = true;
		}

		while (true)
		{
			{
				LOCK_MUTEX_RESET();
				if (!resetRequested_)
					break;
			}
			usleep(5000);
		}
	}

	bool isRunningGBA() const override
	{
		return GBA_.Running();
	}

	bool isFinishedGBA() const override
	{
		return GBA_.Finished();
	}

	void RequestFinish() override
	{
		LOCK_MUTEX_FINISH();
		finishRequested_ = true;
	}

	bool isFinished() const override
	{
		LOCK_MUTEX_FINISH();
		return finished_;
	}

	bool CheckNewKeyFrames() const
	{
		LOCK_MUTEX_LOOP_QUEUE();
		return(!keyFrameQueue_.empty());
	}

	void ResetIfRequested()
	{
		LOCK_MUTEX_RESET();
		if (resetRequested_)
		{
			keyFrameQueue_.clear();
			lastLoopKFId_ = 0;
			resetRequested_ = false;
		}
	}

	bool CheckFinish() const
	{
		LOCK_MUTEX_FINISH();
		return finishRequested_;
	}

	void SetFinish()
	{
		LOCK_MUTEX_FINISH();
		finished_ = true;
	}

	//EIGEN_MAKE_ALIGNED_OPERATOR_NEW

private:

	bool resetRequested_;
	bool finishRequested_;
	bool finished_;
	frameid_t lastLoopKFId_;

	Tracking* tracker_;
	LocalMapping* localMapper_;

	std::list<KeyFrame*> keyFrameQueue_;

	// Loop detector variables
	KeyFrameDatabase* keyframeDB_;
	LoopDetector detector_;

	// Variables related to Global Bundle Adjustment
	LoopCorrector corrector_;
	GlobalBA GBA_;

	mutable std::mutex mutexReset_;
	mutable std::mutex mutexFinish_;
	mutable std::mutex mutexLoopQueue_;
};

LoopClosing::Pointer LoopClosing::Create(Map* map, KeyFrameDatabase* keyframeDB, ORBVocabulary* voc, bool fixScale)
{
	return std::make_unique<LoopClosingImpl>(map, keyframeDB, voc, fixScale);
}

LoopClosing::~LoopClosing() {}

} //namespace ORB_SLAM
