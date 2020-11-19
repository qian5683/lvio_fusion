#include "lvio_fusion/loop/relocation.h"
#include "lvio_fusion/ceres/imu_error.hpp"
#include "lvio_fusion/ceres/loop_error.hpp"
#include "lvio_fusion/ceres/navsat_error.hpp"
#include "lvio_fusion/ceres/visual_error.hpp"
#include "lvio_fusion/utility.h"
#include "lvio_fusion/visual/feature.h"
#include "lvio_fusion/visual/landmark.h"

#include <DBoW3/QueryResults.h>
#include <opencv2/core/eigen.hpp>
#include <pcl/filters/voxel_grid.h>
#include <pcl/io/pcd_io.h>
#include <pcl/registration/icp.h>

namespace lvio_fusion
{

Relocation::Relocation(std::string voc_path)
{
    detector_ = cv::ORB::create();
    voc_ = DBoW3::Vocabulary(voc_path);
    db_ = DBoW3::Database(voc_, false, 0);
    thread_ = std::thread(std::bind(&Relocation::RelocationLoop, this));
}

void Relocation::RelocationLoop()
{
    static double old_time = DBL_MAX;
    static double start_time = 0;
    static Frame::Ptr last_frame;
    static Frame::Ptr last_old_frame;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto new_kfs = map_->GetKeyFrames(head, backend_->head);
        if (new_kfs.empty())
        {
            continue;
        }
        for (auto pair_kf : new_kfs)
        {
            Frame::Ptr frame = pair_kf.second, old_frame;
            AddKeyFrameIntoVoc(frame);
            // if last is loop and this is not loop, then correct all new loops
            if (DetectLoop(frame, old_frame))
            {
                if (!last_old_frame && Relocate(frame, old_frame))
                {
                    // get all new loop frames [start_time, end_time]
                    start_time = pair_kf.first;
                    last_old_frame = old_frame;
                }
                if (last_old_frame)
                {
                    last_frame = frame;
                    last_old_frame = old_frame;
                    old_time = std::min(old_time, old_frame->time);
                }
            }
            else if (last_old_frame)
            {
                if (Relocate(last_frame, last_old_frame))
                {
                    LOG(INFO) << "Detected new loop, and correct it now. old_time:" << old_time << ";start_time:" << start_time << ";end_time:" << last_frame->time;
                    auto t1 = std::chrono::steady_clock::now();
                    CorrectLoop(old_time, start_time, last_frame->time);
                    auto t2 = std::chrono::steady_clock::now();
                    auto time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
                    LOG(INFO) << "Correct Loop cost time: " << time_used.count() << " seconds.";
                }
                old_time = DBL_MAX;
                last_old_frame = last_frame = nullptr;
            }
        }
        head = (--new_kfs.end())->first + epsilon;
    }
}

void Relocation::AddKeyFrameIntoVoc(Frame::Ptr frame)
{
    // compute descriptors
    std::vector<cv::KeyPoint> keypoints;
    for (auto pair_feature : frame->features_left)
    {
        keypoints.push_back(cv::KeyPoint(pair_feature.second->keypoint, 1));
    }
    cv::Mat descriptors;
    detector_->compute(frame->image_left, keypoints, descriptors);
    DBoW3::EntryId id = db_.add(descriptors);
    map_dbow_to_frames_[id] = frame->time;

    // NOTE: detector_->compute maybe remove some row because its descriptor cannot be computed
    int j = 0, i = 0;
    frame->descriptors = cv::Mat::zeros(frame->features_left.size(), 32, CV_8U);
    for (auto pair_feature : frame->features_left)
    {
        if (pair_feature.second->keypoint == keypoints[j].pt && j < descriptors.rows)
        {
            descriptors.row(j).copyTo(frame->descriptors.row(i));
            j++;
        }
        i++;
    }
}

