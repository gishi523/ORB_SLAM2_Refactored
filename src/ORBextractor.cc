/**
* This file is part of ORB-SLAM2.
* This file is based on the file orb.cpp from the OpenCV library (see BSD license below).
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
/**
* Software License Agreement (BSD License)
*
*  Copyright (c) 2009, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
*/

#include "ORBextractor.h"

#include <array>
#include <vector>
#include <iterator>

#include <opencv2/opencv.hpp>

namespace ORB_SLAM2
{

using KeyPoints = std::vector<cv::KeyPoint>;
using Pyramid = std::vector<cv::Mat>;

const int PATCH_SIZE = 31;
const int HALF_PATCH_SIZE = 15;
const int EDGE_THRESHOLD = 19;

static inline int RoundUp(double v) { return static_cast<int>(std::ceil(v)); }
static inline int RoundDn(double v) { return static_cast<int>(std::floor(v)); }

static float IC_Angle(const cv::Mat& image, cv::Point2f pt, const std::vector<int>& u_max)
{
	int m_01 = 0, m_10 = 0;

	const uchar* center = &image.at<uchar>(cvRound(pt.y), cvRound(pt.x));

	// Treat the center line differently, v=0
	for (int u = -HALF_PATCH_SIZE; u <= HALF_PATCH_SIZE; ++u)
		m_10 += u * center[u];

	// Go line by line in the circuI853lar patch
	int step = (int)image.step1();
	for (int v = 1; v <= HALF_PATCH_SIZE; ++v)
	{
		// Proceed over the two lines
		int v_sum = 0;
		int d = u_max[v];
		for (int u = -d; u <= d; ++u)
		{
			int val_plus = center[u + v*step], val_minus = center[u - v*step];
			v_sum += (val_plus - val_minus);
			m_10 += u * (val_plus + val_minus);
		}
		m_01 += v * v_sum;
	}

	return cv::fastAtan2((float)m_01, (float)m_10);
}

static void ComputeOrbDescriptor(const cv::KeyPoint& kpt, const cv::Mat& img, const cv::Point* pattern, uchar* desc)
{
	const float factorPI = (float)(CV_PI / 180.f);
	float angle = (float)kpt.angle*factorPI;
	float a = (float)cos(angle), b = (float)sin(angle);

	const uchar* center = &img.at<uchar>(cvRound(kpt.pt.y), cvRound(kpt.pt.x));
	const int step = (int)img.step;

#define GET_VALUE(idx) \
        center[cvRound(pattern[idx].x*b + pattern[idx].y*a)*step + \
               cvRound(pattern[idx].x*a - pattern[idx].y*b)]

	for (int i = 0; i < 32; ++i, pattern += 16)
	{
		int t0, t1, val;
		t0 = GET_VALUE(0); t1 = GET_VALUE(1);
		val = t0 < t1;
		t0 = GET_VALUE(2); t1 = GET_VALUE(3);
		val |= (t0 < t1) << 1;
		t0 = GET_VALUE(4); t1 = GET_VALUE(5);
		val |= (t0 < t1) << 2;
		t0 = GET_VALUE(6); t1 = GET_VALUE(7);
		val |= (t0 < t1) << 3;
		t0 = GET_VALUE(8); t1 = GET_VALUE(9);
		val |= (t0 < t1) << 4;
		t0 = GET_VALUE(10); t1 = GET_VALUE(11);
		val |= (t0 < t1) << 5;
		t0 = GET_VALUE(12); t1 = GET_VALUE(13);
		val |= (t0 < t1) << 6;
		t0 = GET_VALUE(14); t1 = GET_VALUE(15);
		val |= (t0 < t1) << 7;

		desc[i] = (uchar)val;
	}

#undef GET_VALUE
}

static int bit_pattern_31_[256 * 4] =
{
	8,-3, 9,5/*mean (0), correlation (0)*/,
	4,2, 7,-12/*mean (1.12461e-05), correlation (0.0437584)*/,
	-11,9, -8,2/*mean (3.37382e-05), correlation (0.0617409)*/,
	7,-12, 12,-13/*mean (5.62303e-05), correlation (0.0636977)*/,
	2,-13, 2,12/*mean (0.000134953), correlation (0.085099)*/,
	1,-7, 1,6/*mean (0.000528565), correlation (0.0857175)*/,
	-2,-10, -2,-4/*mean (0.0188821), correlation (0.0985774)*/,
	-13,-13, -11,-8/*mean (0.0363135), correlation (0.0899616)*/,
	-13,-3, -12,-9/*mean (0.121806), correlation (0.099849)*/,
	10,4, 11,9/*mean (0.122065), correlation (0.093285)*/,
	-13,-8, -8,-9/*mean (0.162787), correlation (0.0942748)*/,
	-11,7, -9,12/*mean (0.21561), correlation (0.0974438)*/,
	7,7, 12,6/*mean (0.160583), correlation (0.130064)*/,
	-4,-5, -3,0/*mean (0.228171), correlation (0.132998)*/,
	-13,2, -12,-3/*mean (0.00997526), correlation (0.145926)*/,
	-9,0, -7,5/*mean (0.198234), correlation (0.143636)*/,
	12,-6, 12,-1/*mean (0.0676226), correlation (0.16689)*/,
	-3,6, -2,12/*mean (0.166847), correlation (0.171682)*/,
	-6,-13, -4,-8/*mean (0.101215), correlation (0.179716)*/,
	11,-13, 12,-8/*mean (0.200641), correlation (0.192279)*/,
	4,7, 5,1/*mean (0.205106), correlation (0.186848)*/,
	5,-3, 10,-3/*mean (0.234908), correlation (0.192319)*/,
	3,-7, 6,12/*mean (0.0709964), correlation (0.210872)*/,
	-8,-7, -6,-2/*mean (0.0939834), correlation (0.212589)*/,
	-2,11, -1,-10/*mean (0.127778), correlation (0.20866)*/,
	-13,12, -8,10/*mean (0.14783), correlation (0.206356)*/,
	-7,3, -5,-3/*mean (0.182141), correlation (0.198942)*/,
	-4,2, -3,7/*mean (0.188237), correlation (0.21384)*/,
	-10,-12, -6,11/*mean (0.14865), correlation (0.23571)*/,
	5,-12, 6,-7/*mean (0.222312), correlation (0.23324)*/,
	5,-6, 7,-1/*mean (0.229082), correlation (0.23389)*/,
	1,0, 4,-5/*mean (0.241577), correlation (0.215286)*/,
	9,11, 11,-13/*mean (0.00338507), correlation (0.251373)*/,
	4,7, 4,12/*mean (0.131005), correlation (0.257622)*/,
	2,-1, 4,4/*mean (0.152755), correlation (0.255205)*/,
	-4,-12, -2,7/*mean (0.182771), correlation (0.244867)*/,
	-8,-5, -7,-10/*mean (0.186898), correlation (0.23901)*/,
	4,11, 9,12/*mean (0.226226), correlation (0.258255)*/,
	0,-8, 1,-13/*mean (0.0897886), correlation (0.274827)*/,
	-13,-2, -8,2/*mean (0.148774), correlation (0.28065)*/,
	-3,-2, -2,3/*mean (0.153048), correlation (0.283063)*/,
	-6,9, -4,-9/*mean (0.169523), correlation (0.278248)*/,
	8,12, 10,7/*mean (0.225337), correlation (0.282851)*/,
	0,9, 1,3/*mean (0.226687), correlation (0.278734)*/,
	7,-5, 11,-10/*mean (0.00693882), correlation (0.305161)*/,
	-13,-6, -11,0/*mean (0.0227283), correlation (0.300181)*/,
	10,7, 12,1/*mean (0.125517), correlation (0.31089)*/,
	-6,-3, -6,12/*mean (0.131748), correlation (0.312779)*/,
	10,-9, 12,-4/*mean (0.144827), correlation (0.292797)*/,
	-13,8, -8,-12/*mean (0.149202), correlation (0.308918)*/,
	-13,0, -8,-4/*mean (0.160909), correlation (0.310013)*/,
	3,3, 7,8/*mean (0.177755), correlation (0.309394)*/,
	5,7, 10,-7/*mean (0.212337), correlation (0.310315)*/,
	-1,7, 1,-12/*mean (0.214429), correlation (0.311933)*/,
	3,-10, 5,6/*mean (0.235807), correlation (0.313104)*/,
	2,-4, 3,-10/*mean (0.00494827), correlation (0.344948)*/,
	-13,0, -13,5/*mean (0.0549145), correlation (0.344675)*/,
	-13,-7, -12,12/*mean (0.103385), correlation (0.342715)*/,
	-13,3, -11,8/*mean (0.134222), correlation (0.322922)*/,
	-7,12, -4,7/*mean (0.153284), correlation (0.337061)*/,
	6,-10, 12,8/*mean (0.154881), correlation (0.329257)*/,
	-9,-1, -7,-6/*mean (0.200967), correlation (0.33312)*/,
	-2,-5, 0,12/*mean (0.201518), correlation (0.340635)*/,
	-12,5, -7,5/*mean (0.207805), correlation (0.335631)*/,
	3,-10, 8,-13/*mean (0.224438), correlation (0.34504)*/,
	-7,-7, -4,5/*mean (0.239361), correlation (0.338053)*/,
	-3,-2, -1,-7/*mean (0.240744), correlation (0.344322)*/,
	2,9, 5,-11/*mean (0.242949), correlation (0.34145)*/,
	-11,-13, -5,-13/*mean (0.244028), correlation (0.336861)*/,
	-1,6, 0,-1/*mean (0.247571), correlation (0.343684)*/,
	5,-3, 5,2/*mean (0.000697256), correlation (0.357265)*/,
	-4,-13, -4,12/*mean (0.00213675), correlation (0.373827)*/,
	-9,-6, -9,6/*mean (0.0126856), correlation (0.373938)*/,
	-12,-10, -8,-4/*mean (0.0152497), correlation (0.364237)*/,
	10,2, 12,-3/*mean (0.0299933), correlation (0.345292)*/,
	7,12, 12,12/*mean (0.0307242), correlation (0.366299)*/,
	-7,-13, -6,5/*mean (0.0534975), correlation (0.368357)*/,
	-4,9, -3,4/*mean (0.099865), correlation (0.372276)*/,
	7,-1, 12,2/*mean (0.117083), correlation (0.364529)*/,
	-7,6, -5,1/*mean (0.126125), correlation (0.369606)*/,
	-13,11, -12,5/*mean (0.130364), correlation (0.358502)*/,
	-3,7, -2,-6/*mean (0.131691), correlation (0.375531)*/,
	7,-8, 12,-7/*mean (0.160166), correlation (0.379508)*/,
	-13,-7, -11,-12/*mean (0.167848), correlation (0.353343)*/,
	1,-3, 12,12/*mean (0.183378), correlation (0.371916)*/,
	2,-6, 3,0/*mean (0.228711), correlation (0.371761)*/,
	-4,3, -2,-13/*mean (0.247211), correlation (0.364063)*/,
	-1,-13, 1,9/*mean (0.249325), correlation (0.378139)*/,
	7,1, 8,-6/*mean (0.000652272), correlation (0.411682)*/,
	1,-1, 3,12/*mean (0.00248538), correlation (0.392988)*/,
	9,1, 12,6/*mean (0.0206815), correlation (0.386106)*/,
	-1,-9, -1,3/*mean (0.0364485), correlation (0.410752)*/,
	-13,-13, -10,5/*mean (0.0376068), correlation (0.398374)*/,
	7,7, 10,12/*mean (0.0424202), correlation (0.405663)*/,
	12,-5, 12,9/*mean (0.0942645), correlation (0.410422)*/,
	6,3, 7,11/*mean (0.1074), correlation (0.413224)*/,
	5,-13, 6,10/*mean (0.109256), correlation (0.408646)*/,
	2,-12, 2,3/*mean (0.131691), correlation (0.416076)*/,
	3,8, 4,-6/*mean (0.165081), correlation (0.417569)*/,
	2,6, 12,-13/*mean (0.171874), correlation (0.408471)*/,
	9,-12, 10,3/*mean (0.175146), correlation (0.41296)*/,
	-8,4, -7,9/*mean (0.183682), correlation (0.402956)*/,
	-11,12, -4,-6/*mean (0.184672), correlation (0.416125)*/,
	1,12, 2,-8/*mean (0.191487), correlation (0.386696)*/,
	6,-9, 7,-4/*mean (0.192668), correlation (0.394771)*/,
	2,3, 3,-2/*mean (0.200157), correlation (0.408303)*/,
	6,3, 11,0/*mean (0.204588), correlation (0.411762)*/,
	3,-3, 8,-8/*mean (0.205904), correlation (0.416294)*/,
	7,8, 9,3/*mean (0.213237), correlation (0.409306)*/,
	-11,-5, -6,-4/*mean (0.243444), correlation (0.395069)*/,
	-10,11, -5,10/*mean (0.247672), correlation (0.413392)*/,
	-5,-8, -3,12/*mean (0.24774), correlation (0.411416)*/,
	-10,5, -9,0/*mean (0.00213675), correlation (0.454003)*/,
	8,-1, 12,-6/*mean (0.0293635), correlation (0.455368)*/,
	4,-6, 6,-11/*mean (0.0404971), correlation (0.457393)*/,
	-10,12, -8,7/*mean (0.0481107), correlation (0.448364)*/,
	4,-2, 6,7/*mean (0.050641), correlation (0.455019)*/,
	-2,0, -2,12/*mean (0.0525978), correlation (0.44338)*/,
	-5,-8, -5,2/*mean (0.0629667), correlation (0.457096)*/,
	7,-6, 10,12/*mean (0.0653846), correlation (0.445623)*/,
	-9,-13, -8,-8/*mean (0.0858749), correlation (0.449789)*/,
	-5,-13, -5,-2/*mean (0.122402), correlation (0.450201)*/,
	8,-8, 9,-13/*mean (0.125416), correlation (0.453224)*/,
	-9,-11, -9,0/*mean (0.130128), correlation (0.458724)*/,
	1,-8, 1,-2/*mean (0.132467), correlation (0.440133)*/,
	7,-4, 9,1/*mean (0.132692), correlation (0.454)*/,
	-2,1, -1,-4/*mean (0.135695), correlation (0.455739)*/,
	11,-6, 12,-11/*mean (0.142904), correlation (0.446114)*/,
	-12,-9, -6,4/*mean (0.146165), correlation (0.451473)*/,
	3,7, 7,12/*mean (0.147627), correlation (0.456643)*/,
	5,5, 10,8/*mean (0.152901), correlation (0.455036)*/,
	0,-4, 2,8/*mean (0.167083), correlation (0.459315)*/,
	-9,12, -5,-13/*mean (0.173234), correlation (0.454706)*/,
	0,7, 2,12/*mean (0.18312), correlation (0.433855)*/,
	-1,2, 1,7/*mean (0.185504), correlation (0.443838)*/,
	5,11, 7,-9/*mean (0.185706), correlation (0.451123)*/,
	3,5, 6,-8/*mean (0.188968), correlation (0.455808)*/,
	-13,-4, -8,9/*mean (0.191667), correlation (0.459128)*/,
	-5,9, -3,-3/*mean (0.193196), correlation (0.458364)*/,
	-4,-7, -3,-12/*mean (0.196536), correlation (0.455782)*/,
	6,5, 8,0/*mean (0.1972), correlation (0.450481)*/,
	-7,6, -6,12/*mean (0.199438), correlation (0.458156)*/,
	-13,6, -5,-2/*mean (0.211224), correlation (0.449548)*/,
	1,-10, 3,10/*mean (0.211718), correlation (0.440606)*/,
	4,1, 8,-4/*mean (0.213034), correlation (0.443177)*/,
	-2,-2, 2,-13/*mean (0.234334), correlation (0.455304)*/,
	2,-12, 12,12/*mean (0.235684), correlation (0.443436)*/,
	-2,-13, 0,-6/*mean (0.237674), correlation (0.452525)*/,
	4,1, 9,3/*mean (0.23962), correlation (0.444824)*/,
	-6,-10, -3,-5/*mean (0.248459), correlation (0.439621)*/,
	-3,-13, -1,1/*mean (0.249505), correlation (0.456666)*/,
	7,5, 12,-11/*mean (0.00119208), correlation (0.495466)*/,
	4,-2, 5,-7/*mean (0.00372245), correlation (0.484214)*/,
	-13,9, -9,-5/*mean (0.00741116), correlation (0.499854)*/,
	7,1, 8,6/*mean (0.0208952), correlation (0.499773)*/,
	7,-8, 7,6/*mean (0.0220085), correlation (0.501609)*/,
	-7,-4, -7,1/*mean (0.0233806), correlation (0.496568)*/,
	-8,11, -7,-8/*mean (0.0236505), correlation (0.489719)*/,
	-13,6, -12,-8/*mean (0.0268781), correlation (0.503487)*/,
	2,4, 3,9/*mean (0.0323324), correlation (0.501938)*/,
	10,-5, 12,3/*mean (0.0399235), correlation (0.494029)*/,
	-6,-5, -6,7/*mean (0.0420153), correlation (0.486579)*/,
	8,-3, 9,-8/*mean (0.0548021), correlation (0.484237)*/,
	2,-12, 2,8/*mean (0.0616622), correlation (0.496642)*/,
	-11,-2, -10,3/*mean (0.0627755), correlation (0.498563)*/,
	-12,-13, -7,-9/*mean (0.0829622), correlation (0.495491)*/,
	-11,0, -10,-5/*mean (0.0843342), correlation (0.487146)*/,
	5,-3, 11,8/*mean (0.0929937), correlation (0.502315)*/,
	-2,-13, -1,12/*mean (0.113327), correlation (0.48941)*/,
	-1,-8, 0,9/*mean (0.132119), correlation (0.467268)*/,
	-13,-11, -12,-5/*mean (0.136269), correlation (0.498771)*/,
	-10,-2, -10,11/*mean (0.142173), correlation (0.498714)*/,
	-3,9, -2,-13/*mean (0.144141), correlation (0.491973)*/,
	2,-3, 3,2/*mean (0.14892), correlation (0.500782)*/,
	-9,-13, -4,0/*mean (0.150371), correlation (0.498211)*/,
	-4,6, -3,-10/*mean (0.152159), correlation (0.495547)*/,
	-4,12, -2,-7/*mean (0.156152), correlation (0.496925)*/,
	-6,-11, -4,9/*mean (0.15749), correlation (0.499222)*/,
	6,-3, 6,11/*mean (0.159211), correlation (0.503821)*/,
	-13,11, -5,5/*mean (0.162427), correlation (0.501907)*/,
	11,11, 12,6/*mean (0.16652), correlation (0.497632)*/,
	7,-5, 12,-2/*mean (0.169141), correlation (0.484474)*/,
	-1,12, 0,7/*mean (0.169456), correlation (0.495339)*/,
	-4,-8, -3,-2/*mean (0.171457), correlation (0.487251)*/,
	-7,1, -6,7/*mean (0.175), correlation (0.500024)*/,
	-13,-12, -8,-13/*mean (0.175866), correlation (0.497523)*/,
	-7,-2, -6,-8/*mean (0.178273), correlation (0.501854)*/,
	-8,5, -6,-9/*mean (0.181107), correlation (0.494888)*/,
	-5,-1, -4,5/*mean (0.190227), correlation (0.482557)*/,
	-13,7, -8,10/*mean (0.196739), correlation (0.496503)*/,
	1,5, 5,-13/*mean (0.19973), correlation (0.499759)*/,
	1,0, 10,-13/*mean (0.204465), correlation (0.49873)*/,
	9,12, 10,-1/*mean (0.209334), correlation (0.49063)*/,
	5,-8, 10,-9/*mean (0.211134), correlation (0.503011)*/,
	-1,11, 1,-13/*mean (0.212), correlation (0.499414)*/,
	-9,-3, -6,2/*mean (0.212168), correlation (0.480739)*/,
	-1,-10, 1,12/*mean (0.212731), correlation (0.502523)*/,
	-13,1, -8,-10/*mean (0.21327), correlation (0.489786)*/,
	8,-11, 10,-6/*mean (0.214159), correlation (0.488246)*/,
	2,-13, 3,-6/*mean (0.216993), correlation (0.50287)*/,
	7,-13, 12,-9/*mean (0.223639), correlation (0.470502)*/,
	-10,-10, -5,-7/*mean (0.224089), correlation (0.500852)*/,
	-10,-8, -8,-13/*mean (0.228666), correlation (0.502629)*/,
	4,-6, 8,5/*mean (0.22906), correlation (0.498305)*/,
	3,12, 8,-13/*mean (0.233378), correlation (0.503825)*/,
	-4,2, -3,-3/*mean (0.234323), correlation (0.476692)*/,
	5,-13, 10,-12/*mean (0.236392), correlation (0.475462)*/,
	4,-13, 5,-1/*mean (0.236842), correlation (0.504132)*/,
	-9,9, -4,3/*mean (0.236977), correlation (0.497739)*/,
	0,3, 3,-9/*mean (0.24314), correlation (0.499398)*/,
	-12,1, -6,1/*mean (0.243297), correlation (0.489447)*/,
	3,2, 4,-8/*mean (0.00155196), correlation (0.553496)*/,
	-10,-10, -10,9/*mean (0.00239541), correlation (0.54297)*/,
	8,-13, 12,12/*mean (0.0034413), correlation (0.544361)*/,
	-8,-12, -6,-5/*mean (0.003565), correlation (0.551225)*/,
	2,2, 3,7/*mean (0.00835583), correlation (0.55285)*/,
	10,6, 11,-8/*mean (0.00885065), correlation (0.540913)*/,
	6,8, 8,-12/*mean (0.0101552), correlation (0.551085)*/,
	-7,10, -6,5/*mean (0.0102227), correlation (0.533635)*/,
	-3,-9, -3,9/*mean (0.0110211), correlation (0.543121)*/,
	-1,-13, -1,5/*mean (0.0113473), correlation (0.550173)*/,
	-3,-7, -3,4/*mean (0.0140913), correlation (0.554774)*/,
	-8,-2, -8,3/*mean (0.017049), correlation (0.55461)*/,
	4,2, 12,12/*mean (0.01778), correlation (0.546921)*/,
	2,-5, 3,11/*mean (0.0224022), correlation (0.549667)*/,
	6,-9, 11,-13/*mean (0.029161), correlation (0.546295)*/,
	3,-1, 7,12/*mean (0.0303081), correlation (0.548599)*/,
	11,-1, 12,4/*mean (0.0355151), correlation (0.523943)*/,
	-3,0, -3,6/*mean (0.0417904), correlation (0.543395)*/,
	4,-11, 4,12/*mean (0.0487292), correlation (0.542818)*/,
	2,-4, 2,1/*mean (0.0575124), correlation (0.554888)*/,
	-10,-6, -8,1/*mean (0.0594242), correlation (0.544026)*/,
	-13,7, -11,1/*mean (0.0597391), correlation (0.550524)*/,
	-13,12, -11,-13/*mean (0.0608974), correlation (0.55383)*/,
	6,0, 11,-13/*mean (0.065126), correlation (0.552006)*/,
	0,-1, 1,4/*mean (0.074224), correlation (0.546372)*/,
	-13,3, -9,-2/*mean (0.0808592), correlation (0.554875)*/,
	-9,8, -6,-3/*mean (0.0883378), correlation (0.551178)*/,
	-13,-6, -8,-2/*mean (0.0901035), correlation (0.548446)*/,
	5,-9, 8,10/*mean (0.0949843), correlation (0.554694)*/,
	2,7, 3,-9/*mean (0.0994152), correlation (0.550979)*/,
	-1,-6, -1,-1/*mean (0.10045), correlation (0.552714)*/,
	9,5, 11,-2/*mean (0.100686), correlation (0.552594)*/,
	11,-3, 12,-8/*mean (0.101091), correlation (0.532394)*/,
	3,0, 3,5/*mean (0.101147), correlation (0.525576)*/,
	-1,4, 0,10/*mean (0.105263), correlation (0.531498)*/,
	3,-6, 4,5/*mean (0.110785), correlation (0.540491)*/,
	-13,0, -10,5/*mean (0.112798), correlation (0.536582)*/,
	5,8, 12,11/*mean (0.114181), correlation (0.555793)*/,
	8,9, 9,-6/*mean (0.117431), correlation (0.553763)*/,
	7,-4, 8,-12/*mean (0.118522), correlation (0.553452)*/,
	-10,4, -10,9/*mean (0.12094), correlation (0.554785)*/,
	7,3, 12,4/*mean (0.122582), correlation (0.555825)*/,
	9,-7, 10,-2/*mean (0.124978), correlation (0.549846)*/,
	7,0, 12,-2/*mean (0.127002), correlation (0.537452)*/,
	-1,-6, 0,-11/*mean (0.127148), correlation (0.547401)*/
};

struct QTreeNode
{
	QTreeNode() : divisible(true) {}

