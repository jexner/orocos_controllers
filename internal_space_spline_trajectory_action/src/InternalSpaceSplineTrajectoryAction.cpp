/*
 * Copyright (c) 2010-2015, Robot Control and Pattern Recognition Group, Warsaw University of Technology.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Robot Control and Pattern Recognition Group,
 *       Warsaw University of Technology nor the names of its contributors may
 *       be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * InterrnalSpaceTrajectoryAction.cpp
 *
 *  Created on: 23-09-2010
 *      Author: Konrad Banachowicz
 */

#include "InternalSpaceSplineTrajectoryAction.h"
#include <ocl/Component.hpp>
#include <string>
#include "rtt_rosclock/rtt_rosclock.h"
#include "eigen_conversions/eigen_msg.h"

InternalSpaceSplineTrajectoryAction::InternalSpaceSplineTrajectoryAction(
    const std::string& name)
    : RTT::TaskContext(name, PreOperational),
      numberOfJoints_prop_("number_of_joints", "", 0){
  // Add action server ports to this task's root service
  as_.addPorts(this->provides());

  // Bind action server goal and cancel callbacks (see below)
  as_.registerGoalCallback(
      boost::bind(&InternalSpaceSplineTrajectoryAction::goalCB, this, _1));
  as_.registerCancelCallback(
      boost::bind(&InternalSpaceSplineTrajectoryAction::cancelCB, this, _1));

  this->addPort("trajectoryPtr", trajectory_ptr_port_);
  this->addPort("state", state_pub_port_);
  this->addPort("JointPosition", port_joint_position_);
  this->addPort("JointPositionCommand", port_joint_position_command_);
  this->addEventPort("command",
      command_port_,
      //(void (InternalSpaceSplineTrajectoryAction::*)(RTT::base::PortInterface* pi))&InternalSpaceSplineTrajectoryAction::commandCB);
      boost::bind(&InternalSpaceSplineTrajectoryAction::commandCB, this));
  this->addProperty(numberOfJoints_prop_);
  this->addProperty("joint_names", jointNames_);
  this->addProperty("lower_limits", lowerLimits_);
  this->addProperty("upper_limits", upperLimits_);
}

InternalSpaceSplineTrajectoryAction::~InternalSpaceSplineTrajectoryAction() {
}

bool InternalSpaceSplineTrajectoryAction::configureHook() {
  if (jointNames_.size() <= 0) {
    return false;
  }

  numberOfJoints_ = jointNames_.size();

  feedback_.actual.positions.reserve(numberOfJoints_);
  feedback_.desired.positions.reserve(numberOfJoints_);
  feedback_.error.positions.reserve(numberOfJoints_);
  feedback_.joint_names.reserve(numberOfJoints_);
  control_state_.actual.positions.resize(numberOfJoints_);
  control_state_.desired.positions.resize(numberOfJoints_);
  control_state_.error.positions.resize(numberOfJoints_);
  control_state_.joint_names.reserve(numberOfJoints_);


  for (int i = 0; i < jointNames_.size(); i++) {
    feedback_.joint_names.push_back(jointNames_[i]);
    control_state_.joint_names.push_back(jointNames_[i]);
  }

  remapTable_.resize(numberOfJoints_);

  if (lowerLimits_.size() != numberOfJoints_
      || upperLimits_.size() != numberOfJoints_) {
    RTT::Logger::log(RTT::Logger::Error) << "Limits not loaded"
                                         << RTT::endlog();
    return false;
  }

  return true;
}

bool InternalSpaceSplineTrajectoryAction::startHook() {
  as_.start();
  goal_active_ = false;
  command_active_ = false;
  enable_ = true;

  return true;
}

