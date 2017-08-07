//
// Copyright (c) 2017 CNRS
// Authors: Pierre Fernbach
//
// This file is part of hpp-rbprm
// hpp-core is free software: you can redistribute it
// and/or modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation, either version
// 3 of the License, or (at your option) any later version.
//
// hpp-core is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Lesser Public License for more details.  You should have
// received a copy of the GNU Lesser General Public License along with
// hpp-core  If not, see
// <http://www.gnu.org/licenses/>.


#include <hpp/rbprm/planner/random-shortcut-dynamic.hh>
#include <limits>
#include <deque>
#include <cstdlib>
#include <hpp/util/assertion.hh>
#include <hpp/util/debug.hh>
#include <hpp/core/distance.hh>
#include <hpp/core/path-vector.hh>
#include <hpp/core/problem.hh>
#include <hpp/core/path-projector.hh>
#include <hpp/core/kinodynamic-oriented-path.hh>
#include <hpp/rbprm/planner/rbprm-node.hh>
#include <hpp/core/path-validation.hh>
#include <hpp/core/config-validations.hh>

namespace hpp{
  namespace rbprm{
    using core::PathVector;
    using core::PathVectorPtr_t;
    using core::PathPtr_t;
    using core::Problem;
    using core::ConfigurationIn_t;
    using core::ConfigurationPtr_t;
    using core::Configuration_t;
    using model::value_type;
    using core::DistancePtr_t;
    using core::KinodynamicPathPtr_t;
    using core::KinodynamicPath;
    using core::KinodynamicOrientedPathPtr_t;
    using core::KinodynamicOrientedPath;


    RandomShortcutDynamicPtr_t
    RandomShortcutDynamic::create (const Problem& problem)
    {
      RandomShortcutDynamic* ptr = new RandomShortcutDynamic (problem);
      return RandomShortcutDynamicPtr_t (ptr);
    }

    RandomShortcutDynamic::RandomShortcutDynamic (const Problem& problem) :
      RandomShortcut (problem),
      sm_(boost::dynamic_pointer_cast<SteeringMethodKinodynamic>(problem.steeringMethod())),
      rbprmPathValidation_(boost::dynamic_pointer_cast<RbPrmPathValidation>(problem.pathValidation()))
    {
      assert(sm_ && "Random-shortcut-dynamic must use a kinodynamic-steering-method");
      assert(rbprmPathValidation_ && "Path validation should be a RbPrmPathValidation class for this solver");

      // retrieve parameters from problem :
      try {
        boost::any value_x = problem.get<boost::any> (std::string("sizeFootX"));
        boost::any value_y = problem.get<boost::any> (std::string("sizeFootY"));
        sizeFootX_ = boost::any_cast<double>(value_x)/2.;
        sizeFootY_ = boost::any_cast<double>(value_y)/2.;
        rectangularContact_ = 1;
      } catch (const std::exception& e) {
        hppDout(warning,"Warning : size of foot not definied, use 0 (contact point)");
        sizeFootX_ =0;
        sizeFootY_ =0;
        rectangularContact_ = 0;
      }
      try {
        boost::any value = problem.get<boost::any> (std::string("tryJump"));
        tryJump_ = boost::any_cast<bool>(value);
      } catch (const std::exception& e) {
        tryJump_=false;
      }
      hppDout(notice,"tryJump in random shortcut = "<<tryJump_);

      try {
        boost::any value = problem.get<boost::any> (std::string("friction"));
        mu_ = boost::any_cast<double>(value);
        hppDout(notice,"mu define in python : "<<mu_);
      } catch (const std::exception& e) {
        mu_= 0.5;
        hppDout(notice,"mu not defined, take : "<<mu_<<" as default.");
      }
    }


    // Compute the length of a vector of paths assuming that each element
    // is optimal for the given distance.
    template <bool reEstimateLength = false> struct PathLength {
      static inline value_type run (const PathVectorPtr_t& path,
                                    const DistancePtr_t& /*distance*/)
      {
        if (reEstimateLength) return path->length ();
        else {
          value_type result = 0;
          for (std::size_t i=0; i<path->numberPaths (); ++i) {
            const PathPtr_t& element (path->pathAtRank (i));
            result += element->length();
          }
          return result;
        }
      }
    };




