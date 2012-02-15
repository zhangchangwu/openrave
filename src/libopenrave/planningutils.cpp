// -*- coding: utf-8 -*-
// Copyright (C) 2006-2012 Rosen Diankov <rosen.diankov@gmail.com>
//
// This file is part of OpenRAVE.
// OpenRAVE is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#include "libopenrave.h"

#include <openrave/planningutils.h>

namespace OpenRAVE {
namespace planningutils {

int JitterActiveDOF(RobotBasePtr robot,int nMaxIterations,dReal fRand,const PlannerBase::PlannerParameters::NeighStateFn& neighstatefn)
{
    RAVELOG_VERBOSE("starting jitter active dof...\n");
    vector<dReal> curdof, newdof, deltadof, deltadof2;
    robot->GetActiveDOFValues(curdof);
    newdof.resize(curdof.size());
    deltadof.resize(curdof.size(),0);
    CollisionReport report;
    CollisionReportPtr preport(&report,utils::null_deleter());
    bool bCollision = false;
    bool bConstraint = !!neighstatefn;

    // have to test with perturbations since very small changes in angles can produce collision inconsistencies
    boost::array<dReal,3> perturbations = { { 0,1e-5f,-1e-5f}};
    FOREACH(itperturbation,perturbations) {
        if( bConstraint ) {
            FOREACH(it,deltadof) {
                *it = *itperturbation;
            }
            newdof = curdof;
            if( !neighstatefn(newdof,deltadof,0) ) {
                robot->SetActiveDOFValues(curdof,true);
                return -1;
            }
        }
        else {
            for(size_t i = 0; i < newdof.size(); ++i) {
                newdof[i] = curdof[i]+*itperturbation;
            }
        }
        robot->SetActiveDOFValues(newdof,true);

        if(robot->CheckSelfCollision(preport)) {
            bCollision = true;
            RAVELOG_DEBUG(str(boost::format("JitterActiveDOFs: self collision: %s!\n")%report.__str__()));
            break;
        }
        if( robot->GetEnv()->CheckCollision(KinBodyConstPtr(robot),preport) ) {
            bCollision = true;
            RAVELOG_DEBUG(str(boost::format("JitterActiveDOFs: collision: %s!\n")%report.__str__()));
            break;
        }
    }

    if( !bCollision || fRand <= 0 ) {
        // have to restore to initial non-perturbed configuration!
        robot->SetActiveDOFValues(curdof);
        return -1;
    }

    deltadof2.resize(curdof.size(),0);
    for(int iter = 0; iter < nMaxIterations; ++iter) {
        for(size_t j = 0; j < newdof.size(); ++j) {
            deltadof[j] = fRand * (RaveRandomFloat()-0.5f);
        }
        bCollision = false;
        bool bConstraintFailed = false;
        FOREACH(itperturbation,perturbations) {
            for(size_t j = 0; j < deltadof.size(); ++j) {
                deltadof2[j] = deltadof[j] + *itperturbation;
            }
            if( bConstraint ) {
                newdof = curdof;
                robot->SetActiveDOFValues(newdof,true);
                if( !neighstatefn(newdof,deltadof2,0) ) {
                    if( *itperturbation != 0 ) {
                        RAVELOG_DEBUG(str(boost::format("constraint function failed, pert=%e\n")%*itperturbation));
                    }
                    bConstraintFailed = true;
                    break;
                }
            }
            else {
                for(size_t j = 0; j < deltadof.size(); ++j) {
                    newdof[j] = curdof[j] + deltadof2[j];
                }
            }
            robot->SetActiveDOFValues(newdof,true);
            if(robot->CheckSelfCollision() || robot->GetEnv()->CheckCollision(KinBodyConstPtr(robot)) ) {
                bCollision = true;
                break;
            }
        }
        if( !bCollision && !bConstraintFailed ) {
            // have to restore to non-perturbed configuration!
            if( bConstraint ) {
                newdof = curdof;
                robot->SetActiveDOFValues(newdof,true);
                if( !neighstatefn(newdof,deltadof,0) ) {
                    RAVELOG_WARN("neighstatefn failed, but previously succeeded\n");
                    continue;
                }
            }
            else {
                for(size_t j = 0; j < deltadof.size(); ++j) {
                    newdof[j] = curdof[j] + deltadof[j];
                }
            }
            robot->SetActiveDOFValues(newdof);
            return 1;
        }
    }

    return 0;
}

bool JitterTransform(KinBodyPtr pbody, float fJitter, int nMaxIterations)
{
    RAVELOG_VERBOSE("starting jitter transform...\n");

    // randomly add small offset to the body until it stops being in collision
    Transform transorig = pbody->GetTransform();
    Transform transnew = transorig;
    int iter = 0;
    while(pbody->GetEnv()->CheckCollision(KinBodyConstPtr(pbody)) ) {
        if( iter > nMaxIterations ) {
            return false;
        }
        if( iter > 0 && fJitter <= 0 ) {
            return false;
        }
        transnew.trans = transorig.trans + fJitter * Vector(RaveRandomFloat()-0.5f, RaveRandomFloat()-0.5f, RaveRandomFloat()-0.5f);
        pbody->SetTransform(transnew);
        ++iter;
    }

    return true;
}

class TrajectoryVerifier
{
public:
    TrajectoryVerifier(PlannerBase::PlannerParametersConstPtr parameters) : _parameters(parameters) {
        VerifyParameters();
    }

    void VerifyParameters() {
        OPENRAVE_ASSERT_FORMAT0(!!_parameters,"need planner parameters to verify trajectory",ORE_InvalidArguments);
        OPENRAVE_ASSERT_OP_FORMAT0((int)_parameters->_vConfigLowerLimit.size(), ==, _parameters->GetDOF(), "unexpected size",ORE_InvalidState);
        OPENRAVE_ASSERT_OP_FORMAT0((int)_parameters->_vConfigUpperLimit.size(), ==, _parameters->GetDOF(), "unexpected size",ORE_InvalidState);
        OPENRAVE_ASSERT_OP_FORMAT0((int)_parameters->_vConfigResolution.size(), ==, _parameters->GetDOF(), "unexpected size",ORE_InvalidState);
    }

