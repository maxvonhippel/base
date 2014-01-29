#include <array>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

#include <boost/format.hpp>

#include "Chain.hpp"

#include "constants.hpp"
#include "densities.hpp"
#include "evolve.hpp"
#include "Model.hpp"
#include "Settings.hpp"
#include "WhiteDwarf.hpp"
#include "Utility.hpp"

using std::array;
using std::string;
using std::istringstream;
using std::vector;
using std::cerr;
using std::cout;
using std::endl;
using std::flush;

const int N_AGE = 30;
const int N_FEH = 1;
const int N_MOD = 1;
const int N_ABS = 1;
const int N_Y = 1;
const int N_IFMR_INT = 10;
const int N_IFMR_SLOPE = 10;
const int N_GRID = (N_AGE * N_FEH * N_MOD * N_ABS * N_Y * N_IFMR_INT * N_IFMR_SLOPE);
const int MASTER = 0;       /* taskid of first process */

const int ALLOC_CHUNK = 1;

struct ifmrGridControl
{
    double initialAge;
    double priorMean[NPARAMS];
    double priorVar[NPARAMS];
    double minMag;
    double maxMag;
    int iMag;
    int iStart;
    int modelSet;
    double filterPriorMin[FILTS];
    double filterPriorMax[FILTS];
    int numFilts;
    int nSamples;
    double start[NPARAMS];      /* starting points for grid evaluations */
    double end[NPARAMS];        /* end points for grid evaluations */
};

/* For posterior evaluation on a grid */
struct clustPar
{
    clustPar(double age, double feh, double modulus, double absorption, double ifmrIntercept, double ifmrSlope, double ifmrQuadCoef)
        : age(age), FeH(feh), modulus(modulus), absorption(absorption), ifmrIntercept(ifmrIntercept), ifmrSlope(ifmrSlope), ifmrQuadCoef(ifmrQuadCoef)
    {}

    double age;
    double FeH;
    double modulus;
    double absorption;
    double ifmrIntercept;
    double ifmrSlope;
    double ifmrQuadCoef;
};

typedef struct
{
    double obsPhot[FILTS];
    double variance[FILTS];
    double clustStarPriorDens;  /* cluster membership prior probability */
} obsStar;

/* declare global variables */
array<double, FILTS> filterPriorMin;
array<double, FILTS> filterPriorMax;

/* Used in densities.c. */
double priorMean[NPARAMS], priorVar[NPARAMS];

/* Used by a bunch of different functions. */
vector<int> filters;

/* TEMPORARY - global variable */
double dMass1 = 0.0005;

Settings settings;


/*
 * read control parameters from input stream
 */
