/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2014, Rice University
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
*   * Neither the name of the Rice University nor the names of its
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
*********************************************************************/

/* Author: Caleb Voss */

#include "ompl/base/spaces/AtlasStateSpace.h"

#include "ompl/base/PlannerDataGraph.h"
#include "ompl/base/SpaceInformation.h"
#include "ompl/base/spaces/AtlasChart.h"
#include "ompl/util/Exception.h"

#include <boost/graph/iteration_macros.hpp>

#include <eigen3/Eigen/Core>
#include <eigen3/Eigen/Geometry>

/// AtlasStateSampler

/// Public

ompl::base::AtlasStateSampler::AtlasStateSampler(const SpaceInformation *si)
  : StateSampler(si->getStateSpace().get()), atlas_(*si->getStateSpace()->as<AtlasStateSpace>())
{
    AtlasStateSpace::checkSpace(si);
}

ompl::base::AtlasStateSampler::AtlasStateSampler(const AtlasStateSpace &atlas) : StateSampler(&atlas), atlas_(atlas)
{
}

void ompl::base::AtlasStateSampler::sampleUniform(State *state)
{
    Eigen::Ref<Eigen::VectorXd> rx = state->as<AtlasStateSpace::StateType>()->vectorView();
    Eigen::VectorXd ry(atlas_.getAmbientDimension());
    Eigen::VectorXd ru(atlas_.getManifoldDimension());
    AtlasChart *c;

    // Sampling a point on the manifold.
    int tries = 100;
    do
    {
        // Rejection sampling to find a point inside a chart's polytope.
        do
        {
            // Pick a chart.
            c = atlas_.sampleChart();

            // Sample a point within rho_s of the center. This is done by
            // sampling uniformly on the surface and multiplying by a distance
            // whose distribution is biased according to spherical volume.
            for (int i = 0; i < ru.size(); i++)
                ru[i] = rng_.gaussian01();
            ru *= atlas_.getRho_s() * std::pow(rng_.uniform01(), 1.0 / ru.size()) / ru.norm();
        } while (tries-- > 0 && !c->inPolytope(ru));

        // Project. Will need to try again if this fails.
    } while (tries > 0 && !c->psi(ru, rx));

    if (tries == 0)
    {
        // Consider decreasing rho and/or the exploration paramter if this
        // becomes a problem.
        OMPL_WARN("ompl::base::AtlasStateSpace::sampleUniform(): "
                  "Took too long; returning center of a random chart.");
        rx = c->getXorigin();
    }

    // Extend polytope of neighboring chart wherever point is near the border.
    c->psiInverse(rx, ru);
    c->borderCheck(ru);
    state->as<AtlasStateSpace::StateType>()->setChart(atlas_.owningChart(rx));
}

void ompl::base::AtlasStateSampler::sampleUniformNear(State *state, const State *near, const double distance)
{
    // Find the chart that the starting point is on.
    AtlasStateSpace::StateType *astate = state->as<AtlasStateSpace::StateType>();
    const AtlasStateSpace::StateType *anear = near->as<AtlasStateSpace::StateType>();

    Eigen::Ref<const Eigen::VectorXd> n = anear->constVectorView();
    Eigen::VectorXd rx(atlas_.getAmbientDimension()), ru(atlas_.getManifoldDimension());

    AtlasChart *c = atlas_.getChart(anear);
    if (c == nullptr)
    {
        OMPL_ERROR("ompl::base::AtlasStateSpace::sampleUniformNear(): "
                   "Sampling failed because chart creation failed! Falling back to uniform sample.");
        sampleUniform(state);
        return;
    }

    // Sample a point from the starting chart.
    c->psiInverse(n, ru);

    int tries = 100;
    Eigen::VectorXd uoffset(atlas_.getManifoldDimension());

    // TODO: Is this a hack or is this theoretically sound? Find out more after the break.
    const double distanceClamped = std::min(distance, atlas_.getRho_s());
    do
    {
        // Sample within distance
        for (int i = 0; i < uoffset.size(); ++i)
            uoffset[i] = ru[i] + rng_.gaussian01();

        uoffset *= distanceClamped * std::pow(rng_.uniform01(), 1.0 / uoffset.size()) / uoffset.norm();
    } while (--tries > 0 && !c->psi(uoffset, rx));  // Try again if we can't project.

    if (tries == 0)
    {
        // Consider decreasing the distance argument if this becomes a
        // problem. Check planner code to see how it gets chosen.
        OMPL_WARN("ompl::base:::AtlasStateSpace::sampleUniformNear(): "
                  "Took too long; returning initial point.");
        rx = n;
    }

    // Be lazy about determining the new chart if we are not in the old one
    if (c->psiInverse(rx, ru), !c->inPolytope(ru))
        c = nullptr;
    else
        c->borderCheck(ru);

    astate->setRealState(rx, c);
}