void InternalSpaceSplineTrajectoryAction::updateHook() {
  bool joint_position_data = true;
  bool desired_joint_position_data = true;
  if (port_joint_position_.read(joint_position_) == RTT::NoData) {
    joint_position_data = false;

  }
  
  control_msgs::FollowJointTrajectoryResult res;

  if (port_joint_position_command_.read(desired_joint_position_) == RTT::NoData) {
    desired_joint_position_data = false;
  }
  
  Goal g = activeGoal_.getGoal();
  ros::Time now;
  
  if (command_active_)
  {
    now = rtt_rosclock::host_now();
    if (now > trajectory_finish_time_)
      command_active_ = false;
  }

  if (goal_active_ && joint_position_data) {
    bool violated = false;
    now = rtt_rosclock::host_now();

    if (now > trajectory_finish_time_) {
      violated = false;
      for (int i = 0; i < numberOfJoints_; i++) {
        for (int j = 0; j < g->goal_tolerance.size(); j++) {
          if (g->goal_tolerance[j].name == g->trajectory.joint_names[i]) {
            // Jeśli istnieje ograniczenie to sprawdzam pozycję
            if (joint_position_[remapTable_[i]] + g->goal_tolerance[j].position
                < g->trajectory.points[g->trajectory.points.size() - 1]
                    .positions[i]
                || joint_position_[remapTable_[i]]
                    - g->goal_tolerance[j].position
                    > g->trajectory.points[g->trajectory.points.size() - 1]
                        .positions[i]) {
              violated = true;
              RTT::Logger::log(RTT::Logger::Debug) << g->goal_tolerance[j].name
                  << " violated with position "
                  << joint_position_[remapTable_[i]] << RTT::endlog();
            }
          }
        }
      }

      if (violated && now > trajectory_finish_time_ + g->goal_time_tolerance) {
        res.error_code =
            control_msgs::FollowJointTrajectoryResult::GOAL_TOLERANCE_VIOLATED;
        activeGoal_.setAborted(res, "");
        goal_active_ = false;
      } else if (!violated) {
        res.error_code = control_msgs::FollowJointTrajectoryResult::SUCCESSFUL;
        activeGoal_.setSucceeded(res, "");
        goal_active_ = false;
      }
    } else {
      // Wysyłanie feedback

      for (int i = 0; i < numberOfJoints_; i++) {
        feedback_.actual.positions[i] = joint_position_[i];
        if(desired_joint_position_data)
        {
          feedback_.desired.positions[i] = desired_joint_position_[i];
          feedback_.error.positions[i] = joint_position_[i]
            - desired_joint_position_[i];
        }
      }

      feedback_.header.stamp = rtt_rosclock::host_now();
      activeGoal_.publishFeedback(feedback_);

      // Sprawdzanie PATH_TOLRANCE_VIOLATED
      violated = false;
      for (int i = 0; i < g->path_tolerance.size(); i++) {
        for (int j = 0; j < jointNames_.size(); j++) {
          if (jointNames_[j] == g->path_tolerance[i].name) {
            if(desired_joint_position_data)
            { 
              if (fabs(joint_position_[j] - desired_joint_position_[j])
                  > g->path_tolerance[i].position) {
                violated = true;
                RTT::Logger::log(RTT::Logger::Error) << "Path tolerance violated"
                                                     << RTT::endlog();
              }
            }
          }
        }
      }
      if (violated) {
        trajectory_ptr_port_.write(trajectory_msgs::JointTrajectoryConstPtr());
        res.error_code =
            control_msgs::FollowJointTrajectoryResult::PATH_TOLERANCE_VIOLATED;
        activeGoal_.setAborted(res);
      }
    }
  }
  
  if(joint_position_data)
  {
    // fill up and publish control_state_
    for (int i = 0; i < numberOfJoints_; i++) {
      control_state_.actual.positions[i] = joint_position_[i];
      if(desired_joint_position_data)
      {
        control_state_.desired.positions[i] = desired_joint_position_[i];
        control_state_.error.positions[i] = joint_position_[i]
          - desired_joint_position_[i];
      }
    }
    state_pub_port_.write(control_state_);
  }
  
}