static void initIfmrGridControl (Chain *mc, Model &evoModels, struct ifmrGridControl *ctrl, Settings &s)
{
    ctrl->numFilts = 0;

    if (s.whiteDwarf.wdModel == WdModel::MONTGOMERY)
    {
        ctrl->priorMean[CARBONICITY] = mc->clust.carbonicity = mc->clust.priorMean[CARBONICITY] = settings.cluster.carbonicity;
        ctrl->priorVar[CARBONICITY] = settings.cluster.sigma.carbonicity;
    }
    else
    {
        ctrl->priorMean[CARBONICITY] = mc->clust.carbonicity = mc->clust.priorMean[CARBONICITY] = 0.0;
        ctrl->priorVar[CARBONICITY] = 0.0;
    }


    ctrl->priorMean[FEH] = settings.cluster.Fe_H;
    ctrl->priorVar[FEH] = settings.cluster.sigma.Fe_H;
    if (ctrl->priorVar[FEH] < 0.0)
    {
        ctrl->priorVar[FEH] = 0.0;
    }

    ctrl->priorMean[MOD] = settings.cluster.distMod;
    ctrl->priorVar[MOD] = settings.cluster.sigma.distMod;
    if (ctrl->priorVar[MOD] < 0.0)
    {
        ctrl->priorVar[MOD] = 0.0;
    }

    ctrl->priorMean[ABS] = settings.cluster.Av;
    ctrl->priorVar[ABS] = settings.cluster.sigma.Av;
    if (ctrl->priorVar[ABS] < 0.0)
    {
        ctrl->priorVar[ABS] = 0.0;
    }

    ctrl->initialAge = settings.cluster.logClusAge;
    ctrl->priorVar[AGE] = 1.0;

    ctrl->priorVar[IFMR_INTERCEPT] = 1.0;
    ctrl->priorVar[IFMR_SLOPE] = 1.0;
    if (evoModels.IFMR >= 9)
        ctrl->priorVar[IFMR_QUADCOEF] = 1.0;
    else
        ctrl->priorVar[IFMR_QUADCOEF] = 0.0;

    // copy values to global variables
    priorVar[AGE] = ctrl->priorVar[AGE];
    priorVar[FEH] = ctrl->priorVar[FEH];
    priorVar[MOD] = ctrl->priorVar[MOD];
    priorVar[ABS] = ctrl->priorVar[ABS];
    priorVar[IFMR_INTERCEPT] = ctrl->priorVar[IFMR_INTERCEPT];
    priorVar[IFMR_SLOPE] = ctrl->priorVar[IFMR_SLOPE];
    priorVar[IFMR_QUADCOEF] = ctrl->priorVar[IFMR_QUADCOEF];

    priorMean[FEH] = ctrl->priorMean[FEH];
    priorMean[MOD] = ctrl->priorMean[MOD];
    priorMean[ABS] = ctrl->priorMean[ABS];

    /* prior values for linear IFMR */
    ctrl->priorMean[IFMR_SLOPE] = 0.08;
    ctrl->priorMean[IFMR_INTERCEPT] = 0.65;
    ctrl->priorMean[IFMR_QUADCOEF] = 0.0;
    priorMean[IFMR_SLOPE] = ctrl->priorMean[IFMR_SLOPE];
    priorMean[IFMR_INTERCEPT] = ctrl->priorMean[IFMR_INTERCEPT];
    priorMean[IFMR_QUADCOEF] = ctrl->priorMean[IFMR_QUADCOEF];

    /* open model file, choose model set, and load models */

    if (s.mainSequence.msRgbModel == MsModel::CHABHELIUM)
    {
        scanf ("%lf %lf", &ctrl->priorMean[YYY], &ctrl->priorVar[YYY]);

        if (ctrl->priorVar[YYY] < 0.0)
        {
            ctrl->priorVar[YYY] = 0.0;
        }
    }
    else
    {
        ctrl->priorMean[YYY] = 0.0;
        ctrl->priorVar[YYY] = 0.0;
    }
    priorVar[YYY] = ctrl->priorVar[YYY];
    priorMean[YYY] = ctrl->priorMean[YYY];

    /* open files for reading (data) and writing */

    ctrl->minMag = settings.cluster.minMag;
    ctrl->maxMag = settings.cluster.maxMag;
    ctrl->iMag = settings.cluster.index;
    if (ctrl->iMag < 0 || ctrl->iMag > FILTS)
    {
        cerr << "***Error: " << ctrl->iMag << " not a valid magnitude index.  Choose 0, 1,or 2.***" << endl;
        cerr << "[Exiting...]" << endl;
        exit (1);
    }

    ctrl->iStart = 0;

    /* Initialize filter prior mins and maxes */
    int j;

    for (j = 0; j < FILTS; j++)
    {
        ctrl->filterPriorMin[j] = 1000;
        ctrl->filterPriorMax[j] = -1000;
    }
} // initIfmrGridControl

void readCmdData (Chain &mc, struct ifmrGridControl &ctrl, const Model &evoModels)
{
    string line, pch;

    std::ifstream rData;
    rData.open(settings.files.phot);
    if (!rData)
    {
        cerr << "***Error: Photometry file " << settings.files.phot << " was not found.***" << endl;
        cerr << "[Exiting...]" << endl;
        exit (1);
    }

    //Parse the header of the file to determine which filters are being used
    getline(rData, line);  // Read in the header line

    istringstream header(line); // Ignore the first token (which is "id")

    header >> pch;

    while (!header.eof())
    {
        header >> pch;

        if (pch == "sig")
            break;                      // and check to see if they are 'sig'.  If they are, there are no more filters

        for (int filt = 0; filt < FILTS; filt++)
        {                               // Otherwise check to see what this filter's name is
            if (pch == evoModels.filterSet->getFilterName(filt))
            {
                ctrl.numFilts++;
                filters.push_back(filt);
                const_cast<Model&>(evoModels).numFilts++;
                break;
            }
        }
    }

    // This loop reads in photometry data
    // It also reads a best guess for the mass
    mc.stars.clear();

    while (!rData.eof())
    {
        getline(rData, line);

        if (rData.eof())
            break;

        mc.stars.push_back(Star(line, ctrl.numFilts));

        for (int i = 0; i < ctrl.numFilts; i++)
        {
            if (mc.stars.back().obsPhot[i] < ctrl.filterPriorMin[i])
            {
                ctrl.filterPriorMin[i] = mc.stars.back().obsPhot[i];
            }

            if (mc.stars.back().obsPhot[i] > ctrl.filterPriorMax[i])
            {
                ctrl.filterPriorMax[i] = mc.stars.back().obsPhot[i];
            }

            filterPriorMin[i] = ctrl.filterPriorMin[i];
            filterPriorMax[i] = ctrl.filterPriorMax[i];
        }

        if (!(mc.stars.back().status[0] == 3 || (mc.stars.back().obsPhot[ctrl.iMag] >= ctrl.minMag && mc.stars.back().obsPhot[ctrl.iMag] <= ctrl.maxMag)))
        {
            mc.stars.pop_back();
        }
    }

    rData.close();
} /* readCmdData */

