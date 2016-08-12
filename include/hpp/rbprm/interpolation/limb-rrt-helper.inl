//
// Copyright (c) 2014 CNRS
// Authors: Steve Tonneau (steve.tonneau@laas.fr)
//
// This file is part of hpp-rbprm.
// hpp-rbprm is free software: you can redistribute it
// and/or modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation, either version
// 3 of the License, or (at your option) any later version.
//
// hpp-rbprm is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Lesser Public License for more details.  You should have
// received a copy of the GNU Lesser General Public License along with
// hpp-core  If not, see
// <http://www.gnu.org/licenses/>.

#ifndef HPP_RBPRM_LIMB_RRT_HELPER_UTILS_HH
# define HPP_RBPRM_LIMB_RRT_HELPER_UTILS_HH

#include <hpp/rbprm/interpolation/limb-rrt-shooter.hh>
#include <hpp/rbprm/interpolation/limb-rrt-path-validation.hh>
#include <hpp/rbprm/interpolation/time-dependant.hh>
#include <hpp/core/steering-method-straight.hh>
#include <hpp/core/problem-target/goal-configurations.hh>
#include <hpp/core/bi-rrt-planner.hh>
#include <hpp/core/random-shortcut.hh>
#include <hpp/core/constraint-set.hh>
#include <hpp/constraints/generic-transformation.hh>
#include <hpp/constraints/position.hh>
#include <hpp/constraints/orientation.hh>
#include <hpp/core/config-projector.hh>
#include <hpp/core/locked-joint.hh>
#include <hpp/core/path-vector.hh>
#include <hpp/core/subchain-path.hh>
#include <hpp/model/joint.hh>
#include <hpp/model/object-factory.hh>
#include <hpp/rbprm/tools.hh>
#include <hpp/constraints/relative-com.hh>
#include <hpp/constraints/symbolic-calculus.hh>
#include <hpp/constraints/symbolic-function.hh>

namespace hpp {
using namespace core;
using namespace model;
namespace rbprm {
namespace interpolation {

    struct ComRightSide : public RightHandSideFunctor
    {
        ComRightSide (const core::PathPtr_t comPath, model::JointPtr_t j) : comPath_(comPath), dummyJoint_(j) {}
       ~ComRightSide(){delete dummyJoint_;}
        virtual void operator() (constraints::vectorOut_t output, const constraints::value_type& normalized_input, model::ConfigurationOut_t conf) const
        {
            const interval_t& tR (comPath_->timeRange());
            constraints::value_type unNormalized = (tR.second-tR.first)* normalized_input + tR.first;
            output = comPath_->operator ()(normalized_input).head(3);
        }
        const core::PathPtr_t comPath_;
        const model::JointPtr_t dummyJoint_;
    };

    namespace{
    inline core::DevicePtr_t DeviceFromLimb(const std::string& name, RbPrmLimbPtr_t limb)
    {
        DevicePtr_t limbDevice = Device::create(name);
        limbDevice->rootJoint(limb->limb_->clone());
        JointPtr_t current = limb->limb_, clone = limbDevice->rootJoint();
        while(current->name() != limb->effector_->name())
        {
            current = current->childJoint(0);
            clone->addChildJoint(current->clone());
            clone = clone->childJoint(0);
        }
        limbDevice->setDimensionExtraConfigSpace(1);
        return limbDevice;
    }
    inline core::PathPtr_t generateRootPath(const Problem& problem, const State& from, const State& to)
    {
        Configuration_t startRootConf(from.configuration_);
        Configuration_t endRootConf(to.configuration_);
        return (*(problem.steeringMethod()))(startRootConf, endRootConf);
    }

