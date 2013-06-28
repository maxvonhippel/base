/*** Last update: 19jun06 ***/
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mpi.h>
#include <unistd.h>

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
    FILE *rData;
    FILE *rSampledParamFile;
    FILE *wMassSampleFile;
    FILE *wMembershipFile;
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
    int verbose;
    int useFilt[FILTS];
    int numFilts;
    int nSamples;
    double start[NPARAMS];      /* starting points for grid evaluations */
    double end[NPARAMS];        /* end points for grid evaluations */
};

/* For posterior evaluation on a grid */
typedef struct
{
    double age;
    double FeH;
    double modulus;
    double absorption;
    double ifmrIntercept;
    double ifmrSlope;
    double ifmrQuadCoef;
} clustPar;

typedef struct
{
    double obsPhot[FILTS];
    double variance[FILTS];
    double clustStarPriorDens;  /* cluster membership prior probability */
} obsStar;

static void initIfmrGridControl (Chain *mc, struct ifmrGridControl *ctrl);
static void readCmdData (Chain *mc, struct ifmrGridControl *ctrl);
static void readSampledParams (Chain *mc, struct ifmrGridControl *ctrl, clustPar **sampledPars);
static void initChain (Chain *mc, const struct ifmrGridControl *ctrl);

double margEvolveWithBinary (Cluster *pCluster, Star *pStar);

/* declare global variables */
double filterPriorMin[FILTS];
double filterPriorMax[FILTS];

/* Used by evolve.c */
double ltau[2];
int aFilt = -1;

/* Used in densities.c. */
double priorMean[NPARAMS], priorVar[NPARAMS];
extern double ageLimit[2];      /* Defined in evolve.c, set in the appropriate model during loadModels. */

/* Used by a bunch of different functions. */
int verbose = 0, needMassNow = 0, useFilt[FILTS], numFilts = 0;

/* For random number generator (mt19937ar.c) */
unsigned long seed;

/* TEMPORARY - global variable */
double dMass1 = 0.0005;

Settings settings;

