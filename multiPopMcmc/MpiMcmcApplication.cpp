#include <functional>
#include <iostream>

#include <boost/format.hpp>

#include <gsl/gsl_blas.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_eigen.h>
#include <gsl/gsl_statistics.h>
#include <gsl/gsl_linalg.h>

#include "Chain.hpp"
#include "Cluster.hpp"
#include "marg.hpp"
#include "mpiMcmc.hpp"
#include "mpiFuncs.hpp"
#include "MpiMcmcApplication.hpp"
#include "Settings.hpp"
#include "Star.hpp"
#include "samplers.cpp"
#include "WhiteDwarf.hpp"

using std::array;
using std::cout;
using std::cerr;
using std::endl;
using std::flush;
using std::function;
using std::mutex;
using std::unique_ptr;
using std::vector;

using namespace std::placeholders;

void ensurePriors(const Settings &s, const DualPopCluster &clust)
{
    // Clust A carbonicity
    if (clust.clustA.carbonicity < 0.0)
        throw InvalidCluster("Low carbonicity, Clust A");
    else if (clust.clustA.carbonicity > 1.0)
        throw InvalidCluster("High carbonicity, Clust A");

    // Clust B carbonicity
    else if (clust.clustB.carbonicity < 0.0)
        throw InvalidCluster("Low carbonicity, Clust B");
    else if (clust.clustB.carbonicity > 1.0)
        throw InvalidCluster("High carbonicity, Clust B");

    // Clust A Y
    else if (clust.clustA.yyy < s.multiPopMcmc.YA_lo)
        throw InvalidCluster("Low Y, Clust A");
    else if (clust.clustA.yyy > s.multiPopMcmc.YA_hi)
        throw InvalidCluster("High Y, Clust A");

    // Clust B Y
    else if (clust.clustB.yyy < clust.clustA.yyy)
        throw InvalidCluster("Low Y, Clust B");
    else if (clust.clustB.yyy > s.multiPopMcmc.YB_hi)
        throw InvalidCluster("High Y, Clust B");

    // Lambda
    else if (clust.lambda < 0.0)
        throw InvalidCluster("Low Lambda");
    else if (clust.lambda > 1.0)
        throw InvalidCluster("High Lambda");
}