    inline void DisableUnNecessaryCollisions(core::Problem& problem, rbprm::RbPrmLimbPtr_t limb)
    {
        // TODO should we really disable collisions for other bodies ?
        tools::RemoveNonLimbCollisionRec<core::Problem>(problem.robot()->rootJoint(),
                                                        "all",
                                                        //limb->limb_->name(),
                                                        problem.collisionObstacles(),problem);

        if(limb->disableEndEffectorCollision_)
        {
            hpp::tools::RemoveEffectorCollision<core::Problem>(problem,
                                                               problem.robot()->getJointByName(limb->effector_->name()),
                                                               problem.collisionObstacles());
        }
    }

    template<class Path_T>
    ConfigurationPtr_t TimeConfigFromDevice(const TimeConstraintHelper<Path_T>& helper, const State& state, const double time)
    {
        Configuration_t config(helper.fullBodyDevice_->currentConfiguration());
        config.head(state.configuration_.rows()) = state.configuration_;
        config[config.rows()-1] = time;
        return ConfigurationPtr_t(new Configuration_t(config));
    }

    template<class Path_T>
    void SetConfigShooter(TimeConstraintHelper<Path_T>& helper, RbPrmLimbPtr_t limb, core::PathPtr_t& rootPath)
    {
        ConfigurationShooterPtr_t limbRRTShooter = LimbRRTShooter::create(limb, rootPath,
                                                                          helper.fullBodyDevice_->configSize()-1);
        helper.rootProblem_.configurationShooter(limbRRTShooter);
    }

    inline std::vector<bool> setMaintainRotationConstraints() // direction)
    {
        std::vector<bool> res;
        for(std::size_t i =0; i <3; ++i)
            res.push_back(true);
        return res;
    }

    inline void LockRootAndNonContributingJoints(model::DevicePtr_t device, core::ConfigProjectorPtr_t& projector,
                                          const std::vector<std::string>& fixedContacts,
                                          const State& from, const State& to)
    {
        std::vector<std::string> spared = fixedContacts;
        to.contactCreations(from, spared);
        to.contactBreaks(from, spared);
        tools::LockJointRec(spared, device->rootJoint(), projector);
    }


    typedef constraints::PointCom PointCom;
    typedef constraints::SymbolicFunction<PointCom> PointComFunction;
    typedef constraints::SymbolicFunction<PointCom>::Ptr_t PointComFunctionPtr_t;

    template<class Path_T>
    inline void CreateComConstraint(TimeConstraintHelper<Path_T>& helper)
    {
        model::DevicePtr_t device = helper.rootProblem_.robot();
        core::ConfigProjectorPtr_t& proj = helper.proj_;
        //create identity joint
        model::ObjectFactory ofac;
        const model::Transform3f tr;
        model::JointPtr_t j (ofac.createJointAnchor(tr));
        //constraints::RelativeComPtr_t cons = constraints::RelativeCom::create(device,j,fcl::Vec3f(0,0,0));
        fcl::Transform3f localFrame, globalFrame;
        constraints::PositionPtr_t cons (constraints::Position::create("",device,
                                                                       device->rootJoint(),
                                                                       globalFrame,
                                                                       localFrame));
        core::NumericalConstraintPtr_t nm(core::NumericalConstraint::create (cons,core::Equality::create()));


        /************/
        core::ComparisonTypePtr_t equals = core::Equality::create ();

        // Create the time varying equation for COM
        model::CenterOfMassComputationPtr_t comComp = model::CenterOfMassComputation::
          create (device);
        comComp->add (device->rootJoint());
        comComp->computeMass ();
        PointComFunctionPtr_t comFunc = PointComFunction::create ("COM-walkgen",
            device, PointCom::create (comComp));
        NumericalConstraintPtr_t comEq = NumericalConstraint::create (comFunc, equals);
        //TimeDependant comEqTD (comEq, boost::shared_ptr<ComRightSide>(new ComRightSide(helper.refPath_,j)));
        proj->add(comEq);
        helper.steeringMethod_->tds_.push_back(TimeDependant(comEq, boost::shared_ptr<ComRightSide>(new ComRightSide(helper.refPath_,j))));
       /***************/
        //proj->add(nm);
        //helper.steeringMethod_->tds_.push_back(TimeDependant(nm,boost::shared_ptr<ComRightSide>(new ComRightSide(helper.refPath_,j))));
    }

