#include <string>
#include <fstream>
#include <iostream>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

#include <mpi.h>
#include <unistd.h>
#include <boost/format.hpp>
#include <gsl/gsl_blas.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_eigen.h>
#include <gsl/gsl_statistics.h>
#include <gsl/gsl_linalg.h>

#include "constants.hpp"
#include "mpiMcmc.hpp"
#include "evolve.hpp"
#include "loadModels.hpp"
#include "msRgbEvol.hpp"
#include "gBergMag.hpp"
#include "wdCooling.hpp"
#include "densities.hpp"
#include "decide.hpp"
#include "samplers.hpp"
#include "leastSquares.hpp"
#include "mt19937ar.hpp"
#include "Settings.hpp"

using std::string;
using std::cout;
using std::cerr;
using std::endl;
using std::vector;

int gsl_linalg_cholesky_decomp (gsl_matrix * A);

double margEvolveWithBinary (struct cluster *pCluster, struct star *pStar);

static void initIfmrMcmcControl (struct chain *mc, struct ifmrMcmcControl *ctrl);
static void readCmdData (struct chain *mc, struct ifmrMcmcControl *ctrl);
static void initChain (struct chain *mc, const struct ifmrMcmcControl *ctrl);
static void initStepSizes (struct cluster *clust);
static void propClustMarg (struct cluster *clust, const struct ifmrMcmcControl *ctrl, const int iteration);
static void propClustBigSteps (struct cluster *clust, const struct ifmrMcmcControl *ctrl);
static void propClustIndep (struct cluster *clust, const struct ifmrMcmcControl *ctrl);
static void propClustCorrelated (struct cluster *clust, const struct ifmrMcmcControl *ctrl);
static int acceptClustMarg (double logPostCurr, double logPostProp);

static void printHeader (struct ifmrMcmcControl * const ctrl);
static void initMassGrids (double *msMass1Grid, double *msMassRatioGrid, double *wdMass1Grid, const struct chain mc);

/*** global variables ***/
/* Used by evolve.c */
double ltau[2];
int aFilt = -1;

/* Used in densities.c. */
double filterPriorMin[FILTS];
double filterPriorMax[FILTS];
double priorMean[NPARAMS], priorVar[NPARAMS];
extern double ageLimit[2];      /* Defined in evolve.c, set in loadModels. */
extern struct globalIso isochrone;

int verbose = 0, needMassNow = 0, useFilt[FILTS], numFilts = 0;

Settings settings;

/* For random number generator (mt19937ar.c) */
unsigned long mt[NN];
int mti = NN + 1;

