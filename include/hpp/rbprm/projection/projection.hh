/// Copyright (c) 2015 CNRS
/// Authors: Joseph Mirabel
///
///
// This file is part of hpp-wholebody-step.
// hpp-wholebody-step-planner is free software: you can redistribute it
// and/or modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation, either version
// 3 of the License, or (at your option) any later version.
//
// hpp-wholebody-step-planner is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Lesser Public License for more details. You should have
// received a copy of the GNU Lesser General Public License along with
// hpp-wholebody-step-planner. If not, see
// <http://www.gnu.org/licenses/>.

#ifndef HPP_RBPRM_PROJECTION_HH
# define HPP_RBPRM_PROJECTION_HH

# include <hpp/rbprm/rbprm-state.hh>
# include <hpp/rbprm/rbprm-fullbody.hh>

namespace hpp {
namespace rbprm {
namespace projection{



enum ContactComputationStatus
{
  NO_CONTACT = 0,
  STABLE_CONTACT = 1,
  UNSTABLE_CONTACT = 2
};


struct HPP_RBPRM_DLLAPI ProjectionReport
{
     ProjectionReport(): success_ (false), status_(NO_CONTACT){}
     ProjectionReport(const ProjectionReport&);
    ~ProjectionReport(){}
    bool success_;
    hpp::rbprm::State result_;
    ContactComputationStatus status_;
};

/// Project a configuration to a target position, while maintaining contact constraints.
/// If required, up to maxBrokenContacts can be broken in the process.
/// root position is assumed to be at the 3 first dof.
/// \param fullBody target Robot
/// \param target, desired root position
/// \param currentState current state of the robot (configuration and contacts)
/// \return projection report containing the state projected
ProjectionReport HPP_RBPRM_DLLAPI projectToRootPosition(hpp::rbprm::RbPrmFullBodyPtr_t fullBody, const fcl::Vec3f& target,
                                           const hpp::rbprm::State& currentState);


/// Project a configuration to a target COM position, while maintaining contact constraints.
/// If required, up to maxBrokenContacts can be broken in the process.
/// root position is assumed to be at the 3 first dof.
/// \param fullBody target Robot
/// \param target, desired root position
/// \param currentState current state of the robot (configuration and contacts)
/// \return projection report containing the state projected
ProjectionReport HPP_RBPRM_DLLAPI projectToComPosition(hpp::rbprm::RbPrmFullBodyPtr_t fullBody, const fcl::Vec3f& target,
                                           const hpp::rbprm::State& currentState);


/// Project a configuration to a target root configuration, while maintaining contact constraints.
/// If required, up to maxBrokenContacts can be broken in the process.
/// root position is assumed to be at the 3 first dof.
/// \param fullBody target Robot
/// \param target, desired root position
/// \param currentState current state of the robot (configuration and contacts)
/// \return projection report containing the state projected
ProjectionReport HPP_RBPRM_DLLAPI projectToRootConfiguration(hpp::rbprm::RbPrmFullBodyPtr_t fullBody, const model::ConfigurationIn_t conf,
                                           const hpp::rbprm::State& currentState);

/// Project a configuration such that a given limb configuration is collision free
/// \param fullBody target Robot
/// \param limb considered limb
/// \return projection report containing the state projected
ProjectionReport  HPP_RBPRM_DLLAPI setCollisionFree(hpp::rbprm::RbPrmFullBodyPtr_t fullBody, const core::CollisionValidationPtr_t &validation, const std::string& limb, const hpp::rbprm::State& currentState);


ProjectionReport HPP_RBPRM_DLLAPI projectSampleToObstacle(const hpp::rbprm::RbPrmFullBodyPtr_t& body,const std::string& limbId, const hpp::rbprm::RbPrmLimbPtr_t& limb,
                                                 const sampling::OctreeReport& report, core::CollisionValidationPtr_t validation,
                                                 model::ConfigurationOut_t configuration, const hpp::rbprm::State& current);

ProjectionReport HPP_RBPRM_DLLAPI projectEffector(const hpp::rbprm::RbPrmFullBodyPtr_t& body,const std::string& limbId, const hpp::rbprm::RbPrmLimbPtr_t& limb,
                                           core::CollisionValidationPtr_t validation, model::ConfigurationOut_t configuration,
                                           const fcl::Matrix3f& rotationTarget, const std::vector<bool>& rotationFilter, const fcl::Vec3f& positionTarget, const fcl::Vec3f& normal,
                                           const hpp::rbprm::State& current);

    } // namespace projection
  } // namespace rbprm
} // namespace hpp
#endif // HPP_RBPRM_PROJECTION_HH