    template<class Path_T>
    inline void CreateRootConstraint(TimeConstraintHelper<Path_T>& helper)
    {
        // TODO
    }

    template<class Path_T>
    inline void initializeConstraints(TimeConstraintHelper<Path_T>& helper)
    {
        core::Problem& problem = helper.rootProblem_;
        core::ConstraintSetPtr_t cSet = core::ConstraintSet::create(problem.robot(),"");
        cSet->addConstraint(helper.proj_);
        problem.constraints(cSet);
    }

    template<class Path_T>
    void AddContactConstraints(TimeConstraintHelper<Path_T>& helper, const State& from, const State& to)
    {
        std::vector<bool> cosntraintsR = setMaintainRotationConstraints();
        std::vector<std::string> fixed = to.fixedContacts(from);
        core::Problem& problem = helper.rootProblem_;
        model::DevicePtr_t device = problem.robot();
        core::ConfigProjectorPtr_t& proj = helper.proj_;

        std::cout << "wtf !!!!!!!!!!!!!!!!!! " << std::endl;
        for(std::vector<std::string>::const_iterator cit = fixed.begin();
            cit != fixed.end(); ++cit)
        {
            RbPrmLimbPtr_t limb = helper.fullbody_->GetLimbs().at(*cit);
            const fcl::Vec3f& ppos  = from.contactPositions_.at(*cit);
            const fcl::Matrix3f& rotation = from.contactRotation_.at(*cit);
            JointPtr_t effectorJoint = device->getJointByName(limb->effector_->name());
            std::cout << "wtf " << std::endl;
            proj->add(core::NumericalConstraint::create (
                                    constraints::deprecated::Position::create("",device,
                                                                  effectorJoint,fcl::Vec3f(0,0,0), ppos)));
            if(limb->contactType_ == hpp::rbprm::_6_DOF)
            {
                proj->add(core::NumericalConstraint::create (constraints::deprecated::Orientation::create("", device,
                                                                                  effectorJoint,
                                                                                  rotation,
                                                                                  cosntraintsR)));
            }
        }
        // constrain root to follow position
        //tools::LockJoint(device->rootJoint(), proj, false);
        //LockRootAndNonContributingJoints(device, proj, fixed, from, to );
        // create com constraint
    }

    template<class Path_T>
    void SetPathValidation(TimeConstraintHelper<Path_T>& helper)
    {
        LimbRRTPathValidationPtr_t pathVal = LimbRRTPathValidation::create(
                    helper.fullBodyDevice_, 0.05,helper.fullBodyDevice_->configSize()-1);
        helper.rootProblem_.pathValidation(pathVal);
    }

    inline std::vector<std::string> extractEffectorsName(const rbprm::T_Limb& limbs)
    {
        std::vector<std::string> res;
        for(rbprm::T_Limb::const_iterator cit = limbs.begin(); cit != limbs.end(); ++cit)
        {
            res.push_back(cit->first);
        }
        return res;
    }
    }