int main (int argc, char *argv[])
{
    int i, j, p, filt, iteration, numtasks,     /* total number of MPI process in partitiion */
        numworkers,                     /* number of worker tasks */
        taskid,
        dest,                   /* destination task id to send message */
        index,                  /* index into the array */
        source,                 /* origin task id of message */
        chunksize,                      /* for partitioning the array of stars */
        extra, minchunk, accept = 0, reject = 0, doAccept;

    double logPostCurr;
    double logPostProp;
    double *logPostEachStar;
    double postClusterStar;

    struct chain mc;
    struct ifmrMcmcControl ctrl;
    struct cluster propClustMaster;
    struct cluster propClustWorker;

    double fsLike;
    struct obsStar *obs = 0;    //initialized *obs to 0
    int *starStatus = 0;                //initialized to 0
    double msMass1Grid[N_MS_MASS1 * N_MS_MASS_RATIO];
    double msMassRatioGrid[N_MS_MASS1 * N_MS_MASS_RATIO];
    double wdMass1Grid[N_WD_MASS1];

    /* arrays to evolve all copies of each star simultaneously */
    vector<struct star> wd(N_WD_MASS1);

    MPI_Datatype obsStarType;
    MPI_Status status;

    MPI_Init (&argc, &argv);
    MPI_Comm_rank (MPI_COMM_WORLD, &taskid);
    MPI_Comm_size (MPI_COMM_WORLD, &numtasks);
    MPI_Type_contiguous (2 * FILTS + 1, MPI_DOUBLE, &obsStarType);
    MPI_Type_commit (&obsStarType);

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

    initCluster (&(mc.clust));
    initCluster (&propClustWorker);
    initCluster (&propClustMaster);
    initStepSizes (&mc.clust);

    if (taskid == MASTER)
    {
        initIfmrMcmcControl (&mc, &ctrl);
    }
    else
    {
        ctrl.verbose = 0;
        ctrl.iStart = 0;
    }

    /* /\*** broadcast control parameters to other processes ***\/ */
    if (taskid != MASTER)
    {
        init_genrand (settings.seed);
    }

    MPI_Bcast (&mc.clust.evoModels.WDcooling, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.evoModels.mainSequenceEvol, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.evoModels.filterSet, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.evoModels.brownDwarfEvol, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.evoModels.IFMR, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.carbonicity, 1, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);

    mc.clust.evoModels.WDatm = BERGERON;

    if (taskid != MASTER)
    {                           /* already loaded in the MASTER task */
        if (mc.clust.evoModels.brownDwarfEvol == BARAFFE)
            loadBaraffe (settings.files.models);
        loadMSRgbModels (&mc.clust, settings.files.models, 0);
        loadWDCool (settings.files.models, mc.clust.evoModels.WDcooling);
        loadBergeron (settings.files.models, mc.clust.evoModels.filterSet);
    }

    MPI_Bcast (ctrl.priorVar, NPARAMS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (ctrl.priorMean, NPARAMS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (priorVar, NPARAMS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (priorMean, NPARAMS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);

    for (p = 0; p < NPARAMS; p++)
    {
        mc.clust.priorVar[p] = ctrl.priorVar[p];
        mc.clust.priorMean[p] = ctrl.priorMean[p];
    }

    if (taskid == MASTER)
    {
        readCmdData (&mc, &ctrl);

        obs = new struct obsStar[mc.clust.nStars]();
        starStatus = new int[mc.clust.nStars]();

        for (i = 0; i < mc.clust.nStars; i++)
        {
            for (filt = 0; filt < ctrl.numFilts; filt++)
            {
                obs[i].obsPhot[filt] = mc.stars[i].obsPhot[filt];
                obs[i].variance[filt] = mc.stars[i].variance[filt];
            }
            obs[i].clustStarPriorDens = mc.stars[i].clustStarPriorDens;
            starStatus[i] = mc.stars[i].status[0];
        }

    }

    MPI_Bcast (&ctrl.numFilts, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (ctrl.useFilt, FILTS, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (useFilt, FILTS, MPI_INT, MASTER, MPI_COMM_WORLD);
    mc.clust.evoModels.numFilts = ctrl.numFilts;
    numFilts = ctrl.numFilts;
    MPI_Bcast (ctrl.filterPriorMin, FILTS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (ctrl.filterPriorMax, FILTS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (filterPriorMin, FILTS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (filterPriorMax, FILTS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.nStars, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&ctrl.burnIter, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&ctrl.nIter, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&ctrl.thin, 1, MPI_INT, MASTER, MPI_COMM_WORLD);


    MPI_Barrier (MPI_COMM_WORLD);
    if (taskid != MASTER)
    {
        obs = new struct obsStar[mc.clust.nStars]();
        starStatus = new int[mc.clust.nStars]();
    }

    MPI_Bcast (starStatus, mc.clust.nStars, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (obs, mc.clust.nStars, obsStarType, MASTER, MPI_COMM_WORLD);
    if (taskid != MASTER)
    {
        /* initialize the stars array */
        mc.stars = vector<struct star>(mc.clust.nStars);

        for (i = 0; i < mc.clust.nStars; i++)
        {
            for (filt = 0; filt < ctrl.numFilts; filt++)
            {
                mc.stars[i].obsPhot[filt] = obs[i].obsPhot[filt];
                mc.stars[i].variance[filt] = obs[i].variance[filt];
            }
            mc.stars[i].clustStarPriorDens = obs[i].clustStarPriorDens;
            mc.stars[i].status[0] = starStatus[i];
        }
    }

    initChain (&mc, &ctrl);

    for (i = 0; i < mc.clust.nStars; i++)
    {
        mc.stars[i].isFieldStar = 0;
        mc.stars[i].boundsFlag = 0;
    }

    numworkers = numtasks - 1;
    minchunk = mc.clust.nStars / numworkers;
    extra = mc.clust.nStars % numworkers;

    logPostEachStar = new double[mc.clust.nStars]();

    initMassGrids (msMass1Grid, msMassRatioGrid, wdMass1Grid, mc);

    double logFieldStarLikelihood = 0.0;

    if (mc.clust.nStars > 1)
    {
        for (filt = 0; filt < ctrl.numFilts; filt++)
        {
            logFieldStarLikelihood -= log (ctrl.filterPriorMax[filt] - ctrl.filterPriorMin[filt]);
        }
        fsLike = exp (logFieldStarLikelihood);
    }
    else
    {
        logFieldStarLikelihood = -HUGE_VAL;
        fsLike = 0;
    }

    initCluster (&propClustWorker);
    initCluster (&propClustMaster);

    /**************************** master task ************************************/
    if (taskid == MASTER)
    {
        cout << "Bayesian analysis of stellar evolution" << endl;

        /* open output files */
        ctrl.resFile.open(ctrl.clusterFilename);
        if (!ctrl.resFile)
        {
            cerr << "***Error: File " << ctrl.clusterFilename << " was not available for writing.***" << endl;
            cerr << "[Exiting...]" << endl;
            exit (1);
        }

        ctrl.clusterFilename += ".burnin";
        ctrl.burninFile.open(ctrl.clusterFilename);
        if (!ctrl.burninFile)
        {
            cerr << "***Error: File " << ctrl.clusterFilename << " was not available for writing.***" << endl;
            cerr << "[Exiting...]" << endl;
            exit (1);
        }
        printHeader (&ctrl);
    }

    /* set current log posterior to -HUGE_VAL */
    /* will cause random starting value */
    logPostCurr = -HUGE_VAL;

    MPI_Barrier (MPI_COMM_WORLD);

    /* estimate covariance matrix for more efficient Metropolis updates */
    int nSave = 10;             /*changed from 100 to 10 */
    int increment = ctrl.burnIter / (2 * nSave);
    double **params;

    params = new double*[NPARAMS]();

    for (p = 0; p < NPARAMS; p++)
    {
        params[p] = new double[nSave]();
    }
    double cov;

    int nParamsUsed = 0;

    for (p = 0; p < NPARAMS; p++)
    {
        if (ctrl.priorVar[p] > EPSILON)
        {
            nParamsUsed++;
        }
    }

    /********* MAIN LOOP *********/
    for (iteration = 0; iteration < ctrl.burnIter + ctrl.nIter * ctrl.thin; iteration++)
    {
        propClustMaster = mc.clust;
        // propClustWorker = mc.clust;
        if (taskid == MASTER)
        {
            /* propose and broadcast new value */
            propClustMarg (&propClustMaster, &ctrl, iteration);
            logPostProp = logPriorClust (&propClustMaster);

            /**************************** master task ************************************/
            /* send assignments to workers */
            /* send which star */
            index = 0;
            for (dest = 1; dest <= numworkers; dest++)
            {
                if (dest <= extra)
                    chunksize = minchunk + 1;
                else
                    chunksize = minchunk;

                MPI_Send (&index, 1, MPI_INT, dest, 0, MPI_COMM_WORLD);
                MPI_Send (&chunksize, 1, MPI_INT, dest, 0, MPI_COMM_WORLD);
                MPI_Send (&propClustMaster.parameter, NPARAMS, MPI_DOUBLE, dest, 0, MPI_COMM_WORLD);

                index = index + chunksize;
            }
            /* receive calculated logPosts */
            for (i = 1; i <= numworkers; i++)
            {
                source = i;
                MPI_Recv (&index, 1, MPI_INT, source, 1, MPI_COMM_WORLD, &status);
                MPI_Recv (&chunksize, 1, MPI_INT, source, 1, MPI_COMM_WORLD, &status);
                MPI_Recv (&logPostEachStar[index], chunksize, MPI_DOUBLE, source, 1, MPI_COMM_WORLD, &status);
            }
            for (i = 0; i < mc.clust.nStars; i++)
            {
                logPostProp += logPostEachStar[i];
            }

            /* accept/reject */
            doAccept = acceptClustMarg (logPostCurr, logPostProp);
            if (doAccept)
            {
//                puts("Accepted");
                mc.clust = propClustMaster;
                logPostCurr = logPostProp;
                accept++;
            }
            else
            {
                reject++;
            }
            /* save draws to estimate covariance matrix for more efficient Metropolis */
            if (iteration >= ctrl.burnIter / 2 && iteration < ctrl.burnIter)
            {
                if (iteration % increment == 0)
                {
                    /* save draws */
                    for (p = 0; p < NPARAMS; p++)
                    {
                        if (ctrl.priorVar[p] > EPSILON)
                        {
                            params[p][(iteration - ctrl.burnIter / 2) / increment] = mc.clust.parameter[p];
                        }
                    }
                }
                if (iteration == ctrl.burnIter - 1)
                {
                    /* compute Cholesky decomposition of covariance matrix */
                    int h, k;
                    gsl_matrix *covMat = gsl_matrix_alloc (nParamsUsed, nParamsUsed);

                    h = 0;

                    double cholScale = 1000;    /* for numerical stability */

//                    exit(0);

                    for (i = 0; i < NPARAMS; i++)
                    {
                        if (ctrl.priorVar[i] > EPSILON)
                        {
                            k = 0;
                            for (j = 0; j < NPARAMS; j++)
                            {
                                if (ctrl.priorVar[j] > EPSILON)
                                {
                                    cov = gsl_stats_covariance (params[i], 1, params[j], 1, nSave);
                                    gsl_matrix_set (covMat, h, k, cov * cholScale * cholScale); /* for numerical stability? */

                                    if (h != k)
                                    {
                                        gsl_matrix_set (covMat, k, h, cov * cholScale * cholScale);
                                    }

                                    k++;
                                }
                            }
                            h++;
                        }
                    }

                    for (i = 0; i < nParamsUsed; i++)
                    {
                        for (j = 0; j < nParamsUsed; j++)
                        {
                            cout << gsl_matrix_get (covMat, i, j) << " ";
                        }
                        cout << endl;
                    }
                    fflush (stdout);

                    /* Cholesky decomposition */
                    gsl_linalg_cholesky_decomp (covMat);

                    /* compute proposal matrix from Cholesky factor */

                    /* Gelman, Roberts, Gilks scale */
                    double GRGscale = 0.97;     /* = 2.38 / sqrt(6) */

                    h = 0;
                    for (i = 0; i < NPARAMS; i++)
                    {
                        if (ctrl.priorVar[i] > EPSILON)
                        {
                            k = 0;
                            for (j = 0; j < NPARAMS; j++)
                            {
                                if (ctrl.priorVar[j] > EPSILON)
                                {
                                    if (j <= i)
                                    {
                                        ctrl.propMatrix[i][j] = GRGscale * gsl_matrix_get (covMat, h, k) / cholScale;
                                    }
                                    else
                                    {
                                        ctrl.propMatrix[i][j] = 0.0;
                                    }
                                    k++;
                                }
                                else
                                {
                                    ctrl.propMatrix[i][j] = 0.0;
                                }
                            }
                            h++;
                        }
                        else
                        {
                            for (j = 0; j < NPARAMS; j++)
                            {
                                ctrl.propMatrix[i][j] = 0.0;
                            }
                        }
                    }

                    MPI_Bcast (&(ctrl.propMatrix[0][0]), NPARAMS * NPARAMS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);
                }
            }

            /* Write output */
            if (iteration < ctrl.burnIter)
            {
                for (p = 0; p < NPARAMS; p++)
                {
                    if (ctrl.priorVar[p] > EPS || p == FEH || p == MOD || p == ABS)
                    {
                        ctrl.burninFile << boost::format("%10.6f ") % mc.clust.parameter[p];
                    }
                }

                ctrl.burninFile << boost::format("%10.6f") % logPostCurr << endl;
            }
            else if (iteration % ctrl.thin == 0)
            {
                for (p = 0; p < NPARAMS; p++)
                {
                    if (ctrl.priorVar[p] > EPS || p == FEH || p == MOD || p == ABS)
                    {
                        ctrl.resFile << boost::format("%10.6f ") % mc.clust.parameter[p];
                    }
                }
                ctrl.resFile << boost::format("%10.6f") % logPostCurr << endl;
            }
        }
        else
        {
            /**************************** worker task ************************************/
            /* receive assigned observed star and cluster parameter values */
            propClustWorker = mc.clust;

            source = MASTER;
            MPI_Recv (&index, 1, MPI_INT, source, 0, MPI_COMM_WORLD, &status);
            MPI_Recv (&chunksize, 1, MPI_INT, source, 0, MPI_COMM_WORLD, &status);
            MPI_Recv (&propClustWorker.parameter, NPARAMS, MPI_DOUBLE, source, 0, MPI_COMM_WORLD, &status);


            logPostProp = logPriorClust (&propClustWorker);
            if (fabs (logPostProp + HUGE_VAL) < EPS)
            {
                /* don't bother computing, already know this cluster will be rejected */
                for (i = index; i < index + chunksize; i++)
                {
                    logPostEachStar[i] = -HUGE_VAL;
                }
            }
            else
            {
                /* loop over assigned stars */
                for (i = index; i < index + chunksize; i++)
                {
                    /* loop over all (mass1, mass ratio) pairs */
                    if (mc.stars[i].status[0] == WD)
                    {

                        postClusterStar = 0.0;
                        double tmpLogPost;

                        for (j = 0; j < N_WD_MASS1; j++)
                        {
                            wd[j] = mc.stars[i];
                            wd[j].boundsFlag = 0;
                            wd[j].isFieldStar = 0;
                            wd[j].U = wdMass1Grid[j];
                            wd[j].massRatio = 0.0;
                            evolve (&propClustWorker, wd, j);

                            if (wd[j].boundsFlag)
                            {
                                cerr <<"**wd[" << j << "].boundsFlag" << endl;
                            }
                            else
                            {
                                tmpLogPost = logPost1Star (&wd[j], &propClustWorker);
                                tmpLogPost += log ((mc.clust.M_wd_up - MIN_MASS1) / (double) N_WD_MASS1);

                                postClusterStar += exp (tmpLogPost);
                            }
                        }

                        postClusterStar *= mc.stars[i].clustStarPriorDens;
                    }
                    else
                    {
                        /* marginalize over isochrone */
                        postClusterStar = margEvolveWithBinary (&propClustWorker, &mc.stars[i]);
                        postClusterStar *= mc.stars[i].clustStarPriorDens;
                    }

                    /* marginalize over field star status */
                    logPostEachStar[i] = log ((1.0 - mc.stars[i].clustStarPriorDens) * fsLike + postClusterStar);
                }
            }


            /* Send results back to the master task */
            MPI_Send (&index, 1, MPI_INT, MASTER, 1, MPI_COMM_WORLD);
            MPI_Send (&chunksize, 1, MPI_INT, MASTER, 1, MPI_COMM_WORLD);
            /* return log posterior */
            MPI_Send (&logPostEachStar[index], chunksize, MPI_DOUBLE, MASTER, 1, MPI_COMM_WORLD);

        }         /********** end worker task *********/
    }
    /********* END MAIN LOOP *********/


    if (taskid == MASTER)
    {
        ctrl.resFile.close();
        ctrl.burninFile.close();
        cout << "Acceptance ratio: " << (double) accept / (accept + reject) << endl;
    }


    /* clean up */
    delete[] obs;
    delete[] starStatus;

    freeGlobalIso (&isochrone);

    for (p = 0; p < NPARAMS; p++)
    {
        free (params[p]);
    }
    free (params);
    // MPI_Type_free(&clustParType);
    MPI_Type_free (&obsStarType);
    free (logPostEachStar);
    MPI_Finalize ();

    return 0;
}

/*******************************************
********************************************
** END MAIN FUNCTION
********************************************
*******************************************/

static void initStepSizes (struct cluster *clust)
{
    // clust->stepSize[AGE]    = 0.08;
    // clust->stepSize[FEH]    = 0.02;
    // clust->stepSize[MOD]    = 0.02;
    // clust->stepSize[ABS]    = 0.004;
    // clust->stepSize[YYY]    = 0.002;

    clust->stepSize[AGE] = 0.005;
    clust->stepSize[FEH] = 0.005;
    clust->stepSize[MOD] = 0.005;
    clust->stepSize[ABS] = 0.002;
    clust->stepSize[YYY] = 0.002;
    clust->stepSize[IFMR_INTERCEPT] = 0.01;
    clust->stepSize[IFMR_SLOPE] = 0.008;
    clust->stepSize[IFMR_QUADCOEF] = 0.008;     // ?

    /*clust->stepSize[AGE]    = 0.005;
      clust->stepSize[FEH]    = 0.01;
      clust->stepSize[MOD]    = 0.02;
      clust->stepSize[ABS]    = 0.01;
      clust->stepSize[YYY]    = 0.001;
      clust->stepSize[IFMR_INTERCEPT]  = 0.02;
      clust->stepSize[IFMR_SLOPE]      = 0.03;
    */
}



/* Decides whether to accept a proposed cluster property */
static int acceptClustMarg (double logPostCurr, double logPostProp)
{
    if (isinf (logPostProp))
    {
        puts ("-Inf posterior proposed and rejected");
        return 0;
    }

    double alpha = logPostProp - logPostCurr;

    if (alpha >= 0)             // Short circuit exit to the MH algorithm
    {
        return 1;
    }

    double u = genrand_res53 ();

    if (u < 1.e-15)
        u = 1.e-15;
    u = log (u);

    if (u < alpha)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}


/*
 * read control parameters from input stream
 */
static void initIfmrMcmcControl (struct chain *mc, struct ifmrMcmcControl *ctrl)
{

    double priorSigma;

    ctrl->verbose = 0;
    ctrl->numFilts = 0;

    int ii;

    for (ii = 0; ii < FILTS; ii++)
        ctrl->useFilt[ii] = 0;

    /* Read number of steps, burn-in details, random seed */
    init_genrand (settings.seed);

    /* load models */
    loadModels (0, &mc->clust, settings);

    ctrl->priorMean[FEH] = settings.cluster.Fe_H;
    priorSigma = settings.cluster.sigma.Fe_H;

    if (priorSigma < 0.0)
    {
        priorSigma = 0.0;
    }
    ctrl->priorVar[FEH] = priorSigma * priorSigma;

    ctrl->priorMean[MOD] = settings.cluster.distMod;
    priorSigma = settings.cluster.sigma.distMod;

    if (priorSigma < 0.0)
    {
        priorSigma = 0.0;
    }
    ctrl->priorVar[MOD] = priorSigma * priorSigma;

    ctrl->priorMean[ABS] = settings.cluster.Av;
    priorSigma = settings.cluster.sigma.Av;

    if (priorSigma < 0.0)
    {
        priorSigma = 0.0;
    }
    ctrl->priorVar[ABS] = priorSigma * priorSigma;

    ctrl->initialAge = settings.cluster.logClusAge;
    ctrl->priorVar[AGE] = 1.0;

    if (mc->clust.evoModels.IFMR <= 3)
    {
        ctrl->priorVar[IFMR_SLOPE] = 0.0;
        ctrl->priorVar[IFMR_INTERCEPT] = 0.0;
        ctrl->priorVar[IFMR_QUADCOEF] = 0.0;
    }
    else if (mc->clust.evoModels.IFMR <= 8)
    {
        ctrl->priorVar[IFMR_SLOPE] = 1.0;
        ctrl->priorVar[IFMR_INTERCEPT] = 1.0;
        ctrl->priorVar[IFMR_QUADCOEF] = 0.0;
    }
    else
    {
        ctrl->priorVar[IFMR_SLOPE] = 1.0;
        ctrl->priorVar[IFMR_INTERCEPT] = 1.0;
        ctrl->priorVar[IFMR_QUADCOEF] = 1.0;
    }

    // copy values to global variables
    priorVar[AGE] = ctrl->priorVar[AGE];
    priorVar[FEH] = ctrl->priorVar[FEH];
    priorVar[MOD] = ctrl->priorVar[MOD];
    priorVar[ABS] = ctrl->priorVar[ABS];
    priorVar[IFMR_SLOPE] = ctrl->priorVar[IFMR_SLOPE];
    priorVar[IFMR_INTERCEPT] = ctrl->priorVar[IFMR_INTERCEPT];
    priorVar[IFMR_QUADCOEF] = ctrl->priorVar[IFMR_QUADCOEF];

    priorMean[FEH] = ctrl->priorMean[FEH];
    priorMean[MOD] = ctrl->priorMean[MOD];
    priorMean[ABS] = ctrl->priorMean[ABS];

    /* set starting values for IFMR parameters */
    ctrl->priorMean[IFMR_SLOPE] = 0.08;
    ctrl->priorMean[IFMR_INTERCEPT] = 0.65;
    if (mc->clust.evoModels.IFMR <= 10)
        ctrl->priorMean[IFMR_QUADCOEF] = 0.0001;
    else
        ctrl->priorMean[IFMR_QUADCOEF] = 0.08;
    priorMean[IFMR_SLOPE] = ctrl->priorMean[IFMR_SLOPE];
    priorMean[IFMR_INTERCEPT] = ctrl->priorMean[IFMR_INTERCEPT];
    priorMean[IFMR_QUADCOEF] = ctrl->priorMean[IFMR_QUADCOEF];

    /* open model file, choose model set, and load models */
    if (mc->clust.evoModels.mainSequenceEvol == CHABHELIUM)
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

    /* read burnIter and nIter */
    ctrl->burnIter = settings.mpiMcmc.burnIter;
    ctrl->nIter = settings.mpiMcmc.maxIter;
    ctrl->thin = settings.mpiMcmc.thin;

    /* open files for reading (data) and writing */
    string filename;

    filename = settings.files.phot;
    ctrl->rData.open(filename);
    if (!ctrl->rData)
    {
        cerr << "***Error: Photometry file " << filename << " was not found.***" << endl;
        cerr << "[Exiting...]" << endl;
        exit (1);
    }

    ctrl->minMag = settings.cluster.minMag;
    ctrl->maxMag = settings.cluster.maxMag;
    ctrl->iMag = settings.cluster.index;

    if (ctrl->iMag < 0 || ctrl->iMag > FILTS)
    {
        cerr << "***Error: " << ctrl->iMag << " not a valid magnitude index.  Choose 0, 1,or 2.***" << endl;
        cerr << "[Exiting...]" << endl;
        exit (1);
    }

    ctrl->clusterFilename = settings.files.output + ".res";

    ctrl->iStart = 0;

    /* Initialize filter prior mins and maxes */
    int j;

    for (j = 0; j < FILTS; j++)
    {
        ctrl->filterPriorMin[j] = 1000;
        ctrl->filterPriorMax[j] = -1000;
    }

}                               /* initIfmrMcmcControl */


/*
 * Read data
 */
static void readCmdData (struct chain *mc, struct ifmrMcmcControl *ctrl)
{
    char line[300];
    double tempSigma;
    int filt, i;
    char *pch, sig[] = "sig", comp[] = "   ";

    //Parse the header of the file to determine which filters are being used
    ctrl->rData.getline(line, 300);     // Read in the header line

    pch = strtok (line, " ");   // split the string on these delimiters into "tokens"

    while (pch != NULL)
    {
        pch = strtok (NULL, " ");       // Ignore the first token (which is "id") and move
        // to the next (which should be the first filter name)
        strncpy (comp, pch, 3); // copy the first three letters into the dummy string 'comp'
        if (strcmp (comp, sig) == 0)
            break;                      // and check to see if they are 'sig'.  If they are, there are no more filters

        for (filt = 0; filt < FILTS; filt++)
        {                               // Otherwise check to see what this filter's name is
            if (strcmp (pch, getFilterName (filt)) == 0)
            {
                ctrl->useFilt[filt] = 1;
                mc->clust.evoModels.numFilts++;
                if (aFilt < 0)
                    aFilt = filt;               // Sets this to a band we know we are using (for evolve)
                break;
            }
        }
    }

    for (i = 0; i < FILTS; i++)
    {
        if (ctrl->useFilt[i])
        {
            ctrl->numFilts++;
            if (aFilt < 0)
                aFilt = i;              // Sets this to a band we know we are using (for evolve)
        }
    }

    // This loop reads in photometry data
    // It also reads a best guess for the mass
    int j = 0;
    int moreStars = 1;          // true

    // why is this necessary???
    mc->stars.clear();

    while (moreStars)
    {
        mc->stars.emplace_back();

        ctrl->rData >> line;

        if (ctrl->rData.eof())
            break;

        for (i = 0; i < ctrl->numFilts; i++)
        {
            ctrl->rData >> mc->stars[j].obsPhot[i];

            if (mc->stars[j].obsPhot[i] < ctrl->filterPriorMin[i])
            {
                ctrl->filterPriorMin[i] = mc->stars[j].obsPhot[i];
            }

            if (mc->stars[j].obsPhot[i] > ctrl->filterPriorMax[i])
            {
                ctrl->filterPriorMax[i] = mc->stars[j].obsPhot[i];
            }
        }

        // copy to global variables
        for (i = 0; i < ctrl->numFilts; i++)
        {
            filterPriorMin[i] = ctrl->filterPriorMin[i];
            filterPriorMax[i] = ctrl->filterPriorMax[i];
        }
        for (i = 0; i < ctrl->numFilts; i++)
        {
            ctrl->rData >> tempSigma;
            mc->stars[j].variance[i] = tempSigma * fabs (tempSigma);
            // The fabs() keeps the sign of the variance the same as that input by the user for sigma
            // Negative sigma (variance) is used to signal "don't count this band for this star"
        }

        ctrl->rData >> mc->stars[j].U >> mc->stars[j].massRatio >> mc->stars[j].status[0] >> mc->stars[j].clustStarPriorDens >> mc->stars[j].useDuringBurnIn;

        if (mc->stars[j].status[0] == 3 || (mc->stars[j].obsPhot[ctrl->iMag] >= ctrl->minMag && mc->stars[j].obsPhot[ctrl->iMag] <= ctrl->maxMag))
        {
            j++;
        }
    }
    mc->clust.nStars = j;

    for (j = 0; j < mc->clust.nStars; j++)
    {
        mc->stars[j].massRatio = 0.0;
    }

    // copy to global values
    for (i = 0; i < FILTS; i++)
    {
        useFilt[i] = ctrl->useFilt[i];
    }
    numFilts = ctrl->numFilts;
} /* readCmdData */




/*
 * Initialize chain
 */
static void initChain (struct chain *mc, const struct ifmrMcmcControl *ctrl)
{
    int p;

    for (p = 0; p < NPARAMS; p++)
    {
        mc->acceptClust[p] = mc->rejectClust[p] = 0;
    }

    // mc->eigenAllocated = 0;

    mc->clust.parameter[FEH] = ctrl->priorMean[FEH];
    mc->clust.parameter[MOD] = ctrl->priorMean[MOD];
    mc->clust.parameter[ABS] = ctrl->priorMean[ABS];
    mc->clust.parameter[YYY] = ctrl->priorMean[YYY];
    mc->clust.parameter[AGE] = ctrl->initialAge;
    mc->clust.mean[AGE] = ctrl->initialAge;
    mc->clust.mean[YYY] = ctrl->priorMean[YYY];
    mc->clust.mean[MOD] = ctrl->priorMean[MOD];
    mc->clust.mean[FEH] = ctrl->priorMean[FEH];
    mc->clust.mean[ABS] = ctrl->priorMean[ABS];
    mc->clust.betamabs = 0.0;
    mc->clust.betaFabs = 0.0;

    /* IFMR parameters */
    mc->clust.parameter[IFMR_SLOPE] = ctrl->priorMean[IFMR_SLOPE];
    mc->clust.parameter[IFMR_INTERCEPT] = ctrl->priorMean[IFMR_INTERCEPT];
    mc->clust.parameter[IFMR_QUADCOEF] = ctrl->priorMean[IFMR_QUADCOEF];
    mc->clust.mean[IFMR_SLOPE] = ctrl->priorMean[IFMR_SLOPE];
    mc->clust.mean[IFMR_INTERCEPT] = ctrl->priorMean[IFMR_INTERCEPT];
    mc->clust.mean[IFMR_QUADCOEF] = ctrl->priorMean[IFMR_QUADCOEF];


    int i, j;

    for (j = 0; j < mc->clust.nStars; j++)
    {
        mc->stars[j].meanMassRatio = 0.0;
        mc->stars[j].isFieldStar = 0;
        mc->stars[j].clustStarProposalDens = mc->stars[j].clustStarPriorDens;   // Use prior prob of being clus star
        mc->stars[j].UStepSize = 0.001; // within factor of ~2 for most main sequence stars
        mc->stars[j].massRatioStepSize = 0.001;

        for (i = 0; i < NPARAMS; i++)
        {
            mc->stars[j].beta[i][0] = 0.0;
            mc->stars[j].beta[i][1] = 0.0;
        }

        mc->stars[j].betaMassRatio[0] = 0.0;
        mc->stars[j].betaMassRatio[1] = 0.0;
        mc->stars[j].meanU = 0.0;
        mc->stars[j].varU = 0.0;

        for (i = 0; i < 2; i++)
            mc->stars[j].wdType[i] = 0;

        for (i = 0; i < numFilts; i++)
        {
            mc->stars[j].photometry[i] = 0.0;
        }

        // find photometry for initial values of currentClust and mc->stars
        evolve (&mc->clust, mc->stars, j);

        if (mc->stars[j].status[0] == WD)
        {
            mc->stars[j].UStepSize = 0.05;      // use larger initial step size for white dwarfs
            mc->stars[j].massRatio = 0.0;
        }
    }
} /* initChain */



static void propClustMarg (struct cluster *clust, const struct ifmrMcmcControl *ctrl, const int iteration)
{
    if (iteration < ctrl->burnIter / 2)
    {
        propClustBigSteps (clust, ctrl);
    }
    else if (iteration < ctrl->burnIter)
    {
        propClustIndep (clust, ctrl);
    }
    else
    {
        propClustCorrelated (clust, ctrl);
    }

    /* reflect */
    if (iteration < ctrl->burnIter)
    {
        if (ctrl->priorVar[ABS] > EPSILON)
        {
            clust->parameter[ABS] = fabs (clust->parameter[ABS]);
        }
        if (ctrl->priorVar[IFMR_SLOPE] > EPSILON)
        {
            clust->parameter[IFMR_SLOPE] = fabs (clust->parameter[IFMR_SLOPE]);
        }
    }
}

static void propClustBigSteps (struct cluster *clust, const struct ifmrMcmcControl *ctrl)
{
    /* DOF defined in densities.h */
    double scale = 5.0;
    int p;

    for (p = 0; p < NPARAMS; p++)
    {
        if (ctrl->priorVar[p] > EPSILON)
        {
            clust->parameter[p] += sampleT (scale * scale * clust->stepSize[p] * clust->stepSize[p], DOF);
        }
    }
}

static void propClustIndep (struct cluster *clust, const struct ifmrMcmcControl *ctrl)
{
    /* DOF defined in densities.h */
    int p;

    for (p = 0; p < NPARAMS; p++)
    {
        if (ctrl->priorVar[p] > EPSILON)
        {
            clust->parameter[p] += sampleT (clust->stepSize[p] * clust->stepSize[p], DOF);
        }
    }
}

static void propClustCorrelated (struct cluster *clust, const struct ifmrMcmcControl *ctrl)
{
    /* DOF defined in densities.h */
    double indepProps[NPARAMS] = { 0.0 };
    double corrProps[NPARAMS] = { 0.0 };

    int p, k;

    for (p = 0; p < NPARAMS; p++)
    {
        if (ctrl->priorVar[p] > EPSILON)
        {
            indepProps[p] = sampleT (1.0, DOF);
        }
    }
    for (p = 0; p < NPARAMS; p++)
    {
        if (ctrl->priorVar[p] > EPSILON)
        {
            for (k = 0; k <= p; k++)
            {                           /* propMatrix is lower diagonal */
                if (ctrl->priorVar[k] > EPSILON)
                {
                    corrProps[p] += ctrl->propMatrix[p][k] * indepProps[k];
                }
            }
            clust->parameter[p] += corrProps[p];
        }
    }
}


static void printHeader (struct ifmrMcmcControl * const ctrl)
{
    const char *paramNames[] = { "    logAge",
                                 "         Y",
                                 "       FeH",
                                 "   modulus",
                                 "absorption",
                                 " IFMRconst",
                                 "   IFMRlin",
                                 "  IFMRquad"
    };
    int p;

    for (p = 0; p < NPARAMS; p++)
    {
        if (ctrl->priorVar[p] > EPSILON || p == MOD || p == FEH || p == ABS)
        {
            ctrl->resFile << paramNames[p] << ' ';
            ctrl->burninFile << paramNames[p] << ' ';
        }
    }
    ctrl->resFile << "logPost" << endl;
    ctrl->burninFile << "logPost" << endl;
}



static void initMassGrids (double *msMass1Grid, double *msMassRatioGrid, double *wdMass1Grid, const struct chain mc)
{
    double maxMass1 = mc.clust.M_wd_up;
    double mass1, massRatio;
    double dMsMass1 = (maxMass1 - MIN_MASS1) / (double) N_MS_MASS1;
    double dMsMassRatio = 1.0 / (double) N_MS_MASS_RATIO;
    double dWdMass1 = (maxMass1 - MIN_MASS1) / (double) N_WD_MASS1;

    int i = 0;

    for (mass1 = MIN_MASS1; mass1 < maxMass1; mass1 += dMsMass1)
    {
        for (massRatio = 0.0; massRatio < 1.0; massRatio += dMsMassRatio)
        {
            msMass1Grid[i] = mass1;
            msMassRatioGrid[i] = massRatio;
            i++;
        }
    }

    i = 0;
    for (mass1 = MIN_MASS1; mass1 < maxMass1; mass1 += dWdMass1)
    {
        wdMass1Grid[i] = mass1;
        i++;
    }
}