bool Relocation::DetectLoop(Frame::Ptr frame, Frame::Ptr &old_frame)
{
    // NOTE: DBow3 is not good
    // //first query; then add this frame into database!
    // DBoW3::QueryResults ret;
    // db_.query(frame->descriptors, ret, 4, frame->id - 20);
    // // ret[0] is the nearest neighbour's score. threshold change with neighour score
    // bool find_loop = false;
    // cv::Mat loop_result;
    // // a good match with its nerghbour
    // if (ret.size() >= 1 && ret[0].Score > 0.05)
    //     for (unsigned int i = 1; i < ret.size(); i++)
    //     {
    //         if (ret[i].Score > 0.015)
    //         {
    //             find_loop = true;
    //         }
    //     }
    // if (find_loop && frame->id > 20)
    // {
    //     int max_index = -1;
    //     for (unsigned int i = 0; i < ret.size(); i++)
    //     {
    //         if (max_index == -1 || (ret[i].Id > max_index && ret[i].Score > 0.015))
    //             max_index = ret[i].Id;
    //     }
    //     old_frame = map_->GetAllKeyFrames()[map_dbow_to_frames_[max_index]];
    //     // check the distance
    //     if ((frame->pose.inverse().translation() - old_frame->pose.inverse().translation()).norm() < 20)
    //     {
    //         return true;
    //     }
    // }
    // return false;
    Frames candidate_kfs = map_->GetKeyFrames(0, backend_->head - 10);
    double min_distance = 10;
    for (auto pair_kf : candidate_kfs)
    {
        Vector3d vec = (pair_kf.second->pose.inverse().translation() - frame->pose.inverse().translation());
        vec.z() = 0;
        double distance = vec.norm();
        if (distance < min_distance)
        {
            Frame::Ptr prev_frame = map_->GetKeyFrames(0, frame->time, 1).begin()->second;
            Frame::Ptr subs_frame = map_->GetKeyFrames(frame->time, 0, 1).begin()->second;
            Vector3d prev_vec = (pair_kf.second->pose.inverse().translation() - prev_frame->pose.inverse().translation());
            Vector3d subs_vec = (pair_kf.second->pose.inverse().translation() - subs_frame->pose.inverse().translation());
            prev_vec.z() = 0;
            subs_vec.z() = 0;
            double prev_distance = prev_vec.norm();
            double subs_distance = subs_vec.norm();
            if (prev_distance < min_distance && subs_distance < min_distance)
            {
                min_distance = distance;
                old_frame = pair_kf.second;
            }
        }
    }
    if (old_frame)
    {
        return true;
    }
    return false;
}

bool Relocation::Relocate(Frame::Ptr frame, Frame::Ptr old_frame)
{
    loop::LoopConstraint::Ptr loop_constraint = loop::LoopConstraint::Ptr(new loop::LoopConstraint());
    // RelocateByImage(frames, old_frames, loop_constraint);
    if (!lidar_ || RelocateByPoints(frame, old_frame, loop_constraint))
    {
        loop_constraint->frame_old = old_frame;
        frame->loop_constraint = loop_constraint;
        return true;
    }
    return false;
}