int main (int argc, char *argv[])
{
    int i, j, filt, numtasks,   /* total number of MPI process in partitiion */
        taskid,                 /* task identifier */
        dest,                   /* destination task id to send message */
        index,                  /* index into the array */
        source,                 /* origin task id of message */
        chunksize,                      /* for partitioning the array */
        extra, minchunk, nWDs = 0, nWDLogPosts;

    Chain mc;
    struct ifmrGridControl ctrl;

    double *wdMass;
    double *clusMemPost;
    double fsLike;
    obsStar *obs;
    int *starStatus;

    clustPar *sampledPars;
    double *unifs;              /* draw uniform random numbers ahead of time */

    MPI_Datatype clustParType;
    MPI_Datatype obsStarType;
    MPI_Status status;


    MPI_Init (&argc, &argv);
    MPI_Comm_rank (MPI_COMM_WORLD, &taskid);
    MPI_Comm_size (MPI_COMM_WORLD, &numtasks);

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

    MPI_Type_contiguous (7, MPI_DOUBLE, &clustParType);
    MPI_Type_commit (&clustParType);
    MPI_Type_contiguous (2 * FILTS + 1, MPI_DOUBLE, &obsStarType);
    MPI_Type_commit (&obsStarType);

    if (taskid == MASTER)
    {
        initIfmrGridControl (&mc, &ctrl);
    }
    else
    {
        ctrl.verbose = 0;
        ctrl.iStart = 0;
    }

    /*** broadcast control parameters to other processes ***/
    MPI_Bcast (&seed, 1, MPI_UNSIGNED_LONG, MASTER, MPI_COMM_WORLD);
    if (taskid != MASTER)
        init_genrand (seed);
    MPI_Bcast (&mc.clust.evoModels.WDcooling, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.evoModels.mainSequenceEvol, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.evoModels.filterSet, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.evoModels.brownDwarfEvol, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.evoModels.IFMR, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (&mc.clust.carbonicity, 1, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);

    mc.clust.evoModels.WDatm = BERGERON;

    printf ("carbonicity: %lf\n", mc.clust.carbonicity);

    if (taskid != MASTER) /* already loaded in the MASTER task */
    {
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

    MPI_Bcast (ctrl.start, NPARAMS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (ctrl.end, NPARAMS, MPI_DOUBLE, MASTER, MPI_COMM_WORLD);


    if (taskid == MASTER)
    {
        readCmdData (&mc, &ctrl);

        obs = new obsStar[mc.clust.nStars]();
        starStatus = new int[mc.clust.nStars]();

        for (i = 0; i < mc.clust.nStars; i++)
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

    MPI_Barrier (MPI_COMM_WORLD);
    if (taskid != MASTER)
    {
        obs = new obsStar[mc.clust.nStars]();
        starStatus = new int[mc.clust.nStars]();
    }

    MPI_Bcast (starStatus, mc.clust.nStars, MPI_INT, MASTER, MPI_COMM_WORLD);
    MPI_Bcast (obs, mc.clust.nStars, obsStarType, MASTER, MPI_COMM_WORLD);
    if (taskid != MASTER)
    {
        /* initialize the stars array */
        mc.stars.resize(mc.clust.nStars);

        for (i = 0; i < mc.clust.nStars; i++)
        {
            for (filt = 0; filt < ctrl.numFilts; filt++)
            {
                mc.stars.at(i).obsPhot[filt] = obs[i].obsPhot[filt];
                mc.stars.at(i).variance[filt] = obs[i].variance[filt];
            }
            mc.stars.at(i).clustStarPriorDens = obs[i].clustStarPriorDens;
            mc.stars.at(i).status[0] = starStatus[i];
            if (starStatus[i] == WD)
            {
                nWDs++;
            }
        }
    }

    initChain (&mc, &ctrl);

    for (i = 0; i < mc.clust.nStars; i++)
    {
        mc.stars.at(i).isFieldStar = 0;
        mc.stars.at(i).boundsFlag = 0;
    }

    double logFieldStarLikelihood = 0.0;

    for (filt = 0; filt < ctrl.numFilts; filt++)
    {
        logFieldStarLikelihood -= log (ctrl.filterPriorMax[filt] - ctrl.filterPriorMin[filt]);
    }
    fsLike = exp (logFieldStarLikelihood);


    int m;

    if (taskid == MASTER)
    {
        readSampledParams (&mc, &ctrl, &sampledPars);
        printf ("sampledPars[0].age = %lf\n", sampledPars[0].age);
        fflush (stdout);

        if ((unifs = (double *) calloc (ctrl.nSamples * nWDs, sizeof (double))) == NULL)
            perror ("MEMORY ALLOCATION ERROR \n");
        for (j = 0; j < ctrl.nSamples * nWDs; j++)
        {
            unifs[j] = genrand_res53 ();
        }
    }

    MPI_Bcast (&ctrl.nSamples, 1, MPI_INT, MASTER, MPI_COMM_WORLD);

    if (taskid != MASTER)
    {
        if ((sampledPars = (clustPar *) calloc (ctrl.nSamples, sizeof (clustPar))) == NULL)
            perror ("MEMORY ALLOCATION ERROR \n");
        if ((unifs = (double *) calloc (ctrl.nSamples * nWDs, sizeof (double))) == NULL)
            perror ("MEMORY ALLOCATION ERROR \n");
    }

    if (taskid == MASTER)
    {
        minchunk = ctrl.nSamples / numtasks;
        extra = ctrl.nSamples % numtasks;

        /*** divide workload ***/
        index = (extra > 0) ? (minchunk + 1) : minchunk;

        for (dest = 1; dest < numtasks; dest++)
        {
            chunksize = (dest < extra) ? (minchunk + 1) : minchunk;

            MPI_Send (&index, 1, MPI_INT, dest, 0, MPI_COMM_WORLD);
            MPI_Send (&chunksize, 1, MPI_INT, dest, 0, MPI_COMM_WORLD);
            MPI_Send (&sampledPars[index], chunksize, clustParType, dest, 0, MPI_COMM_WORLD);
            MPI_Send (&unifs[index * nWDs], chunksize * nWDs, MPI_DOUBLE, dest, 0, MPI_COMM_WORLD);
            index += chunksize;
        }

        /* master task takes beginning of array */
        index = 0;
        chunksize = (extra > 0) ? (minchunk + 1) : minchunk;
    }
    else
    {
        /* Receive portion of array from the master task */
        source = MASTER;
        MPI_Recv (&index, 1, MPI_INT, source, 0, MPI_COMM_WORLD, &status);
        MPI_Recv (&chunksize, 1, MPI_INT, source, 0, MPI_COMM_WORLD, &status);
        MPI_Recv (&sampledPars[index], chunksize, clustParType, source, 0, MPI_COMM_WORLD, &status);
        MPI_Recv (&unifs[index * nWDs], chunksize * nWDs, MPI_DOUBLE, source, 0, MPI_COMM_WORLD, &status);
    }

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

    double u, cumSum;

    for (m = index; m < index + chunksize; m++)
    {
        mc.clust.parameter[AGE] = sampledPars[m].age;
        mc.clust.parameter[FEH] = sampledPars[m].FeH;
        mc.clust.parameter[MOD] = sampledPars[m].modulus;
        mc.clust.parameter[ABS] = sampledPars[m].absorption;

        if (mc.clust.evoModels.IFMR >= 4)
        {
            mc.clust.parameter[IFMR_INTERCEPT] = sampledPars[m].ifmrIntercept;
            mc.clust.parameter[IFMR_SLOPE] = sampledPars[m].ifmrSlope;
        }

        if (mc.clust.evoModels.IFMR >= 9)
        {
            mc.clust.parameter[IFMR_QUADCOEF] = sampledPars[m].ifmrQuadCoef;
        }

        /************ sample WD masses for different parameters ************/
        int iWD = 0;
        int im;
        double wdPostSum, maxWDLogPost, mass1;
        double postClusterStar;

        for (j = 0; j < mc.clust.nStars; j++)
        {
            if (mc.stars.at(j).status[0] == WD)
            {

                postClusterStar = 0.0;

                im = 0;
                for (mass1 = 0.15; mass1 < mc.clust.M_wd_up; mass1 += dMass1)
                {
                    /* condition on WD being cluster star */
                    mc.stars.at(j).U = mass1;
                    mc.stars.at(j).massRatio = 0.0;
                    evolve (&mc.clust, mc.stars, j);

                    if (!mc.stars.at(j).boundsFlag)
                    {
                        wdLogPost[im] = logPost1Star (&mc.stars.at(j), &mc.clust);
                        postClusterStar += exp (wdLogPost[im]);
                    }
                    else
                    {
                        wdLogPost[im] = -HUGE_VAL;
                    }
                    im++;
                }
                im = 0;

                /* compute the maximum value */
                maxWDLogPost = wdLogPost[0];
                for (mass1 = 0.15; mass1 < mc.clust.M_wd_up; mass1 += dMass1)
                {
                    if (wdLogPost[im] > maxWDLogPost)
                        maxWDLogPost = wdLogPost[im];
                    im++;
                }

                /* compute the normalizing constant */
                wdPostSum = 0.0;
                im = 0;
                for (mass1 = 0.15; mass1 < mc.clust.M_wd_up; mass1 += dMass1)
                {
                    wdPostSum += exp (wdLogPost[im] - maxWDLogPost);
                    im++;
                }

                /* now sample a particular mass */
                u = unifs[m * nWDs + iWD];
                cumSum = 0.0;
                mass1 = 0.15;
                im = 0;
                while (cumSum < u && mass1 < mc.clust.M_wd_up)
                {
                    cumSum += exp (wdLogPost[im] - maxWDLogPost) / wdPostSum;
                    mass1 += dMass1;
                    im++;
                }
                mass1 -= dMass1;        /* maybe not necessary */

                wdMass[m * nWDs + iWD] = mass1;
                iWD++;

                postClusterStar *= (mc.clust.M_wd_up - 0.15);

                clusMemPost[m * nWDs + iWD] = mc.stars.at(j).clustStarPriorDens * postClusterStar / (mc.stars.at(j).clustStarPriorDens * postClusterStar + (1.0 - mc.stars.at(j).clustStarPriorDens) * fsLike);
            }
        }
    }


    /********** compile results *********/
    /*** now report sampled masses and parameters ***/
    if (taskid == MASTER)
    {
        /* Now wait to receive back the results from each worker task */
        for (i = 1; i < numtasks; i++)
        {
            source = i;
            MPI_Recv (&index, 1, MPI_INT, source, 1, MPI_COMM_WORLD, &status);
            MPI_Recv (&chunksize, 1, MPI_INT, source, 1, MPI_COMM_WORLD, &status);
            MPI_Recv (&sampledPars[index], chunksize, clustParType, source, 1, MPI_COMM_WORLD, &status);
            MPI_Recv (&wdMass[index * nWDs], chunksize * nWDs, MPI_DOUBLE, source, 1, MPI_COMM_WORLD, &status);
            MPI_Recv (&clusMemPost[index * nWDs], chunksize * nWDs, MPI_DOUBLE, source, 1, MPI_COMM_WORLD, &status);
        }

        /* Write output */
        for (i = 0; i < ctrl.nSamples; i++)
        {
            fprintf (ctrl.wMassSampleFile, "%10.6f ", sampledPars[i].age);
            fprintf (ctrl.wMassSampleFile, "%10.6f ", sampledPars[i].FeH);
            fprintf (ctrl.wMassSampleFile, "%10.6f ", sampledPars[i].modulus);
            fprintf (ctrl.wMassSampleFile, "%10.6f ", sampledPars[i].absorption);
            if (mc.clust.evoModels.IFMR >= 4)
            {
                fprintf (ctrl.wMassSampleFile, "%10.6f ", sampledPars[i].ifmrIntercept);
                fprintf (ctrl.wMassSampleFile, "%10.6f ", sampledPars[i].ifmrSlope);
            }
            if (mc.clust.evoModels.IFMR >= 9)
                fprintf (ctrl.wMassSampleFile, "%10.6f ", sampledPars[i].ifmrQuadCoef);
            for (j = 0; j < nWDs; j++)
            {
                fprintf (ctrl.wMassSampleFile, "%10.6f ", wdMass[i * nWDs + j]);
            }
            for (j = 0; j < nWDs; j++)
            {
                fprintf (ctrl.wMembershipFile, "%10.6f ", clusMemPost[i * nWDs + j]);
            }
            fprintf (ctrl.wMassSampleFile, "\n");
            fprintf (ctrl.wMembershipFile, "\n");
        }
        fclose (ctrl.wMassSampleFile);
        fclose (ctrl.wMembershipFile);

        printf ("Part 2 completed successfully\n");
    }
    else
    {
        /* Send results back to the master task */
        MPI_Send (&index, 1, MPI_INT, MASTER, 1, MPI_COMM_WORLD);
        MPI_Send (&chunksize, 1, MPI_INT, MASTER, 1, MPI_COMM_WORLD);
        MPI_Send (&sampledPars[index], chunksize, clustParType, MASTER, 1, MPI_COMM_WORLD);
        MPI_Send (&wdMass[index * nWDs], chunksize * nWDs, MPI_DOUBLE, MASTER, 1, MPI_COMM_WORLD);
        MPI_Send (&clusMemPost[index * nWDs], chunksize * nWDs, MPI_DOUBLE, MASTER, 1, MPI_COMM_WORLD);
    }


    /* clean up */
    free (wdLogPost);

    free (wdMass);              /* 1D array */
    free (clusMemPost);         /* 1D array */

    free (sampledPars);
    free (unifs);

    delete[] (obs);
    delete[] (starStatus);

    MPI_Type_free (&clustParType);
    MPI_Type_free (&obsStarType);
    MPI_Finalize ();
    return 0;
}


/*
 * read control parameters from input stream
 */
static void initIfmrGridControl (Chain *mc, struct ifmrGridControl *ctrl)
{
    ctrl->verbose = 0;
    ctrl->numFilts = 0;

    int ii;

    for (ii = 0; ii < FILTS; ii++)
    {
        ctrl->useFilt[ii] = 0;
    }

    seed = settings.seed;
    init_genrand (seed);

    loadModels (0, &mc->clust, settings);

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
    if (mc->clust.evoModels.IFMR >= 9)
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

    /* open files for reading (data) and writing */

    char filename[100];

    strcpy (filename, settings.files.phot.c_str());

    if ((ctrl->rData = fopen (filename, "r")) == NULL)
    {
        printf ("***Error: file %s was not found.***\n", filename);
        printf ("[Exiting...]\n");
        exit (1);
    }

    strcpy (filename, settings.files.output.c_str());
    strcat (filename, ".res");
    if ((ctrl->rSampledParamFile = fopen (filename, "r")) == NULL)
    {
        printf ("***Error: file %s was not found.***\n", filename);
        printf ("[Exiting...]\n");
        exit (1);
    }

    ctrl->minMag = settings.cluster.minMag;
    ctrl->maxMag = settings.cluster.maxMag;
    ctrl->iMag = settings.cluster.index;
    if (ctrl->iMag < 0 || ctrl->iMag > FILTS)
    {
        printf ("***Error: %d not a valid magnitude index.  Choose 0, 1,or 2.***\n", ctrl->iMag);
        printf ("[Exiting...]\n");
        exit (1);
    }

    strcpy (filename, settings.files.output.c_str());
    strcat (filename, ".massSamples");
    if ((ctrl->wMassSampleFile = fopen (filename, "w")) == NULL)
    {
        printf ("***Error: File %s was not available for writing.***\n", filename);
        printf ("[Exiting...]\n");
        exit (1);
    }
    strcat (filename, ".membership");
    if ((ctrl->wMembershipFile = fopen (filename, "w")) == NULL)
    {
        printf ("***Error: File %s was not available for writing.***\n", filename);
        printf ("[Exiting...]\n");
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


/*
 * Read CMD data
 */
static void readCmdData (Chain *mc, struct ifmrGridControl *ctrl)
{
    char line[300];
    double tempSigma;
    int filt, i;
    char *pch, sig[] = "sig\0", comp[] = "   \0";

    //Parse the header of the file to determine which filters are being used
    fgets (line, 300, ctrl->rData);     // Read in the header line

    pch = strtok (line, " ");   // split the string on these delimiters into "tokens"

    while (pch != NULL)
    {
        pch = strtok (NULL, " ");       // Ignore the first token (which is "id") and move
        // to the next (which should be the first filter name)

        strncpy (comp, pch, 3); // copy the first three letters into the dummy string 'comp'
        if (strcmp (comp, sig) == 0)
        {
            break;                      // and check to see if they are 'sig'.  If they are, there are no more filters
        }

        for (filt = 0; filt < FILTS; filt++)    // Otherwise check to see what this filter's name is
        {
            if (strcmp (pch, getFilterName (filt)) == 0)
            {
                ctrl->useFilt[filt] = 1;
                mc->clust.evoModels.numFilts++;

                if (aFilt < 0)
                {
                    aFilt = filt;               // Sets this to a band we know we are using (for evolve)
                }

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
            {
                aFilt = i;              // Sets this to a band we know we are using (for evolve)
            }
        }
    }

    // This loop reads in photometry data
    // It also reads a best guess for the mass
    int nr, j = 0;
    int moreStars = 1;          // true

    while (moreStars)
    {
        mc->stars.emplace_back();

        nr = fscanf (ctrl->rData, "%*s");

        if (nr == EOF)
            break;

        for (i = 0; i < ctrl->numFilts; i++)
        {
            fscanf (ctrl->rData, "%lf", &(mc->stars.at(j).obsPhot[i]));

            if (mc->stars.at(j).obsPhot[i] < ctrl->filterPriorMin[i])
                ctrl->filterPriorMin[i] = mc->stars.at(j).obsPhot[i];

            if (mc->stars.at(j).obsPhot[i] > ctrl->filterPriorMax[i])
                ctrl->filterPriorMax[i] = mc->stars.at(j).obsPhot[i];
        }

        // copy to global variables
        for (i = 0; i < ctrl->numFilts; i++)
        {
            filterPriorMin[i] = ctrl->filterPriorMin[i];
            filterPriorMax[i] = ctrl->filterPriorMax[i];
        }

        for (i = 0; i < ctrl->numFilts; i++)
        {
            fscanf (ctrl->rData, "%lf", &tempSigma);
            mc->stars.at(j).variance[i] = tempSigma * fabs (tempSigma);
            // The fabs() keeps the sign of the variance the same as that input by the user for sigma
            // Negative sigma (variance) is used to signal "don't count this band for this star"
        }

        fscanf (ctrl->rData, "%lf %lf %d %lf %d", &(mc->stars.at(j).U), &(mc->stars.at(j).massRatio), &(mc->stars.at(j).status[0]), &(mc->stars.at(j).clustStarPriorDens), &(mc->stars.at(j).useDuringBurnIn));

        if (mc->stars.at(j).status[0] == 3 || (mc->stars.at(j).obsPhot[ctrl->iMag] >= ctrl->minMag && mc->stars.at(j).obsPhot[ctrl->iMag] <= ctrl->maxMag))
        {
            j++;
        }
    }
    mc->clust.nStars = j;

    for (j = 0; j < mc->clust.nStars; j++)
    {
        mc->stars.at(j).massRatio = 0.0;
    }

    // copy to global values
    for (i = 0; i < FILTS; i++)
    {
        useFilt[i] = ctrl->useFilt[i];
    }

    numFilts = ctrl->numFilts;

    fclose (ctrl->rData);
} // readCmdData


/*
 * Read sampled params
 */
static void readSampledParams (Chain *mc, struct ifmrGridControl *ctrl, clustPar ** sampledPars)
{
    int nr, j = 0;
    int morePars = 1;           // true
    void *tempAlloc;            // temporary for allocation
    double logPost;

    char line[300];

    *sampledPars = nullptr;

    fgets (line, 300, ctrl->rSampledParamFile); // skip first header line

    while (morePars)
    {
        if ((j % ALLOC_CHUNK) == 0)
        {
            if ((tempAlloc = (void *) realloc (*sampledPars, (j + ALLOC_CHUNK) * sizeof (clustPar))) == NULL)
                perror ("MEMORY ALLOCATION ERROR \n");
            else
                *sampledPars = (clustPar *) tempAlloc;
        }

        nr = fscanf (ctrl->rSampledParamFile, "%lf", &(*sampledPars)[j].age);

        if (nr == EOF)
            break;

        fscanf (ctrl->rSampledParamFile, "%lf", &(*sampledPars)[j].FeH);
        fscanf (ctrl->rSampledParamFile, "%lf", &(*sampledPars)[j].modulus);
        fscanf (ctrl->rSampledParamFile, "%lf", &(*sampledPars)[j].absorption);

        if (mc->clust.evoModels.IFMR >= 9)
        {
            fscanf (ctrl->rSampledParamFile, "%lf", &(*sampledPars)[j].ifmrIntercept);
            fscanf (ctrl->rSampledParamFile, "%lf", &(*sampledPars)[j].ifmrSlope);
        }

        if (mc->clust.evoModels.IFMR >= 9)
        {
            fscanf (ctrl->rSampledParamFile, "%lf", &(*sampledPars)[j].ifmrQuadCoef);
        }

        fscanf (ctrl->rSampledParamFile, "%lf", &logPost);

        j++;
    }
    ctrl->nSamples = j;
    fclose (ctrl->rSampledParamFile);
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
    mc->clust.parameter[FEH] = ctrl->priorMean[FEH];
    mc->clust.parameter[MOD] = ctrl->priorMean[MOD];
    mc->clust.parameter[ABS] = ctrl->priorMean[ABS];
    mc->clust.parameter[YYY] = ctrl->priorMean[YYY];
    mc->clust.parameter[AGE] = ctrl->initialAge;
    mc->clust.parameter[IFMR_INTERCEPT] = ctrl->priorMean[IFMR_INTERCEPT];
    mc->clust.parameter[IFMR_SLOPE] = ctrl->priorMean[IFMR_SLOPE];
    mc->clust.parameter[IFMR_QUADCOEF] = ctrl->priorMean[IFMR_QUADCOEF];
    mc->clust.mean[AGE] = ctrl->initialAge;
    mc->clust.mean[YYY] = ctrl->priorMean[YYY];
    mc->clust.mean[MOD] = ctrl->priorMean[MOD];
    mc->clust.mean[FEH] = ctrl->priorMean[FEH];
    mc->clust.mean[ABS] = ctrl->priorMean[ABS];
    mc->clust.mean[IFMR_INTERCEPT] = ctrl->priorMean[IFMR_INTERCEPT];
    mc->clust.mean[IFMR_SLOPE] = ctrl->priorMean[IFMR_SLOPE];
    mc->clust.mean[IFMR_QUADCOEF] = ctrl->priorMean[IFMR_QUADCOEF];
    mc->clust.betamabs = 0.0;
    mc->clust.betaFabs = 0.0;

    int i, j;

    for (j = 0; j < mc->clust.nStars; j++)
    {
        mc->stars.at(j).meanMassRatio = 0.0;
        mc->stars.at(j).isFieldStar = 0;
        mc->stars.at(j).clustStarProposalDens = mc->stars.at(j).clustStarPriorDens;   // Use prior prob of being clus star
        mc->stars.at(j).UStepSize = 0.001; // within factor of ~2 for most main sequence stars
        mc->stars.at(j).massRatioStepSize = 0.001;

        for (i = 0; i < NPARAMS; i++)
        {
            mc->stars.at(j).beta[i][0] = 0.0;
            mc->stars.at(j).beta[i][1] = 0.0;
        }
        mc->stars.at(j).betaMassRatio[0] = 0.0;
        mc->stars.at(j).betaMassRatio[1] = 0.0;
        mc->stars.at(j).meanU = 0.0;
        mc->stars.at(j).varU = 0.0;

        for (i = 0; i < 2; i++)
            mc->stars.at(j).wdType[i] = DA;

        for (i = 0; i < numFilts; i++)
        {
            mc->stars.at(j).photometry[i] = 0.0;
        }

        // find photometry for initial values of currentClust and mc->stars
        if (mc->stars.at(j).status[0] == WD)
        {
            mc->stars.at(j).UStepSize = 0.05;      // use larger initial step size for white dwarfs
            mc->stars.at(j).massRatio = 0.0;
        }
    }

} // initChain
