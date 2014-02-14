#include <array>

#include "Star.hpp"
#include "Cluster.hpp"

#include "Model.hpp"

double margEvolveWithBinary (const Cluster &, const Star &, const Model&, const std::vector<int>&, std::array<double, 2>&, const std::array<double, FILTS>&, const std::array<double, FILTS>&);