bool Relocation::RelocateByImage(Frame::Ptr frame, Frame::Ptr old_frame, loop::LoopConstraint::Ptr loop_constraint)
{
    std::vector<cv::Point3f> points_3d;
    std::vector<cv::Point2f> points_2d;
    // search by BRIEFDes
    auto descriptors = mat2briefs(frame);
    auto descriptors_old = mat2briefs(old_frame);
    for (auto pair_desciptor : descriptors)
    {
        unsigned long best_id = 0;
        if (SearchInAera(pair_desciptor.second, descriptors_old, best_id))
        {
            cv::Point2f point_2d = old_frame->features_left[best_id]->keypoint;
            visual::Landmark::Ptr landmark = old_frame->features_left[best_id]->landmark.lock();
            visual::Feature::Ptr new_left_feature = visual::Feature::Create(frame, point_2d, landmark);
            points_2d.push_back(point_2d);
            points_3d.push_back(eigen2cv(landmark->position));
        }
    }
    // solve pnp ransca
    cv::Mat K;
    cv::eigen2cv(camera_left_->K(), K);
    cv::Mat rvec, tvec, inliers, D, cv_R;
    if (points_2d.size() >= 20 && cv::solvePnPRansac(points_3d, points_2d, K, D, rvec, tvec, false, 100, 8.0F, 0.98, cv::noArray(), cv::SOLVEPNP_EPNP))
    {
        cv::Rodrigues(rvec, cv_R);
        Matrix3d R;
        cv::cv2eigen(cv_R, R);
        loop_constraint->relative_pose = camera_left_->extrinsic.inverse() * SE3d(SO3d(R), Vector3d(tvec.at<double>(0, 0), tvec.at<double>(1, 0), tvec.at<double>(2, 0)));
        loop_constraint->frame_old = old_frame;
        return true;
    }
    return false;
}

bool Relocation::RelocateByPoints(Frame::Ptr frame, Frame::Ptr old_frame, loop::LoopConstraint::Ptr loop_constraint)
{
    if (!frame->feature_lidar || !old_frame->feature_lidar)
    {
        return false;
    }

    // init relative pose
    SE3d init_transform = old_frame->pose * frame->pose.inverse();
    init_transform.translation().z() = 0;

    // build two pointclouds
    PointICloud::Ptr pc = PointICloud::Ptr(new PointICloud);
    *pc = frame->feature_lidar->points_less_sharp + frame->feature_lidar->points_less_flat;
    PointICloud::Ptr pc_old = PointICloud::Ptr(new PointICloud);
    *pc_old = old_frame->feature_lidar->points_less_sharp + old_frame->feature_lidar->points_less_flat;
    Frame::Ptr old_frame_prev = map_->GetKeyFrames(0, old_frame->time, 1).begin()->second;
    if (old_frame->feature_lidar)
    {
        PointICloud pc_old_prev_less_flat, pc_old_prev_less_sharp;
        mapping_->MergeScan(old_frame_prev->feature_lidar->points_less_flat, old_frame_prev->pose, pc_old_prev_less_flat);
        mapping_->MergeScan(old_frame_prev->feature_lidar->points_less_sharp, old_frame_prev->pose, pc_old_prev_less_sharp);
        *pc_old += pc_old_prev_less_sharp;
        *pc_old += pc_old_prev_less_flat;
    }
    Frame::Ptr old_frame_subs = map_->GetKeyFrames(old_frame->time, 0, 1).begin()->second;
    if (old_frame->feature_lidar)
    {
        PointICloud pc_old_subs_less_flat, pc_old_subs_less_sharp;
        mapping_->MergeScan(old_frame_subs->feature_lidar->points_less_flat, old_frame_subs->pose, pc_old_subs_less_flat);
        mapping_->MergeScan(old_frame_subs->feature_lidar->points_less_sharp, old_frame_subs->pose, pc_old_subs_less_sharp);
        *pc_old += pc_old_subs_less_sharp;
        *pc_old += pc_old_subs_less_flat;
    }
    pcl::transformPointCloud(*pc, *pc, init_transform.matrix().cast<float>());

    // downsample old pointclouds
    PointICloud::Ptr pc_old_filtered(new PointICloud);
    pcl::VoxelGrid<PointI> voxel_filter;
    voxel_filter.setLeafSize(0.4, 0.4, 0.4);
    voxel_filter.setInputCloud(pc_old);
    voxel_filter.filter(*pc_old_filtered);

    // icp
    pcl::IterativeClosestPoint<PointI, PointI> icp;
    icp.setInputSource(pc);
    icp.setInputTarget(pc_old_filtered);
    icp.setMaximumIterations(100);
    icp.setMaxCorrespondenceDistance(1);
    PointICloud::Ptr aligned(new PointICloud);
    icp.align(*aligned);
    if (!icp.hasConverged()) // || icp.getFitnessScore() > 1)
    {
        return false;
    }

    // optimize
    Matrix4d transform_matrix = icp.getFinalTransformation().cast<double>();
    Matrix3d R(transform_matrix.block(0, 0, 3, 3));
    Quaterniond q(R);
    Vector3d t(0, 0, 0);
    t << transform_matrix(0, 3), transform_matrix(1, 3), transform_matrix(2, 3);
    SE3d transform(q, t);
    SE3d relative_pose = (transform * init_transform).inverse();

    Frame::Ptr frame_copy = Frame::Ptr(new Frame);
    *frame_copy = *frame;
    frame_copy->pose = relative_pose * old_frame->pose;
    static int num_iters = 4;
    for (int i = 0; i < num_iters; i++)
    {
        ceres::Problem problem;
        ceres::LossFunction *lidar_loss_function = new ceres::HuberLoss(0.1);
        ceres::LocalParameterization *local_parameterization = new ceres::ProductParameterization(
            new ceres::EigenQuaternionParameterization(),
            new ceres::IdentityParameterization(3));

        double *para_kf = frame_copy->pose.data();
        problem.AddParameterBlock(para_kf, SE3d::num_parameters, local_parameterization);
        double *para_kf_old = old_frame->pose.data();
        problem.AddParameterBlock(para_kf_old, SE3d::num_parameters, local_parameterization);
        problem.SetParameterBlockConstant(para_kf_old);

        // association_->Associate(frame_copy, old_frame, problem, lidar_loss_function);

        ceres::Solver::Options options;
        options.linear_solver_type = ceres::DENSE_QR;
        options.function_tolerance = DBL_MIN;
        options.gradient_tolerance = DBL_MIN;
        options.max_num_iterations = 1;
        options.num_threads = 4;
        ceres::Solver::Summary summary;
        ceres::Solve(options, &problem, &summary);
        if (summary.final_cost / summary.initial_cost > 0.99)
            break;
    }
    loop_constraint->relative_pose = frame_copy->pose * old_frame->pose.inverse();
    return true;
}

