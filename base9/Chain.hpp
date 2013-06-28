#include <array>
#include <vector>

class Chain
{
  public:
    std::array<bool, NPARAMS> acceptClust;
    std::array<bool, NPARAMS> rejectClust;
    std::vector<Star> stars;

    Cluster clust;

    int *acceptMass;
    int *rejectMass;
    int *acceptMassRatio;
    int *rejectMassRatio;
    int *isFieldStarNow;
    int *isClusterStarNow;

    double temperature;
};