    PathVectorPtr_t RandomShortcutDynamic::optimize (const PathVectorPtr_t& path)
    {
      hppDout(notice,"!! Start optimize()");
      using std::numeric_limits;
      using std::make_pair;
      bool finished = false;
      value_type t[4];
      Configuration_t q[4];
      q[0] = path->initial ();
      q[3] = path->end ();
      PathVectorPtr_t tmpPath = path;

      // Maximal number of iterations without improvements
      std::size_t n = problem().getParameter<std::size_t>("PathOptimizersNumberOfLoops", 5);
      n = 100;
      std::cout<<" number of loops : "<<n<<std::endl;
      std::size_t projectionError = n;
      std::deque <value_type> length (n-1,
                                      numeric_limits <value_type>::infinity ());
      length.push_back (PathLength<>::run (tmpPath, problem ().distance ()));
      PathVectorPtr_t result;
      Configuration_t q1 (path->outputSize ()),
          q2 (path->outputSize ());

      q[1] = q1;
      q[2] = q2;
      while (!finished && projectionError != 0) {
        t[0] = tmpPath->timeRange ().first;
        t[3] = tmpPath->timeRange ().second;
        value_type u2 = t[0] + (t[3] -t[0]) * rand ()/RAND_MAX;
        value_type u1 = t[0] + (t[3] -t[0]) * rand ()/RAND_MAX;
        if (u1 < u2) {t[1] = u1; t[2] = u2;} else {t[1] = u2; t[2] = u1;}
        if (!(*tmpPath) (q[1], t[1])) {
          hppDout (error, "Configuration at param " << t[1] << " could not be "
                                                               "projected");
          projectionError--;
          continue;
        }
        if (!(*tmpPath) (q[2], t[2])) {
          hppDout (error, "Configuration at param " << t[2] << " could not be "
                                                               "projected");
          projectionError--;
          continue;
        }
        // Validate sub parts
        bool replaceValid(false);
        bool valid [3];
        bool orientedValid[3];
        PathPtr_t straight [3];
        PathPtr_t oriented [3];
        KinodynamicPathPtr_t castedPath;
        core::PathValidationReportPtr_t report;
        PathPtr_t validPart;
        PathPtr_t resultPaths[3];

        for (unsigned i=0; i<3; ++i) {
          straight [i] = steer (q[i], q[i+1]);
          orientedValid[i]=false;
          if (!straight [i]) valid[i] = false;
          else { // with kinodynamic path, we are not assured that a 'straight line' is shorter than the previously found path
            valid[i] = (straight[i]->length() < PathLength<true>::run (tmpPath->extract(make_pair <value_type, value_type> (t[i], t[i+1]))->as <PathVector> (), problem ().distance ()));
            if(valid[i])
              valid[i] = problem ().pathValidation ()->validate(straight [i], false, validPart, report);

            if(valid[i]){
              resultPaths[i] = straight[i];
              castedPath = boost::dynamic_pointer_cast<KinodynamicPath>(straight[i]);
              if(castedPath){
                oriented[i] = KinodynamicOrientedPath::createCopy(castedPath);
                orientedValid[i] = problem ().pathValidation ()->validate(oriented[i], false, validPart, report);
              }
            }
          }
          if(!valid[i])
            resultPaths[i] = tmpPath->extract(make_pair <value_type, value_type> (t[i], t[i+1]))->as <PathVector> ();
        }
        hppDout(notice,"t0 = "<<t[0]<<" ; t1 = "<<t[1]<<" ; t2 = "<<t[2]<<" ; t3 = "<<t[3]);
        hppDout(notice,"first segment : oriented : "<<orientedValid[0] <<" ; valid : "<<valid[0]);
        hppDout(notice,"mid segment   : oriented : "<<orientedValid[1] <<" ; valid : "<<valid[1]);
        hppDout(notice,"last segment  : oriented : "<<orientedValid[2] <<" ; valid : "<<valid[2]);


        // If we replace a path with an oriented path, we must adjust the initial and/or end config
        // for the previous and/or next path to avoid discontinuitie in orientation :
        PathPtr_t tmpResult[3];
        PathVectorPtr_t replaceVectorTmp;
        PathPtr_t replaceTmp;
        replaceValid = false;
        if(orientedValid[1]){ // start with the middle segment
          hppDout(notice,"Mid segment oriented, try to adjust the first segment : ");
          // check the previous segment :
          if(orientedValid[0]){
            tmpResult[0] = oriented[0];
            replaceValid = true;
          }else if (valid[0]){
            replaceTmp = steer(q[0],oriented[1]->initial());
            if(replaceTmp){
              replaceValid = problem ().pathValidation ()->validate(replaceTmp, false, validPart, report);
              if (replaceValid){
                tmpResult[0] = replaceTmp;
              }
            }
          }else{
            replaceVectorTmp = tmpPath->extract(make_pair <value_type, value_type> (t[0], t[1]))->as <PathVector> ();
            replaceTmp = replaceVectorTmp->pathAtRank(replaceVectorTmp->numberPaths ()-1); // extract last path
            replaceTmp = steer(replaceTmp->initial(),oriented[1]->initial()); // change the orientation of the end of the last path
            if(replaceTmp){
              replaceValid = problem ().pathValidation ()->validate(replaceTmp, false, validPart, report);
              if (replaceValid){ // create a new pathVector for the first segment with all the old path exept the last one replaced with the new orientation
                PathVectorPtr_t pv = PathVector::create (replaceTmp->outputSize(),replaceTmp->outputDerivativeSize());
                for (std::size_t i=0; i<replaceVectorTmp->numberPaths () - 1; ++i) {
                  const PathPtr_t& element (replaceVectorTmp->pathAtRank (i));
                  pv->appendPath(element);
                }
                pv->appendPath(replaceTmp);
                tmpResult[0] = pv;
              }
            }
          }// else : first segment is a pathvector
          hppDout(notice,"First segment adjusted : "<<replaceValid);
          if(replaceValid){
            hppDout(notice,"Mid segment oriented, try to adjust the last segment : ");
          // check the last segment
            replaceValid = false;
            if(orientedValid[2]){
              tmpResult[2] = oriented[2];
              replaceValid = true;
            }else if (valid[2]){
              replaceTmp = steer(oriented[1]->end(),q[3]);
              if(replaceTmp){
                replaceValid = problem ().pathValidation ()->validate(replaceTmp, false, validPart, report);
                if (replaceValid){
                  tmpResult[2] = replaceTmp;
                }
              }
            }else{
              replaceVectorTmp = tmpPath->extract(make_pair <value_type, value_type> (t[2], t[3]))->as <PathVector> ();
              replaceTmp = replaceVectorTmp->pathAtRank(0); // extract first path
              replaceTmp = steer(oriented[1]->end(),replaceTmp->end()); // change the orientation of the init of the first path
              if(replaceTmp){
                replaceValid = problem ().pathValidation ()->validate(replaceTmp, false, validPart, report);
                if (replaceValid){ // create a new pathVector for the last segment with all the old path exept the first one replaced with the new orientation
                  PathVectorPtr_t pv = PathVector::create (replaceTmp->outputSize(),replaceTmp->outputDerivativeSize());
                  pv->appendPath(replaceTmp);
                  for (std::size_t i=1; i<replaceVectorTmp->numberPaths (); ++i) {
                    const PathPtr_t& element (replaceVectorTmp->pathAtRank (i));
                    pv->appendPath(element);
                  }
                  tmpResult[2] = pv;
                }
              }
            } // else (last segment is a pathVector)
          }// if(replaceValid) : test for the last segment
          hppDout(notice,"Last segment adjusted : "<<replaceValid);
          if(replaceValid){ // mid segment is oriented and first and last segment were successfuly adjusted :
            hppDout(notice,"Both segment successfully adjusted, replace the tmp values.");
            resultPaths[1] = oriented[1];
            resultPaths[0] = tmpResult[0];
            resultPaths[2] = tmpResult[2];
          }
        }else{ // check if first or last segment are oriented and try to adjust middle segment :
          if(orientedValid[0]){
            hppDout(notice,"First segment is oriented, try to adjust mid segment : ");
            // check if mid segment can be adjusted :
            if (valid[1]){
              replaceTmp = steer(oriented[0]->end(),q[2]);
              if(replaceTmp){
                replaceValid = problem ().pathValidation ()->validate(replaceTmp, false, validPart, report);
                if (replaceValid){
                  hppDout(notice,"Mid segment is valid, replace both");
                  resultPaths[1] = replaceTmp;
                  resultPaths[0] = oriented[0];
                }
              }
            }else{
              replaceVectorTmp = tmpPath->extract(make_pair <value_type, value_type> (t[1], t[2]))->as <PathVector> ();
              replaceTmp = replaceVectorTmp->pathAtRank(0); // extract first path
              replaceTmp = steer(oriented[0]->end(),replaceTmp->end()); // change the orientation of the init config of the first path
              if(replaceTmp){
                replaceValid = problem ().pathValidation ()->validate(replaceTmp, false, validPart, report);
                if (replaceValid){ // create a new pathVector for the mid segment with all the old path exept the first one replaced with the new orientation
                  PathVectorPtr_t pv = PathVector::create (replaceTmp->outputSize(),replaceTmp->outputDerivativeSize());
                  pv->appendPath(replaceTmp);
                  for (std::size_t i=1; i<replaceVectorTmp->numberPaths (); ++i) {
                    const PathPtr_t& element (replaceVectorTmp->pathAtRank (i));
                    pv->appendPath(element);
                  }
                  hppDout(notice,"Mid segment is a path vector, successfully adjusted, replace both");
                  resultPaths[1] = pv;
                  resultPaths[0] = oriented[0];
                }
              }
            } // mid segment is a pathVector

          } // if first segment is oriented

          if(orientedValid[2]){
            hppDout(notice,"Last segment is oriented, try to adjust mid segment : ");
            // check if mid segment can be adjusted :
            if (valid[1]){
              replaceTmp = steer(q[1],oriented[2]->initial());
              if(replaceTmp){
                replaceValid = problem ().pathValidation ()->validate(replaceTmp, false, validPart, report);
                if (replaceValid){
                  hppDout(notice,"Mid segment is valid, replace both");
                  resultPaths[1] = replaceTmp;
                  resultPaths[2] = oriented[2];
                }
              }
            }else{
              replaceVectorTmp = tmpPath->extract(make_pair <value_type, value_type> (t[1], t[2]))->as <PathVector> ();
              replaceTmp = replaceVectorTmp->pathAtRank(replaceVectorTmp->numberPaths() - 1 ); // extract last path
              replaceTmp = steer(replaceTmp->initial(),oriented[2]->initial()); // change the orientation of the end config of the last path
              if(replaceTmp){
                replaceValid = problem ().pathValidation ()->validate(replaceTmp, false, validPart, report);
                if (replaceValid){ // create a new pathVector for the mid segment with all the old path exept the last one replaced with the new orientation
                  PathVectorPtr_t pv = PathVector::create (replaceTmp->outputSize(),replaceTmp->outputDerivativeSize());
                  for (std::size_t i=0; i<replaceVectorTmp->numberPaths ()-1; ++i) {
                    const PathPtr_t& element (replaceVectorTmp->pathAtRank (i));
                    pv->appendPath(element);
                  }
                  pv->appendPath(replaceTmp);
                  hppDout(notice,"Mid segment is a path vector, successfully adjusted, replace both");
                  resultPaths[1] = pv;
                  resultPaths[2] = oriented[2];
                }
              }
            } // mid segment is a pathVector

          } // if last segment is oriented
        } // if mid segment was not oriented


        // Replace valid parts
        result = PathVector::create (path->outputSize (),
                                     path->outputDerivativeSize ());

        for (unsigned i=0; i<3; ++i) {
          try {
            if (valid [i] || orientedValid[i])
              result->appendPath (resultPaths [i]);
            else
              result->concatenate (resultPaths[i]->as <PathVector> ());
          } catch (const core::projection_error& e) {
            hppDout (error, "Caught exception at with time " << t[i] << " and " <<
                                                                        t[i+1] << ": " << e.what ());
            projectionError--;
            result = tmpPath;
            continue;
          }
        }

        length.push_back (PathLength<>::run (result, problem ().distance ()));
        length.pop_front ();
        finished = (length [0] - length [n-1]) <= 1e-4 * length[n-1];
        hppDout (info, "length = " << length [n-1]);
        tmpPath = result;
      }
      for (std::size_t i = 0; i < result->numberPaths (); ++i) {
        if (result->pathAtRank(i)->constraints())
          hppDout (info, "At rank " << i << ", constraints are " <<
                   *result->pathAtRank(i)->constraints());
        else
          hppDout (info, "At rank " << i << ", no constraints");
      }
      return result;
    } // optimize