void ompl::base::AtlasStateSampler::sampleGaussian(State *state, const State *mean, const double stdDev)
{
    AtlasStateSpace::StateType *astate = state->as<AtlasStateSpace::StateType>();
    const AtlasStateSpace::StateType *amean = mean->as<AtlasStateSpace::StateType>();

    Eigen::Ref<const Eigen::VectorXd> m = amean->constVectorView();
    const std::size_t k = atlas_.getManifoldDimension();
    Eigen::VectorXd rx(atlas_.getAmbientDimension()), ru(k);

    AtlasChart *c = atlas_.getChart(amean);
    if (c == nullptr)
    {
        OMPL_ERROR("ompl::base::AtlasStateSpace::sampleGaussian(): "
                   "Sampling failed because chart creation failed! Falling back to uniform sample.");
        sampleUniform(state);
        return;
    }

    c->psiInverse(m, ru);

    // Sample a point in a normal distribution on the starting chart.
    int tries = 100;
    Eigen::VectorXd rand(k);

    const double stdDevClamped = std::min(stdDev, atlas_.getRho_s());
    do
    {
        const double s = stdDevClamped / std::sqrt(k);
        for (std::size_t i = 0; i < k; i++)
            rand[i] = ru[i] + rng_.gaussian(0, s);
    } while (--tries > 0 && !c->psi(rand, rx));  // Try again if we can't project.

    if (tries == 0)
    {
        OMPL_WARN("ompl::base::AtlasStateSpace::sampleUniforGaussian(): "
                  "Took too long; returning initial point.");
        rx = m;
    }

    // Be lazy about determining the new chart if we are not in the old one
    if (c->psiInverse(rx, ru), !c->inPolytope(ru))
        c = nullptr;
    else
        c->borderCheck(ru);

    astate->setRealState(rx, c);
}

/// AtlasValidStateSampler

/// Public

ompl::base::AtlasValidStateSampler::AtlasValidStateSampler(const SpaceInformation *si)
  : ValidStateSampler(si), sampler_(si)
{
    AtlasStateSpace::checkSpace(si);
}

bool ompl::base::AtlasValidStateSampler::sample(State *state)
{
    // Rejection sample for at most attempts_ tries.
    unsigned int tries = 0;
    bool valid;
    do
        sampler_.sampleUniform(state);
    while (!(valid = si_->isValid(state)) && ++tries < attempts_);

    return valid;
}

bool ompl::base::AtlasValidStateSampler::sampleNear(State *state, const State *near, const double distance)
{
    // Rejection sample for at most attempts_ tries.
    unsigned int tries = 0;
    bool valid;
    do
        sampler_.sampleUniformNear(state, near, distance);
    while (!(valid = si_->isValid(state)) && ++tries < attempts_);

    return valid;
}

/// AtlasStateSpace

/// Public

ompl::base::AtlasStateSpace::AtlasStateSpace(const StateSpace *ambientSpace, const Constraint *constraint)
  : ConstrainedStateSpace(ambientSpace, constraint)
  , epsilon_(0.1)
  , exploration_(0.5)
  , lambda_(2)
  , separate_(true)
  , maxChartsPerExtension_(200)
{
    setName("Atlas" + ss_->getName());

    setRho(0.1);
    setAlpha(M_PI / 16);

    chartNN_.setDistanceFunction(&chartNNDistanceFunction);
}

ompl::base::AtlasStateSpace::~AtlasStateSpace()
{
    for (AtlasChart *c : charts_)
        delete c;
}

