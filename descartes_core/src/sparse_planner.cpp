/*
 * Software License Agreement (Apache License)
 *
 * Copyright (c) 2014, Southwest Research Institute
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * sparse_planner.cpp
 *
 *  Created on: Dec 17, 2014
 *      Author: ros developer 
 */

#include <descartes_core/sparse_planner.h>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>

namespace descartes_core
{

const int MAX_REPLANNING_ATTEMPTS = 100;
const int INVALID_INDEX = -1;

SparsePlanner::SparsePlanner(RobotModelConstPtr &model,double sampling):
    PlanningGraph(model),
    sampling_(sampling)
{

}

SparsePlanner::~SparsePlanner()
{

}

void SparsePlanner::setSampling(double sampling)
{
  sampling_ = sampling;
}

bool SparsePlanner::setTrajectoryPoints(const std::vector<TrajectoryPtPtr>& traj)
{
  cart_points_.assign(traj.begin(),traj.end());
  std::vector<TrajectoryPtPtr> sparse_trajectory_array;
  sampleTrajectory(sampling_,cart_points_,sparse_trajectory_array);
  ROS_INFO_STREAM("Sampled trajectory contains "<<sparse_trajectory_array.size()<<" points from "<<cart_points_.size()<<
                  " points in the dense trajectory");

  if(insertGraph(&sparse_trajectory_array) && plan())
  {
    int planned_count = sparse_trajectory_array.size();
    int interp_count = cart_points_.size()  - sparse_trajectory_array.size();
    ROS_INFO("Sparse plan succeeded with %i planned point and %i interpolated points",planned_count,interp_count);
  }
  else
  {
    return false;
  }
  return true;
}

bool SparsePlanner::addTrajectoryPointAfter(const TrajectoryPt::ID& ref_id,TrajectoryPtPtr cp)
{
  int sparse_index;
  int index;
  TrajectoryPt::ID prev_id, next_id;

  sparse_index= findNearestSparsePointIndex(ref_id);
  if(sparse_index == INVALID_INDEX)
  {
    ROS_ERROR_STREAM("A point in sparse array near point "<<ref_id<<" could not be found, aborting");
    return false;
  }

  // setting ids from sparse array
  prev_id = std::get<1>(sparse_solution_array_[sparse_index - 1])->getID();
  next_id = std::get<1>(sparse_solution_array_[sparse_index])->getID();

  // inserting into dense array
  index = getDensePointIndex(ref_id);
  if(index == INVALID_INDEX)
  {
    ROS_ERROR_STREAM("Point  "<<ref_id<<" could not be found in dense array, aborting");
    return false;
  }
  auto pos = cart_points_.begin();
  std::advance(pos,index + 1);
  cart_points_.insert(pos,cp);

  // replanning
  if(addTrajectory(cp,prev_id,next_id) && plan())
  {
    int planned_count = sparse_solution_array_.size();
    int interp_count = cart_points_.size()  - sparse_solution_array_.size();
    ROS_INFO("Sparse plan succeeded with %i planned point and %i interpolated points",planned_count,interp_count);
  }
  else
  {
    return false;
  }

  return true;
}

bool SparsePlanner::addTrajectoryPointBefore(const TrajectoryPt::ID& ref_id,TrajectoryPtPtr cp)
{
  int sparse_index;
  int index;
  TrajectoryPt::ID prev_id, next_id;

  sparse_index= findNearestSparsePointIndex(ref_id,false);
  if(sparse_index == INVALID_INDEX)
  {
    ROS_ERROR_STREAM("A point in sparse array near point "<<ref_id<<" could not be found, aborting");
    return false;
  }

  prev_id = (sparse_index == 0) ? boost::uuids::nil_uuid() : std::get<1>(sparse_solution_array_[sparse_index - 1])->getID();
  next_id = std::get<1>(sparse_solution_array_[sparse_index])->getID();

  // inserting into dense array
  index = getDensePointIndex(ref_id);
  if(index == INVALID_INDEX)
  {
    ROS_ERROR_STREAM("Point  "<<ref_id<<" could not be found in dense array, aborting");
    return false;
  }
  auto pos = cart_points_.begin();
  std::advance(pos,index);
  cart_points_.insert(pos,cp);

  if(addTrajectory(cp,prev_id,next_id) && plan())
  {
    int planned_count = sparse_solution_array_.size();
    int interp_count = cart_points_.size()  - sparse_solution_array_.size();
    ROS_INFO("Sparse plan succeeded with %i planned point and %i interpolated points",planned_count,interp_count);
  }
  else
  {
    return false;
  }

  return true;
}

bool SparsePlanner::removeTrajectoryPoint(const TrajectoryPt::ID& ref_id)
{
  int index = getDensePointIndex(ref_id);
  if(index == INVALID_INDEX)
  {
    ROS_ERROR_STREAM("Point  "<<ref_id<<" could not be found in dense array, aborting");
    return false;
  }

  if(isInSparseTrajectory(ref_id))
  {
    if(!removeTrajectory(cart_points_[index]))
    {
      ROS_ERROR_STREAM("Failed to removed point "<<ref_id<<" from sparse trajectory, aborting");
      return false;
    }
  }

  // removing from dense array
  auto pos = cart_points_.begin();
  std::advance(pos,index);
  cart_points_.erase(pos);

  if(plan())
  {
    int planned_count = sparse_solution_array_.size();
    int interp_count = cart_points_.size()  - sparse_solution_array_.size();
    ROS_INFO("Sparse plan succeeded with %i planned point and %i interpolated points",planned_count,interp_count);
  }
  else
  {
    return false;
  }

  return true;
}

bool SparsePlanner::modifyTrajectoryPoint(const TrajectoryPt::ID& ref_id,TrajectoryPtPtr cp)
{
  int sparse_index;
  TrajectoryPt::ID prev_id, next_id;

  sparse_index= getSparsePointIndex(ref_id);
  cp->setID(ref_id);
  if(sparse_index == INVALID_INDEX)
  {
    sparse_index = findNearestSparsePointIndex(ref_id);
    prev_id = std::get<1>(sparse_solution_array_[sparse_index - 1])->getID();
    next_id = std::get<1>(sparse_solution_array_[sparse_index])->getID();
    if(!addTrajectory(cp,prev_id,next_id))
    {
      ROS_ERROR_STREAM("Failed to add point to sparse trajectory, aborting");
      return false;
    }
  }
  else
  {
    if(!modifyTrajectory(cp))
    {
      ROS_ERROR_STREAM("Failed to modify point in sparse trajectory, aborting");
      return false;
    }
  }

  int index = getDensePointIndex(ref_id);
  cart_points_[index] = cp;
  if( plan())
  {
    int planned_count = sparse_solution_array_.size();
    int interp_count = cart_points_.size()  - sparse_solution_array_.size();
    ROS_INFO("Sparse plan succeeded with %i planned point and %i interpolated points",planned_count,interp_count);
  }
  else
  {
    return false;
  }

  return true;
}

bool SparsePlanner::isInSparseTrajectory(const TrajectoryPt::ID& ref_id)
{
  auto predicate = [&ref_id](std::tuple<int,TrajectoryPtPtr,JointTrajectoryPt>& t)
    {
      return ref_id == std::get<1>(t)->getID();
    };

  return (std::find_if(sparse_solution_array_.begin(),
                       sparse_solution_array_.end(),predicate) != sparse_solution_array_.end());
}

int SparsePlanner::getDensePointIndex(const TrajectoryPt::ID& ref_id)
{
  int index = INVALID_INDEX;
  auto predicate = [ref_id](TrajectoryPtPtr cp)
    {
      return ref_id== cp->getID();
    };

  auto pos = std::find_if(cart_points_.begin(),cart_points_.end(),predicate);
  if(pos != cart_points_.end())
  {
    index = std::distance(cart_points_.begin(),pos);
  }

  return index;
}

int SparsePlanner::getSparsePointIndex(const TrajectoryPt::ID& ref_id)
{
  int index = INVALID_INDEX;
  auto predicate = [ref_id](std::tuple<int,TrajectoryPtPtr,JointTrajectoryPt>& t)
    {
      return ref_id == std::get<1>(t)->getID();
    };

  auto pos = std::find_if(sparse_solution_array_.begin(),sparse_solution_array_.end(),predicate);
  if(pos != sparse_solution_array_.end())
  {
    index = std::distance(sparse_solution_array_.begin(),pos);
  }

  return index;
}

int SparsePlanner::findNearestSparsePointIndex(const TrajectoryPt::ID& ref_id,bool skip_equal)
{
  int index = INVALID_INDEX;
  int dense_index = getDensePointIndex(ref_id);

  if(dense_index == INVALID_INDEX)
  {
    return index;
  }

  auto predicate = [&dense_index,&skip_equal](std::tuple<int,TrajectoryPtPtr,JointTrajectoryPt>& t)
    {

      if(skip_equal)
      {
        return dense_index < std::get<0>(t);
      }
      else
      {
        return dense_index < std::get<0>(t);
      }
    };

  auto pos = std::find_if(sparse_solution_array_.begin(),sparse_solution_array_.end(),predicate);
  if(pos != sparse_solution_array_.end())
  {
    index = std::distance(sparse_solution_array_.begin(), pos);
  }

  return index;
}

bool SparsePlanner::getSparseSolutionArray(SolutionArray& sparse_solution_array)
{
  std::list<JointTrajectoryPt> sparse_joint_points;
  std::vector<TrajectoryPtPtr> sparse_cart_points;
  double cost;

  if(!getShortestPath(cost,sparse_joint_points) ||
      !getOrderedSparseCartesianArray(sparse_cart_points) ||
      (sparse_joint_points.size() != sparse_cart_points.size()))
  {
    ROS_ERROR_STREAM("Failed to find sparse joint solution");
    return false;
  }

  unsigned int i = 0;
  unsigned int index;
  sparse_solution_array.clear();
  sparse_solution_array.reserve(sparse_cart_points.size());
  for(auto& item : sparse_joint_points)
  {
    TrajectoryPtPtr cp = sparse_cart_points[i];
    JointTrajectoryPt& jp = item;
    index = getDensePointIndex(cp->getID());

    if(index == INVALID_INDEX)
    {
      ROS_ERROR_STREAM("Cartesian point "<<cp->getID()<<" not found");
      return false;
    }

    sparse_solution_array.push_back(std::make_tuple(index,cp,jp));
  }

  return true;
}

bool SparsePlanner::getOrderedSparseCartesianArray(std::vector<TrajectoryPtPtr>& sparse_array)
{
  const CartesianMap& cart_map = getCartesianMap();
  TrajectoryPt::ID first_id = boost::uuids::nil_uuid();
  auto predicate = [&first_id](const std::pair<TrajectoryPt::ID,CartesianPointInformation>& p)
    {
      const auto& info = p.second;
      if(info.links_.id_previous == boost::uuids::nil_uuid())
      {
        first_id = p.first;
        return true;
      }
      else
      {
        return false;
      }
    };

  // finding first point
  if(cart_map.empty()
      || (std::find_if(cart_map.begin(),cart_map.end(),predicate) == cart_map.end())
      || first_id == boost::uuids::nil_uuid())
  {
    return false;
  }

  // copying point pointers in order
  sparse_array.resize(cart_map.size());
  TrajectoryPt::ID current_id = first_id;
  for(int i = 0; i < sparse_array.size(); i++)
  {
    if(cart_map.count(current_id) == 0)
    {
      ROS_ERROR_STREAM("Trajectory point "<<current_id<<" was not found in sparse trajectory.");
      return false;
    }

    const CartesianPointInformation& info  = cart_map.at(current_id);
    sparse_array[i] = info.source_trajectory_;
    current_id = info.links_.id_next;
  }

  return true;
}

bool SparsePlanner::getSolutionJointPoint(const CartTrajectoryPt::ID& cart_id, JointTrajectoryPt& j)
{
  if(joint_points_map_.count(cart_id) > 0)
  {
    j = joint_points_map_[cart_id];
  }
  else
  {
    return false;
  }

  return true;
}

void SparsePlanner::sampleTrajectory(double sampling,const std::vector<TrajectoryPtPtr>& dense_trajectory_array,
                      std::vector<TrajectoryPtPtr>& sparse_trajectory_array)
{
  int skip = dense_trajectory_array.size()/sampling;
  for(int i = 0; i < dense_trajectory_array.size();i+=skip)
  {
    sparse_trajectory_array.push_back(dense_trajectory_array[i]);
  }

  // add the last one
  if(sparse_trajectory_array.back()->getID() != dense_trajectory_array.back()->getID())
  {
    sparse_trajectory_array.push_back(dense_trajectory_array.back());
  }
}

bool SparsePlanner::interpolateJointPose(const std::vector<double>& start,const std::vector<double>& end,
    double t,std::vector<double>& interp)
{
  if(start.size() != end.size() && (t > 1 || t < 0))
  {
    return false;
  }

  interp.resize(start.size());
  double val = 0.0f;
  for(int i = 0; i < start.size(); i++)
  {
    val = end[i] - (end[i] - start[i]) * (1 - t);
    interp[i] = val;
  }

  return true;
}

bool SparsePlanner::plan()
{

  // solving coarse trajectory
  bool replan = true;
  bool succeeded = false;
  int replanning_attempts = 0;
  while(replan && (replanning_attempts++ < MAX_REPLANNING_ATTEMPTS) && getSparseSolutionArray(sparse_solution_array_))
  {
    int sparse_index, point_pos;
    int result = interpolateSparseTrajectory(sparse_solution_array_,sparse_index,point_pos);
    TrajectoryPt::ID prev_id, next_id;
    TrajectoryPtPtr cart_point;
    auto sparse_iter = sparse_solution_array_.begin();
    switch(result)
    {
      case int(InterpolationResult::REPLAN):
          replan = true;
          cart_point = cart_points_[point_pos];

          if(sparse_index == 0)
          {
            prev_id = boost::uuids::nil_uuid();
            next_id = std::get<1>(sparse_solution_array_[sparse_index])->getID();
          }
          else
          {
            prev_id = std::get<1>(sparse_solution_array_[sparse_index-1])->getID();
            next_id = std::get<1>(sparse_solution_array_[sparse_index])->getID();
          }

          if(addTrajectory(cart_point,prev_id,next_id))
          {
            sparse_solution_array_.clear();
            ROS_INFO_STREAM("Added new point to sparse trajectory from dense trajectory at position "<<
                            point_pos<<", re-planning entire trajectory");
          }
          else
          {
            ROS_INFO_STREAM("Adding point "<<point_pos <<"to sparse trajectory failed, aborting");
            replan = false;
            succeeded = false;
          }

          break;
      case int(InterpolationResult::SUCCESS):
          replan = false;
          succeeded = true;
          break;
      case int(InterpolationResult::ERROR):
          replan = false;
          succeeded = false;
          break;
    }

  }

  return true;
}

int SparsePlanner::interpolateSparseTrajectory(const SolutionArray& sparse_solution_array,int &sparse_index, int &point_pos)
{
  // populating full path
  joint_points_map_.clear();
  std::vector<double> start_jpose, end_jpose, rough_interp, aprox_interp, seed_pose(robot_model_->getDOF(),0);
  for(int k = 1; k < sparse_solution_array.size(); k++)
  {
    auto start_index = std::get<0>(sparse_solution_array[k-1]);
    auto end_index = std::get<0>(sparse_solution_array[k]);
    TrajectoryPtPtr start_tpoint = std::get<1>(sparse_solution_array[k-1]);
    TrajectoryPtPtr end_tpoint = std::get<1>(sparse_solution_array[k]);
    const JointTrajectoryPt& start_jpoint = std::get<2>(sparse_solution_array[k-1]);
    const JointTrajectoryPt& end_jpoint = std::get<2>(sparse_solution_array[k]);

    start_jpoint.getNominalJointPose(seed_pose,*robot_model_,start_jpose);
    end_jpoint.getNominalJointPose(seed_pose,*robot_model_,end_jpose);

    // adding start joint point to solution
    joint_points_map_.insert(std::make_pair(start_tpoint->getID(),start_jpoint));

    // interpolating
    int step = end_index - start_index;
    for(int j = 1; (j < step) && ( (start_index + j) < cart_points_.size()); j++)
    {
      int pos = start_index+j;
      double t = double(j)/double(step);
      if(!interpolateJointPose(start_jpose,end_jpose,t,rough_interp))
      {
        ROS_ERROR_STREAM("Interpolation for point at position "<<pos<< "failed, aborting");
        return (int)InterpolationResult::ERROR;
      }

      TrajectoryPtPtr cart_point = cart_points_[pos];
      if(cart_point->getClosestJointPose(rough_interp,*robot_model_,aprox_interp))
      {
        joint_points_map_.insert(std::make_pair(cart_point->getID(),JointTrajectoryPt(aprox_interp)));
      }
      else
      {
          sparse_index = k;
          point_pos = pos;
          return (int)InterpolationResult::REPLAN;
      }
    }

    // adding end joint point to solution
    joint_points_map_.insert(std::make_pair(end_tpoint->getID(),start_jpoint));
  }

  return (int)InterpolationResult::SUCCESS;
}

} /* namespace descartes_core */