MpiMcmcApplication::MpiMcmcApplication(Settings &s)
    : evoModels(makeModel(s)), settings(s), gen(uint32_t(s.seed * uint32_t(2654435761))), pool(s.threads) // Applies Knuth's multiplicative hash for obfuscation (TAOCP Vol. 3)
{
    ctrl.priorVar.fill(0);

    mainClust.clustA.feh = settings.cluster.starting.Fe_H;
    mainClust.clustA.priorMean[FEH] = clust.clustA.feh = clust.clustA.priorMean[FEH] = settings.cluster.Fe_H;
    ctrl.priorVar[FEH] = settings.cluster.sigma.Fe_H;

    mainClust.clustA.mod = settings.cluster.starting.distMod;
    mainClust.clustA.priorMean[MOD] = clust.clustA.mod = clust.clustA.priorMean[MOD] = settings.cluster.distMod;
    ctrl.priorVar[MOD] = settings.cluster.sigma.distMod;

    mainClust.clustA.abs = settings.cluster.starting.Av;
    mainClust.clustA.priorMean[ABS] = clust.clustA.abs = clust.clustA.priorMean[ABS] = fabs(settings.cluster.Av);
    ctrl.priorVar[ABS] = settings.cluster.sigma.Av;

    mainClust.clustA.age = mainClust.clustA.priorMean[AGE] = clust.clustA.age = clust.clustA.priorMean[AGE] = settings.cluster.logClusAge;
    ctrl.priorVar[AGE] = 1.0;

    mainClust.clustA.carbonicity = settings.cluster.starting.carbonicity;
    mainClust.clustA.priorMean[CARBONICITY] = clust.clustA.carbonicity = clust.clustA.priorMean[CARBONICITY] = settings.cluster.carbonicity;
    ctrl.priorVar[CARBONICITY] = settings.cluster.sigma.carbonicity;

    mainClust.clustA.yyy = settings.cluster.starting.Y;
    mainClust.clustA.priorMean[YYY] = clust.clustA.yyy = clust.clustA.priorMean[YYY] = settings.cluster.Y;
    ctrl.priorVar[YYY] = 0.0;


    // No IFMR code

    clust.clustA.setM_wd_up(settings.whiteDwarf.M_wd_up);
    mainClust.clustA.setM_wd_up(settings.whiteDwarf.M_wd_up);

    for (auto &var : ctrl.priorVar)
    {
        if (var < 0.0)
        {
            var = 0.0;
        }
        else
        {
            var = var * var;
        }
    }

    std::copy(ctrl.priorVar.begin(), ctrl.priorVar.end(), clust.clustA.priorVar.begin());
    std::copy(ctrl.priorVar.begin(), ctrl.priorVar.end(), mainClust.clustA.priorVar.begin());

    clust.clustB = clust.clustA;
    mainClust.clustB = mainClust.clustA;

    // Multi-pop Y values
    mainClust.clustA.yyy = clust.clustA.yyy = settings.multiPopMcmc.YA_start;
    mainClust.clustB.yyy = clust.clustB.yyy = settings.multiPopMcmc.YB_start;
    mainClust.lambda     = clust.lambda     = 0.5;

    /* read burnIter and nIter */
    {
        ctrl.burnIter = settings.mpiMcmc.burnIter;
        int nParamsUsed = 0;

        for (int p = 0; p < NPARAMS; p++)
        {
            if (ctrl.priorVar.at(p) > EPSILON || p == YYA || p == YYB || p == LAMBDA)
            {
                nParamsUsed++;
            }
        }

        if (ctrl.burnIter < 2 * (nParamsUsed + 1))
        {
            ctrl.burnIter = 2 * (nParamsUsed + 1);
            cerr << "burnIter below minimum allowable size. Increasing to " << ctrl.burnIter << endl;
        }
    }

    ctrl.nIter = settings.mpiMcmc.maxIter;
    ctrl.thin = settings.mpiMcmc.thin;

    ctrl.clusterFilename = settings.files.output + ".res";
}