/*
 * Read sampled params
 */
static void readSampledParams (struct ifmrGridControl *ctrl, vector<clustPar> &sampledPars, Model &evoModels)
{
    string line;
    std::ifstream parsFile;
    parsFile.open(settings.files.output + ".res");

    getline(parsFile, line); // Eat the header

    while (!parsFile.eof())
    {
        double newAge, newFeh, newMod, newAbs, newIInter, newISlope, newIQuad, ignore;
        newAge = newFeh = newMod = newAbs = newIInter = newISlope = newIQuad = 0.0;

        parsFile >> newAge
                 >> newFeh
                 >> newMod
                 >> newAbs;

        if (evoModels.IFMR >= 4)
        {
            parsFile >> newIInter
                     >> newISlope;
        }

        if (evoModels.IFMR >= 9)
        {
            parsFile >> newIQuad;
        }

        parsFile >> ignore; // logPost

        if (!parsFile.eof())
        {
            sampledPars.emplace_back(newAge, newFeh, newMod, newAbs, newIInter, newISlope, newIQuad);
        }
    }

    parsFile.close();

    ctrl->nSamples = sampledPars.size();
}


/*
 * Initialize chain
 */
static void initChain (Chain *mc, const struct ifmrGridControl *ctrl)
{
    int p;

    for (p = 0; p < NPARAMS; p++)
    {
        mc->acceptClust[p] = mc->rejectClust[p] = 0;
    }

    // If there is no beta in file, initialize everything to prior means
    mc->clust.feh = ctrl->priorMean[FEH];
    mc->clust.mod = ctrl->priorMean[MOD];
    mc->clust.abs = ctrl->priorMean[ABS];
    mc->clust.yyy = ctrl->priorMean[YYY];
    mc->clust.age = ctrl->initialAge;
    mc->clust.ifmrIntercept = ctrl->priorMean[IFMR_INTERCEPT];
    mc->clust.ifmrSlope = ctrl->priorMean[IFMR_SLOPE];
    mc->clust.ifmrQuadCoef = ctrl->priorMean[IFMR_QUADCOEF];
    mc->clust.mean[AGE] = ctrl->initialAge;
    mc->clust.mean[YYY] = ctrl->priorMean[YYY];
    mc->clust.mean[MOD] = ctrl->priorMean[MOD];
    mc->clust.mean[FEH] = ctrl->priorMean[FEH];
    mc->clust.mean[ABS] = ctrl->priorMean[ABS];
    mc->clust.mean[IFMR_INTERCEPT] = ctrl->priorMean[IFMR_INTERCEPT];
    mc->clust.mean[IFMR_SLOPE] = ctrl->priorMean[IFMR_SLOPE];
    mc->clust.mean[IFMR_QUADCOEF] = ctrl->priorMean[IFMR_QUADCOEF];

    int i;

    for (auto star : mc->stars)
    {
        star.meanMassRatio = 0.0;
        star.isFieldStar = 0;
        star.clustStarProposalDens = star.clustStarPriorDens;   // Use prior prob of being clus star
        star.UStepSize = 0.001; // within factor of ~2 for most main sequence stars
        star.massRatioStepSize = 0.001;

        for (i = 0; i < NPARAMS; i++)
        {
            star.beta[i][0] = 0.0;
            star.beta[i][1] = 0.0;
        }
        star.betaMassRatio[0] = 0.0;
        star.betaMassRatio[1] = 0.0;
        star.meanU = 0.0;
        star.varU = 0.0;

        for (i = 0; i < 2; i++)
            star.wdType[i] = WdAtmosphere::DA;

        for (i = 0; i < ctrl->numFilts; i++)
        {
            star.photometry[i] = 0.0;
        }

        // find photometry for initial values of currentClust and mc->stars
        if (star.status[0] == WD)
        {
            star.UStepSize = 0.05;      // use larger initial step size for white dwarfs
            star.massRatio = 0.0;
        }
    }
} // initChain