	void divide(std::array<QTreeNode, 4>& nodes) const
	{
		const int hx = RoundUp(0.5 * (BR.x - TL.x));
		const int hy = RoundUp(0.5 * (BR.y - TL.y));

		const int x0 = TL.x;
		const int x1 = TL.x + hx;
		const int x2 = BR.x;

		const int y0 = TL.y;
		const int y1 = TL.y + hy;
		const int y2 = BR.y;

		// Define boundaries of childs
		nodes[0].TL = cv::Point(x0, y0);
		nodes[0].BR = cv::Point(x1, y1);

		nodes[1].TL = cv::Point(x1, y0);
		nodes[1].BR = cv::Point(x2, y1);

		nodes[2].TL = cv::Point(x0, y1);
		nodes[2].BR = cv::Point(x1, y2);

		nodes[3].TL = cv::Point(x1, y1);
		nodes[3].BR = cv::Point(x2, y2);

		for (int i = 0; i < 4; i++)
			nodes[i].keypoints.reserve(keypoints.size());

		// Associate points to childs
		for (const cv::KeyPoint& keypoint : keypoints)
		{
			const float x = keypoint.pt.x;
			const float y = keypoint.pt.y;
			const int index = x < x1 ? (y < y1 ? 0 : 2) : (y < y1 ? 1 : 3);
			nodes[index].keypoints.push_back(keypoint);
		}

		for (int i = 0; i < 4; i++)
			if (nodes[i].keypoints.size() == 1)
				nodes[i].divisible = false;
	}