int MpiMcmcApplication::run()
{
    cout << "Bayesian Analysis of Stellar Evolution (Multiple Populations)" << endl;

    double fsLike;

    array<double, NPARAMS> stepSize;
    std::copy(settings.mpiMcmc.stepSize.begin(), settings.mpiMcmc.stepSize.end(), stepSize.begin());
    stepSize.at(LAMBDA) = settings.multiPopMcmc.lambdaStep;

    {
        // Read photometry and calculate fsLike
        vector<double> filterPriorMin;
        vector<double> filterPriorMax;

        // open files for reading (data) and writing
        // rData implcitly relies on going out of scope to close the photometry file
        // This is awful, but pretty (since this code is, at time of writing, in restricted, anonymous scope
        std::ifstream rData(settings.files.phot);

        if (!rData)
        {
            cerr << "***Error: Photometry file " << settings.files.phot << " was not found.***" << endl;
            cerr << "[Exiting...]" << endl;
            exit (-1);
        }

        auto ret = base::utility::readPhotometry (rData, filterPriorMin, filterPriorMax, settings);
        auto filterNames = ret.first;

        for (auto r : ret.second)
        {
            if (r.observedStatus == StarStatus::MSRG)
                systems.push_back(r);
            else if (r.observedStatus == StarStatus::WD)
                cerr << "Found unsupported WD in photometry... Continuing anyway." << endl;
        }

        evoModels.restrictFilters(filterNames);

        if (settings.cluster.index < 0 || settings.cluster.index > filterNames.size())
        {
            cerr << "***Error: " << settings.cluster.index << " not a valid magnitude index.  Choose 0, 1,or 2.***" << endl;
            cerr << "[Exiting...]" << endl;
            exit (1);
        }

        if (systems.size() > 1)
        {
            double logFieldStarLikelihood = 0.0;

            for (size_t filt = 0; filt < filterNames.size(); filt++)
            {
                logFieldStarLikelihood -= log (filterPriorMax.at(filt) - filterPriorMin.at(filt));
            }
            fsLike = exp (logFieldStarLikelihood);
        }
        else
        {
            fsLike = 0;
        }
    }

    // Begin initChain
    {
        for (auto system : systems)
        {
            system.clustStarProposalDens = system.clustStarPriorDens;   // Use prior prob of being clus star
        }
    }
    // end initChain

    // Assuming fsLike doesn't change, this is the "global" logPost function
    auto logPostFunc = std::bind(&MpiMcmcApplication::logPostStep, this, _1, fsLike);

    // Run Burnin
    std::ofstream burninFile(ctrl.clusterFilename + ".burnin");
    if (!burninFile)
    {
        cerr << "***Error: File " << ctrl.clusterFilename + ".burnin" << " was not available for writing.***" << endl;
        cerr << "[Exiting...]" << endl;
        exit (1);
    }

    printHeader (burninFile, ctrl.priorVar);

    Chain<DualPopCluster> burninChain(static_cast<uint32_t>(std::uniform_int_distribution<>()(gen)), ctrl.priorVar, clust, burninFile);
    std::function<void(const DualPopCluster&)> checkPriors = std::bind(&ensurePriors, std::cref(settings), _1);

    try
    {
        int adaptiveBurnIter = 0;
        bool acceptedOne = false;  // Keeps track of whether we have two accepted trials in a ros
        bool acceptedOnce = false; // Keeps track of whether we have ever accepted a trial

        // Stage 1 burnin
        {
            cout << "\nRunning Stage 1 burnin..." << flush;

            auto proposalFunc = std::bind(&MpiMcmcApplication::propClustBigSteps, this, _1, std::cref(ctrl), std::cref(stepSize));
            burninChain.run(proposalFunc, logPostFunc, checkPriors, settings.mpiMcmc.adaptiveBigSteps);

            cout << " Complete (acceptanceRatio = " << burninChain.acceptanceRatio() << ")" << endl;

            burninChain.reset(); // Reset the chain to forget this part of the burnin.
        }

        cout << "\nRunning Stage 2 (adaptive) burnin..." << endl;

        // Run adaptive burnin (stage 2)
        // -----------------------------
        // Exits after two consecutive trials with a 20% < x < 40% acceptanceRatio,
        // or after the numeber of iterations exceeds the maximum burnin iterations.
        do
        {
            // Convenience variable
            const int trialIter = settings.mpiMcmc.trialIter;

            // Increment the number of iterations we've gone through
            // If the step sizes aren't converging on an acceptable acceptance ratio, this can kick us out of the burnin
            adaptiveBurnIter += trialIter;

            // Reset the ratio to determine step scaling for this trial
            burninChain.resetRatio();

            std::function<DualPopCluster(DualPopCluster)> proposalFunc = std::bind(&MpiMcmcApplication::propClustIndep, this, _1, std::cref(ctrl), std::cref(stepSize), 5);

            if (settings.mpiMcmc.bigStepBurnin)
            {
                // Run big steps for the entire trial
                burninChain.run(proposalFunc, logPostFunc, checkPriors, trialIter);
            }
            else if (acceptedOnce)
            {
                proposalFunc = std::bind(&MpiMcmcApplication::propClustIndep, this, _1, std::cref(ctrl), std::cref(stepSize), 1);
                burninChain.run(proposalFunc, logPostFunc, checkPriors, trialIter / 2);
            }
            else
            {
                // Run big steps for half the trial
                burninChain.run(proposalFunc, logPostFunc, checkPriors, trialIter / 2);

                // Then run smaller steps for the second half
                proposalFunc = std::bind(&MpiMcmcApplication::propClustIndep, this, _1, std::cref(ctrl), std::cref(stepSize), 1);
                burninChain.run(proposalFunc, logPostFunc, checkPriors, trialIter / 2);
            }

            double acceptanceRatio = burninChain.acceptanceRatio();

            if (acceptanceRatio <= 0.4 && acceptanceRatio >= 0.2)
            {
                acceptedOnce = true;

                if (acceptedOne)
                {
                    cout << "  Leaving adaptive burnin early with an acceptance ratio of " << acceptanceRatio << " (iteration " << adaptiveBurnIter + settings.mpiMcmc.adaptiveBigSteps << ")" << endl;

                    break;
                }
                else
                {
                    cout << "    Acceptance ratio: " << acceptanceRatio << ". Trying for trend." << endl;
                    acceptedOne = true;
                }
            }
            else
            {
                acceptedOne = false;
                cout << "    Acceptance ratio: " << acceptanceRatio << ". Retrying." << endl;

                scaleStepSizes(stepSize, burninChain.acceptanceRatio()); // Adjust step sizes
            }
        } while (adaptiveBurnIter < ctrl.burnIter);

        burninFile.close();
    }
    catch (...)
    {
        burninFile.close();
        throw;
    }

    // Main run
    ofstream resultFile(ctrl.clusterFilename);
    if (!resultFile)
    {
        cerr << "***Error: File " << ctrl.clusterFilename << " was not available for writing.***" << endl;
        cerr << "[Exiting...]" << endl;
        exit (1);
    }

    try
    {
        printHeader (resultFile, ctrl.priorVar);

        Chain<DualPopCluster> mainChain(static_cast<uint32_t>(std::uniform_int_distribution<>()(gen)), ctrl.priorVar, burninChain.getCluster(), resultFile);

        {
            // Make sure and pull the covariance matrix before resetting the burninChain
            auto proposalFunc = std::bind(&MpiMcmcApplication::propClustCorrelated, this, _1, std::cref(ctrl), burninChain.makeCholeskyDecomp());

            cout << "\nStarting adaptive run... " << flush;

            mainChain.run(proposalFunc, logPostFunc, checkPriors, settings.mpiMcmc.stage3Iter);
            cout << " Preliminary acceptanceRatio = " << mainChain.acceptanceRatio() << endl;
        }

        // Begin main run
        // Main run proceeds in increments of 1, adapting the covariance matrix after every increment
        for (auto iters = 0; iters < ctrl.nIter; ++iters)
        {
            auto proposalFunc = std::bind(&MpiMcmcApplication::propClustCorrelated, this, _1, std::cref(ctrl), mainChain.makeCholeskyDecomp());

            mainChain.run(proposalFunc, logPostFunc, checkPriors, 1, ctrl.thin);
        }

        cout << "\nFinal acceptance ratio: " << mainChain.acceptanceRatio() << endl;

        resultFile.close();
    }
    catch (...)
    {
        resultFile.close();
        throw;
    }

    return 0;
}