void InternalSpaceSplineTrajectoryAction::goalCB(GoalHandle gh) {
  if (!goal_active_) {
    trajectory_msgs::JointTrajectory* trj_ptr =
        new trajectory_msgs::JointTrajectory;
    Goal g = gh.getGoal();

    control_msgs::FollowJointTrajectoryResult res;

    RTT::Logger::log(RTT::Logger::Debug) << "Received trajectory contain "
                                         << g->trajectory.points.size()
                                         << " points" << RTT::endlog();

    // fill remap table
    for (unsigned int i = 0; i < numberOfJoints_; i++) {
      int jointId = -1;
      for (unsigned int j = 0; j < g->trajectory.joint_names.size(); j++) {
        if (g->trajectory.joint_names[j] == jointNames_[i]) {
          jointId = j;
          break;
        }
      }
      if (jointId < 0) {
        RTT::Logger::log(RTT::Logger::Error)
            << "Trajectory contains invalid joint" << RTT::endlog();
        res.error_code =
            control_msgs::FollowJointTrajectoryResult::INVALID_JOINTS;
        gh.setRejected(res, "");
        return;
      } else {
        remapTable_[i] = jointId;
      }
    }

    // Sprawdzenie ograniczeń w jointach INVALID_GOAL
    bool invalid_goal = false;
    for (unsigned int i = 0; i < numberOfJoints_; i++) {
      for (int j = 0; j < g->trajectory.points.size(); j++) {
        if (g->trajectory.points[j].positions[i] > upperLimits_[remapTable_[i]]
            || g->trajectory.points[j].positions[i]
                < lowerLimits_[remapTable_[i]]) {
          RTT::Logger::log(RTT::Logger::Debug)
              << "Invalid goal [" << i << "]: " << upperLimits_[remapTable_[i]]
              << ">" << g->trajectory.points[j].positions[i] << ">"
              << lowerLimits_[remapTable_[i]] << RTT::endlog();
          invalid_goal = true;
        }
      }
    }

    if (invalid_goal) {
      RTT::Logger::log(RTT::Logger::Error)
          << "Trajectory contains invalid goal!" << RTT::endlog();
      res.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_GOAL;
      gh.setRejected(res, "");
      goal_active_ = false;
      return;
    }

    // Remap joints
    trj_ptr->header = g->trajectory.header;
    trj_ptr->points.resize(g->trajectory.points.size());

    for (unsigned int i = 0; i < g->trajectory.points.size(); i++) {
      trj_ptr->points[i].positions.resize(
          g->trajectory.points[i].positions.size());
      for (unsigned int j = 0; j < g->trajectory.points[i].positions.size();
          j++) {
        trj_ptr->points[i].positions[j] =
            g->trajectory.points[i].positions[remapTable_[j]];
      }

      trj_ptr->points[i].velocities.resize(
          g->trajectory.points[i].velocities.size());
      for (unsigned int j = 0; j < g->trajectory.points[i].velocities.size();
          j++) {
        trj_ptr->points[i].velocities[j] =
            g->trajectory.points[i].velocities[remapTable_[j]];
      }

      trj_ptr->points[i].accelerations.resize(
          g->trajectory.points[i].accelerations.size());
      for (unsigned int j = 0; j < g->trajectory.points[i].accelerations.size();
          j++) {
        trj_ptr->points[i].accelerations[j] = g->trajectory.points[i]
            .accelerations[remapTable_[j]];
      }

      trj_ptr->points[i].time_from_start = g->trajectory.points[i]
          .time_from_start;
    }

    // Check the time in the header OLD_HEADER_TIMESTAMP
    // accept timestamp of zero (means now)
    if (g->trajectory.header.stamp.toSec() !=0 )
    {
      if (g->trajectory.header.stamp < rtt_rosclock::host_now()) {
        RTT::Logger::log(RTT::Logger::Error) << "Old header timestamp"
                                           << RTT::endlog();
        res.error_code =
            control_msgs::FollowJointTrajectoryResult::OLD_HEADER_TIMESTAMP;
        gh.setRejected(res, "");
      }
      trajectory_finish_time_ = g->trajectory.header.stamp
        + g->trajectory.points[g->trajectory.points.size() - 1].time_from_start;
    }
    else
    {
      trajectory_finish_time_ = rtt_rosclock::host_now()
        + g->trajectory.points[g->trajectory.points.size() - 1].time_from_start;
      // modify original time to now
      trj_ptr->header.stamp = rtt_rosclock::host_now();
    }
    activeGoal_ = gh;
    goal_active_ = true;

    bool ok = true;

    RTT::TaskContext::PeerList peers = this->getPeerList();
    for (size_t i = 0; i < peers.size(); i++) {
      RTT::Logger::log(RTT::Logger::Debug) << "Starting peer : " << peers[i]
                                           << RTT::endlog();
      if(!this->getPeer(peers[i])->isRunning())
        ok = ok && this->getPeer(peers[i])->start();
    }

    if (ok) {
      trajectory_msgs::JointTrajectoryConstPtr trj_cptr =
          trajectory_msgs::JointTrajectoryConstPtr(trj_ptr);

      trajectory_ptr_port_.write(trj_cptr);

      gh.setAccepted();
      goal_active_ = true;
    } else {
      RTT::Logger::log(RTT::Logger::Error) << "peer did not start : " << RTT::endlog();
      gh.setRejected();
      goal_active_ = false;
    }
  } else {
    gh.setRejected();
  }
}

void InternalSpaceSplineTrajectoryAction::cancelCB(GoalHandle gh) {
  goal_active_ = false;
}