    PathPtr_t RandomShortcutDynamic::steer (ConfigurationIn_t q1,
                                            ConfigurationIn_t q2) const
    {
      // according to optimize() method : the path is always in the direction q1 -> q2
      // first : create a node and fill all informations about contacts for the initial state (q1):
      core::RbprmNodePtr_t x1(new core::RbprmNode (ConfigurationPtr_t (new Configuration_t(q1))));
      core::ValidationReportPtr_t report;
      rbprmPathValidation_->getValidator()->randomnizeCollisionPairs(); // FIXME : remove if we compute all collision pairs
      rbprmPathValidation_->getValidator()->computeAllContacts(true);
      problem().configValidations()->validate(q1,report);
      rbprmPathValidation_->getValidator()->computeAllContacts(false);

      x1->fillNodeMatrices(report,rectangularContact_,sizeFootX_,sizeFootY_,problem().robot()->mass(),mu_);

      // call steering method kinodynamic with the newly created node
      PathPtr_t dp = (*sm_)(x1,q2);
      if (dp) {
        if((dp->initial() != q1)  || (dp->end() != q2)){
          return PathPtr_t ();
        }
        if (!problem().pathProjector()) return dp;
        PathPtr_t pp;
        if (problem().pathProjector()->apply (dp, pp))
          return pp;
      }
      return PathPtr_t ();
    }










  } // rbprm
}//hpp
