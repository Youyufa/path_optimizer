//
// Created by ljn on 19-8-16.
//
#include <iostream>
#include <cmath>
#include <ctime>
#include "path_optimizer/path_optimizer.hpp"
#include "path_optimizer/reference_path_smoother/reference_path_smoother.hpp"
#include "path_optimizer/tools/tools.hpp"
#include "path_optimizer/data_struct/reference_path.hpp"
#include "path_optimizer/data_struct/data_struct.hpp"
#include "path_optimizer/data_struct/vehicle_state_frenet.hpp"
#include "path_optimizer/tools/collosion_checker.hpp"
#include "path_optimizer/tools/Map.hpp"
#include "path_optimizer/tools/spline.h"
#include "path_optimizer/solver/solver_factory.hpp"
#include "path_optimizer/solver/solver.hpp"
#include "tinyspline_ros/tinysplinecpp.h"
#include "path_optimizer/reference_path_smoother/angle_diff_smoother.hpp"
#include "path_optimizer/reference_path_smoother/tension_smoother.hpp"

namespace PathOptimizationNS {

PathOptimizer::PathOptimizer(const State &start_state,
                             const State &end_state,
                             const grid_map::GridMap &map) :
    grid_map_(new Map{map}),
    collision_checker_(new CollisionChecker{map}),
    reference_path_(new ReferencePath),
    vehicle_state_(new VehicleState{start_state, end_state, 0, 0}) {
    updateConfig();
}

PathOptimizer::~PathOptimizer() {
    delete grid_map_;
    delete collision_checker_;
    delete reference_path_;
    delete vehicle_state_;
}

bool PathOptimizer::solve(const std::vector<State> &reference_points, std::vector<State> *final_path) {
    if (FLAGS_enable_computation_time_output) std::cout << "------" << std::endl;
    CHECK_NOTNULL(final_path);

    auto t1 = std::clock();
    if (reference_points.empty()) {
        LOG(WARNING) << "Empty input, quit path optimization";
        return false;
    }
    reference_path_->clear();

    // Smooth reference path.
    // TODO: refactor this part!
    TensionSmoother
        reference_path_smoother(reference_points, vehicle_state_->getStartState(), *grid_map_);
    bool smoothing_ok = reference_path_smoother.solve(reference_path_, &smoothed_path_);
    reference_searching_display_ = reference_path_smoother.display();
    if (!smoothing_ok) {
        LOG(WARNING) << "Path optimization FAILED!";
        return false;
    }

    auto t2 = std::clock();
    // Divide reference path into segments;
    if (!segmentSmoothedPath()) {
        LOG(WARNING) << "Path optimization FAILED!";
        return false;
    }

    auto t3 = std::clock();
    // Optimize.
    if (optimizePath(final_path)) {
        auto t4 = std::clock();
        if (FLAGS_enable_computation_time_output) {
            time_ms_out(t1, t2, "Reference smoothing");
            time_ms_out(t2, t3, "Reference segmentation");
            time_ms_out(t3, t4, "Optimization phase");
            time_ms_out(t1, t4, "All");
        }
        LOG(INFO) << "Path optimization SUCCEEDED! Total time cost: " << time_s(t1, t4) << " s";
        return true;
    } else {
        LOG(WARNING) << "Path optimization FAILED!";
        return false;
    }
}

bool PathOptimizer::solveWithoutSmoothing(const std::vector<PathOptimizationNS::State> &reference_points,
                                          std::vector<PathOptimizationNS::State> *final_path) {
    // This function is used to calculate once more based on the previous result.
    if (FLAGS_enable_computation_time_output) std::cout << "------" << std::endl;
    CHECK_NOTNULL(final_path);
    auto t1 = std::clock();
    if (reference_points.empty()) {
        LOG(WARNING) << "Empty input, quit path optimization!";
        return false;
    }
    vehicle_state_->setInitError(0, 0);
    // Set reference path.
    reference_path_->clear();
    reference_path_->setReference(reference_points);
    reference_path_->updateBounds(*grid_map_);
    reference_path_->updateLimits();
    size_ = reference_path_->getSize();

    if (optimizePath(final_path)) {
        auto t2 = std::clock();
        if (FLAGS_enable_computation_time_output) {
            time_ms_out(t1, t2, "Solve without smoothing");
        }
        LOG(INFO) << "Path optimization without smoothing SUCCEEDED! Total time cost: "
                  << time_s(t1, t2) << " s";
        return true;
    } else {
        LOG(WARNING) << "Path optimization without smoothing FAILED!";
        return false;
    }
}

bool PathOptimizer::segmentSmoothedPath() {
    if (reference_path_->getLength() == 0) {
        LOG(WARNING) << "Smoothed path is empty!";
        return false;
    }

    // Calculate the initial deviation and angle difference.
    State first_point;
    first_point.x = reference_path_->getXS(0);
    first_point.y = reference_path_->getYS(0);
    first_point.z = getHeading(reference_path_->getXS(), reference_path_->getYS(), 0);
    auto first_point_local = global2Local(vehicle_state_->getStartState(), first_point);
    // In reference smoothing, the closest point tu the vehicle is found and set as the
    // first point. So the distance here is simply the initial offset.
    double min_distance = distance(vehicle_state_->getStartState(), first_point);
    double initial_offset = first_point_local.y < 0 ? min_distance : -min_distance;
    double initial_heading_error = constraintAngle(vehicle_state_->getStartState().z - first_point.z);
    vehicle_state_->setInitError(initial_offset, initial_heading_error);
    // If the start heading differs a lot with the ref path, quit.
    if (fabs(initial_heading_error) > 75 * M_PI / 180) {
        LOG(WARNING) << "Initial epsi is larger than 75°, quit path optimization!";
        return false;
    }

    double end_distance =
        sqrt(pow(vehicle_state_->getEndState().x - reference_path_->getXS(reference_path_->getLength()), 2) +
            pow(vehicle_state_->getEndState().y - reference_path_->getYS(reference_path_->getLength()), 2));
    if (end_distance > 0.001) {
        // If the goal position is not the same as the end position of the reference line,
        // then find the closest point to the goal and change max_s of the reference line.
        double search_delta_s = 0;
        if (FLAGS_enable_exact_position) {
            search_delta_s = 0.1;
        } else {
            search_delta_s = 0.3;
        }
        double tmp_s = reference_path_->getLength() - search_delta_s;
        auto min_dis_to_goal = end_distance;
        double min_dis_s = reference_path_->getLength();
        while (tmp_s > 0) {
            double x = reference_path_->getXS(tmp_s);
            double y = reference_path_->getYS(tmp_s);
            double tmp_dis = sqrt(pow(x - vehicle_state_->getEndState().x, 2) + pow(y - vehicle_state_->getEndState().y, 2));
            if (tmp_dis < min_dis_to_goal) {
                min_dis_to_goal = tmp_dis;
                min_dis_s = tmp_s;
            }
            tmp_s -= search_delta_s;
        }
        reference_path_->setLength(min_dis_s);
    }

    // Divide the reference path. Intervals are smaller at the beginning.
    double delta_s_smaller = 0.3;
    // If we want to make the result path dense later, the interval here is 1.0m. This makes computation faster;
    // If we want to output the result directly, the interval is controlled by config_->output_interval..
    double delta_s_larger = FLAGS_enable_raw_output ? FLAGS_output_spacing : 1.0;
    // If the initial heading error with the reference path is small, then set intervals equal.
    if (fabs(initial_heading_error) < 20 * M_PI / 180) delta_s_smaller = delta_s_larger;
    reference_path_->buildReferenceFromSpline(delta_s_smaller, delta_s_larger);
    reference_path_->updateBounds(*grid_map_);
    reference_path_->updateLimits();
    size_ = reference_path_->getSize();
    LOG(INFO) << "Reference path segmentation succeeded. Size: " << size_;
    return true;
}

bool PathOptimizer::optimizePath(std::vector<State> *final_path) {
    // Solve problem.
    std::shared_ptr<OsqpSolver> solver{SolverFactory::create(*reference_path_, *vehicle_state_, size_)};
    if (solver && !solver->solve(final_path)) {
        LOG(WARNING) << "QP failed.";
        return false;
    }
    LOG(INFO) << "QP succeeded.";

    // Output. Choose from:
    // 1. set the interval smaller and output the result directly.
    // 2. set the interval larger and use interpolation to make the result dense.
    if (FLAGS_enable_raw_output) {
        double s{0};
        for (auto iter = final_path->begin(); iter != final_path->end(); ++iter) {
            if (iter != final_path->begin()) s += distance(*(iter - 1), *iter);
            iter->s = s;
            if (FLAGS_enable_collision_check && !collision_checker_->isSingleStateCollisionFreeImproved(*iter)) {
                final_path->erase(iter, final_path->end());
                LOG(WARNING) << "collision check failed at " << final_path->back().s << "m.";
                return final_path->back().s >= 20;
            }
        }
        LOG(INFO) << "Output raw result.";
        return true;
    } else {
        std::vector<double> result_x, result_y, result_s;
        for (const auto &p : *final_path) {
            result_x.emplace_back(p.x);
            result_y.emplace_back(p.y);
            result_s.emplace_back(p.s);
        }
        tk::spline x_s, y_s;
        x_s.set_points(result_s, result_x);
        y_s.set_points(result_s, result_y);
        final_path->clear();
        double delta_s = FLAGS_output_spacing;
        for (int i = 0; i * delta_s <= result_s.back(); ++i) {
            double tmp_s = i * delta_s;
            State tmp_state{x_s(tmp_s),
                            y_s(tmp_s),
                            getHeading(x_s, y_s, tmp_s),
                            getCurvature(x_s, y_s, tmp_s),
                            tmp_s};
            if (FLAGS_enable_collision_check && !collision_checker_->isSingleStateCollisionFreeImproved(tmp_state)) {
                LOG(WARNING) << "[PathOptimizer] collision check failed at " << final_path->back().s << "m.";
                return final_path->back().s >= 20;
            }
            final_path->emplace_back(tmp_state);
        }
        LOG(INFO) << "Output densified result.";
        return true;
    }
}

const std::vector<State> &PathOptimizer::getSmoothedPath() const {
    return this->smoothed_path_;
}

std::vector<std::tuple<State, double, double>> PathOptimizer::display_abnormal_bounds() const {
    return this->reference_path_->display_abnormal_bounds();
}

const std::vector<std::vector<double>>& PathOptimizer::getSearchResult() const {
    return this->reference_searching_display_;
}

}
