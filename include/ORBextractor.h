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

#ifndef ORBEXTRACTOR_H
#define ORBEXTRACTOR_H

#include <vector>
#include <list>
#include <opencv/cv.h>

namespace ORB_SLAM2
{

class ORBextractor
{
public:

	struct Parameters
	{
		int nfeatures;
		float scaleFactor;
		int nlevels;
		int iniThFAST;
		int minThFAST;

		Parameters(int nfeatures = 2000, float scaleFactor = 1.2f, int nlevels = 8, int iniThFAST = 20, int minThFAST = 7);
	};

	ORBextractor(int nfeatures, float scaleFactor, int nlevels, int iniThFAST, int minThFAST);
	ORBextractor(const Parameters& param);
	~ORBextractor() {}

	// Compute the ORB features and descriptors on an image.
	// ORB are dispersed on the image using an octree.
	// Mask is ignored in the current implementation.
	void operator()(cv::InputArray image, cv::InputArray mask,
		std::vector<cv::KeyPoint>& keypoints,
		cv::OutputArray descriptors);

	int inline GetLevels() {
		return nlevels;
	}

	float inline GetScaleFactor() {
		return scaleFactor;
	}

	std::vector<float> inline GetScaleFactors() {
		return mvScaleFactor;
	}

	std::vector<float> inline GetInverseScaleFactors() {
		return mvInvScaleFactor;
	}

	std::vector<float> inline GetScaleSigmaSquares() {
		return mvLevelSigma2;
	}

	std::vector<float> inline GetInverseScaleSigmaSquares() {
		return mvInvLevelSigma2;
	}

	std::vector<cv::Mat> mvImagePyramid;

protected:

	void ComputePyramid(cv::Mat image);
	void ComputeKeyPointsOctTree(std::vector<std::vector<cv::KeyPoint> >& allKeypoints);
	std::vector<cv::KeyPoint> DistributeOctTree(const std::vector<cv::KeyPoint>& vToDistributeKeys, const int &minX,
		const int &maxX, const int &minY, const int &maxY, const int &nFeatures, const int &level);

	int nfeatures;
	double scaleFactor;
	int nlevels;
	int iniThFAST;
	int minThFAST;

	std::vector<cv::Point> pattern;
	std::vector<int> mnFeaturesPerLevel;

	std::vector<int> umax;

	std::vector<float> mvScaleFactor;
	std::vector<float> mvInvScaleFactor;
	std::vector<float> mvLevelSigma2;
	std::vector<float> mvInvLevelSigma2;
};

} //namespace ORB_SLAM

#endif