/// Static.
void ompl::base::AtlasStateSpace::checkSpace(const SpaceInformation *si)
{
    if (dynamic_cast<AtlasStateSpace *>(si->getStateSpace().get()) == nullptr)
        throw ompl::Exception("ompl::base::AtlasStateSpace(): "
                              "si needs to use an AtlasStateSpace!");
}

void ompl::base::AtlasStateSpace::clear()
{
    // Delete the non-anchor charts
    std::vector<AtlasChart *> anchorCharts;
    for (AtlasChart *c : charts_)
    {
        if (c->isAnchor())
            anchorCharts.push_back(c);
        else
            delete c;
    }

    charts_.clear();
    chartNN_.clear();

    // Reinstate the anchor charts
    for (AtlasChart *anchor : anchorCharts)
    {
        anchor->clear();

        if (separate_)
            for (AtlasChart *c : charts_)
                AtlasChart::generateHalfspace(c, anchor);

        anchor->setID(charts_.size());
        chartNN_.add(std::make_pair<>(&anchor->getXorigin(), charts_.size()));
        charts_.push_back(anchor);
    }
}

void ompl::base::AtlasStateSpace::setSeparate(const bool separate)
{
    separate_ = separate;

    if (!separate_)
        for (AtlasChart *c : charts_)
        {
            std::vector<NNElement> nearbyCharts;
            chartNN_.nearestR(std::make_pair(&c->getXorigin(), 0), 2 * rho_, nearbyCharts);
            for (auto &near : nearbyCharts)
                AtlasChart::generateHalfspace(charts_[near.second], c);
        }
    else
        for (AtlasChart *c : charts_)
            c->clear();
}

ompl::base::AtlasChart *ompl::base::AtlasStateSpace::anchorChart(const Eigen::VectorXd &xorigin) const
{
    // This could fail with an exception. We cannot recover if that happens.
    AtlasChart *c = newChart(xorigin);
    if (c == nullptr)
    {
        throw ompl::Exception("ompl::base::AtlasStateSpace::anchorChart(): "
                              "Initial chart creation failed. Cannot proceed.");
    }
    c->makeAnchor();
    return c;
}

ompl::base::AtlasChart *ompl::base::AtlasStateSpace::newChart(const Eigen::VectorXd &xorigin) const
{
    AtlasChart *addedC;
    try
    {
        addedC = new AtlasChart(this, xorigin);
    }
    catch (ompl::Exception &e)
    {
        OMPL_ERROR("ompl::base::AtlasStateSpace::newChart(): "
                   "Failed because manifold looks degenerate here.");
        return nullptr;
    }

    // Ensure all charts respect boundaries of the new one, and vice versa, but
    // only look at nearby ones (within 2*rho).

    if (separate_)
    {
        std::vector<NNElement> nearbyCharts;
        chartNN_.nearestR(std::make_pair(&addedC->getXorigin(), 0), 2 * rho_, nearbyCharts);
        for (auto &near : nearbyCharts)
            AtlasChart::generateHalfspace(charts_[near.second], addedC);
    }

    addedC->setID(charts_.size());
    chartNN_.add(std::make_pair<>(&addedC->getXorigin(), charts_.size()));
    charts_.push_back(addedC);

    return addedC;
}

ompl::base::AtlasChart *ompl::base::AtlasStateSpace::sampleChart() const
{
    if (charts_.empty())
        throw ompl::Exception("ompl::base::AtlasStateSpace::sampleChart(): "
                              "Atlas sampled before any charts were made. Use AtlasStateSpace::anchorChart() first.");

    return charts_[rng_.uniformInt(0, charts_.size() - 1)];
}

ompl::base::AtlasChart *ompl::base::AtlasStateSpace::getChart(const StateType *state) const
{
    AtlasChart *c = state->getChart();
    Eigen::Ref<const Eigen::VectorXd> n = state->constVectorView();

    if (c == nullptr)
    {
        c = owningChart(n);

        if (c == nullptr)
            c = newChart(n);

        if (c != nullptr)
            state->setChart(c);
    }

    return c;
}