void MpiMcmcApplication::scaleStepSizes (array<double, NPARAMS> &stepSize, double acceptanceRatio)
{
    function<double(double)> scaleFactor = [](double acceptanceRatio) {
        double factor = 1.0;

        if (acceptanceRatio > 0.9)
        {
            factor = 2.0;
        }
        else if (acceptanceRatio > 0.7)
        {
            factor = 1.8;
        }
        else if (acceptanceRatio > 0.5)
        {
            factor = 1.5;
        }
        else if (acceptanceRatio > 0.4)
        {
            factor = 1.2;
        }
        else if (acceptanceRatio < 0.2)
        {
            factor = 1 / 1.5;
        }
        else if (acceptanceRatio < 0.15)
        {
            factor = 1 / 1.8;
        }
        else if (acceptanceRatio < 0.05)
        {
            factor = 0.5;
        }

        return factor;
    };

    for (int p = 0; p < NPARAMS; p++)
    {
        if (ctrl.priorVar.at(p) > EPSILON || p == YYA || p == YYB || p == LAMBDA)
        {
            stepSize.at(p) *= scaleFactor(acceptanceRatio);
        }
    }
}


DualPopCluster MpiMcmcApplication::propClustBigSteps (DualPopCluster clust, struct ifmrMcmcControl const &ctrl, array<double, NPARAMS> const &stepSize)
{
    return propClustIndep(clust, ctrl, stepSize, 25.0);
}