void InternalSpaceSplineTrajectoryAction::commandCB() {
  
  if (command_port_.read(command_msg_) == RTT::NoData) {
    return;

  }  
  
  if (!goal_active_) {
    trajectory_msgs::JointTrajectory* trj_ptr =
        new trajectory_msgs::JointTrajectory;

    RTT::Logger::log(RTT::Logger::Debug) << "Received trajectory contain "
                                         << command_msg_.points.size()
                                         << " points" << RTT::endlog();

    // fill remap table
    for (unsigned int i = 0; i < numberOfJoints_; i++) {
      int jointId = -1;
      for (unsigned int j = 0; j < command_msg_.joint_names.size(); j++) {
        if (command_msg_.joint_names[j] == jointNames_[i]) {
          jointId = j;
          break;
        }
      }
      if (jointId < 0) {
        RTT::Logger::log(RTT::Logger::Error)
            << "Trajectory contains invalid joint" << RTT::endlog();
        return;
      } else {
        remapTable_[i] = jointId;
      }
    }

    // Sprawdzenie ograniczeń w jointach INVALID_GOAL
    bool invalid_goal = false;
    for (unsigned int i = 0; i < numberOfJoints_; i++) {
      for (int j = 0; j < command_msg_.points.size(); j++) {
        if (command_msg_.points[j].positions[i] > upperLimits_[remapTable_[i]]
            || command_msg_.points[j].positions[i]
                < lowerLimits_[remapTable_[i]]) {
          RTT::Logger::log(RTT::Logger::Debug)
              << "Invalid goal [" << i << "]: " << upperLimits_[remapTable_[i]]
              << ">" << command_msg_.points[j].positions[i] << ">"
              << lowerLimits_[remapTable_[i]] << RTT::endlog();
          invalid_goal = true;
        }
      }
    }

    if (invalid_goal) {
      RTT::Logger::log(RTT::Logger::Error)
          << "Trajectory contains invalid goal!" << RTT::endlog();
      return;
    }

    // Remap joints
    trj_ptr->header = command_msg_.header;
    trj_ptr->points.resize(command_msg_.points.size());

    for (unsigned int i = 0; i < command_msg_.points.size(); i++) {
      trj_ptr->points[i].positions.resize(
          command_msg_.points[i].positions.size());
      for (unsigned int j = 0; j < command_msg_.points[i].positions.size();
          j++) {
        trj_ptr->points[i].positions[j] =
            command_msg_.points[i].positions[remapTable_[j]];
      }

      trj_ptr->points[i].velocities.resize(
          command_msg_.points[i].velocities.size());
      for (unsigned int j = 0; j < command_msg_.points[i].velocities.size();
          j++) {
        trj_ptr->points[i].velocities[j] =
            command_msg_.points[i].velocities[remapTable_[j]];
      }

      trj_ptr->points[i].accelerations.resize(
          command_msg_.points[i].accelerations.size());
      for (unsigned int j = 0; j < command_msg_.points[i].accelerations.size();
          j++) {
        trj_ptr->points[i].accelerations[j] = command_msg_.points[i]
            .accelerations[remapTable_[j]];
      }

      trj_ptr->points[i].time_from_start = command_msg_.points[i]
          .time_from_start;
    }

    // Check the time in the header OLD_HEADER_TIMESTAMP
    // accept timestamp of zero (means now)
    if (command_msg_.header.stamp.toSec() !=0 )
    {
      if (command_msg_.header.stamp < rtt_rosclock::host_now()) {
        RTT::Logger::log(RTT::Logger::Error) << "Old header timestamp"
                                           << RTT::endlog();
      }
      trajectory_finish_time_ = command_msg_.header.stamp
        + command_msg_.points[command_msg_.points.size() - 1].time_from_start;
    }
    else
    {
      trajectory_finish_time_ = rtt_rosclock::host_now()
        + command_msg_.points[command_msg_.points.size() - 1].time_from_start;
      // modify original time to now
      trj_ptr->header.stamp = rtt_rosclock::host_now();
    }

    bool ok = true;

    RTT::TaskContext::PeerList peers = this->getPeerList();
    for (size_t i = 0; i < peers.size(); i++) {
      RTT::Logger::log(RTT::Logger::Debug) << "Starting peer : " << peers[i]
                                           << RTT::endlog();
      if(!this->getPeer(peers[i])->isRunning())
        ok = ok && this->getPeer(peers[i])->start();
    }

    if (ok) {
      trajectory_msgs::JointTrajectoryConstPtr trj_cptr =
          trajectory_msgs::JointTrajectoryConstPtr(trj_ptr);
      
      trajectory_ptr_port_.write(trj_cptr);
      command_active_ = true;

    } else {
      RTT::Logger::log(RTT::Logger::Error) << "peer did not start : " << RTT::endlog();
    }
  } 
  else {
    RTT::Logger::log(RTT::Logger::Error) << "A goal still active, not replacing current trajectory"
                                         << RTT::endlog();
  }
  
}

ORO_CREATE_COMPONENT(InternalSpaceSplineTrajectoryAction)