ompl::base::AtlasChart *ompl::base::AtlasStateSpace::owningChart(const Eigen::VectorXd &x) const
{
    // Search through all nearby charts for a match
    std::vector<NNElement> nearbyCharts;
    chartNN_.nearestR(std::make_pair(&x, 0), rho_, nearbyCharts);
    Eigen::VectorXd tempx(n_), tempu(k_);
    for (auto &near : nearbyCharts)
    {
        // The point must lie in the chart's validity region and polytope
        AtlasChart *c = charts_[near.second];
        c->psiInverse(x, tempu);
        c->phi(tempu, tempx);
        if ((tempx - x).norm() < epsilon_ && tempu.norm() < rho_ && c->inPolytope(tempu))
            return c;
    }

    return nullptr;
}

// Static
double ompl::base::AtlasStateSpace::chartNNDistanceFunction(const NNElement &e1, const NNElement &e2)
{
    return (*e1.first - *e2.first).norm();
}

std::size_t ompl::base::AtlasStateSpace::getChartCount() const
{
    return charts_.size();
}

bool ompl::base::AtlasStateSpace::traverseManifold(const State *from, const State *to, const bool interpolate,
                                                   std::vector<ompl::base::State *> *stateList) const
{
    const StateType *fromT = from->as<StateType>();
    const StateType *toT = to->as<StateType>();

    if (!constraint_->isSatisfied(from))
        return false;

    AtlasChart *c = getChart(fromT);
    if (c == nullptr)
    {
        OMPL_DEBUG("ompl::base::AtlasStateSpace::traverseManifold(): "
                   "'from' state has no chart!");
        return false;
    }

    // Save a copy of the from state.
    if (stateList != nullptr)
    {
        stateList->clear();
        stateList->push_back(cloneState(from));
    }

    // No need to traverse the manifold if we are already there
    if (validSegmentCount(from, to) == 0)
        return true;

    unsigned int chartsCreated = 0;
    Eigen::Ref<const Eigen::VectorXd> x_a = fromT->constVectorView();
    Eigen::Ref<const Eigen::VectorXd> x_b = toT->constVectorView();

    const StateValidityCheckerPtr &svc = si_->getStateValidityChecker();

    Eigen::VectorXd u_j(k_), u_b(k_);

    StateType *scratch = cloneState(from)->as<StateType>();
    Eigen::Ref<Eigen::VectorXd> x_j = scratch->vectorView();

    // Project from and to points onto the chart
    c->psiInverse(x_j, u_j);
    c->psiInverse(x_b, u_b);

    // We will stop if we exit the ball of radius distance_max centered at x_a.
    const double distance_max = (x_a - x_b).norm();
    double distance = 0;

    Eigen::VectorXd tempx(n_);
    while (((u_b - u_j).squaredNorm() > delta_ * delta_))
    {
        // Step by delta toward the target and project.
        // Note the correction to the pseudocode line 13 (Jaillet et al.).
        u_j += delta_ * (u_b - u_j).normalized();

        // Project state onto manifold
        if (!c->psi(u_j, tempx))
            break;

        const double distance_step = (tempx - x_j).norm();
        distance += distance_step;

        // Update state
        x_j = tempx;
        scratch->setChart(c);

        // Collision check unless interpolating.
        if (!interpolate && (!x_j.allFinite() || !svc->isValid(scratch)))
            break;

        const bool exceedMaxDistance = (x_j - x_a).squaredNorm() > (distance_max * distance_max);
        const bool exceedWandering = distance > (lambda_ * distance_max);
        const bool exceedChartLimit = chartsCreated > maxChartsPerExtension_;

        if (exceedMaxDistance || exceedWandering || exceedChartLimit)
            break;

        // Check if we left the validity region or polytope of the chart.
        c->phi(u_j, tempx);

        const bool exceedsEpsilon = (x_j - tempx).squaredNorm() > (epsilon_ * epsilon_);
        const bool exceedsAngle = delta_ / distance_step < cos_alpha_;
        const bool exceedsRadius = u_j.squaredNorm() > (rho_ * rho_);
        const bool outsidePolytope = !c->inPolytope(u_j);

        // Find or make a new chart if new state is off of current chart
        if (exceedsEpsilon || exceedsAngle || exceedsRadius || outsidePolytope)
        {
            c = owningChart(x_j);
            if (c == nullptr)
            {
                c = newChart(x_j);
                chartsCreated++;
            }

            if (c == nullptr)
            {
                // Pretend like we hit an obstacle.
                OMPL_ERROR("ompl::base::AtlasStateSpace::traverseManifold(): "
                           "Treating singularity as an obstacle.");
                break;
            }

            // Re-project onto the next chart.
            c->psiInverse(x_j, u_j);
            c->psiInverse(x_b, u_b);
        }

        // Keep the state in a list, if requested.
        if (stateList != nullptr)
            stateList->push_back(cloneState(scratch));
    }

    if (chartsCreated > maxChartsPerExtension_)
        OMPL_WARN("ompl::base::AtlasStateSpace::traverseManifold(): "
                  "Too many new charts. Stopping extension early.");

    // Reached goal if final point is within delta and both current and goal are valid.
    const bool currentValid = interpolate || (x_j.allFinite() && svc->isValid(scratch));
    const bool goalValid = interpolate || svc->isValid(to);
    const bool reached = ((x_b - x_j).squaredNorm() <= delta_ * delta_) && currentValid && goalValid;

    // Append a copy of the target state, since we're within delta, but didn't hit it exactly.
    if (reached && (stateList != nullptr))
    {
        State *scratch = cloneState(to);
        if (toT->getChart() == nullptr)
            scratch->as<StateType>()->setChart(c);
        stateList->push_back(scratch);
    }

    freeState(scratch);
    return reached;
}