bool Relocation::SearchInAera(const BRIEF descriptor, const std::map<unsigned long, BRIEF> &descriptors_old, unsigned long &best_id)
{
    cv::Point2f best_pt;
    int best_distance = 256;
    for (auto pair_desciptor : descriptors_old)
    {
        int distance = Hamming(descriptor, pair_desciptor.second);
        if (distance < best_distance)
        {
            best_distance = distance;
            best_id = pair_desciptor.first;
        }
    }
    return best_distance < 160;
}

int Relocation::Hamming(const BRIEF &a, const BRIEF &b)
{
    BRIEF xor_of_bitset = a ^ b;
    int dis = xor_of_bitset.count();
    return dis;
}

void Relocation::BuildProblem(Frames &active_kfs, ceres::Problem &problem)
{
    ceres::LossFunction *loss_function = new ceres::TrivialLoss();
    ceres::LocalParameterization *local_parameterization = new ceres::ProductParameterization(
        new ceres::EigenQuaternionParameterization(),
        new ceres::IdentityParameterization(3));

    double start_time = active_kfs.begin()->first;

    Frame::Ptr last_frame;
    for (auto pair_kf : active_kfs)
    {
        auto frame = pair_kf.second;
        double *para_kf = frame->pose.data();
        problem.AddParameterBlock(para_kf, SE3d::num_parameters, local_parameterization);
        if (last_frame)
        {
            double *para_last_kf = last_frame->pose.data();
            ceres::CostFunction *cost_function;
            cost_function = PoseGraphError::Create(last_frame->pose, frame->pose);
            problem.AddResidualBlock(cost_function, loss_function, para_last_kf, para_kf);
        }
        last_frame = frame;
    }

    // for (auto pair_kf : active_kfs)
    // {
    //     auto frame = pair_kf.second;
    //     double *para_kf = frame->pose.data();
    //     problem.AddParameterBlock(para_kf, SE3d::num_parameters, local_parameterization);
    //     for (auto pair_feature : frame->features_left)
    //     {
    //         auto feature = pair_feature.second;
    //         auto landmark = feature->landmark.lock();
    //         auto first_frame = landmark->FirstFrame().lock();
    //         ceres::CostFunction *cost_function;
    //         if (first_frame->time < start_time)
    //         {
    //             cost_function = PoseOnlyReprojectionError::Create(cv2eigen(feature->keypoint), landmark->ToWorld(), camera_left_);
    //             problem.AddResidualBlock(cost_function, loss_function, para_kf);
    //         }
    //         else if (first_frame != frame)
    //         {
    //             if (active_kfs.find(first_frame->time) == active_kfs.end())
    //             {
    //                 auto old_frame = active_kfs[inner_old_frame.upper_bound(first_frame->time)->first];
    //                 SE3d relative_pose = first_frame->pose * old_frame->pose.inverse();
    //                 double *para_old_kf = old_frame->pose.data();
    //                 cost_function = TwoFrameReprojectionErrorBasedLoop::Create(landmark->position, cv2eigen(feature->keypoint), camera_left_, relative_pose);
    //                 problem.AddResidualBlock(cost_function, loss_function, para_old_kf, para_kf);
    //             }
    //             else
    //             {
    //                 double *para_fist_kf = first_frame->pose.data();
    //                 cost_function = TwoFrameReprojectionError::Create(landmark->position, cv2eigen(feature->keypoint), camera_left_);
    //                 problem.AddResidualBlock(cost_function, loss_function, para_fist_kf, para_kf);
    //             }
    //         }
    //     }
    // }

    // // navsat constraints
    // auto navsat_map = map_->navsat_map;
    // if (map_->navsat_map != nullptr && navsat_map->initialized)
    // {
    //     ceres::LossFunction *navsat_loss_function = new ceres::TrivialLoss();
    //     for (auto pair_kf : active_kfs)
    //     {
    //         auto frame = pair_kf.second;
    //         auto para_kf = frame->pose.data();
    //         auto np_iter = navsat_map->navsat_points.lower_bound(pair_kf.first);
    //         auto navsat_point = np_iter->second;
    //         navsat_map->Transfrom(navsat_point);
    //         if (std::fabs(navsat_point.time - frame->time) < 1e-2)
    //         {
    //             ceres::CostFunction *cost_function = NavsatError::Create(navsat_point.position);
    //             problem.AddResidualBlock(cost_function, navsat_loss_function, para_kf);
    //         }
    //     }
    // }

    // TODO:
    // imu constraints
    // if (imu_ && imu_->initialized)
    // {
    //     Frame::Ptr last_frame;
    //     Frame::Ptr current_frame;
    //     for (auto pair_kf : active_kfs)
    //     {
    //         current_frame = pair_kf.second;
    //         if (!current_frame->preintegration)
    //             continue;
    //         auto para_kf = current_frame->pose.data();
    //         auto para_v = current_frame->preintegration->v0.data();
    //         auto para_ba = current_frame->preintegration->linearized_ba.data();
    //         auto para_bg = current_frame->preintegration->linearized_bg.data();
    //         problem.AddParameterBlock(para_v, 3);
    //         problem.AddParameterBlock(para_ba, 3);
    //         problem.AddParameterBlock(para_bg, 3);
    //         if (last_frame && last_frame->preintegration)
    //         {
    //             auto para_kf_last = last_frame->pose.data();
    //             auto para_v_last = last_frame->preintegration->v0.data();
    //             auto para_ba_last = last_frame->preintegration->linearized_ba.data();
    //             auto para_bg_last = last_frame->preintegration->linearized_bg.data();
    //             ceres::CostFunction *cost_function = ImuError::Create(last_frame->preintegration);
    //             problem.AddResidualBlock(cost_function, NULL, para_kf_last, para_v_last, para_ba_last, para_bg_last, para_kf, para_v, para_ba, para_bg);
    //         }
    //         last_frame = current_frame;
    //     }
    // }

    // loop constraint
    for (auto pair_kf : active_kfs)
    {
        auto frame = pair_kf.second;
        if (frame->loop_constraint)
        {
            double *para_kf = frame->pose.data();
            problem.SetParameterBlockConstant(para_kf);

            auto old_frame = frame->loop_constraint->frame_old;
            if (old_frame->time >= start_time)
            {
                double *para_old_kf = old_frame->pose.data();
                problem.SetParameterBlockConstant(para_old_kf);
            }
        }
    }
}