int main (int argc, char *argv[])
{
    int filt, nWDs = 0, nWDLogPosts;

    Chain mc;
    struct ifmrGridControl ctrl;

    double *wdMass;
    double *clusMemPost;
    double fsLike;
    obsStar *obs;
    int *starStatus;

    vector<clustPar> sampledPars;

    settings.fromCLI (argc, argv);
    if (!settings.files.config.empty())
    {
        settings.fromYaml (settings.files.config);
    }
    else
    {
        settings.fromYaml ("base9.yaml");
    }

    settings.fromCLI (argc, argv);

    std::mt19937 gen(settings.seed * uint32_t(2654435761)); // Applies Knuth's multiplicative hash for obfuscation (TAOCP Vol. 3)
    {
        const int warmupIter = 10000;

        cout << "Warming up generator..." << flush;

        for (auto i = 0; i < warmupIter; ++i)
        {
            std::generate_canonical<double, 10>(gen);
        }

        cout << " Done." << endl;
        cout << "Generated " << warmupIter << " values." << endl;
    }

    Model evoModels = makeModel(settings);

    initIfmrGridControl (&mc, evoModels, &ctrl, settings);

    readCmdData (mc, ctrl, evoModels);

    obs = new obsStar[mc.stars.size()]();
    starStatus = new int[mc.stars.size()]();

    for (decltype(mc.stars.size()) i = 0; i < mc.stars.size(); i++)
    {
        for (filt = 0; filt < ctrl.numFilts; filt++)
        {
            obs[i].obsPhot[filt] = mc.stars.at(i).obsPhot[filt];
            obs[i].variance[filt] = mc.stars.at(i).variance[filt];
        }
        obs[i].clustStarPriorDens = mc.stars.at(i).clustStarPriorDens;
        starStatus[i] = mc.stars.at(i).status[0];

        if (starStatus[i] == WD)
        {
            nWDs++;
        }
    }

    evoModels.numFilts = ctrl.numFilts;

    initChain (&mc, &ctrl);

    for (decltype(mc.stars.size()) i = 0; i < mc.stars.size(); i++)
    {
        mc.stars.at(i).isFieldStar = 0;
    }

    double logFieldStarLikelihood = 0.0;

    for (filt = 0; filt < ctrl.numFilts; filt++)
    {
        logFieldStarLikelihood -= log (ctrl.filterPriorMax[filt] - ctrl.filterPriorMin[filt]);
    }
    fsLike = exp (logFieldStarLikelihood);

    readSampledParams (&ctrl, sampledPars, evoModels);
    cout << "sampledPars[0].age    = " << sampledPars.at(0).age << endl;
    cout << "sampledPars[last].age = " << sampledPars.back().age << endl;

    /* initialize WD logpost array and WD indices */
    nWDLogPosts = (int) ceil ((mc.clust.M_wd_up - 0.15) / dMass1);

    /* try 1D array? */
    if ((wdMass = (double *) calloc (ctrl.nSamples * nWDs, sizeof (double))) == NULL)
        perror ("MEMORY ALLOCATION ERROR \n");

    if ((clusMemPost = (double *) calloc ((ctrl.nSamples * nWDs) + 1, sizeof (double))) == NULL)
        perror ("MEMORY ALLOCATION ERROR \n");

    double *wdLogPost;

    if ((wdLogPost = (double *) calloc (nWDLogPosts + 1, sizeof (double))) == NULL)
        perror ("MEMORY ALLOCATION ERROR \n");

    /********** compile results *********/
    /*** now report sampled masses and parameters ***/

    // Open the file
    string filename = settings.files.output + ".massSamples";

    std::ofstream massSampleFile;
    massSampleFile.open(filename);
    if (!massSampleFile)
    {
        cerr << "***Error: File " << filename << " was not available for writing.***" << endl;
        cerr << "[Exiting...]" << endl;
        exit (1);
    }

    filename += ".membership";

    std::ofstream membershipFile;
    membershipFile.open(filename);
    if (!membershipFile)
    {
        cerr << "***Error: File " << filename << " was not available for writing.***" << endl;
        cerr << "[Exiting...]" << endl;
        exit (1);
    }
    
    mutex theMutex, rndMutex;

    base::utility::ThreadPool pool(settings.threads);

//    for (int m = 0; m < ctrl.nSamples; m++)

    pool.parallelFor(ctrl.nSamples, [=, &theMutex, &rndMutex, &massSampleFile, &membershipFile, &gen](int m)
    {
        Cluster internalCluster(mc.clust);

        internalCluster.age = sampledPars.at(m).age;
        internalCluster.feh = sampledPars.at(m).FeH;
        internalCluster.mod = sampledPars.at(m).modulus;
        internalCluster.abs = sampledPars.at(m).absorption;

        if (evoModels.IFMR >= 4)
        {
            internalCluster.ifmrIntercept = sampledPars.at(m).ifmrIntercept;
            internalCluster.ifmrSlope = sampledPars.at(m).ifmrSlope;
        }

        if (evoModels.IFMR >= 9)
        {
            internalCluster.ifmrQuadCoef = sampledPars.at(m).ifmrQuadCoef;
        }

        /************ sample WD masses for different parameters ************/
        int iWD = 0;
        int im;
        double wdPostSum, maxWDLogPost, mass1;
        double postClusterStar, u;

        internalCluster.AGBt_zmass = evoModels.mainSequenceEvol->deriveAgbTipMass(filters, internalCluster.feh, internalCluster.yyy, internalCluster.age);

        for (auto star : mc.stars)
        {
            if (star.status[0] == WD)
            {
                postClusterStar = 0.0;

                im = 0;

                for (mass1 = 0.15; mass1 < internalCluster.M_wd_up; mass1 += dMass1)
                {
                    /* condition on WD being cluster star */
                    star.U = mass1;
                    star.massRatio = 0.0;

                    try
                    {
                        array<double, FILTS> globalMags;
                        array<double, 2> ltau;
                        evolve (internalCluster, evoModels, globalMags, filters, star, ltau);

                        wdLogPost[im] = logPost1Star (star, internalCluster, evoModels, filterPriorMin, filterPriorMax);
                        postClusterStar += exp (wdLogPost[im]);
                    }
                    catch ( WDBoundsError &e )
                    {
                        cerr << e.what() << endl;

                        wdLogPost[im] = -HUGE_VAL;
                    }

                    im++;
                }
                im = 0;

                /* compute the maximum value */
                maxWDLogPost = wdLogPost[0];
                for (mass1 = 0.15; mass1 < internalCluster.M_wd_up; mass1 += dMass1)
                {
                    if (wdLogPost[im] > maxWDLogPost)
                        maxWDLogPost = wdLogPost[im];
                    im++;
                }

                /* compute the normalizing constant */
                wdPostSum = 0.0;
                im = 0;
                for (mass1 = 0.15; mass1 < internalCluster.M_wd_up; mass1 += dMass1)
                {
                    wdPostSum += exp (wdLogPost[im] - maxWDLogPost);
                    im++;
                }

                /* now sample a particular mass */
                {
                    std::lock_guard<mutex> lk(rndMutex);
                    u = std::generate_canonical<double, 53>(gen);
                }

                double cumSum = 0.0;
                mass1 = 0.15;
                im = 0;
                while (cumSum < u && mass1 < internalCluster.M_wd_up)
                {
                    cumSum += exp (wdLogPost[im] - maxWDLogPost) / wdPostSum;
                    mass1 += dMass1;
                    im++;
                }
                mass1 -= dMass1;        /* maybe not necessary */

                wdMass[m * nWDs + iWD] = mass1;

                postClusterStar *= (internalCluster.M_wd_up - 0.15);

                clusMemPost[m * nWDs + iWD] = star.clustStarPriorDens * postClusterStar / (star.clustStarPriorDens * postClusterStar + (1.0 - star.clustStarPriorDens) * fsLike);
                iWD++;
            }
        }

        std::lock_guard<mutex> lk(theMutex);

        massSampleFile << boost::format("%10.6f") % sampledPars.at(m).age
                       << boost::format("%10.6f") % sampledPars.at(m).FeH
                       << boost::format("%10.6f") % sampledPars.at(m).modulus
                       << boost::format("%10.6f") % sampledPars.at(m).absorption;

        if (evoModels.IFMR >= 4)
        {
            massSampleFile << boost::format("%10.6f") % sampledPars.at(m).ifmrIntercept
                           << boost::format("%10.6f") % sampledPars.at(m).ifmrSlope;
        }

        if (evoModels.IFMR >= 9)
        {
            massSampleFile << boost::format("%10.6f") % sampledPars.at(m).ifmrQuadCoef;
        }

        for (int j = 0; j < nWDs; j++)
        {
            massSampleFile << boost::format("%10.6f") % wdMass[m  * nWDs + j];
            membershipFile << boost::format("%10.6f") % clusMemPost[m * nWDs + j];
        }

        massSampleFile << endl;
        membershipFile << endl;
    });

    massSampleFile.close();
    membershipFile.close();

    cout << "Part 2 completed successfully" << endl;

    /* clean up */
    free (wdLogPost);

    free (wdMass);              /* 1D array */
    free (clusMemPost);         /* 1D array */

    delete[] (obs);
    delete[] (starStatus);

    return 0;
}
