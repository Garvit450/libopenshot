/**
 * @file
 * @brief Source file for CVStabilization class
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

/* LICENSE
 *
 * Copyright (c) 2008-2019 OpenShot Studios, LLC
 * <http://www.openshotstudios.com/>. This file is part of
 * OpenShot Library (libopenshot), an open-source project dedicated to
 * delivering high quality video editing and animation solutions to the
 * world. For more information visit <http://www.openshot.org/>.
 *
 * OpenShot Library (libopenshot) is free software: you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * OpenShot Library (libopenshot) is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with OpenShot Library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../include/CVStabilization.h"

// Set default smoothing window value to compute stabilization 
CVStabilization::CVStabilization():smoothingWindow(30) {}

// Set desirable smoothing window value to compute stabilization
CVStabilization::CVStabilization(int _smoothingWindow): smoothingWindow(_smoothingWindow){}

// Process clip and store necessary stabilization data
void CVStabilization::ProcessClip(openshot::Clip &video){
    // Get total number of frames in video
    int videoLenght = video.Reader()->info.video_length;

    // Extract and track opticalflow features for each frame
    for (long int frame_number = 0; frame_number <= videoLenght; frame_number++)
    {
        std::shared_ptr<openshot::Frame> f = video.GetFrame(frame_number);
        
        // Grab OpenCV Mat image
        cv::Mat cvimage = f->GetImageCV();
        cv::cvtColor(cvimage, cvimage, cv::COLOR_RGB2GRAY);
        TrackFrameFeatures(cvimage, frame_number);
    }

    // Calculate trajectory data
    std::vector <CamTrajectory> trajectory = ComputeFramesTrajectory();

    // Calculate and save smoothed trajectory data
    trajectoryData = SmoothTrajectory(trajectory);

    // Calculate and save transformation data
    transformationData = GenNewCamPosition(trajectoryData);
}

// Track current frame features and find the relative transformation
void CVStabilization::TrackFrameFeatures(cv::Mat frame, int frameNum){
    if(prev_grey.empty()){
        prev_grey = frame;
        return;
    }

    // OpticalFlow features vector
    std::vector <cv::Point2f> prev_corner, cur_corner;
    std::vector <cv::Point2f> prev_corner2, cur_corner2;
    std::vector <uchar> status;
    std::vector <float> err;
    
    // Extract new image features
    cv::goodFeaturesToTrack(prev_grey, prev_corner, 200, 0.01, 30);
    // Track features
    cv::calcOpticalFlowPyrLK(prev_grey, frame, prev_corner, cur_corner, status, err);

    // Remove untracked features
    for(size_t i=0; i < status.size(); i++) {
        if(status[i]) {
            prev_corner2.push_back(prev_corner[i]);
            cur_corner2.push_back(cur_corner[i]);
        }
    }
    // Translation + rotation only
    cv::Mat T = estimateRigidTransform(prev_corner2, cur_corner2, false); // false = rigid transform, no scaling/shearing

    // If no transformation is found, just use the last known good transform.
    if(T.data == NULL) {
        last_T.copyTo(T);
    }

    T.copyTo(last_T);
    // Decompose T
    double dx = T.at<double>(0,2);
    double dy = T.at<double>(1,2);
    double da = atan2(T.at<double>(1,0), T.at<double>(0,0));

    prev_to_cur_transform.push_back(TransformParam(dx, dy, da));

    cur.copyTo(prev);
    frame.copyTo(prev_grey);

    // Show processing info
    cout << "Frame: " << frameNum << " - good optical flow: " << prev_corner2.size() << endl;
}

std::vector<CamTrajectory> CVStabilization::ComputeFramesTrajectory(){

    // Accumulated frame to frame transform
    double a = 0;
    double x = 0;
    double y = 0;

    vector <CamTrajectory> trajectory; // trajectory at all frames
    
    // Compute global camera trajectory. First frame is the origin 
    for(size_t i=0; i < prev_to_cur_transform.size(); i++) {
        x += prev_to_cur_transform[i].dx;
        y += prev_to_cur_transform[i].dy;
        a += prev_to_cur_transform[i].da;

        // Save trajectory data to vector
        trajectory.push_back(CamTrajectory(x,y,a));
    }

    return trajectory;
}

std::map<size_t,CamTrajectory> CVStabilization::SmoothTrajectory(std::vector <CamTrajectory> &trajectory){

    std::map <size_t,CamTrajectory> smoothed_trajectory; // trajectory at all frames

    for(size_t i=0; i < trajectory.size(); i++) {
        double sum_x = 0;
        double sum_y = 0;
        double sum_a = 0;
        int count = 0;

        for(int j=-smoothingWindow; j <= smoothingWindow; j++) {
            if(i+j >= 0 && i+j < trajectory.size()) {
                sum_x += trajectory[i+j].x;
                sum_y += trajectory[i+j].y;
                sum_a += trajectory[i+j].a;

                count++;
            }
        }

        double avg_a = sum_a / count;
        double avg_x = sum_x / count;
        double avg_y = sum_y / count;

        // Add smoothed trajectory data to map
        smoothed_trajectory[i] = CamTrajectory(avg_x, avg_y, avg_a);
    }
    return smoothed_trajectory;
}

// Generate new transformations parameters for each frame to follow the smoothed trajectory
std::map<size_t,TransformParam> CVStabilization::GenNewCamPosition(std::map <size_t,CamTrajectory> &smoothed_trajectory){
    std::map <size_t,TransformParam> new_prev_to_cur_transform;

    // Accumulated frame to frame transform
    double a = 0;
    double x = 0;
    double y = 0;

    for(size_t i=0; i < prev_to_cur_transform.size(); i++) {
        x += prev_to_cur_transform[i].dx;
        y += prev_to_cur_transform[i].dy;
        a += prev_to_cur_transform[i].da;

        // target - current
        double diff_x = smoothed_trajectory[i].x - x;
        double diff_y = smoothed_trajectory[i].y - y;
        double diff_a = smoothed_trajectory[i].a - a;

        double dx = prev_to_cur_transform[i].dx + diff_x;
        double dy = prev_to_cur_transform[i].dy + diff_y;
        double da = prev_to_cur_transform[i].da + diff_a;

        // Add transformation data to map
        new_prev_to_cur_transform[i] = TransformParam(dx, dy, da);
    }
    return new_prev_to_cur_transform;
}




// Save stabilization data to protobuf file
bool CVStabilization::SaveStabilizedData(std::string outputFilePath){
    // Create stabilization message
    libopenshotstabilize::Stabilization stabilizationMessage;

    std::map<size_t,CamTrajectory>::iterator trajData = trajectoryData.begin();
    std::map<size_t,TransformParam>::iterator transData = transformationData.begin();

    // Iterate over all frames data and save in protobuf message
    for(; trajData != trajectoryData.end(); ++trajData, ++transData){
        AddFrameDataToProto(stabilizationMessage.add_frame(), trajData->second, transData->second, trajData->first);
    }
    // Add timestamp
    *stabilizationMessage.mutable_last_updated() = TimeUtil::SecondsToTimestamp(time(NULL));

    // Write the new message to disk.
    std::fstream output(outputFilePath, ios::out | ios::trunc | ios::binary);
    if (!stabilizationMessage.SerializeToOstream(&output)) {
        cerr << "Failed to write protobuf message." << endl;
        return false;
    }

    // Delete all global objects allocated by libprotobuf.
    google::protobuf::ShutdownProtobufLibrary();

    return true;
}

// Add frame stabilization data into protobuf message
void CVStabilization::AddFrameDataToProto(libopenshotstabilize::Frame* pbFrameData, CamTrajectory& trajData, TransformParam& transData, size_t frame_number){

    // Save frame number 
    pbFrameData->set_id(frame_number);

    // Save camera trajectory data
    pbFrameData->set_a(trajData.a);
    pbFrameData->set_x(trajData.x);
    pbFrameData->set_y(trajData.y);

    // Save transformation data
    pbFrameData->set_da(transData.da);
    pbFrameData->set_dx(transData.dx);
    pbFrameData->set_dy(transData.dy);
}

// Load protobuf data file
bool CVStabilization::LoadStabilizedData(std::string inputFilePath){
    // Create stabilization message
    libopenshotstabilize::Stabilization stabilizationMessage;

    // Read the existing tracker message.
    fstream input(inputFilePath, ios::in | ios::binary);
    if (!stabilizationMessage.ParseFromIstream(&input)) {
        cerr << "Failed to parse protobuf message." << endl;
        return false;
    }

    // Make sure the data maps are empty
    transformationData.clear();
    trajectoryData.clear();

    // Iterate over all frames of the saved message and assign to the data maps 
    for (size_t i = 0; i < stabilizationMessage.frame_size(); i++) {
        const libopenshotstabilize::Frame& pbFrameData = stabilizationMessage.frame(i);
    
        // Load frame number  
        int id = pbFrameData.id();

        // Load camera trajectory data
        float x = pbFrameData.x();
        float y = pbFrameData.y();
        float a = pbFrameData.a();

        // Assign data to trajectory map
        trajectoryData[i] = CamTrajectory(x,y,a);

        // Load transformation data
        float dx = pbFrameData.dx();
        float dy = pbFrameData.dy();
        float da = pbFrameData.da();

        // Assing data to transformation map
        transformationData[i] = TransformParam(dx,dy,da);
    }

    // Show the time stamp from the last update in stabilization data file  
    if (stabilizationMessage.has_last_updated()) {
        cout << "  Loaded Data. Saved Time Stamp: " << TimeUtil::ToString(stabilizationMessage.last_updated()) << endl;
    }

    // Delete all global objects allocated by libprotobuf.
    google::protobuf::ShutdownProtobufLibrary();

    return true;
}