	KeyPoints keypoints;
	cv::Point TL, BR;
	std::list<QTreeNode>::iterator it;
	bool divisible;
};

static void ComputePyramid(const cv::Mat& image, std::vector<cv::Mat>& images, const std::vector<float>& invScaleFactors)
{
	CV_Assert(image.type() == CV_8U);

	const int nlevels = static_cast<int>(invScaleFactors.size());
	images.resize(nlevels);

	image.copyTo(images[0]);
	for (int s = 1; s < nlevels; s++)
	{
		const float invScale = invScaleFactors[s];
		const int h = cvRound(invScale * image.rows);
		const int w = cvRound(invScale * image.cols);
		cv::resize(images[s - 1], images[s], cv::Size(w, h));
	}
}

static void ComputeNumFeaturesPerScale(int total, float scaleFactor, int nlevels, std::vector<int>& nfeaturesPerScale)
{
	// compute number of features in each scale
	nfeaturesPerScale.resize(nlevels);

	const double factor = 1 / scaleFactor;
	double nfeatues = total * (1 - factor) / (1 - std::pow(factor, nlevels));
	int sumfeatures = 0;
	for (int s = 0; s < nlevels - 1; s++)
	{
		nfeaturesPerScale[s] = cvRound(nfeatues);
		sumfeatures += nfeaturesPerScale[s];
		nfeatues *= factor;
	}
	nfeaturesPerScale[nlevels - 1] = std::max(total - sumfeatures, 0);
}

static void DetectFAST(const cv::Mat& image, cv::Rect roi, KeyPoints& keypoints, int iniThFAST, int minThFAST)
{
	const int CELL_SIZE = 30;

	keypoints.clear();

	if (roi.width <= 0 || roi.height <= 0)
	{
		roi = cv::Rect(0, 0, image.cols, image.rows);
	}

	const int w = roi.width;
	const int h = roi.height;

	const int minx = roi.x;
	const int miny = roi.y;
	const int maxx = roi.x + w;
	const int maxy = roi.y + h;

	const int gridw = w / CELL_SIZE;
	const int gridh = h / CELL_SIZE;
	const int cellw = RoundUp(1. * w / gridw);
	const int cellh = RoundUp(1. * h / gridh);

	const int FAST_RADIUS = 3;
	const int DIAMETER = 2 * FAST_RADIUS;

	KeyPoints _keypoints;
	_keypoints.reserve(cellw * cellh);

	for (int cy = 0, y0 = miny; cy < gridh && y0 + DIAMETER < maxy; cy++, y0 += cellh)
	{
		for (int cx = 0, x0 = minx; cx < gridw && x0 + DIAMETER < maxx; cx++, x0 += cellw)
		{
			const int y1 = std::min(y0 + cellh + DIAMETER, maxy);
			const int x1 = std::min(x0 + cellw + DIAMETER, maxx);

			cv::Mat _image = image(cv::Range(y0, y1), cv::Range(x0, x1));
			cv::FAST(_image, _keypoints, iniThFAST, true);

			if (_keypoints.empty())
				cv::FAST(_image, _keypoints, minThFAST, true);

			for (cv::KeyPoint& keypoint : _keypoints)
			{
				keypoint.pt.x += x0;
				keypoint.pt.y += y0;
				keypoints.push_back(keypoint);
			}
		}
	}
}

static void QuadTreeSuppression(const KeyPoints& src, cv::Rect roi, KeyPoints& dst, size_t nfeatures)
{
	if (src.empty() || roi.width <= 0 || roi.height <= 0)
		return;

	const int nnodes0 = cvRound(1. * roi.width / roi.height);
	const double hx = 1. * roi.width / nnodes0;

	std::list<QTreeNode> nodes;
	std::vector<QTreeNode*> pnodes(nnodes0);
	for (int i = 0; i < nnodes0; i++)
	{
		QTreeNode node;
		node.TL = cv::Point(static_cast<int>(roi.x + hx * (i + 0)), roi.y);
		node.BR = cv::Point(static_cast<int>(roi.x + hx * (i + 1)), roi.y + roi.height);
		node.keypoints.reserve(src.size());
		nodes.push_back(node);
		pnodes[i] = &nodes.back();
	}

	for (const cv::KeyPoint& keypoint : src)
	{
		const int nodeid = static_cast<int>((keypoint.pt.x - roi.x) / hx);
		CV_Assert(nodeid < nnodes0);
		pnodes[nodeid]->keypoints.push_back(keypoint);
	}

	for (auto it = nodes.begin(); it != nodes.end();)
	{
		if (it->keypoints.empty())
		{
			it = nodes.erase(it);
		}
		else
		{
			if (it->keypoints.size() == 1)
				it->divisible = false;
			++it;
		}
	}

	struct DivisibleNode { size_t size; const QTreeNode* ptr; };
	std::vector<DivisibleNode> divisibles;
	divisibles.reserve(4 * nodes.size());

	bool finish = false;
	while (!finish)
	{
		size_t prevSize = nodes.size();
		divisibles.clear();

		for (auto it = nodes.begin(); it != nodes.end();)
		{
			if (!it->divisible)
			{
				// If node only contains one point do not subdivide and continue
				++it;
				continue;
			}

			// If more than one point, subdivide
			std::array<QTreeNode, 4> children;
			it->divide(children);

			// Add children if they contain points
			for (const QTreeNode& child : children)
			{
				if (child.keypoints.empty())
					continue;

				nodes.push_front(child);
				if (child.keypoints.size() > 1)
				{
					QTreeNode& front = nodes.front();
					front.it = nodes.begin();
					divisibles.push_back({ child.keypoints.size(), &front });
				}
			}

			it = nodes.erase(it);
		}

		// Finish if there are more nodes than required features
		// or all nodes contain just one point
		if (nodes.size() >= nfeatures || nodes.size() == prevSize)
		{
			finish = true;
			break;
		}

		const int toExpand = static_cast<int>(divisibles.size());
		if (nodes.size() + 3 * toExpand > nfeatures)
		{
			while (!finish)
			{
				prevSize = nodes.size();

				std::vector<DivisibleNode> prevDivisibles = divisibles;
				divisibles.clear();

				std::sort(std::begin(prevDivisibles), std::end(prevDivisibles),
					[](const DivisibleNode& lhs, const DivisibleNode& rhs) { return lhs.size > rhs.size; });

				for (const auto& node : prevDivisibles)
				{
					std::array<QTreeNode, 4> children;
					node.ptr->divide(children);

					// Add children if they contain points
					for (const QTreeNode& child : children)
					{
						if (child.keypoints.empty())
							continue;

						nodes.push_front(child);
						if (child.keypoints.size() > 1)
						{
							QTreeNode& front = nodes.front();
							front.it = nodes.begin();
							divisibles.push_back({ child.keypoints.size(), &front });
						}
					}

					nodes.erase(node.ptr->it);
					if (nodes.size() >= nfeatures)
						break;
				}

				if (nodes.size() >= nfeatures || nodes.size() == prevSize)
					finish = true;
			}
		}
	}

	// Retain the best point in each node
	dst.clear();
	dst.reserve(src.size());
	for (const auto& node : nodes)
	{
		const cv::KeyPoint* bestKeypoint = nullptr;
		float maxResponse = 0.f;
		for (const cv::KeyPoint& keypoint : node.keypoints)
		{
			if (keypoint.response > maxResponse)
			{
				maxResponse = keypoint.response;
				bestKeypoint = &keypoint;
			}
		}
		dst.push_back(*bestKeypoint);
	}
}

ORBextractor::ORBextractor(const Parameters& param) : param_(param) { Init(); }

void ORBextractor::Init()
{
	const int npoints = 512;
	const cv::Point* pattern0 = reinterpret_cast<const cv::Point*>(bit_pattern_31_);
	std::copy(pattern0, pattern0 + npoints, std::back_inserter(pattern_));

	// This is for orientation
	// pre-compute the end of a row in a circular patch
	umax_.resize(HALF_PATCH_SIZE + 1);
	const int vmax = RoundDn(HALF_PATCH_SIZE * sqrt(2.) / 2 + 1);
	const int vmin = RoundUp(HALF_PATCH_SIZE * sqrt(2.) / 2);
	for (int v = 0; v <= vmax; ++v)
		umax_[v] = cvRound(sqrt(HALF_PATCH_SIZE * HALF_PATCH_SIZE - v * v));

	// Make sure we are symmetric
	for (int v = HALF_PATCH_SIZE, v0 = 0; v >= vmin; --v)
	{
		while (umax_[v0] == umax_[v0 + 1])
			++v0;
		umax_[v] = v0;
		++v0;
	}

	// Compute scales
	const int nlevels = param_.nlevels;
	const float scaleFactor = param_.scaleFactor;
	scaleFactors_.resize(nlevels);
	invScaleFactors_.resize(nlevels);
	sigmaSq_.resize(nlevels);
	invSigmaSq_.resize(nlevels);

	float scale = 1.f;
	for (int s = 0; s < nlevels; s++)
	{
		scaleFactors_[s] = scale;
		invScaleFactors_[s] = 1.f / scale;
		sigmaSq_[s] = scale * scale;
		invSigmaSq_[s] = 1.f / (scale * scale);

		scale *= scaleFactor;
	}

	// Compute number of features in each scale
	ComputeNumFeaturesPerScale(param_.nfeatures, scaleFactor, nlevels, nfeaturesPerScale_);
}

void ORBextractor::Extract(const cv::Mat& image, KeyPoints& keypoints, cv::Mat& descriptors)
{
	const int nfeatures = param_.nfeatures;
	const float scaleFactor = param_.scaleFactor;
	const int nlevels = param_.nlevels;

	keypoints_.resize(nlevels);
	blurImages_.resize(nlevels);

	// Compute pyramid image
	ComputePyramid(image, images_, invScaleFactors_);

	// Detect FAST corners
	const int BORDER = EDGE_THRESHOLD - 3;
	int nkeypoints = 0;
	for (int s = 0; s < nlevels; s++)
	{
		const cv::Mat& _image = images_[s];
		const cv::Rect roi(BORDER, BORDER, _image.cols - 2 * BORDER, _image.rows - 2 * BORDER);

		KeyPoints& _keypoints = keypoints_[s];
		_keypoints.reserve(10 * nfeatures);

		DetectFAST(_image, roi, _keypoints, param_.iniThFAST, param_.minThFAST);
		QuadTreeSuppression(_keypoints, roi, _keypoints, nfeaturesPerScale_[s]);

		for (cv::KeyPoint& keypoint : _keypoints)
		{
			keypoint.octave = s;
			keypoint.size = scaleFactors_[s] * PATCH_SIZE;
			keypoint.angle = IC_Angle(_image, keypoint.pt, umax_);
		}

		nkeypoints += static_cast<int>(_keypoints.size());
	}

	if (nkeypoints == 0)
	{
		descriptors.release();
		return;
	}

	// Compute descriptors
	descriptors.create(nkeypoints, 32, CV_8U);
	descriptors.setTo(0);

	keypoints.clear();
	keypoints.reserve(nkeypoints);

	int offset = 0;
	for (int s = 0; s < nlevels; s++)
	{
		KeyPoints& _keypoints = keypoints_[s];
		if (_keypoints.empty())
			continue;

		// preprocess the resized image
		cv::GaussianBlur(images_[s], blurImages_[s], cv::Size(7, 7), 2, 2, cv::BORDER_REFLECT_101);

		// Compute the descriptors
		const int npoints = static_cast<int>(_keypoints.size());
		for (int i = 0; i < npoints; i++)
		{
			uchar* desc = descriptors.ptr(i + offset);
			ComputeOrbDescriptor(_keypoints[i], blurImages_[s], pattern_.data(), desc);
		}
		offset += npoints;

		// Scale keypoint coordinates
		if (s > 0)
		{
			for (cv::KeyPoint& keypoint : _keypoints)
				keypoint.pt *= scaleFactors_[s];
		}

		// And add the keypoints to the output
		keypoints.insert(std::end(keypoints), std::begin(_keypoints), std::end(_keypoints));
	}
}

int ORBextractor::GetLevels() const { return param_.nlevels; }
float ORBextractor::GetScaleFactor() const { return param_.scaleFactor; }
const std::vector<float>& ORBextractor::GetScaleFactors() const { return scaleFactors_; }
const std::vector<float>& ORBextractor::GetInverseScaleFactors() const { return invScaleFactors_; }
const std::vector<float>& ORBextractor::GetScaleSigmaSquares() const { return sigmaSq_; }
const std::vector<float>& ORBextractor::GetInverseScaleSigmaSquares() const { return invSigmaSq_; }
const std::vector<cv::Mat>& ORBextractor::GetImagePyramid() const { return images_; }

ORBextractor::Parameters::Parameters(int nfeatures, float scaleFactor, int nlevels, int iniThFAST, int minThFAST)
	: nfeatures(nfeatures), scaleFactor(scaleFactor), nlevels(nlevels), iniThFAST(iniThFAST), minThFAST(minThFAST)
{
}

} //namespace ORB_SLAM