DualPopCluster MpiMcmcApplication::propClustIndep (DualPopCluster clust, struct ifmrMcmcControl const &ctrl, array<double, NPARAMS> const &stepSize, double scale)
{
    /* DOF defined in densities.h */
    int p;

    for (p = 0; p < NPARAMS; p++)
    {
        if (ctrl.priorVar.at(p) > EPSILON && p != YYA && p != YYB && p != LAMBDA)
        {
            clust.clustA.setParam(p, clust.clustA.getParam(p) + sampleT (gen, scale * stepSize.at(p) * stepSize.at(p)));
            clust.clustB.setParam(p, clust.clustA.getParam(p));
        }
    }

    clust.clustA.yyy += sampleT (gen, scale * stepSize.at(YYY) * stepSize.at(YYY));
    clust.clustB.yyy += sampleT (gen, scale * stepSize.at(YYY) * stepSize.at(YYY));

    clust.lambda += sampleT (gen, scale * stepSize.at(LAMBDA) * stepSize.at(LAMBDA));

    return clust;
}

DualPopCluster MpiMcmcApplication::propClustCorrelated (DualPopCluster clust, struct ifmrMcmcControl const &ctrl, Matrix<double, NPARAMS, NPARAMS> const &propMatrix)
{
    array<double, NPARAMS> tDraws;
    
    for (auto &d : tDraws)
    {
        d = sampleT (gen, 1.0);
    }

    for (int p = 0; p < NPARAMS; ++p)
    {
        if (ctrl.priorVar.at(p) > EPSILON || p == YYA || p == YYB || p == LAMBDA)
        {
            double corrProps = 0;

            for (int k = 0; k <= p; ++k)
            {                           /* propMatrix is lower diagonal */
                if (ctrl.priorVar.at(k) > EPSILON || k == YYA || k == YYB || k == LAMBDA)
                {
                    corrProps += propMatrix.at(p).at(k) * tDraws[k];
                }
            }

            if (p == YYA)
                clust.clustA.yyy += corrProps;
            else if (p == YYB)
                clust.clustB.yyy += corrProps;
            else if (p == LAMBDA)
                clust.lambda += corrProps;
            else
            {
                // Set the value in clustA
                clust.clustA.setParam(p, clust.clustA.getParam(p) + corrProps);
                // Then copy the value from clustA to clustB
                clust.clustB.setParam(p, clust.clustA.getParam(p));
            }
        }
    }

    return clust;
}

double MpiMcmcApplication::logPostStep(DualPopCluster &propClust, double fsLike)
{
    double logPostProp = propClust.clustA.logPrior (evoModels);

    array<unique_ptr<Isochrone>, 2> isochrone;
    array<Cluster*, 2> cluster = { &propClust.clustA, &propClust.clustB };

    pool.parallelFor(2, [=,&isochrone](int i)
    {
        isochrone[i].reset(evoModels.mainSequenceEvol->deriveIsochrone(cluster[i]->feh, cluster[i]->yyy, cluster[i]->age));
    });

    auto sSize = systems.size();

    const double lambdaA = propClust.lambda;
    const double lambdaB = (1 - lambdaA);

    /* marginalize over isochrone */
    auto postA = margEvolveWithBinary (propClust.clustA, systems, evoModels, *isochrone.at(0), pool, settings.noBinaries);
    auto postB = margEvolveWithBinary (propClust.clustB, systems, evoModels, *isochrone.at(1), pool, settings.noBinaries);

    for (size_t i = 0; i < sSize; ++i)
    {
        // Give both A and B a chance to be the only contributor for this star.
        if (postA[i] > 0.0)
        {
            postA[i] *= lambdaA;
        }
        else
        {
            postA[i] = 0.0;
        }

        // Independent of above
        if (postB[i] > 0.0)
        {
            postA[i] += lambdaB * postB[i];
        }

        // It's also possible that it comes out zero and we waste our time doing this multiplication
        // But it's faster to do it than to check for zero.
        postA[i] *= systems[i].clustStarPriorDens;

        /* marginalize over field star status */
        logPostProp += __builtin_log ((1.0 - systems[i].clustStarPriorDens) * fsLike + postA[i]);
    }

    return logPostProp;
}


double MpiMcmcApplication::wdGridMass (int point) const
{
    // I think this only gets calculated once, but I can't quite be sure...
    // It may not even matter, but every little bit helps, eh? And hey, preoptimization...
    static const double dWdMass1 = (settings.whiteDwarf.M_wd_up - MIN_MASS1) / (double) N_WD_MASS1;

    return MIN_MASS1 + (dWdMass1 * point);
}