void Relocation::CorrectLoop(double old_time, double start_time, double end_time)
{
    if (start_time == end_time)
        return;

    // build the active submaps
    Frames active_kfs = map_->GetKeyFrames(old_time, end_time);
    Frames new_submap_kfs = map_->GetKeyFrames(start_time, end_time);
    SE3d old_pose = (--active_kfs.end())->second->pose;
    {
        // optimize new submap pose graph
        ceres::Problem problem;
        BuildProblem(new_submap_kfs, problem);

        // update pose of new submap
        for (auto pair_kf : new_submap_kfs)
        {
            Frame::Ptr frame = pair_kf.second;
            if (frame->loop_constraint)
            {
                frame->pose = frame->loop_constraint->relative_pose * frame->loop_constraint->frame_old->pose;
            }
        }

        ceres::Solver::Options options;
        options.linear_solver_type = ceres::DENSE_SCHUR;
        options.max_num_iterations = 5;
        options.num_threads = 4;
        ceres::Solver::Summary summary;
        ceres::Solve(options, &problem, &summary);
        LOG(INFO) << summary.FullReport();
    }
    SE3d new_pose = (--active_kfs.end())->second->pose;

    // forward propogate
    {
        std::unique_lock<std::mutex> lock1(backend_->mutex);
        std::unique_lock<std::mutex> lock2(frontend_->mutex);

        Frame::Ptr last_frame = frontend_->last_frame;
        Frames forward_kfs = map_->GetKeyFrames(end_time + epsilon);
        if (forward_kfs.find(last_frame->time) == forward_kfs.end())
        {
            forward_kfs[last_frame->time] = last_frame;
        }
        SE3d transform = old_pose.inverse() * new_pose;
        for (auto pair_kf : forward_kfs)
        {
            pair_kf.second->pose = pair_kf.second->pose * transform;
            // TODO: Repropagate
        }
        frontend_->UpdateCache();
    }

    // add submap
    std::map<double, SE3d> inner_old_frames = atlas_.GetActiveSubMaps(active_kfs, old_time, start_time, end_time);
    atlas_.AddSubMap(old_time, start_time, end_time);

    // optimize pose graph
    ceres::Problem problem;
    BuildProblem(active_kfs, problem);

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_SCHUR;
    options.max_num_iterations = 5;
    options.num_threads = 4;
    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);

    // update pose of inner submaps
    Frames all_kfs = map_->GetKeyFrames(old_time, end_time);
    for (auto pair_old_frame : inner_old_frames)
    {
        auto old_frame = active_kfs[pair_old_frame.first];
        // T2_new = T2 * T1.inverse() * T1_new
        SE3d transform = pair_old_frame.second.inverse() * old_frame->pose;
        for (auto iter = ++all_kfs.find(pair_old_frame.first); active_kfs.find(iter->first) == active_kfs.end(); iter++)
        {
            auto frame = iter->second;
            frame->pose = frame->pose * transform;
        }
    }

    // mapping
    if (lidar_)
    {
        mapping_->Optimize(active_kfs);
    }
}

} // namespace lvio_fusion