unsigned int ompl::base::AtlasStateSpace::piecewiseInterpolate(const std::vector<State *> &stateList, const double t,
                                                               State *state) const
{
    unsigned int i = ConstrainedStateSpace::piecewiseInterpolate(stateList, t, state);

    // Set the correct chart, guessing it might be one of the adjacent charts.
    StateType *astate = state->as<StateType>();

    Eigen::Ref<const Eigen::VectorXd> x = astate->constVectorView();
    Eigen::VectorXd u(k_);

    AtlasChart *c1 = stateList[i > 0 ? i - 1 : 0]->as<StateType>()->getChart();
    AtlasChart *c2 = stateList[i]->as<StateType>()->getChart();

    if (c1->psiInverse(x, u), c1->inPolytope(u))
        astate->setChart(c1);

    else if (c2->psiInverse(x, u), c2->inPolytope(u))
        astate->setChart(c2);

    else
    {
        AtlasChart *c = owningChart(x);
        if (c == nullptr)
            c = newChart(x);

        astate->setChart(c);
    }

    return i;
}

ompl::base::StateSamplerPtr ompl::base::AtlasStateSpace::allocDefaultStateSampler() const
{
    return StateSamplerPtr(new AtlasStateSampler(*this));
}

double ompl::base::AtlasStateSpace::estimateFrontierPercent() const
{
    double frontier = 0;
    for (AtlasChart *c : charts_)
        frontier += c->estimateIsFrontier() ? 1 : 0;
    return (100 * frontier) / static_cast<double>(charts_.size());
}

void ompl::base::AtlasStateSpace::printPLY(std::ostream &out) const
{
    std::stringstream v, f;
    std::size_t vcount = 0;
    std::size_t fcount = 0;
    std::vector<Eigen::VectorXd> vertices;
    for (AtlasChart *c : charts_)
    {
        vertices.clear();
        c->toPolygon(vertices);

        std::stringstream poly;
        std::size_t fvcount = 0;
        for (Eigen::VectorXd &vert : vertices)
        {
            v << vert.transpose() << "\n";
            poly << vcount++ << " ";
            fvcount++;
        }

        if (fvcount > 2)
        {
            f << fvcount << " " << poly.str() << "\n";
            fcount += 1;
        }
    }
    vertices.clear();

    out << "ply\n";
    out << "format ascii 1.0\n";
    out << "element vertex " << vcount << "\n";
    out << "property float x\n";
    out << "property float y\n";
    out << "property float z\n";
    out << "element face " << fcount << "\n";
    out << "property list uint uint vertex_index\n";
    out << "end_header\n";
    out << v.str() << f.str();
}