    void VerifyTrajectory(TrajectoryBaseConstPtr trajectory, dReal samplingstep)
    {
        OPENRAVE_ASSERT_FORMAT0(!!trajectory,"need valid trajectory",ORE_InvalidArguments);

        ConfigurationSpecification velspec =  _parameters->_configurationspecification.ConvertToVelocitySpecification();

        dReal fresolutionmean = 0;
        FOREACHC(it,_parameters->_vConfigResolution) {
            fresolutionmean += *it;
        }
        fresolutionmean /= _parameters->_vConfigResolution.size();

        dReal fthresh = 5e-5f;
        vector<dReal> deltaq(_parameters->GetDOF(),0);
        std::vector<dReal> vdata, vdatavel, vdiff;
        for(size_t ipoint = 0; ipoint < trajectory->GetNumWaypoints(); ++ipoint) {
            trajectory->GetWaypoint(ipoint,vdata,_parameters->_configurationspecification);
            trajectory->GetWaypoint(ipoint,vdatavel,velspec);
            BOOST_ASSERT((int)vdata.size()==_parameters->GetDOF());
            BOOST_ASSERT((int)vdatavel.size()==_parameters->GetDOF());
            for(size_t i = 0; i < vdata.size(); ++i) {
                if( !(vdata[i] >= _parameters->_vConfigLowerLimit[i]-fthresh) || !(vdata[i] <= _parameters->_vConfigUpperLimit[i]+fthresh) ) {
                    throw OPENRAVE_EXCEPTION_FORMAT("limits exceeded configuration %d dof %d: %f in [%f,%f]", ipoint%i%vdata[i]%_parameters->_vConfigLowerLimit[i]%_parameters->_vConfigUpperLimit[i], ORE_InconsistentConstraints);
                }
            }
            for(size_t i = 0; i < _parameters->_vConfigVelocityLimit.size(); ++i) {
                if( !(RaveFabs(vdatavel.at(i)) <= _parameters->_vConfigVelocityLimit[i]+fthresh) ) { // !(x<=y) necessary for catching NaNs
                    throw OPENRAVE_EXCEPTION_FORMAT("velocity exceeded configuration %d dof %d: %f>%f", ipoint%i%RaveFabs(vdatavel.at(i))%_parameters->_vConfigVelocityLimit[i], ORE_InconsistentConstraints);
                }
            }
            _parameters->_setstatefn(vdata);
            vector<dReal> newq;
            _parameters->_getstatefn(newq);
            BOOST_ASSERT(vdata.size() == newq.size());
            vdiff = newq;
            _parameters->_diffstatefn(vdiff,vdata);
            for(size_t i = 0; i < vdiff.size(); ++i) {
                if( !(RaveFabs(vdiff.at(i)) <= 0.001 * _parameters->_vConfigResolution[i]) ) {
                    string filename = str(boost::format("%s/failedtrajectory%d.xml")%RaveGetHomeDirectory()%(RaveRandomInt()%1000));
                    ofstream f(filename.c_str());
                    f << std::setprecision(std::numeric_limits<dReal>::digits10+1);     /// have to do this or otherwise precision gets lost
                    trajectory->serialize(f);
                    throw OPENRAVE_EXCEPTION_FORMAT("setstate/getstate inconsistent configuration %d dof %d: %f != %f, wrote trajectory to %s",ipoint%i%vdata.at(i)%newq.at(i)%filename,ORE_InconsistentConstraints);
                }
            }
            if( !!_parameters->_neighstatefn ) {
                newq = vdata;
                if( !_parameters->_neighstatefn(newq,vdiff,0) ) {
                    throw OPENRAVE_EXCEPTION_FORMAT("neighstatefn is rejecting configuration %d, wrote trajectory %s",ipoint%DumpTrajectory(trajectory),ORE_InconsistentConstraints);
                }
                dReal fdist = _parameters->_distmetricfn(newq,vdata);
                OPENRAVE_ASSERT_OP_FORMAT(fdist,<=,0.01 * fresolutionmean, "neighstatefn is rejecting configuration %d, wrote trajectory %s",ipoint%DumpTrajectory(trajectory),ORE_InconsistentConstraints);
            }
        }

        if( !!_parameters->_checkpathconstraintsfn && trajectory->GetNumWaypoints() >= 2 ) {

            if( trajectory->GetDuration() > 0 && samplingstep > 0 ) {
                // use sampling and check segment constraints
                std::vector<dReal> vprevdata, vprevdatavel;
                PlannerBase::ConfigurationListPtr configs(new PlannerBase::ConfigurationList());
                trajectory->Sample(vprevdata,0,_parameters->_configurationspecification);
                trajectory->Sample(vprevdatavel,0,velspec);
                for(dReal ftime = 0; ftime < trajectory->GetDuration(); ftime += samplingstep ) {
                    configs->clear();
                    trajectory->Sample(vdata,ftime+samplingstep,_parameters->_configurationspecification);
                    trajectory->Sample(vdatavel,ftime+samplingstep,velspec);
                    vdiff = vdata;
                    _parameters->_diffstatefn(vdiff,vprevdata);
                    for(size_t i = 0; i < _parameters->_vConfigVelocityLimit.size(); ++i) {
                        dReal velthresh = _parameters->_vConfigVelocityLimit.at(i)*samplingstep+fthresh;
                        OPENRAVE_ASSERT_OP_FORMAT(RaveFabs(vdiff.at(i)), <=, velthresh, "time %fs-%fs, dof %d traveled %f, but maxvelocity only allows %f, wrote trajectory to %s",ftime%(ftime+samplingstep)%i%RaveFabs(vdiff.at(i))%velthresh%DumpTrajectory(trajectory),ORE_InconsistentConstraints);
                    }
                    if( !_parameters->_checkpathconstraintsfn(vprevdata,vdata,IT_Closed, configs) ) {
                        throw OPENRAVE_EXCEPTION_FORMAT("time %fs-%fs, checkpathconstraintsfn failed, wrote trajectory to %s",ftime%(ftime+samplingstep)%DumpTrajectory(trajectory),ORE_InconsistentConstraints);
                    }
                    PlannerBase::ConfigurationList::iterator itprevconfig = configs->begin();
                    PlannerBase::ConfigurationList::iterator itcurconfig = ++configs->begin();
                    for(; itcurconfig != configs->end(); ++itcurconfig) {
                        BOOST_ASSERT( (int)itcurconfig->size() == _parameters->GetDOF());
                        for(size_t i = 0; i < itcurconfig->size(); ++i) {
                            deltaq.at(i) = itcurconfig->at(i) - itprevconfig->at(i);
                        }
                        _parameters->_setstatefn(*itprevconfig);
                        vector<dReal> vtemp = *itprevconfig;
                        if( !_parameters->_neighstatefn(vtemp,deltaq,0) ) {
                            throw OPENRAVE_EXCEPTION_FORMAT("time %fs-%fs, neighstatefn is rejecting configurations from checkpathconstraintsfn, wrote trajectory to %s",ftime%(ftime+samplingstep)%DumpTrajectory(trajectory),ORE_InconsistentConstraints);
                        }
                        else {
                            dReal fprevdist = _parameters->_distmetricfn(*itprevconfig,vtemp);
                            dReal fcurdist = _parameters->_distmetricfn(*itcurconfig,vtemp);
                            OPENRAVE_ASSERT_OP_FORMAT(fprevdist, >, fcurdist, "time %fs-%fs, neightstatefn returned a configuration closer to the previous configuration %f than the expected current %f, wrote trajectory to %s",ftime%(ftime+samplingstep)%fprevdist%fcurdist%DumpTrajectory(trajectory), ORE_InconsistentConstraints);
                        }
                        itprevconfig=itcurconfig;
                    }
                    vprevdata=vdata;
                    vprevdatavel=vdatavel;
                }
            }
            else {
                for(size_t i = 0; i < trajectory->GetNumWaypoints(); ++i) {
                    trajectory->GetWaypoint(i,vdata,_parameters->_configurationspecification);
                    if( !_parameters->_checkpathconstraintsfn(vdata,vdata,IT_OpenStart, PlannerBase::ConfigurationListPtr()) ) {
                        throw OPENRAVE_EXCEPTION_FORMAT("checkpathconstraintsfn failed at %d, wrote trajectory to %s",i%DumpTrajectory(trajectory),ORE_InconsistentConstraints);
                    }
                }
            }
        }
    }

