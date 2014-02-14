#ifndef SETTINGS_H
#define SETTINGS_H

#include <array>
#include <string>

#include "constants.hpp"
#include "yaml-cpp/yaml.h"

class Settings
{
  public:
    void fromYaml (const std::string);
    void fromCLI (int, char **);

    uint32_t seed;
    unsigned int threads = std::numeric_limits<int>::max();

    int verbose;

    struct MainSequenceSettings
    {
        MsFilter filterSet;
        MsModel msRgbModel;
    };

    struct WhiteDwarfSettings
    {
        int ifmr;
        WdModel wdModel;
        double M_wd_up;
    };

    struct MpiMcmcSettings
    {
        int burnIter;
        int maxIter;
        int thin;

        int adaptiveBigSteps;
        int trialIter;

        bool bigStepBurnin = false;

        std::array<double, NPARAMS> stepSize;
    };

    struct SimClusterSettings
    {
        int nStars;
        int nFieldStars;
        int percentBinary;      // Fraction * 100
        int percentDB;          // Fraction * 100
    };

    struct ScatterClusterSettings
    {
        int relevantFilt;
        std::array<double, 14> exposures;
        double brightLimit;
        double faintLimit;
        double limitS2N;
    };

    struct ClusterSigmas
    {
        double Fe_H;
        double distMod;
        double Av;
        double Y;
        double carbonicity;
    };

    struct ClusterSettings
    {
        double Fe_H;
        double distMod;
        double Av;
        double Y;
        double carbonicity;

        struct ClusterSigmas sigma;

        double logClusAge;

        double minMag;
        double maxMag;
        int index;
    };

    struct SampleMassSettings
    {
        double deltaMass;
        double deltaMassRatio;
    };

    struct Files
    {
        std::string phot;
        std::string output;
        std::string scatter;
        std::string config;
        std::string models;
    };

    struct Files files;
    struct MainSequenceSettings mainSequence;
    struct WhiteDwarfSettings whiteDwarf;
    struct MpiMcmcSettings mpiMcmc;
    struct ClusterSettings cluster;
    struct SimClusterSettings simCluster;
    struct ScatterClusterSettings scatterCluster;
    struct SampleMassSettings sampleMass;

  private:
    template <typename T> T getDefault (YAML::Node &, std::string &&, T);
    template <typename T> T getOrDie (YAML::Node &, std::string &&);
    YAML::Node getNode (YAML::Node &, std::string &&);
    [[noreturn]] void exitWith (std::string &&);
};

#endif