    template<class Path_T>
    PathVectorPtr_t interpolateStates(TimeConstraintHelper<Path_T> &helper, const State& from, const State& to)
    {
        PathVectorPtr_t res;
        core::PathPtr_t rootPath = helper.refPath_;
        const rbprm::T_Limb& limbs = helper.fullbody_->GetLimbs();
        // get limbs that moved
        std::vector<std::string> variations = to.allVariations(from, extractEffectorsName(limbs));
        for(std::vector<std::string>::const_iterator cit = variations.begin();
            cit != variations.end(); ++cit)
        {
            SetPathValidation(helper);
            DisableUnNecessaryCollisions(helper.rootProblem_, limbs.at(*cit));
            SetConfigShooter(helper,limbs.at(*cit),rootPath);

            ConfigurationPtr_t start = TimeConfigFromDevice(helper, from, 0.);
            ConfigurationPtr_t end   = TimeConfigFromDevice(helper, to  , 1.);
            helper.rootProblem_.initConfig(start);
            BiRRTPlannerPtr_t planner = BiRRTPlanner::create(helper.rootProblem_);
            ProblemTargetPtr_t target = problemTarget::GoalConfigurations::create (planner);
            helper.rootProblem_.target (target);
            helper.rootProblem_.addGoalConfig(end);
            AddContactConstraints(helper, from, to);
            initializeConstraints(helper);

            res = planner->solve();
            helper.rootProblem_.resetGoalConfigs();
        }
        return res;
    }

    template<class Path_T>
    PathVectorPtr_t interpolateStatesCom(TimeConstraintHelper<Path_T> &helper, const State& from, const State& to)
    {
        PathVectorPtr_t res;
        core::PathPtr_t rootPath = helper.refPath_;
        const rbprm::T_Limb& limbs = helper.fullbody_->GetLimbs();
        // get limbs that moved
        std::vector<std::string> variations = to.allVariations(from, extractEffectorsName(limbs));
        for(std::vector<std::string>::const_iterator cit = variations.begin();
            cit != variations.end(); ++cit)
        {
            SetPathValidation(helper);
            DisableUnNecessaryCollisions(helper.rootProblem_, limbs.at(*cit));
            SetConfigShooter(helper,limbs.at(*cit),rootPath);

            ConfigurationPtr_t start = TimeConfigFromDevice(helper, from, 0.);
            ConfigurationPtr_t end   = TimeConfigFromDevice(helper, to  , 1.);
            helper.rootProblem_.initConfig(start);
            BiRRTPlannerPtr_t planner = BiRRTPlanner::create(helper.rootProblem_);
            ProblemTargetPtr_t target = problemTarget::GoalConfigurations::create (planner);
            helper.rootProblem_.target (target);
            helper.rootProblem_.addGoalConfig(end);
            AddContactConstraints(helper, from, to);
            initializeConstraints(helper);

            res = planner->solve();
            helper.rootProblem_.resetGoalConfigs();
        }
        return res;
    }

    namespace
    {
        template<class Path_T>
        PathVectorPtr_t optimize(TimeConstraintHelper<Path_T>& helper, PathVectorPtr_t partialPath, const std::size_t numOptimizations)
        {
            core::RandomShortcutPtr_t rs = core::RandomShortcut::create(helper.rootProblem_);
            for(std::size_t j=0; j<numOptimizations;++j)
            {
                partialPath = rs->optimize(partialPath);
            }
            return partialPath;
        }

        inline std::size_t checkPath(const std::size_t& distance, bool valid[])
        {
            std::size_t numValid(distance);
            for(std::size_t i = 0; i < distance; ++i)
            {
               if (!valid[i])
               {
                    numValid= i;
                    break;
               }
            }
            if (numValid==0)
                throw std::runtime_error("No path found at state 0");
            else if(numValid != distance)
            {
                std::cout << "No path found at state " << numValid << std::endl;
            }
            return numValid;
        }

        inline PathPtr_t ConcatenateAndResizePath(PathVectorPtr_t res[], std::size_t numValid)
        {
            PathVectorPtr_t completePath = res[0];
            for(std::size_t i = 1; i < numValid; ++i)
            {
                completePath->concatenate(*res[i]);
            }
            // reducing path
            core::SizeInterval_t interval(0, completePath->initial().rows()-1);
            core::SizeIntervals_t intervals;
            intervals.push_back(interval);
            PathPtr_t reducedPath = core::SubchainPath::create(completePath,intervals);
            return reducedPath;
        }
    }

