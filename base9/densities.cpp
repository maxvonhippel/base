#include <array>
#include <vector>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Cluster.hpp"
#include "Star.hpp"

#include "constants.hpp"
#include "structures.hpp"
#include "densities.hpp"
#include "WhiteDwarf.hpp"

#include <iostream>

using std::array;
using std::vector;

constexpr double sqr(double a)
{
    return a * a;
}

static_assert(M_PI > 3.141592, "M_PI is defined and and at least 3.141592");
static_assert(M_PI < 3.15, "M_PI is defined and less than 3.15");

const double mf_sigma = 0.67729, mf_mu = -1.02;
const double loglog10 = log (log (10));

double Cluster::logPriorMass (double primaryMass) const
// Compute log prior density
{
    assert (primaryMass > 0.1 && primaryMass <= M_wd_up);

    double log_m1 = log10 (primaryMass);
    double logPrior = getLogMassNorm() + -0.5 * sqr (log_m1 - mf_mu) / sqr (mf_sigma) - log (primaryMass) - loglog10;

    return logPrior;
}

// Compute log prior density for cluster properties
double Cluster::logPrior (const Model &evoModels) const
{
    if ((age < evoModels.mainSequenceEvol->getMinAge())
        || (age > evoModels.mainSequenceEvol->getMaxAge())
        || (ifmrSlope < 0.0)
        || (abs < 0.0)
        || ((evoModels.IFMR == 11) && (ifmrQuadCoef < 0.0)))
    {
        throw InvalidCluster("Invalid cluster parameter");
    }

    // enforce monotonicity in IFMR
    if (evoModels.IFMR == 10)
    {
        double massLower = 0.15;
        double massUpper = M_wd_up;
        double massShift = 3.0;
        double angle = atan (ifmrSlope);
        double aa = cos (angle) * (1 + ifmrSlope * ifmrSlope);
        double xLower = aa * (massLower - massShift);
        double xUpper = aa * (massUpper - massShift);

        double dydx_xLower = ifmrQuadCoef * (xLower - xUpper);
        double dydx_xUpper = -dydx_xLower;

        double slopeLower = tan (angle + atan (dydx_xLower));
        double slopeUpper = tan (angle + atan (dydx_xUpper));

        // if IFMR is decreasing at either endpoint, reject
        if (slopeLower < 0.0 || slopeUpper < 0.0)
            throw std::range_error("IFMR decreasing at endpoint");
    }

    double prior = 0.0;
    //DS: with a uniform prior on carbonicity, the above won't change since log(1) = 0.

    if (priorVar.at(FEH) > EPSILON)
        prior += (-0.5) * sqr (feh - priorMean.at(FEH)) / priorVar.at(FEH);
    if (priorVar.at(MOD) > EPSILON)
        prior += (-0.5) * sqr (mod - priorMean.at(MOD)) / priorVar.at(MOD);
    if (priorVar.at(ABS) > EPSILON)
        prior += (-0.5) * sqr (abs - priorMean.at(ABS)) / priorVar.at(ABS);
    if (priorVar.at(YYY) > EPSILON)
        prior += (-0.5) * sqr (yyy - priorMean.at(YYY)) / priorVar.at(YYY);

    return prior;
}

// double logLikelihood (int numFilts, const StellarSystem &system, const array<double, FILTS> &mags)
// // Computes log likelihood
// {
//     int i;
//     double likelihood = 0.0;

//     for (i = 0; i < numFilts; i++)
//     {
//         if (system.variance.at(i) > 1e-9)
//             likelihood -= 0.5 * (log (2 * M_PI * system.variance.at(i)) + (sqr (mags.at(i) - system.obsPhot.at(i)) / system.variance.at(i)));
//     }

//     return likelihood;
// }

// double tLogLikelihood (int numFilts, const StellarSystem &system, const array<double, FILTS> &mags)
// // Computes log likelihood
// {
//     double likelihood = 0.0;
//     double dof = 3.0;
//     double quadratic_sum = 0.0;

//     for (int i = 0; i < numFilts; i++)
//     {
//         if (system.variance.at(i) > 1e-9)
//         {
//             quadratic_sum += sqr (mags.at(i) - system.obsPhot.at(i)) / system.variance.at(i);
//             likelihood -= 0.5 * (log (M_PI * system.variance.at(i)));
//         }
//     }

//     likelihood += lgamma (0.5 * (dof + (double) numFilts)) - lgamma (0.5 * dof);
//     likelihood -= 0.5 * (dof + (double) numFilts) * log (1 + quadratic_sum);

//     return likelihood;
// }

static double scaledLogLike (const vector<double> &obsPhot, const vector<double> &variance, const array<double, FILTS> &mags, double varScale)
{
    double likelihood = 0.0;

    for (decltype(variance.size()) i = 0; i < variance.size(); i++)
    {
        if (variance.at(i) > 1e-9)
        {
            likelihood -= 0.5 * (log (2 * M_PI * varScale * variance.at(i)) + (sqr (mags.at(i) - obsPhot.at(i)) / (varScale * variance.at(i))));
        }
    }

    return likelihood;
}


// Calculates the posterior density for a stellar system
double StellarSystem::logPost (const Cluster &clust, const Model &evoModels, const vector<int> &filters) const
{
    // AGBt_zmass never set because age and/or metallicity out of range of models.
    if (clust.AGBt_zmass < EPS)
    {
        throw WDBoundsError("Bounds error in evolve.cpp");
    }

    const array<double, FILTS> mags = deriveCombinedMags(clust, evoModels, filters);

    double logPrior = clust.logPriorMass (primary.mass);

    double likelihood = scaledLogLike (obsPhot, variance, mags, clust.varScale);

    return (logPrior + likelihood);
}
