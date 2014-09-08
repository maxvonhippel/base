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

const double TWO_M_PI = 2 * M_PI;

void ensurePriors(const Settings&, const Cluster &clust)
{
    // Clust A carbonicity
    if (clust.carbonicity < 0.0)
        throw InvalidCluster("Low carbonicity");
    else if (clust.carbonicity > 1.0)
        throw InvalidCluster("High carbonicity");
}

MpiMcmcApplication::MpiMcmcApplication(Settings &s)
    : evoModels(makeModel(s)), settings(s), gen(uint32_t(s.seed * uint32_t(2654435761))), pool(s.threads) // Applies Knuth's multiplicative hash for obfuscation (TAOCP Vol. 3)
{
    ctrl.priorVar.fill(0);

    mainClust.feh = settings.cluster.starting.Fe_H;
    mainClust.priorMean[FEH] = clust.feh = clust.priorMean[FEH] = settings.cluster.Fe_H;
    ctrl.priorVar[FEH] = settings.cluster.sigma.Fe_H;

    mainClust.mod = settings.cluster.starting.distMod;
    mainClust.priorMean[MOD] = clust.mod = clust.priorMean[MOD] = settings.cluster.distMod;
    ctrl.priorVar[MOD] = settings.cluster.sigma.distMod;

    mainClust.abs = settings.cluster.starting.Av;
    mainClust.priorMean[ABS] = clust.abs = clust.priorMean[ABS] = fabs(settings.cluster.Av);
    ctrl.priorVar[ABS] = settings.cluster.sigma.Av;

    mainClust.age = mainClust.priorMean[AGE] = clust.age = clust.priorMean[AGE] = settings.cluster.logClusAge;
    ctrl.priorVar[AGE] = 1.0;

    mainClust.carbonicity = settings.cluster.starting.carbonicity;
    mainClust.priorMean[CARBONICITY] = clust.carbonicity = clust.priorMean[CARBONICITY] = settings.cluster.carbonicity;
    ctrl.priorVar[CARBONICITY] = settings.cluster.sigma.carbonicity;

    mainClust.yyy = settings.cluster.starting.Y;
    mainClust.priorMean[YYY] = clust.yyy = clust.priorMean[YYY] = settings.cluster.Y;
    ctrl.priorVar[YYY] = settings.cluster.sigma.Y;


    if (evoModels.IFMR <= 3)
    {
        ctrl.priorVar[IFMR_SLOPE] = 0.0;
        ctrl.priorVar[IFMR_INTERCEPT] = 0.0;
        ctrl.priorVar[IFMR_QUADCOEF] = 0.0;
    }
    else if (evoModels.IFMR <= 8)
    {
        ctrl.priorVar[IFMR_SLOPE] = 1.0;
        ctrl.priorVar[IFMR_INTERCEPT] = 1.0;
        ctrl.priorVar[IFMR_QUADCOEF] = 0.0;
    }
    else
    {
        ctrl.priorVar[IFMR_SLOPE] = 1.0;
        ctrl.priorVar[IFMR_INTERCEPT] = 1.0;
        ctrl.priorVar[IFMR_QUADCOEF] = 1.0;
    }

    /* set starting values for IFMR parameters */
    mainClust.ifmrSlope = mainClust.priorMean[IFMR_SLOPE] =clust.ifmrSlope = clust.priorMean[IFMR_SLOPE] = 0.08;
    mainClust.ifmrIntercept = mainClust.priorMean[IFMR_INTERCEPT] = clust.ifmrIntercept = clust.priorMean[IFMR_INTERCEPT] = 0.65;

    if (evoModels.IFMR <= 10)
        mainClust.ifmrQuadCoef = mainClust.priorMean[IFMR_QUADCOEF] = clust.ifmrQuadCoef = clust.priorMean[IFMR_QUADCOEF] = 0.0001;
    else
        mainClust.ifmrQuadCoef = mainClust.priorMean[IFMR_QUADCOEF] = clust.ifmrQuadCoef = clust.priorMean[IFMR_QUADCOEF] = 0.08;

    clust.setM_wd_up(settings.whiteDwarf.M_wd_up);
    mainClust.setM_wd_up(settings.whiteDwarf.M_wd_up);

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

    /* read burnIter and nIter */
    {
        ctrl.burnIter = settings.mpiMcmc.burnIter;
        int nParamsUsed = 0;

        for (int p = 0; p < NPARAMS; p++)
        {
            if (ctrl.priorVar.at(p) > EPSILON)
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

    ctrl.clusterFilename = settings.files.output + '_' + std::to_string(settings.seed) + ".res";

    std::copy(ctrl.priorVar.begin(), ctrl.priorVar.end(), clust.priorVar.begin());
    std::copy(ctrl.priorVar.begin(), ctrl.priorVar.end(), mainClust.priorVar.begin());
}


int MpiMcmcApplication::run()
{
    cout << "Bayesian Analysis of Stellar Evolution" << endl;

    double fsLike;

    array<double, NPARAMS> stepSize;
    std::copy(settings.mpiMcmc.stepSize.begin(), settings.mpiMcmc.stepSize.end(), stepSize.begin());

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
                msSystems.push_back(r);
            else if (r.observedStatus == StarStatus::WD)
                wdSystems.push_back(r);
            else
                cerr << "Found unsupported star in photometry, type '" << r.observedStatus << "'... Continuing anyway." << endl;
        }

        if ( msSystems.empty() && wdSystems.empty())
        {
            cerr << "No stars loaded... Exiting." << endl;
            exit(-1);
        }


        evoModels.restrictFilters(filterNames);

        if (settings.cluster.index < 0 || settings.cluster.index > filterNames.size())
        {
            cerr << "***Error: " << settings.cluster.index << " not a valid magnitude index.  Choose 0, 1,or 2.***" << endl;
            cerr << "[Exiting...]" << endl;
            exit (1);
        }

        if ((msSystems.size() + wdSystems.size()) > 1)
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

    // Initialize SSE memory for noBinaries
    // THIS LEAKS MEMORY LIKE A SIEVE DUE TO FORGETTING TALLOC
    // But it's okay if we only call it once (until it gets moved to the constructor)
    {
        using aligned_m128 = std::aligned_storage<16, 16>::type;

        if (! msSystems.empty())
        {
            // howManyFilts has to be a multiple of two to make the SSE code happy
            // Add 1 and round down.
                   howManyFiltsAligned = ((msSystems.front().obsPhot.size() + 1) & ~0x1);
                   howManyFilts        = msSystems.front().obsPhot.size();
            size_t howManyWeNeed       = msSystems.size() * howManyFiltsAligned;
            size_t howManyWeAlloc      = howManyWeNeed / 2;

            aligned_m128* talloc = new aligned_m128[howManyWeAlloc];
            sysVars = new(talloc) double[howManyWeNeed];

            talloc  = new aligned_m128[howManyWeAlloc];
            sysVar2 = new(talloc) double[howManyWeNeed];

            talloc = new aligned_m128[howManyWeAlloc];
            sysObs = new(talloc) double[howManyWeNeed];

        }

        int i = 0;

        for (auto s : msSystems)
        {
            for (size_t k = 0; k < howManyFiltsAligned; ++k, ++i)
            {
                if ((k < s.variance.size()) && (s.variance.at(k) > EPS))
                {
                    sysVars[i] = s.variance.at(k) * clust.varScale;
                    sysVar2[i] = __builtin_log (TWO_M_PI * sysVars[i]);

                    // Removes a division from the noBinaries loop which turns a 14 cycle loop into a 3.8 cycle loop
                    sysVars[i] = 1 / sysVars[i];

                    sysObs [i] = s.obsPhot.at(k);
                }
                else
                {
                    sysVars[i] = 0.0;
                    sysVar2[i] = 0.0;

                    sysObs [i] = 0.0;
                }
            }
        }
    }

    // Begin initChain
    {
        for (auto system : msSystems)
        {
            system.clustStarProposalDens = system.clustStarPriorDens;   // Use prior prob of being clus star
        }

        for (auto system : wdSystems)
        {
            system.clustStarProposalDens = system.clustStarPriorDens;   // Use prior prob of being clus star

            system.setMassRatio(0.0);
        }
    }
    // end initChain

    cout << "Seed:   " << settings.seed << endl;
    cout << "FSlike: " << fsLike << endl;

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

    Chain<Cluster> burninChain(static_cast<uint32_t>(std::uniform_int_distribution<>()(gen)), ctrl.priorVar, clust, burninFile);
    std::function<void(const Cluster&)> checkPriors = std::bind(&ensurePriors, std::cref(settings), _1);

    try
    {
        int  adaptiveBurnIter = 0;
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

            std::function<Cluster(Cluster)> proposalFunc = std::bind(&MpiMcmcApplication::propClustIndep, this, _1, std::cref(ctrl), std::cref(stepSize), 5);

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
    std::ofstream resultFile(ctrl.clusterFilename);
    if (!resultFile)
    {
        cerr << "***Error: File " << ctrl.clusterFilename << " was not available for writing.***" << endl;
        cerr << "[Exiting...]" << endl;
        exit (1);
    }

    try
    {
        printHeader (resultFile, ctrl.priorVar);

        Chain<Cluster> mainChain(static_cast<uint32_t>(std::uniform_int_distribution<>()(gen)), ctrl.priorVar, burninChain.getCluster(), resultFile);

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
        if (ctrl.priorVar.at(p) > EPSILON)
        {
            stepSize.at(p) *= scaleFactor(acceptanceRatio);
        }
    }
}

Cluster MpiMcmcApplication::propClustBigSteps (Cluster clust, struct ifmrMcmcControl const &ctrl, array<double, NPARAMS> const &stepSize)
{
    return propClustIndep(clust, ctrl, stepSize, 25.0);
}

Cluster MpiMcmcApplication::propClustIndep (Cluster clust, struct ifmrMcmcControl const &ctrl, array<double, NPARAMS> const &stepSize, double scale)
{
    /* DOF defined in densities.h */
    int p;

    for (p = 0; p < NPARAMS; p++)
    {
        if (ctrl.priorVar.at(p) > EPSILON)
        {
            clust.setParam(p, clust.getParam(p) + sampleT (gen, scale * stepSize.at(p) * stepSize.at(p)));
        }
    }

    return clust;
}

Cluster MpiMcmcApplication::propClustCorrelated (Cluster clust, struct ifmrMcmcControl const &ctrl, Matrix<double, NPARAMS, NPARAMS> const &propMatrix)
{
    array<double, NPARAMS> tDraws;

    for (auto &d : tDraws)
    {
        d = sampleT (gen, 1.0);
    }

    for (int p = 0; p < NPARAMS; ++p)
    {
        if (ctrl.priorVar.at(p) > EPSILON)
        {
            double corrProps = 0;

            for (int k = 0; k <= p; ++k)
            {                           /* propMatrix is lower diagonal */
                if (ctrl.priorVar.at(k) > EPSILON)
                {
                    corrProps += propMatrix.at(p).at(k) * tDraws[k];
                }
            }

            clust.setParam(p, clust.getParam(p) + corrProps);
        }
    }

    return clust;
}

double MpiMcmcApplication::logPostStep(Cluster &propClust, double fsLike)
{
    double logPostProp = propClust.logPrior (evoModels);

    unique_ptr<Isochrone> isochrone(evoModels.mainSequenceEvol->deriveIsochrone(propClust.feh, propClust.yyy, propClust.age));

    // Handle WDs specially (this needs to be moved to a new function)
    if ( ! wdSystems.empty() )
    {
        Star s;
        vector<double> primaryMags, secondaryMags, combinedMags;

        {
            s.mass = 0.0;

            secondaryMags = s.getMags(propClust, evoModels, *isochrone);
        }

        vector<double> post(wdSystems.size(), 0.0);

        const auto m_wd_up = propClust.getM_wd_up();
        const auto wdSize  = wdSystems.size();
        const auto wdMassPrior = __builtin_log ((m_wd_up - MIN_MASS1) / (double) N_WD_MASS1);

        for (int j = 0; j < N_WD_MASS1; ++j)
        {
            const double primaryMass = wdGridMass(j);
            const double logPrior    = propClust.logPriorMass (primaryMass);

            s.mass       = primaryMass;
            primaryMags  = s.getMags(propClust, evoModels, *isochrone);
            combinedMags = StellarSystem::deriveCombinedMags(propClust, evoModels, *isochrone, primaryMags, secondaryMags);

            for (size_t i = 0; i < wdSize; ++i)
            {
                double tmpLogPost;

                tmpLogPost  = wdSystems[i].logPost(propClust, evoModels, *isochrone, logPrior, combinedMags);
                tmpLogPost += wdMassPrior;

                post[i] += __builtin_exp(tmpLogPost);
            }
        }

        for (size_t i = 0; i < wdSize; ++i)
        {
            if (post[i] > 0.0)
            {
                post[i] *= wdSystems[i].clustStarPriorDens;
            }
            else
            {
                post[i] = 0.0;
            }

            /* marginalize over field star status */
            logPostProp += __builtin_log ((1.0 - wdSystems[i].clustStarPriorDens) * fsLike + post[i]);
        }
    }

    if ( ! msSystems.empty() )
    {
        auto msSize = msSystems.size();

        vector<double> post;

        if (settings.noBinaries)
            post = margEvolveNoBinaries (propClust, evoModels, *isochrone, pool, sysVars, sysVar2, sysObs, msSize, howManyFiltsAligned, howManyFilts);
        else
            post = margEvolveWithBinary (propClust, msSystems, evoModels, *isochrone, pool);

        for (size_t i = 0; i < msSize; ++i)
        {
            if (post[i] > 0.0)
            {
                post[i] *= msSystems[i].clustStarPriorDens;
            }
            else
            {
                post[i] = 0.0;
            }

            // marginalize over field star status
            logPostProp += __builtin_log ((1.0 - msSystems[i].clustStarPriorDens) * fsLike + post[i]);
        }
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