    string DumpTrajectory(TrajectoryBaseConstPtr trajectory)
    {
        string filename = str(boost::format("%s/failedtrajectory%d.xml")%RaveGetHomeDirectory()%(RaveRandomInt()%1000));
        ofstream f(filename.c_str());
        f << std::setprecision(std::numeric_limits<dReal>::digits10+1);     /// have to do this or otherwise precision gets lost
        trajectory->serialize(f);
        return filename;
    }

protected:
    PlannerBase::PlannerParametersConstPtr _parameters;
};

void VerifyTrajectory(PlannerBase::PlannerParametersConstPtr parameters, TrajectoryBaseConstPtr trajectory, dReal samplingstep)
{
    EnvironmentMutex::scoped_lock lockenv(trajectory->GetEnv()->GetMutex());
    TrajectoryVerifier v(parameters);
    v.VerifyTrajectory(trajectory,samplingstep);
}

void _PlanActiveDOFTrajectory(TrajectoryBasePtr traj, RobotBasePtr probot, bool hastimestamps, dReal fmaxvelmult, const std::string& plannername, const std::string& interpolation, bool bsmooth)
{
    if( traj->GetNumWaypoints() == 1 ) {
        // don't need velocities, but should at least add a time group
        ConfigurationSpecification spec = traj->GetConfigurationSpecification();
        spec.AddDeltaTimeGroup();
        vector<dReal> data;
        traj->GetWaypoints(0,traj->GetNumWaypoints(),data,spec);
        traj->Init(spec);
        traj->Insert(0,data);
        return;
    }

    EnvironmentMutex::scoped_lock lockenv(traj->GetEnv()->GetMutex());
    PlannerBasePtr planner = RaveCreatePlanner(traj->GetEnv(),plannername.size() > 0 ? plannername : string("parabolicsmoother"));
    PlannerBase::PlannerParametersPtr params(new PlannerBase::PlannerParameters());
    params->SetRobotActiveJoints(probot);
    FOREACH(it,params->_vConfigVelocityLimit) {
        *it *= fmaxvelmult;
    }
    if( !bsmooth ) {
        params->_setstatefn.clear();
        params->_checkpathconstraintsfn.clear();
    }

    params->_sExtraParameters += str(boost::format("<hastimestamps>%d</hastimestamps>")%hastimestamps);
    if( interpolation.size() > 0 ) {
        params->_sExtraParameters += str(boost::format("<interpolation>%s</interpolation>")%interpolation);
    }
    if( !planner->InitPlan(probot,params) ) {
        throw OPENRAVE_EXCEPTION_FORMAT0("failed to InitPlan",ORE_Failed);
    }
    if( planner->PlanPath(traj) != PS_HasSolution ) {
        throw OPENRAVE_EXCEPTION_FORMAT0("failed to PlanPath",ORE_Failed);
    }

    if( bsmooth && (RaveGetDebugLevel() & Level_VerifyPlans) ) {
        RobotBase::RobotStateSaver saver(probot);
        planningutils::VerifyTrajectory(params,traj);
    }
}

static void diffstatefn(std::vector<dReal>& q1, const std::vector<dReal>& q2, const std::vector<int>& vrotaxes)
{
    BOOST_ASSERT(q1.size()==q2.size());
    for(size_t i = 0; i < q1.size(); ++i) {
        if( find(vrotaxes.begin(),vrotaxes.end(),i) != vrotaxes.end() ) {
            q1[i] = utils::SubtractCircularAngle(q1[i],q2[i]);
        }
        else {
            q1[i] -= q2[i];
        }
    }
}

static void _SetTransformBody(std::vector<dReal>::const_iterator itvalues, KinBodyPtr pbody, int index, int affinedofs,const Vector& vActvAffineRotationAxis)
{
    Transform t = pbody->GetTransform();
    RaveGetTransformFromAffineDOFValues(t,itvalues+index,affinedofs,vActvAffineRotationAxis);
    pbody->SetTransform(t);
}

static void _GetTransformBody(std::vector<dReal>::iterator itvalues, KinBodyPtr pbody, int index, int affinedofs,const Vector& vActvAffineRotationAxis)
{
    Transform t = pbody->GetTransform();
    RaveGetAffineDOFValuesFromTransform(itvalues+index,t, affinedofs,vActvAffineRotationAxis);
}

void _SetAffineState(const std::vector<dReal>& v, const std::list< boost::function< void(std::vector<dReal>::const_iterator) > >& listsetfunctions)
{
    FOREACHC(itfn,listsetfunctions) {
        (*itfn)(v.begin());
    }
}

void _GetAffineState(std::vector<dReal>& v, const std::list< boost::function< void(std::vector<dReal>::iterator) > >& listgetfunctions)
{
    FOREACHC(itfn,listgetfunctions) {
        (*itfn)(v.begin());
    }
}

class PlannerStateSaver
{
public:
    PlannerStateSaver(int dof, const PlannerBase::PlannerParameters::SetStateFn& setfn, const PlannerBase::PlannerParameters::GetStateFn& getfn) : _setfn(setfn) {
        _savedvalues.resize(dof);
        getfn(_savedvalues);
        BOOST_ASSERT(!!_setfn);
    }
    virtual ~PlannerStateSaver() {
        _setfn(_savedvalues);
    }

private:
    const PlannerBase::PlannerParameters::SetStateFn& _setfn;
    vector<dReal> _savedvalues;
};

// this function is very messed up...?
static void _PlanAffineTrajectory(TrajectoryBasePtr traj, const std::vector<dReal>& maxvelocities, const std::vector<dReal>& maxaccelerations, bool hastimestamps, const std::string& plannername, const std::string& interpolation, bool bsmooth)
{
    if( traj->GetNumWaypoints() == 1 ) {
        // don't need retiming, but should at least add a time group
        ConfigurationSpecification spec = traj->GetConfigurationSpecification();
        spec.AddDeltaTimeGroup();
        vector<dReal> data;
        traj->GetWaypoints(0,traj->GetNumWaypoints(),data,spec);
        traj->Init(spec);
        traj->Insert(0,data);
        return;
    }

    EnvironmentMutex::scoped_lock lockenv(traj->GetEnv()->GetMutex());
    ConfigurationSpecification newspec = traj->GetConfigurationSpecification().GetTimeDerivativeSpecification(0);
    if( newspec.GetDOF() != (int)maxvelocities.size() || newspec.GetDOF() != (int)maxaccelerations.size() ) {
        throw OPENRAVE_EXCEPTION_FORMAT("traj values (%d) do not match maxvelocity size (%d) or maxaccelerations size (%d)",newspec.GetDOF()%maxvelocities.size()%maxaccelerations.size(), ORE_InvalidArguments);
    }
    ConvertTrajectorySpecification(traj,newspec);
    PlannerBasePtr planner = RaveCreatePlanner(traj->GetEnv(),plannername.size() > 0 ? plannername : string("parabolicsmoother"));
    PlannerBase::PlannerParametersPtr params(new PlannerBase::PlannerParameters());
    params->_vConfigVelocityLimit = maxvelocities;
    params->_vConfigAccelerationLimit = maxaccelerations;
    params->_configurationspecification = traj->GetConfigurationSpecification();
    params->_vConfigLowerLimit.resize(traj->GetConfigurationSpecification().GetDOF());
    params->_vConfigUpperLimit.resize(traj->GetConfigurationSpecification().GetDOF());
    params->_vConfigResolution.resize(traj->GetConfigurationSpecification().GetDOF());
    for(size_t i = 0; i < params->_vConfigLowerLimit.size(); ++i) {
        params->_vConfigLowerLimit[i] = -1e6;
        params->_vConfigUpperLimit[i] = 1e6;
        params->_vConfigResolution[i] = 0.01;
    }

    list< boost::function<void(std::vector<dReal>::const_iterator) > > listsetfunctions;
    list< boost::function<void(std::vector<dReal>::iterator) > > listgetfunctions;
    std::vector<int> vrotaxes;
    // analyze the configuration for identified dimensions
    KinBodyPtr robot;
    FOREACHC(itgroup,traj->GetConfigurationSpecification()._vgroups) {
        if( itgroup->name.size() >= 16 && itgroup->name.substr(0,16) == "affine_transform" ) {
            string tempname;
            int affinedofs=0;
            stringstream ss(itgroup->name.substr(16));
            ss >> tempname >> affinedofs;
            BOOST_ASSERT( !!ss );
            KinBodyPtr pbody = traj->GetEnv()->GetKinBody(tempname);
            BOOST_ASSERT( !!pbody );
            Vector vaxis(0,0,1);
            if( affinedofs & DOF_RotationAxis ) {
                vrotaxes.push_back(itgroup->offset+RaveGetIndexFromAffineDOF(affinedofs,DOF_RotationAxis));
                ss >> vaxis.x >> vaxis.y >> vaxis.z;
            }
            robot = pbody;
            listsetfunctions.push_back(boost::bind(_SetTransformBody,_1,pbody,itgroup->offset,affinedofs,vaxis));
            listgetfunctions.push_back(boost::bind(_GetTransformBody,_1,pbody,itgroup->offset,affinedofs,vaxis));
        }
        else if( itgroup->name.size() >= 14 && itgroup->name.substr(0,14) == "ikparam_values" ) {
            int iktypeint = 0;
            stringstream ss(itgroup->name.substr(14));
            ss >> iktypeint;
            if( !!ss ) {
                IkParameterizationType iktype=static_cast<IkParameterizationType>(iktypeint);
                switch(iktype) {
                case IKP_TranslationXYOrientation3D: vrotaxes.push_back(itgroup->offset+2); break;
                default:
                    break;
                }
            }
            if( bsmooth ) {
                RAVELOG_VERBOSE("cannot smooth state for IK configurations\n");
            }
        }
    }

    boost::shared_ptr<PlannerStateSaver> statesaver;
    if( bsmooth ) {
        if( listsetfunctions.size() > 0 ) {
            params->_setstatefn = boost::bind(_SetAffineState,_1,boost::ref(listsetfunctions));
            params->_getstatefn = boost::bind(_GetAffineState,_1,boost::ref(listgetfunctions));
            boost::shared_ptr<LineCollisionConstraint> pcollision(new LineCollisionConstraint());
            params->_checkpathconstraintsfn = boost::bind(&LineCollisionConstraint::Check,pcollision,PlannerBase::PlannerParametersWeakPtr(params), robot, _1, _2, _3, _4);
            statesaver.reset(new PlannerStateSaver(traj->GetConfigurationSpecification().GetDOF(), params->_setstatefn, params->_getstatefn));
        }
    }
    else {
        params->_setstatefn.clear();
        params->_getstatefn.clear();
        params->_checkpathconstraintsfn.clear();
    }

    params->_diffstatefn = boost::bind(diffstatefn,_1,_2,boost::ref(vrotaxes));

    //params->_distmetricfn;
    params->_sExtraParameters += str(boost::format("<hastimestamps>%d</hastimestamps>")%hastimestamps);
    if( interpolation.size() > 0 ) {
        params->_sExtraParameters += str(boost::format("<interpolation>%s</interpolation>")%interpolation);
    }
    if( !planner->InitPlan(RobotBasePtr(),params) ) {
        throw OPENRAVE_EXCEPTION_FORMAT0("failed to InitPlan",ORE_Failed);
    }
    if( planner->PlanPath(traj) != PS_HasSolution ) {
        throw OPENRAVE_EXCEPTION_FORMAT0("failed to PlanPath",ORE_Failed);
    }
}

void SmoothActiveDOFTrajectory(TrajectoryBasePtr traj, RobotBasePtr robot, bool hastimestamps, dReal fmaxvelmult, const std::string& plannername)
{
    _PlanActiveDOFTrajectory(traj,robot,hastimestamps,fmaxvelmult,plannername.size() > 0 ? plannername : "parabolicsmoother", "", true);
}

void SmoothAffineTrajectory(TrajectoryBasePtr traj, const std::vector<dReal>& maxvelocities, const std::vector<dReal>& maxaccelerations, bool hastimestamps, const std::string& plannername)
{
    _PlanAffineTrajectory(traj, maxvelocities, maxaccelerations, hastimestamps, plannername.size() > 0 ? plannername : "parabolicsmoother", "", true);
}

void RetimeActiveDOFTrajectory(TrajectoryBasePtr traj, RobotBasePtr robot, bool hastimestamps, dReal fmaxvelmult, const std::string& plannername)
{
    std::string newplannername = plannername;
    std::string interpolation;
    if( newplannername.size() == 0 ) {
        // check out the trajectory interpolation values and take it from there
        FOREACHC(itgroup,traj->GetConfigurationSpecification()._vgroups) {
            if( itgroup->name.size() >= 12 && itgroup->name.substr(0,12) == "joint_values" ) {
                interpolation = itgroup->interpolation;
                break;
            }
            else if( itgroup->name.size() >= 16 && itgroup->name.substr(0,16) == "affine_transform" ) {
                interpolation = itgroup->interpolation;
            }
        }

        if( interpolation == "quadratic" ) {
            newplannername = "parabolictrajectoryretimer";
        }
        else if( interpolation == "linear" ) {
            newplannername = "lineartrajectoryretimer";
        }
        else {
            newplannername = "lineartrajectoryretimer";
        }
    }
    _PlanActiveDOFTrajectory(traj,robot,hastimestamps,fmaxvelmult,newplannername, interpolation, false);
}

void RetimeAffineTrajectory(TrajectoryBasePtr traj, const std::vector<dReal>& maxvelocities, const std::vector<dReal>& maxaccelerations, bool hastimestamps, const std::string& plannername)
{
    _PlanAffineTrajectory(traj, maxvelocities, maxaccelerations, hastimestamps, plannername.size() > 0 ? plannername : "lineartrajectoryretimer", "", false);
}

void InsertActiveDOFWaypointWithRetiming(int waypointindex, const std::vector<dReal>& dofvalues, const std::vector<dReal>& dofvelocities, TrajectoryBasePtr traj, RobotBasePtr robot, dReal fmaxvelmult, const std::string& plannername)
{
    BOOST_ASSERT((int)dofvalues.size()==robot->GetActiveDOF());
    BOOST_ASSERT(traj->GetEnv()==robot->GetEnv());
    vector<dReal> v1pos(robot->GetActiveDOF(),0), v1vel(robot->GetActiveDOF(),0);
    ConfigurationSpecification newspec = robot->GetActiveConfigurationSpecification();

    string interpolation = "";
    FOREACH(it,newspec._vgroups) {
        std::vector<ConfigurationSpecification::Group>::const_iterator itgroup = traj->GetConfigurationSpecification().FindCompatibleGroup(*it, false);
        if( itgroup == traj->GetConfigurationSpecification()._vgroups.end() ) {
            throw OPENRAVE_EXCEPTION_FORMAT("could not find group %s in trajectory",newspec._vgroups.at(0).name,ORE_InvalidArguments);
        }
        if( itgroup->interpolation.size() > 0 ) {
            it->interpolation = itgroup->interpolation;
            interpolation = itgroup->interpolation;
        }
    }
    newspec.AddVelocityGroups(false);

    vector<dReal> vwaypointstart, vwaypointend, vtargetvalues;
    if( waypointindex == 0 ) {
        vwaypointstart.resize(newspec.GetDOF());
        ConfigurationSpecification::ConvertData(vwaypointstart.begin(), newspec, dofvalues.begin(), robot->GetActiveConfigurationSpecification(), 1, traj->GetEnv(), true);
        if( dofvalues.size() == dofvelocities.size() ) {
            ConfigurationSpecification::ConvertData(vwaypointstart.begin(), newspec.ConvertToVelocitySpecification(), dofvelocities.begin(), robot->GetActiveConfigurationSpecification().ConvertToVelocitySpecification(), 1, traj->GetEnv(), false);
        }
        traj->GetWaypoint(0,vwaypointend, newspec);
        traj->GetWaypoint(0,vtargetvalues); // in target spec
    }
    else if( waypointindex == (int)traj->GetNumWaypoints() ) {
        traj->GetWaypoint(waypointindex-1,vtargetvalues); // in target spec
        traj->GetWaypoint(waypointindex-1,vwaypointstart, newspec);

        vwaypointend.resize(newspec.GetDOF());
        ConfigurationSpecification::ConvertData(vwaypointend.begin(), newspec, dofvalues.begin(), robot->GetActiveConfigurationSpecification(), 1, traj->GetEnv(), true);
        if( dofvalues.size() == dofvelocities.size() ) {
            ConfigurationSpecification::ConvertData(vwaypointend.begin(), newspec.ConvertToVelocitySpecification(), dofvelocities.begin(), robot->GetActiveConfigurationSpecification().ConvertToVelocitySpecification(), 1, traj->GetEnv(), false);
        }
    }
    else {
        throw OPENRAVE_EXCEPTION_FORMAT0("do no support inserting waypoints in middle of trajectories yet",ORE_InvalidArguments);
    }

    TrajectoryBasePtr trajinitial = RaveCreateTrajectory(traj->GetEnv(),traj->GetXMLId());
    trajinitial->Init(newspec);
    trajinitial->Insert(0,vwaypointstart);
    trajinitial->Insert(1,vwaypointend);
    std::string newplannername = plannername;
    if( newplannername.size() == 0 ) {
        if( interpolation == "linear" ) {
            newplannername = "lineartrajectoryretimer";
        }
        else if( interpolation.size() == 0 || interpolation == "quadratic" ) {
            newplannername = "parabolictrajectoryretimer";
        }
        else {
            throw OPENRAVE_EXCEPTION_FORMAT("currently do not support retiming for %s interpolations",interpolation,ORE_InvalidArguments);
        }
    }
    RetimeActiveDOFTrajectory(trajinitial,robot,false,fmaxvelmult,newplannername);

    // retiming is done, now merge the two trajectories
    size_t targetdof = vtargetvalues.size();
    vtargetvalues.resize(targetdof*trajinitial->GetNumWaypoints());
    for(size_t i = targetdof; i < vtargetvalues.size(); i += targetdof) {
        std::copy(vtargetvalues.begin(),vtargetvalues.begin()+targetdof,vtargetvalues.begin()+i);
    }
    trajinitial->GetWaypoints(0,trajinitial->GetNumWaypoints(),vwaypointstart);

    // copy to the target values while preserving other data
    ConfigurationSpecification::ConvertData(vtargetvalues.begin(), traj->GetConfigurationSpecification(), vwaypointstart.begin(), trajinitial->GetConfigurationSpecification(), trajinitial->GetNumWaypoints(), traj->GetEnv(), false);

    if( waypointindex == 0 ) {
        // have to insert the first N-1 and overwrite the Nth
        vwaypointstart.resize(targetdof);
        std::copy(vtargetvalues.begin()+vtargetvalues.size()-targetdof,vtargetvalues.end(),vwaypointstart.begin());
        traj->Insert(waypointindex,vwaypointstart,true);
        vtargetvalues.resize(vtargetvalues.size()-targetdof);
        if( vtargetvalues.size() > 0 ) {
            traj->Insert(waypointindex,vtargetvalues,false);
        }
    }
    else {
        // overwrite 1st and insert last N-1
        vwaypointstart.resize(targetdof);
        std::copy(vtargetvalues.begin(),vtargetvalues.begin()+targetdof,vwaypointstart.begin());
        traj->Insert(waypointindex-1,vwaypointstart,true);
        vtargetvalues.erase(vtargetvalues.begin(), vtargetvalues.begin()+targetdof);
        if( vtargetvalues.size() > 0 ) {
            traj->Insert(waypointindex,vtargetvalues,false);
        }
    }
}

void ConvertTrajectorySpecification(TrajectoryBasePtr traj, const ConfigurationSpecification& spec)
{
    if( traj->GetConfigurationSpecification() != spec ) {
        size_t numpoints = traj->GetConfigurationSpecification().GetDOF() > 0 ? traj->GetNumWaypoints() : 0;
        vector<dReal> data;
        if( numpoints > 0 ) {
            traj->GetWaypoints(0,numpoints,data,spec);
        }
        traj->Init(spec);
        if( numpoints > 0 ) {
            traj->Insert(0,data);
        }
    }
}

TrajectoryBasePtr ReverseTrajectory(TrajectoryBaseConstPtr sourcetraj)
{
    vector<dReal> sourcedata;
    size_t numpoints = sourcetraj->GetNumWaypoints();
    int dof = sourcetraj->GetConfigurationSpecification().GetDOF();
    vector<uint8_t> velocitydofs(dof,0);
    int timeoffset = -1;
    vector<uint8_t> velocitynextinterp(dof,0);
    FOREACHC(itgroup, sourcetraj->GetConfigurationSpecification()._vgroups) {
        if( itgroup->name.find("_velocities") != string::npos ) {
            bool bnext = itgroup->interpolation == "next";
            for(int i = 0; i < itgroup->dof; ++i) {
                velocitydofs.at(itgroup->offset+i) = 1;
                velocitynextinterp.at(itgroup->offset+i) = bnext;
            }
        }
        else if( itgroup->name == "deltatime" ) {
            timeoffset = itgroup->offset;
        }
    }
    sourcetraj->GetWaypoints(0,numpoints,sourcedata);
    vector<dReal> targetdata(sourcedata.size());
    dReal prevdeltatime = 0;
    for(size_t i = 0; i < numpoints; ++i) {
        vector<dReal>::iterator ittarget = targetdata.begin()+i*dof;
        vector<dReal>::iterator itsource = sourcedata.begin()+(numpoints-i-1)*dof;
        for(int j = 0; j < dof; ++j) {
            if( velocitydofs[j] ) {
                if( velocitynextinterp[j] ) {
                    if( i < numpoints-1 ) {
                        *(ittarget+j+dof) = -*(itsource+j);
                    }
                    else {
                        targetdata.at(j) = 0;
                    }
                }
                else {
                    *(ittarget+j) = -*(itsource+j);
                }
            }
            else {
                *(ittarget+j) = *(itsource+j);
            }
        }

        if( timeoffset >= 0 ) {
            *(ittarget+timeoffset) = prevdeltatime;
            prevdeltatime = *(itsource+timeoffset);
        }
    }

    TrajectoryBasePtr traj = RaveCreateTrajectory(sourcetraj->GetEnv(),sourcetraj->GetXMLId());
    traj->Init(sourcetraj->GetConfigurationSpecification());
    traj->Insert(0,targetdata);
    return traj;
}

TrajectoryBasePtr MergeTrajectories(const std::list<TrajectoryBaseConstPtr>& listtrajectories)
{
    TrajectoryBasePtr presulttraj;
    if( listtrajectories.size() == 0 ) {
        return presulttraj;
    }
    if( listtrajectories.size() == 1 ) {
        presulttraj = RaveCreateTrajectory(listtrajectories.front()->GetEnv(),listtrajectories.front()->GetXMLId());
        presulttraj->Clone(listtrajectories.front(),0);
        return presulttraj;
    }

    ConfigurationSpecification spec;
    vector<dReal> vpointdata;
    vector<dReal> vtimes; vtimes.reserve(listtrajectories.front()->GetNumWaypoints());
    int totaldof = 1; // for delta time
    FOREACHC(ittraj,listtrajectories) {
        const ConfigurationSpecification& trajspec = (*ittraj)->GetConfigurationSpecification();
        ConfigurationSpecification::Group gtime = trajspec.GetGroupFromName("deltatime");
        spec += trajspec;
        totaldof += trajspec.GetDOF()-1;
        if( trajspec.FindCompatibleGroup("iswaypoint",true) != trajspec._vgroups.end() ) {
            totaldof -= 1;
        }
        dReal curtime = 0;
        for(size_t ipoint = 0; ipoint < (*ittraj)->GetNumWaypoints(); ++ipoint) {
            (*ittraj)->GetWaypoint(ipoint,vpointdata);
            curtime += vpointdata.at(gtime.offset);
            vector<dReal>::iterator it = lower_bound(vtimes.begin(),vtimes.end(),curtime);
            if( *it != curtime ) {
                vtimes.insert(it,curtime);
            }
        }
    }

    vector<ConfigurationSpecification::Group>::const_iterator itwaypointgroup = spec.FindCompatibleGroup("iswaypoint",true);
    vector<dReal> vwaypoints;
    if( itwaypointgroup != spec._vgroups.end() ) {
        totaldof += 1;
        vwaypoints.resize(vtimes.size(),0);
    }

    if( totaldof != spec.GetDOF() ) {
        throw OPENRAVE_EXCEPTION_FORMAT("merged configuration needs to have %d DOF, currently has %d",totaldof%spec.GetDOF(),ORE_InvalidArguments);
    }
    presulttraj = RaveCreateTrajectory(listtrajectories.front()->GetEnv(),listtrajectories.front()->GetXMLId());
    presulttraj->Init(spec);

    if( vtimes.size() == 0 ) {
        return presulttraj;
    }

    // need to find all waypoints
    vector<dReal> vtemp, vnewdata;

    int deltatimeoffset = spec.GetGroupFromName("deltatime").offset;
    FOREACHC(ittraj,listtrajectories) {
        vector<ConfigurationSpecification::Group>::const_iterator itwaypointgrouptraj = (*ittraj)->GetConfigurationSpecification().FindCompatibleGroup("iswaypoint",true);
        int waypointoffset = -1;
        if( itwaypointgrouptraj != (*ittraj)->GetConfigurationSpecification()._vgroups.end() ) {
            waypointoffset = itwaypointgrouptraj->offset;
        }
        if( vnewdata.size() == 0 ) {
            vnewdata.reserve(vtimes.size()*spec.GetDOF());
            for(size_t i = 0; i < vtimes.size(); ++i) {
                (*ittraj)->Sample(vtemp,vtimes[i],spec);
                vnewdata.insert(vnewdata.end(),vtemp.begin(),vtemp.end());
                if( waypointoffset >= 0 ) {
                    vwaypoints[i] += vtemp[waypointoffset];
                }
            }
        }
        else {
            vpointdata.resize(0);
            for(size_t i = 0; i < vtimes.size(); ++i) {
                (*ittraj)->Sample(vtemp,vtimes[i]);
                vpointdata.insert(vpointdata.end(),vtemp.begin(),vtemp.end());
                if( waypointoffset >= 0 ) {
                    vwaypoints[i] += vtemp[waypointoffset];
                }
            }
            ConfigurationSpecification::ConvertData(vnewdata.begin(),spec,vpointdata.begin(),(*ittraj)->GetConfigurationSpecification(),vtimes.size(),presulttraj->GetEnv(),false);
        }
    }

    vnewdata.at(deltatimeoffset) = vtimes[0];
    for(size_t i = 1; i < vtimes.size(); ++i) {
        vnewdata.at(i*spec.GetDOF()+deltatimeoffset) = vtimes[i]-vtimes[i-1];
    }
    if( itwaypointgroup != spec._vgroups.end() ) {
        vnewdata.at(itwaypointgroup->offset) = vwaypoints[0];
        for(size_t i = 1; i < vtimes.size(); ++i) {
            vnewdata.at(i*spec.GetDOF()+itwaypointgroup->offset) = vwaypoints[i];
        }
    }
    presulttraj->Insert(0,vnewdata);
    return presulttraj;
}

void SetPlannerParametersFromSpecification(PlannerBase::PlannerParametersPtr parameters, const ConfigurationSpecification& spec)
{
    throw OPENRAVE_EXCEPTION_FORMAT0("SetPlannerParametersFromSpecification not implemented",ORE_NotImplemented);
}

LineCollisionConstraint::LineCollisionConstraint()
{
    _report.reset(new CollisionReport());
}

bool LineCollisionConstraint::Check(PlannerBase::PlannerParametersWeakPtr _params, KinBodyPtr robot, const std::vector<dReal>& pQ0, const std::vector<dReal>& pQ1, IntervalType interval, PlannerBase::ConfigurationListPtr pvCheckedConfigurations)
{
    // set the bounds based on the interval type
    PlannerBase::PlannerParametersPtr params = _params.lock();
    if( !params ) {
        return false;
    }
    int start=0;
    bool bCheckEnd=false;
    switch (interval) {
    case IT_Open:
        start = 1;  bCheckEnd = false;
        break;
    case IT_OpenStart:
        start = 1;  bCheckEnd = true;
        break;
    case IT_OpenEnd:
        start = 0;  bCheckEnd = false;
        break;
    case IT_Closed:
        start = 0;  bCheckEnd = true;
        break;
    default:
        BOOST_ASSERT(0);
    }

    // first make sure the end is free
    _vtempconfig.resize(params->GetDOF());
    if (bCheckEnd) {
        params->_setstatefn(pQ1);
        if (robot->GetEnv()->CheckCollision(KinBodyConstPtr(robot),_report) ) {
            return false;
        }
        if( robot->CheckSelfCollision(_report) ) {
            return false;
        }
    }

    // compute  the discretization
    dQ = pQ1;
    params->_diffstatefn(dQ,pQ0);
    int i, numSteps = 1;
    std::vector<dReal>::const_iterator itres = params->_vConfigResolution.begin();
    BOOST_ASSERT((int)params->_vConfigResolution.size()==params->GetDOF());
    int totalsteps = 0;
    for (i = 0; i < params->GetDOF(); i++,itres++) {
        int steps;
        if( *itres != 0 ) {
            steps = (int)(fabs(dQ[i]) / *itres);
        }
        else {
            steps = (int)(fabs(dQ[i]) * 100);
        }
        totalsteps += steps;
        if (steps > numSteps) {
            numSteps = steps;
        }
    }
    if((totalsteps == 0)&&(start > 0)) {
        return true;
    }

    if (start == 0 ) {
        params->_setstatefn(pQ0);
        if (robot->GetEnv()->CheckCollision(KinBodyConstPtr(robot),_report) ) {
            return false;
        }
        if( robot->CheckSelfCollision(_report) ) {
            return false;
        }
        start = 1;
    }

    dReal fisteps = dReal(1.0f)/numSteps;
    for(std::vector<dReal>::iterator it = dQ.begin(); it != dQ.end(); ++it) {
        *it *= fisteps;
    }

    // check for collision along the straight-line path
    // NOTE: this does not check the end config, and may or may
    // not check the start based on the value of 'start'
    for (i = 0; i < params->GetDOF(); i++) {
        _vtempconfig[i] = pQ0[i];
    }
    if( start > 0 ) {
        params->_setstatefn(_vtempconfig);
        if( !params->_neighstatefn(_vtempconfig, dQ,0) ) {
            return false;
        }
    }
    for (int f = start; f < numSteps; f++) {
        params->_setstatefn(_vtempconfig);
        if( robot->GetEnv()->CheckCollision(KinBodyConstPtr(robot)) ) {
            return false;
        }
        if( robot->CheckSelfCollision() ) {
            return false;
        }
        if( !!params->_getstatefn ) {
            params->_getstatefn(_vtempconfig);     // query again in order to get normalizations/joint limits
        }
        if( !!pvCheckedConfigurations ) {
            pvCheckedConfigurations->push_back(_vtempconfig);
        }
        if( !params->_neighstatefn(_vtempconfig,dQ,0) ) {
            return false;
        }
    }

    if( bCheckEnd && !!pvCheckedConfigurations ) {
        pvCheckedConfigurations->push_back(pQ1);
    }
    return true;
}

SimpleDistanceMetric::SimpleDistanceMetric(RobotBasePtr robot) : _robot(robot)
{
    _robot->GetActiveDOFWeights(weights2);
    for(std::vector<dReal>::iterator it = weights2.begin(); it != weights2.end(); ++it) {
        *it *= *it;
    }
}

dReal SimpleDistanceMetric::Eval(const std::vector<dReal>& c0, const std::vector<dReal>& c1)
{
    std::vector<dReal> c = c0;
    _robot->SubtractActiveDOFValues(c,c1);
    dReal dist = 0;
    for(int i=0; i < _robot->GetActiveDOF(); i++) {
        dist += weights2.at(i)*c.at(i)*c.at(i);
    }
    return RaveSqrt(dist);
}

SimpleNeighborhoodSampler::SimpleNeighborhoodSampler(SpaceSamplerBasePtr psampler, const boost::function<dReal(const std::vector<dReal>&, const std::vector<dReal>&)>& distmetricfn) : _psampler(psampler), _distmetricfn(distmetricfn)
{
}

bool SimpleNeighborhoodSampler::Sample(std::vector<dReal>& vNewSample, const std::vector<dReal>& vCurSample, dReal fRadius)
{
    _psampler->SampleSequence(vNewSample);
    size_t dof = vCurSample.size();
    BOOST_ASSERT(dof==vNewSample.size() && &vNewSample != &vCurSample);
    dReal fDist = _distmetricfn(vNewSample,vCurSample);
    while(fDist > fRadius) {
        for (size_t i = 0; i < dof; i++) {
            vNewSample[i] = 0.5f*vCurSample[i]+0.5f*vNewSample[i];
        }
        fDist = _distmetricfn(vNewSample,vCurSample);
    }
    for(int iter = 0; iter < 20; ++iter) {
        for (size_t i = 0; i < dof; i++) {
            vNewSample[i] = 1.2f*vNewSample[i]-0.2f*vCurSample[i];
        }
        if(_distmetricfn(vNewSample, vCurSample) > fRadius ) {
            // take the previous
            for (size_t i = 0; i < dof; i++) {
                vNewSample[i] = 0.833333333333333*vNewSample[i]-0.16666666666666669*vCurSample[i];
            }
            break;
        }
    }

    //        vNewSample.resize(lower.size());
    //        for (size_t i = 0; i < dof; i++) {
    //            if( sample[i] < lower[i] ) {
    //                vNewSample[i] = lower[i];
    //            }
    //            else if( sample[i] > upper[i] ) {
    //                vNewSample[i] = upper[i];
    //            }
    //            else {
    //                vNewSample[i] = sample[i];
    //            }
    //        }

    return true;
}

bool SimpleNeighborhoodSampler::Sample(std::vector<dReal>& samples)
{
    _psampler->SampleSequence(samples,1,IT_Closed);
    return samples.size()>0;
}

ManipulatorIKGoalSampler::ManipulatorIKGoalSampler(RobotBase::ManipulatorConstPtr pmanip, const std::list<IkParameterization>& listparameterizations, int nummaxsamples, int nummaxtries, dReal fsampleprob) : _pmanip(pmanip), _nummaxsamples(nummaxsamples), _nummaxtries(nummaxtries), _fsampleprob(fsampleprob)
{
    _fjittermaxdist = 0;
    _probot = _pmanip->GetRobot();
    _pindexsampler = RaveCreateSpaceSampler(_probot->GetEnv(),"mt19937");
    int orgindex = 0;
    for(std::list<IkParameterization>::const_iterator it = listparameterizations.begin(); it != listparameterizations.end(); ++it) {
        SampleInfo s;
        s._orgindex = orgindex++;
        s._ikparam = *it;
        s._numleft = _nummaxsamples;
        _listsamples.push_back(s);
    }
    _report.reset(new CollisionReport());
    pmanip->GetIkSolver()->GetFreeParameters(_vfreestart);
    // the halton sequence centers around 0.5, so make it center around vfreestart
    for(std::vector<dReal>::iterator it = _vfreestart.begin(); it != _vfreestart.end(); ++it) {
        *it -= 0.5;
    }
}

bool ManipulatorIKGoalSampler::Sample(std::vector<dReal>& vgoal)
{
    vgoal.resize(0);
    std::vector<dReal> vindex;
    _pindexsampler->SampleSequence(vindex,1,IT_OpenEnd);
    if( vindex.at(0) > _fsampleprob ) {
        return false;
    }
    if( _viksolutions.size() > 0 ) {
        vgoal = _viksolutions.back();
        _viksolutions.pop_back();
        return true;
    }

    for(int itry = 0; itry < _nummaxtries; ++itry ) {
        if( _listsamples.size() == 0 ) {
            return false;
        }
        _pindexsampler->SampleSequence(vindex,1,IT_OpenEnd);
        int isampleindex = (int)(vindex.at(0)*_listsamples.size());
        std::list<SampleInfo>::iterator itsample = _listsamples.begin();
        advance(itsample,isampleindex);

        bool bCheckEndEffector = itsample->_ikparam.GetType() == IKP_Transform6D;
        // if first grasp, quickly prune grasp is end effector is in collision
        IkParameterization ikparam = itsample->_ikparam;
        if( itsample->_numleft == _nummaxsamples && bCheckEndEffector ) {
            if( _pmanip->CheckEndEffectorCollision(ikparam.GetTransform6D(),_report) ) {
                bool bcollision=true;
                if( _fjittermaxdist > 0 ) {
                    // try jittering the end effector out
                    RAVELOG_VERBOSE("starting jitter transform...\n");

                    // randomly add small offset to the ik until it stops being in collision
                    Transform transorig = ikparam.GetTransform6D();
                    Transform transnew = transorig;
                    int iiter = 0;
                    int nMaxIterations = 100;
                    std::vector<dReal> xyzsamples(3);
                    for(iiter = 0; iiter < nMaxIterations; ++iiter) {
                        _pindexsampler->SampleSequence(xyzsamples,3,IT_Closed);
                        transnew.trans = transorig.trans + _fjittermaxdist * Vector(xyzsamples[0]-0.5f, xyzsamples[1]-0.5f, xyzsamples[2]-0.5f);
                        if( !_pmanip->CheckEndEffectorCollision(transnew,_report) ) {
                            ikparam.SetTransform6D(transnew);
                            bcollision = false;
                            break;
                        }
                    }
                }
                if( bcollision ) {
                    RAVELOG_VERBOSE(str(boost::format("sampleiksolutions gripper in collision: %s.\n")%_report->__str__()));
                    _listsamples.erase(itsample);
                    continue;
                }
            }
        }

        std::vector<dReal> vfree;
        int orgindex = itsample->_orgindex;
        if( _pmanip->GetIkSolver()->GetNumFreeParameters() > 0 ) {
            if( !itsample->_psampler ) {
                itsample->_psampler = RaveCreateSpaceSampler(_probot->GetEnv(),"halton");
                itsample->_psampler->SetSpaceDOF(_pmanip->GetIkSolver()->GetNumFreeParameters());
            }
            itsample->_psampler->SampleSequence(vfree,1);
            for(size_t i = 0; i < _vfreestart.size(); ++i) {
                vfree.at(i) += _vfreestart[i];
                if( vfree[i] < 0 ) {
                    vfree[i] += 1;
                }
                if( vfree[i] > 1 ) {
                    vfree[i] -= 1;
                }
            }
        }
        bool bsuccess = _pmanip->FindIKSolutions(ikparam, vfree, _viksolutions, IKFO_CheckEnvCollisions|(bCheckEndEffector ? IKFO_IgnoreEndEffectorCollisions : 0));
        if( --itsample->_numleft <= 0 || vfree.size() == 0 ) {
            _listsamples.erase(itsample);
        }

        if( bsuccess ) {
            for(size_t j = 0; j < _viksolutions.size(); ++j) {
                _listreturnedsamples.push_back(orgindex);
            }
            vgoal = _viksolutions.back();
            _viksolutions.pop_back();
            return true;
        }
    }
    return false;
}

int ManipulatorIKGoalSampler::GetIkParameterizationIndex(int index)
{
    BOOST_ASSERT(index >= 0 && index < (int)_listreturnedsamples.size());
    std::list<int>::iterator it = _listreturnedsamples.begin();
    advance(it,index);
    return *it;
}

void ManipulatorIKGoalSampler::SetSamplingProb(dReal fsampleprob)
{
    _fsampleprob = fsampleprob;
}

void ManipulatorIKGoalSampler::SetJitter(dReal maxdist)
{
    _fjittermaxdist = maxdist;
}

} // planningutils
} // OpenRAVE