    template<class Path_T>
    PathPtr_t interpolateStates(RbPrmFullBodyPtr_t fullbody, core::ProblemPtr_t referenceProblem, const PathPtr_t rootPath,
                                      const CIT_StateFrame &startState, const CIT_StateFrame &endState, const  std::size_t numOptimizations)
    {
        PathVectorPtr_t res[100];
        bool valid[100];
        std::size_t distance = std::distance(startState,endState);
        assert(distance < 100);
        // treat each interpolation between two states separatly
        // in a different thread
        #pragma omp parallel for
        for(std::size_t i = 0; i < distance; ++i)
        {
            CIT_StateFrame a, b;
            a = (startState+i);
            b = (startState+i+1);
            TimeConstraintHelper<Path_T> helper(fullbody, referenceProblem,rootPath);
                                 //rootPath->extract(core::interval_t(a->first, b->first)));
            PathVectorPtr_t partialPath = interpolateStates(helper, a->second, b->second);
            if(partialPath)
            {
                res[i] = optimize(helper,partialPath, numOptimizations);
                valid[i]=true;
            }
            else
            {
                valid[i] = false;
            }
        }
        std::size_t numValid = checkPath(distance, valid);
        return ConcatenateAndResizePath(res, numValid);
    }

    template<class Path_T>
    PathPtr_t interpolateStatesTrackCOM(RbPrmFullBodyPtr_t fullbody, core::ProblemPtr_t referenceProblem, const PathPtr_t rootPath,
                                      const CIT_StateFrame &startState, const CIT_StateFrame &endState, const  std::size_t numOptimizations)
    {
        PathVectorPtr_t res[100];
        bool valid[100];
        std::size_t distance = std::distance(startState,endState);
        assert(distance < 100);
        // treat each interpolation between two states separatly
        // in a different thread
        #pragma omp parallel for
        for(std::size_t i = 0; i < distance; ++i)
        {
            CIT_StateFrame a, b;
            a = (startState+i);
            b = (startState+i+1);
            TimeConstraintHelper<Path_T> helper(fullbody, referenceProblem,rootPath);
                                 //rootPath->extract(core::interval_t(a->first, b->first)));
            CreateComConstraint<Path_T>(helper);
            PathVectorPtr_t partialPath = interpolateStates(helper, a->second, b->second);
            if(partialPath)
            {
                res[i] = optimize(helper,partialPath, numOptimizations);
                valid[i]=true;
            }
            else
            {
                valid[i] = false;
            }
        }
        std::size_t numValid = checkPath(distance, valid);
        return ConcatenateAndResizePath(res, numValid);
    }

    template<class Path_T, typename StateConstIterator>
    PathPtr_t interpolateStates(RbPrmFullBodyPtr_t fullbody, core::ProblemPtr_t referenceProblem,
                                      const StateConstIterator &startState, const StateConstIterator &endState, const std::size_t numOptimizations)
    {
        PathVectorPtr_t res[100];
        bool valid[100];
        std::size_t distance = std::distance(startState,endState);
        assert(distance < 100);
        // treat each interpolation between two states separatly
        // in a different thread
        #pragma omp parallel for
        for(std::size_t i = 0; i < distance; ++i)
        {
            StateConstIterator a, b;
            a = (startState+i);
            b = (startState+i+1);
            TimeConstraintHelper<Path_T> helper(fullbody, referenceProblem, generateRootPath(*referenceProblem, *a, *b));
            PathVectorPtr_t partialPath = interpolateStates(helper, *a, *b);
            if(partialPath)
            {
                res[i] = optimize(helper,partialPath, numOptimizations);
                valid[i]=true;
            }
            else
            {
                valid[i] = false;
            }
        }
        std::size_t numValid = checkPath(distance, valid);
        return ConcatenateAndResizePath(res, numValid);
    }

  }// namespace interpolation
  }// namespace rbprm
}// namespace hpp

#endif // HPP_RBPRM_LIMB_RRT_HELPER_UTILS_HH
